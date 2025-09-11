#include "schemaregistry/serdes/avro/AvroSerializer.h"

#include <algorithm>
#include <sstream>

#include "schemaregistry/serdes/avro/AvroUtils.h"

namespace schemaregistry::serdes::avro {

// AvroSerde implementation

std::pair<::avro::ValidSchema, std::vector<::avro::ValidSchema>>
AvroSerde::getParsedSchema(
    const schemaregistry::rest::model::Schema &schema,
    std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client) {
    // Generate cache key (reuse the updated SerdeTypes cache key generation)
    nlohmann::json j;
    to_json(j, schema);
    std::string cache_key = j.dump();

    // Check cache first
    {
        std::shared_lock lock(mutex_);
        auto it = parsed_schemas_.find(cache_key);
        if (it != parsed_schemas_.end()) {
            return it->second;
        }
    }

    // Parse schema with references
    std::vector<std::string> named_schema_strings;
    std::unordered_set<std::string> visited;
    resolveNamedSchema(schema, client, named_schema_strings, visited);

    // Parse the schema
    if (!schema.getSchema().has_value()) {
        throw AvroError("Schema string is not available");
    }

    auto parsed = utils::parseSchemaWithNamed(schema.getSchema().value(),
                                              named_schema_strings);

    // Cache the result
    {
        std::unique_lock lock(mutex_);
        parsed_schemas_[cache_key] = parsed;
    }

    return parsed;
}

void AvroSerde::resolveNamedSchema(
    const schemaregistry::rest::model::Schema &schema,
    std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
    std::vector<std::string> &schemas,
    std::unordered_set<std::string> &visited) {
    if (!schema.getReferences().has_value()) {
        return;
    }

    for (const auto &ref : schema.getReferences().value()) {
        std::string name = ref.getName().value_or("");
        if (visited.find(name) != visited.end()) {
            continue;
        }
        visited.insert(name);

        // Fetch referenced schema
        std::string subject = ref.getSubject().value_or("");
        int32_t version = ref.getVersion().value_or(-1);

        try {
            auto ref_schema =
                client->getVersion(subject, version, true, std::nullopt);
            auto converted_schema = ref_schema.toSchema();

            // Recursively resolve references
            resolveNamedSchema(converted_schema, client, schemas, visited);

            // Add this schema's content
            if (ref_schema.getSchema().has_value()) {
                schemas.push_back(ref_schema.getSchema().value());
            }
        } catch (const std::exception &e) {
            throw AvroError("Failed to resolve schema reference: " +
                            std::string(e.what()));
        }
    }
}

void AvroSerde::clear() {
    std::unique_lock lock(mutex_);
    parsed_schemas_.clear();
}

// AvroSerializer implementation (PIMPL)

class AvroSerializer::Impl {
  public:
    Impl(std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
         std::optional<schemaregistry::rest::model::Schema> schema,
         std::shared_ptr<RuleRegistry> rule_registry,
         const SerializerConfig &config)
        : schema_(std::move(schema)),
          base_(std::make_shared<BaseSerializer>(
              Serde(std::move(client), rule_registry), config)),
          serde_(std::make_shared<AvroSerde>()) {
        std::vector<std::shared_ptr<RuleExecutor>> executors;
        if (rule_registry) {
            executors = rule_registry->getExecutors();
        } else {
            executors = global_registry::getRuleExecutors();
        }
        for (const auto &executor : executors) {
            try {
                auto cfg = base_->getSerde().getClient()->getConfiguration();
                executor->configure(cfg, config.rule_config);
            } catch (const std::exception &e) {
                throw AvroError("Failed to configure rule executor: " +
                                std::string(e.what()));
            }
        }
    }

