#include "schemaregistry/serdes/json/JsonDeserializer.h"

#include "schemaregistry/serdes/json/JsonUtils.h"

namespace schemaregistry::serdes::json {

using namespace utils;

// Helper function to extract record name from JSON Schema model
static std::string getJsonSchemaRecordName(const std::optional<Schema> &schema) {
    if (!schema.has_value() || !schema->getSchema().has_value()) {
        return "";
    }
    try {
        auto json = nlohmann::json::parse(schema->getSchema().value());
        if (json.is_object()) {
            // Try "title" first, then "$id"
            if (json.contains("title") && json["title"].is_string()) {
                return json["title"].get<std::string>();
            }
            if (json.contains("$id") && json["$id"].is_string()) {
                std::string id = json["$id"].get<std::string>();
                // Extract name from URI (last path component)
                auto pos = id.find_last_of('/');
                if (pos != std::string::npos && pos < id.length() - 1) {
                    return id.substr(pos + 1);
                }
                return id;
            }
        }
    } catch (...) {
        // Fall through to return empty
    }
    return "";
}

class JsonDeserializer::Impl {
  public:
    Impl(std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
         std::shared_ptr<RuleRegistry> rule_registry,
         const DeserializerConfig &config)
        : base_(std::make_shared<BaseDeserializer>(
              Serde(std::move(client), rule_registry), config)),
          serde_(std::make_unique<JsonSerde>()),
          subject_name_strategy_(configureSubjectNameStrategy(
              config.subject_name_strategy_type, getJsonSchemaRecordName)) {
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
                throw JsonError("Failed to configure rule executor: " +
                                std::string(e.what()));
            }
        }
    }

    nlohmann::json deserialize(const SerializationContext &ctx,
                               const std::vector<uint8_t> &data) {
        // Get initial subject using configured subject name strategy (without schema)
        std::string initial_subject =
            subject_name_strategy_(ctx.topic, ctx.serde_type, std::nullopt);
        std::optional<schemaregistry::rest::model::RegisteredSchema>
            latest_schema;

        // Try to get reader schema with initial subject
        if (!initial_subject.empty()) {
            try {
                latest_schema = base_->getSerde().getReaderSchema(
                    initial_subject, std::nullopt,
                    base_->getConfig().use_schema);
            } catch (const std::exception &e) {
                // Schema not found - will be determined from writer schema
            }
        }

        // Parse schema ID from data
        SchemaId schema_id(SerdeFormat::Json);
        auto id_deserializer = base_->getConfig().schema_id_deserializer;
        size_t bytes_read = id_deserializer(data, ctx, schema_id);

        const uint8_t *message_data = data.data() + bytes_read;
        size_t message_size = data.size() - bytes_read;

        // Get writer schema
        auto writer_schema_raw =
            base_->getWriterSchema(schema_id, std::make_optional(initial_subject), std::nullopt);
        auto writer_schema = getParsedSchema(writer_schema_raw);

        // Recompute subject with writer schema (needed for Record/TopicRecord strategies)
        std::string subject =
            subject_name_strategy_(ctx.topic, ctx.serde_type,
                                   std::make_optional(writer_schema_raw));

        // If subject changed, try to get reader schema again
        if (subject != initial_subject && !subject.empty()) {
            try {
                latest_schema = base_->getSerde().getReaderSchema(
                    subject, std::nullopt,
                    base_->getConfig().use_schema);
            } catch (const std::exception &e) {
                // Schema not found
            }
        }

        if (subject.empty()) {
            throw JsonError("Could not determine subject name");
        }

        // Handle encoding rules
        std::vector<uint8_t> decoded_data;
        if (writer_schema_raw.getRuleSet().has_value()) {
            decoded_data.assign(message_data, message_data + message_size);
            auto rule_set = writer_schema_raw.getRuleSet().value();
            if (rule_set.getEncodingRules().has_value()) {
                auto bytes_value =
                    SerdeValue::newBytes(SerdeFormat::Json, decoded_data);
                auto result = base_->getSerde().executeRulesWithPhase(
                    ctx, subject, Phase::Encoding, Mode::Read, std::nullopt,
                    std::make_optional(writer_schema_raw), *bytes_value, {});
                decoded_data = result->asBytes();
            }
            message_data = decoded_data.data();
            message_size = decoded_data.size();
        }

        // Schema evolution handling
        std::vector<Migration> migrations;
        schemaregistry::rest::model::Schema reader_schema_raw;
        std::shared_ptr<jsoncons::jsonschema::json_schema<jsoncons::ojson>>
            reader_schema;

        if (latest_schema.has_value()) {
            // Schema evolution path
            migrations = base_->getSerde().getMigrations(
                subject, writer_schema_raw, latest_schema.value(),
                std::nullopt);
            reader_schema_raw = latest_schema->toSchema();
            reader_schema = getParsedSchema(reader_schema_raw);
        } else {
            // No evolution - writer and reader schemas are the same
            migrations = {};
            reader_schema_raw = writer_schema_raw;
            reader_schema = writer_schema;
        }

        // Parse JSON from bytes
        std::string json_string(reinterpret_cast<const char *>(message_data),
                                message_size);
        nlohmann::json value;
        try {
            value = nlohmann::json::parse(json_string);
        } catch (const nlohmann::json::parse_error &e) {
            throw JsonError("Failed to parse JSON: " + std::string(e.what()));
        }

        // Apply migrations if needed
        if (!migrations.empty()) {
            value = executeMigrations(ctx, subject, migrations, value);
        }

        // Create field transformer lambda
        auto field_transformer =
            [this, &reader_schema](
                RuleContext &ctx, const std::string &rule_type,
                const SerdeValue &msg) -> std::unique_ptr<SerdeValue> {
            if (msg.getFormat() == SerdeFormat::Json) {
                auto json = asJson(msg);
                auto transformed = utils::value_transform::transformFields(
                    ctx, reader_schema, json);
                return makeJsonValue(transformed);
            }
            return msg.clone();
        };

        auto json_value = makeJsonValue(value);

        // Execute rules on the serde value
        auto transformed_value = base_->getSerde().executeRules(
            ctx, subject, Mode::Read, std::nullopt, reader_schema_raw,
            *json_value, {},
            std::make_shared<FieldTransformer>(field_transformer));

        // Extract Json value from result
        if (transformed_value->getFormat() == SerdeFormat::Json) {
            value = asJson(*transformed_value);
        } else {
            throw JsonError(
                "Unexpected serde value type returned from rule execution");
        }

        // Validate JSON against reader schema if validation is enabled
        if (base_->getConfig().validate) {
            try {
                validation_utils::validateJson(reader_schema, value);
            } catch (const std::exception &e) {
                throw JsonValidationError("JSON validation failed: " +
                                          std::string(e.what()));
            }
        }

        return value;
    }

    void close() { serde_->clear(); }

    std::shared_ptr<jsoncons::jsonschema::json_schema<jsoncons::ojson>>
    getParsedSchema(const schemaregistry::rest::model::Schema &schema) {
        return serde_->getParsedSchema(schema, base_->getSerde().getClient());
    }

    nlohmann::json executeMigrations(const SerializationContext &ctx,
                                     const std::string &subject,
                                     const std::vector<Migration> &migrations,
                                     const nlohmann::json &value) {
        auto serde_value = makeJsonValue(value);
        auto migrated_value = base_->getSerde().executeMigrations(
            ctx, subject, migrations, *serde_value);
        return asJson(*migrated_value);
    }

  private:
    std::shared_ptr<BaseDeserializer> base_;
    std::unique_ptr<JsonSerde> serde_;
    SubjectNameStrategyFunc subject_name_strategy_;
};

JsonDeserializer::JsonDeserializer(
    std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
    std::shared_ptr<RuleRegistry> rule_registry,
    const DeserializerConfig &config)
    : impl_(std::make_unique<Impl>(std::move(client), std::move(rule_registry),
                                   config)) {}

JsonDeserializer::~JsonDeserializer() = default;

nlohmann::json JsonDeserializer::deserialize(const SerializationContext &ctx,
                                             const std::vector<uint8_t> &data) {
    return impl_->deserialize(ctx, data);
}

void JsonDeserializer::close() { impl_->close(); }

}  // namespace schemaregistry::serdes::json