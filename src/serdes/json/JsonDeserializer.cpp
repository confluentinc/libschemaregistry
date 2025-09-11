#include "schemaregistry/serdes/json/JsonDeserializer.h"

#include "schemaregistry/serdes/json/JsonUtils.h"

namespace schemaregistry::serdes::json {

using namespace utils;

class JsonDeserializer::Impl {
  public:
    Impl(std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
         std::shared_ptr<RuleRegistry> rule_registry,
         const DeserializerConfig &config)
        : base_(std::make_shared<BaseDeserializer>(
              Serde(std::move(client), rule_registry), config)),
          serde_(std::make_unique<JsonSerde>()) {
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
        // Determine subject
        auto strategy = base_->getConfig().subject_name_strategy;
        auto subject_opt = strategy(ctx.topic, ctx.serde_type, std::nullopt);
        std::optional<schemaregistry::rest::model::RegisteredSchema>
            latest_schema;
        bool has_subject = subject_opt.has_value();

        if (has_subject) {
            latest_schema = base_->getSerde().getReaderSchema(
                subject_opt.value(), std::nullopt,
                base_->getConfig().use_schema);
        }

        // Parse schema ID from data
        SchemaId schema_id(SerdeFormat::Json);
        auto id_deserializer = base_->getConfig().schema_id_deserializer;
        size_t bytes_read = id_deserializer(data, ctx, schema_id);

        const uint8_t *message_data = data.data() + bytes_read;
        size_t message_size = data.size() - bytes_read;

        // Get writer schema
        auto writer_schema_raw =
            base_->getWriterSchema(schema_id, subject_opt, std::nullopt);
        auto writer_schema = getParsedSchema(writer_schema_raw);

        // Re-determine subject if needed
        if (!has_subject) {
            subject_opt = strategy(ctx.topic, ctx.serde_type,
                                   std::make_optional(writer_schema_raw));
            if (subject_opt.has_value()) {
                latest_schema = base_->getSerde().getReaderSchema(
                    subject_opt.value(), std::nullopt,
                    base_->getConfig().use_schema);
            }
        }

        if (!subject_opt.has_value()) {
            throw JsonError("Could not determine subject name");
        }
        std::string subject = subject_opt.value();

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