    std::vector<uint8_t> serialize(const SerializationContext &ctx,
                                   const ::avro::GenericDatum &datum) {
        auto value = datum;  // Copy for potential transformation

        // Get subject using strategy
        auto strategy = base_->getConfig().subject_name_strategy;
        auto subject_opt =
            strategy(ctx.topic, ctx.serde_type,
                     schema_.has_value() ? std::make_optional(schema_.value())
                                         : std::nullopt);
        if (!subject_opt.has_value()) {
            throw AvroError("Subject name strategy returned no subject");
        }
        std::string subject = subject_opt.value();

        // Get or register schema
        SchemaId schema_id(SerdeFormat::Avro);
        std::optional<schemaregistry::rest::model::RegisteredSchema>
            latest_schema;

        try {
            latest_schema = base_->getSerde().getReaderSchema(
                subject, std::nullopt, base_->getConfig().use_schema);
        } catch (const std::exception &e) {
            // Schema not found - will use provided schema
        }

        if (latest_schema.has_value()) {
            // Use latest schema from registry
            schema_id = SchemaId(SerdeFormat::Avro, latest_schema->getId(),
                                 latest_schema->getGuid(), std::nullopt);

            auto schema = latest_schema->toSchema();
            auto parsed_schema =
                serde_->getParsedSchema(schema, base_->getSerde().getClient());

            // Create field transformer lambda
            auto field_transformer =
                [this, &parsed_schema](
                    RuleContext &ctx, const std::string &rule_type,
                    const SerdeValue &msg) -> std::unique_ptr<SerdeValue> {
                if (msg.getFormat() == SerdeFormat::Avro) {
                    auto avro_datum = asAvro(msg);
                    auto transformed = utils::transformFields(
                        ctx, parsed_schema.first, avro_datum);
                    return makeAvroValue(transformed);
                }
                return msg.clone();
            };

            auto avro_value = makeAvroValue(value);

            // Execute rules on the serde value
            auto transformed_value = base_->getSerde().executeRules(
                ctx, subject, Mode::Write, std::nullopt,
                std::make_optional(schema), *avro_value,
                utils::getInlineTags(
                    nlohmann::json::parse(schema.getSchema().value())),
                std::make_shared<FieldTransformer>(field_transformer));

            // Extract Avro value from result
            if (transformed_value->getFormat() == SerdeFormat::Avro) {
                value = asAvro(*transformed_value);
            } else {
                throw AvroError(
                    "Unexpected serde value type returned from rule execution");
            }
        } else {
            // Use provided schema and register/lookup
            if (!schema_.has_value()) {
                throw AvroError(
                    "No schema provided and none found in registry");
            }

            schemaregistry::rest::model::RegisteredSchema registered_schema;
            if (base_->getConfig().auto_register_schemas) {
                registered_schema =
                    base_->getSerde().getClient()->registerSchema(
                        subject, schema_.value(),
                        base_->getConfig().normalize_schemas);
            } else {
                registered_schema = base_->getSerde().getClient()->getBySchema(
                    subject, schema_.value(),
                    base_->getConfig().normalize_schemas, false);
            }

            schema_id = SchemaId(SerdeFormat::Avro, registered_schema.getId(),
                                 registered_schema.getGuid(), std::nullopt);
        }

        // Parse schema for serialization
        auto schema_to_use = latest_schema.has_value()
                                 ? latest_schema->toSchema()
                                 : schema_.value();
        auto parsed_schema = serde_->getParsedSchema(
            schema_to_use, base_->getSerde().getClient());

        // Serialize Avro data
        auto avro_bytes = utils::serializeAvroData(value, parsed_schema.first,
                                                   parsed_schema.second);

        // Apply encoding rules if present
        if (latest_schema.has_value()) {
            auto schema = latest_schema->toSchema();
            if (schema.getRuleSet().has_value()) {
                auto rule_set = schema.getRuleSet().value();
                if (rule_set.getEncodingRules().has_value()) {
                    auto bytes_value =
                        SerdeValue::newBytes(SerdeFormat::Avro, avro_bytes);
                    auto result = base_->getSerde().executeRulesWithPhase(
                        ctx, subject, Phase::Encoding, Mode::Write,
                        std::nullopt, std::make_optional(schema), *bytes_value,
                        {});
                    avro_bytes = result->asBytes();
                }
            }
        }

        // Add schema ID header
        auto id_serializer = base_->getConfig().schema_id_serializer;
        return id_serializer(avro_bytes, ctx, schema_id);
    }

    std::vector<uint8_t> serializeJson(const SerializationContext &ctx,
                                       const nlohmann::json &json_value) {
        if (!schema_.has_value()) {
            throw AvroError("Schema required for JSON to Avro conversion");
        }
        auto parsed_schema = serde_->getParsedSchema(
            schema_.value(), base_->getSerde().getClient());
        auto datum = utils::jsonToAvro(json_value, parsed_schema.first);
        return serialize(ctx, datum);
    }

    void close() {
        if (serde_) {
            serde_->clear();
        }
    }

    std::pair<::avro::ValidSchema, std::vector<::avro::ValidSchema>>
    getParsedSchema(const schemaregistry::rest::model::Schema &schema) {
        return serde_->getParsedSchema(schema, base_->getSerde().getClient());
    }

  private:
    std::optional<schemaregistry::rest::model::Schema> schema_;
    std::shared_ptr<BaseSerializer> base_;
    std::shared_ptr<AvroSerde> serde_;
};

AvroSerializer::AvroSerializer(
    std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
    std::optional<schemaregistry::rest::model::Schema> schema,
    std::shared_ptr<RuleRegistry> rule_registry, const SerializerConfig &config)
    : impl_(std::make_unique<Impl>(std::move(client), std::move(schema),
                                   std::move(rule_registry), config)) {}

AvroSerializer::~AvroSerializer() = default;

std::vector<uint8_t> AvroSerializer::serialize(
    const SerializationContext &ctx, const ::avro::GenericDatum &datum) {
    return impl_->serialize(ctx, datum);
}

std::vector<uint8_t> AvroSerializer::serializeJson(
    const SerializationContext &ctx, const nlohmann::json &json_value) {
    return impl_->serializeJson(ctx, json_value);
}

void AvroSerializer::close() { impl_->close(); }

// Static utility methods

::avro::ValidSchema AvroSerializer::compileJsonSchema(
    const std::string &schema_str) {
    // Delegate to the existing AvroUtils implementation
    return utils::compileJsonSchema(schema_str);
}

}  // namespace schemaregistry::serdes::avro