#include "schemaregistry/serdes/avro/AvroDeserializer.h"

#include <algorithm>
#include <sstream>

#include "schemaregistry/rest/RestException.h"
#include "schemaregistry/serdes/SerdeTypes.h"
#include "schemaregistry/serdes/avro/AvroUtils.h"

namespace schemaregistry::serdes::avro {

// PIMPL: AvroDeserializer::Impl
class AvroDeserializer::Impl {
  public:
    Impl(std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
         std::shared_ptr<RuleRegistry> rule_registry,
         const DeserializerConfig &config)
        : base_(std::make_shared<BaseDeserializer>(
              Serde(std::move(client), rule_registry), config)),
          serde_(std::make_shared<AvroSerde>()),
          subject_name_strategy_(configureSubjectNameStrategy(
              config.subject_name_strategy_type,
              base_->getSerde().getClient(),
              config.subject_name_strategy_config,
              [this](const std::optional<Schema> &s) { return getRecordName(s); })) {
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

    NamedValue deserialize(const SerializationContext &ctx,
                           const std::vector<uint8_t> &data) {
        // Get initial subject using configured subject name strategy (without schema)
        auto initial_subject =
            subject_name_strategy_(ctx.topic, ctx.serde_type, std::nullopt);
        std::optional<schemaregistry::rest::model::RegisteredSchema>
            latest_schema;

        // Try to get reader schema with initial subject
        if (initial_subject.has_value()) {
            try {
                latest_schema = base_->getSerde().getReaderSchema(
                    initial_subject.value(), std::nullopt,
                    base_->getConfig().use_schema);
            } catch (const schemaregistry::rest::RestException &e) {
                if (e.getStatus() != 404) {
                    throw;
                }
                // Schema not found - will be determined from writer schema
            }
        }

        // Extract schema ID from data
        SchemaId schema_id(SerdeFormat::Avro);
        auto id_deserializer = base_->getConfig().schema_id_deserializer;
        size_t bytes_read = id_deserializer(data, ctx, schema_id);
        std::vector<uint8_t> payload_data(data.begin() + bytes_read,
                                          data.end());

        // Get writer schema (pass nullopt when initial subject is unknown)
        auto writer_schema_raw =
            base_->getWriterSchema(schema_id, initial_subject, std::nullopt);
        auto writer_parsed = serde_->getParsedSchema(
            writer_schema_raw, base_->getSerde().getClient());

        // Recompute subject with writer schema (needed for Record/TopicRecord strategies)
        auto subject_opt = subject_name_strategy_(
            ctx.topic, ctx.serde_type, std::make_optional(writer_schema_raw));
        if (!subject_opt.has_value()) {
            throw SerializationError("Could not determine subject for deserialization");
        }
        const std::string &subject = subject_opt.value();

        // If subject changed, try to get reader schema again
        if (subject != initial_subject.value_or("") && !subject.empty()) {
            try {
                latest_schema = base_->getSerde().getReaderSchema(
                    subject, std::nullopt,
                    base_->getConfig().use_schema);
            } catch (const schemaregistry::rest::RestException &e) {
                if (e.getStatus() != 404) {
                    throw;
                }
                // Schema not found
            }
        }

        // Apply encoding rules if present (pre-decode)
        if (writer_schema_raw.getRuleSet().has_value()) {
            auto rule_set = writer_schema_raw.getRuleSet().value();
            if (rule_set.getEncodingRules().has_value()) {
                auto bytes_value =
                    SerdeValue::newBytes(SerdeFormat::Avro, payload_data);
                auto result = base_->getSerde().executeRulesWithPhase(
                    ctx, subject, Phase::Encoding, Mode::Read, std::nullopt,
                    std::make_optional(writer_schema_raw), *bytes_value, {});
                payload_data = result->asBytes();
            }
        }

        // Migrations processing
        std::vector<Migration> migrations;
        schemaregistry::rest::model::Schema reader_schema_raw;
        std::pair<::avro::ValidSchema, std::vector<::avro::ValidSchema>>
            reader_parsed;

        if (latest_schema.has_value()) {
            // Schema evolution path
            migrations = base_->getSerde().getMigrations(
                subject, writer_schema_raw, latest_schema.value(),
                std::nullopt);
            reader_schema_raw = latest_schema->toSchema();
            reader_parsed = serde_->getParsedSchema(
                reader_schema_raw, base_->getSerde().getClient());
        } else {
            // No evolution - writer and reader schemas are the same
            reader_schema_raw = writer_schema_raw;
            reader_parsed = writer_parsed;
        }

        // Deserialize Avro data
        ::avro::GenericDatum value;
        if (latest_schema.has_value()) {
            // Two-step process for schema evolution
            // 1. Deserialize with writer schema
            auto intermediate =
                utils::deserializeAvroData(payload_data, writer_parsed.first,
                                           nullptr, writer_parsed.second);

            // 2. Convert to JSON for migration
            auto json_value = utils::avroToJson(intermediate);
            auto json_serde_value =
                SerdeValue::newJson(SerdeFormat::Json, json_value);

            // 3. Apply migrations
            auto migrated = base_->getSerde().executeMigrations(
                ctx, subject, migrations, *json_serde_value);

            if (migrated->getFormat() != SerdeFormat::Json) {
                throw AvroError("Expected JSON value after migrations");
            }
            auto migrated_json = migrated->asJson();

            // 4. Convert back to Avro with reader schema
            value = utils::jsonToAvro(migrated_json, reader_parsed.first);
        } else {
            // Direct deserialization without evolution
            value = utils::deserializeAvroData(
                payload_data, writer_parsed.first, &reader_parsed.first,
                reader_parsed.second);
        }

        // Apply transformation rules
        auto parsed_schema = writer_parsed;

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

        auto serde_value = makeAvroValue(value);

        auto transformed = base_->getSerde().executeRules(
            ctx, subject, Mode::Read, std::nullopt,
            std::make_optional(reader_schema_raw), *serde_value,
            utils::getInlineTags(
                nlohmann::json::parse(reader_schema_raw.getSchema().value())),
            std::make_shared<FieldTransformer>(field_transformer));
        if (transformed->getFormat() == SerdeFormat::Avro) {
            value = asAvro(*transformed);
        }

        return NamedValue{getName(reader_parsed.first), std::move(value)};
    }

    nlohmann::json deserializeToJson(const SerializationContext &ctx,
                                     const std::vector<uint8_t> &data) {
        auto named_value = deserialize(ctx, data);
        return utils::avroToJson(named_value.value);
    }

    void close() {
        if (serde_) {
            serde_->clear();
        }
    }

  private:
    std::optional<std::string> getName(const ::avro::ValidSchema &schema) {
        return utils::getSchemaName(schema);
    }

    std::pair<::avro::ValidSchema, std::vector<::avro::ValidSchema>>
    getParsedSchema(const schemaregistry::rest::model::Schema &schema) {
        return serde_->getParsedSchema(schema, base_->getSerde().getClient());
    }

    std::string getRecordName(const std::optional<Schema> &schema) {
        if (!schema.has_value()) return "";
        auto [valid_schema, refs] = getParsedSchema(*schema);
        auto name = utils::getSchemaName(valid_schema);
        if (!name.has_value()) {
            throw AvroError("Could not determine record name from schema");
        }
        return name.value();
    }

    std::pair<size_t, ::avro::ValidSchema> resolveUnion(
        const ::avro::ValidSchema &schema, const ::avro::GenericDatum &datum) {
        return utils::resolveUnion(schema, datum);
    }

    FieldType getFieldType(const ::avro::ValidSchema &schema) {
        return utils::avroSchemaToFieldType(schema);
    }

  private:
    std::shared_ptr<BaseDeserializer> base_;
    std::shared_ptr<AvroSerde> serde_;
    SubjectNameStrategyFunc subject_name_strategy_;
};

// AvroDeserializer forwarding methods
AvroDeserializer::AvroDeserializer(
    std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
    std::shared_ptr<RuleRegistry> rule_registry,
    const DeserializerConfig &config)
    : impl_(std::make_unique<Impl>(std::move(client), std::move(rule_registry),
                                   config)) {}

AvroDeserializer::~AvroDeserializer() = default;

NamedValue AvroDeserializer::deserialize(const SerializationContext &ctx,
                                         const std::vector<uint8_t> &data) {
    return impl_->deserialize(ctx, data);
}

nlohmann::json AvroDeserializer::deserializeToJson(
    const SerializationContext &ctx, const std::vector<uint8_t> &data) {
    return impl_->deserializeToJson(ctx, data);
}

void AvroDeserializer::close() { impl_->close(); }

}  // namespace schemaregistry::serdes::avro