#include "schemaregistry/serdes/SerdeConfig.h"

#include <algorithm>

#include "schemaregistry/serdes/Serde.h"
#include "schemaregistry/serdes/SerdeError.h"

namespace schemaregistry::serdes {

// SerializerConfig implementation
SerializerConfig::SerializerConfig()
    : auto_register_schemas(true),
      use_schema(std::nullopt),
      normalize_schemas(false),
      validate(false),
      rule_config({}),
      subject_name_strategy(topicNameStrategy),
      schema_id_serializer(prefixSchemaIdSerializer) {}

SerializerConfig::SerializerConfig(
    bool auto_register_schemas, std::optional<SchemaSelector> use_schema,
    bool normalize_schemas, bool validate,
    const std::unordered_map<std::string, std::string> &rule_config)
    : auto_register_schemas(auto_register_schemas),
      use_schema(use_schema),
      normalize_schemas(normalize_schemas),
      validate(validate),
      rule_config(rule_config),
      subject_name_strategy(topicNameStrategy),
      schema_id_serializer(prefixSchemaIdSerializer) {}

SerializerConfig SerializerConfig::createDefault() {
    return SerializerConfig();
}

// DeserializerConfig implementation
DeserializerConfig::DeserializerConfig()
    : use_schema(std::nullopt),
      validate(false),
      rule_config({}),
      subject_name_strategy(topicNameStrategy),
      schema_id_deserializer(dualSchemaIdDeserializer) {}

DeserializerConfig::DeserializerConfig(
    std::optional<SchemaSelector> use_schema, bool validate,
    const std::unordered_map<std::string, std::string> &rule_config)
    : use_schema(use_schema),
      validate(validate),
      rule_config(rule_config),
      subject_name_strategy(topicNameStrategy),
      schema_id_deserializer(dualSchemaIdDeserializer) {}

DeserializerConfig DeserializerConfig::createDefault() {
    return DeserializerConfig();
}

// Default strategy functions implementation

std::optional<std::string> topicNameStrategy(
    const std::string &topic, SerdeType serde_type,
    const std::optional<Schema> &schema) {
    switch (serde_type) {
        case SerdeType::Key:
            return topic + "-key";
        case SerdeType::Value:
            return topic + "-value";
        default:
            return std::nullopt;
    }
}

std::vector<uint8_t> prefixSchemaIdSerializer(
    const std::vector<uint8_t> &payload, const SerializationContext &ser_ctx,
    const SchemaId &schema_id) {
    try {
        std::vector<uint8_t> id_bytes = schema_id.idToBytes();
        std::vector<uint8_t> result;
        result.reserve(id_bytes.size() + payload.size());
        result.insert(result.end(), id_bytes.begin(), id_bytes.end());
        result.insert(result.end(), payload.begin(), payload.end());
        return result;
    } catch (const std::exception &e) {
        throw SerializationError("Failed to serialize schema ID: " +
                                 std::string(e.what()));
    }
}

std::vector<uint8_t> headerSchemaIdSerializer(
    const std::vector<uint8_t> &payload, const SerializationContext &ser_ctx,
    const SchemaId &schema_id) {
    try {
        if (!ser_ctx.headers.has_value()) {
            throw SerializationError(
                "Headers are required for header schema ID serialization");
        }

        auto &headers = const_cast<SerdeHeaders &>(ser_ctx.headers.value());

        std::string header_key;
        switch (ser_ctx.serde_type) {
            case SerdeType::Key:
                header_key = KEY_SCHEMA_ID_HEADER;
                break;
            case SerdeType::Value:
                header_key = VALUE_SCHEMA_ID_HEADER;
                break;
            default:
                throw SerializationError(
                    "Invalid serde type for header serialization");
        }

        std::vector<uint8_t> header_value = schema_id.guidToBytes();
        SerdeHeader header(header_key, header_value);
        headers.insert(header);

        return payload;
    } catch (const std::exception &e) {
        throw SerializationError("Failed to serialize schema ID to header: " +
                                 std::string(e.what()));
    }
}

size_t dualSchemaIdDeserializer(const std::vector<uint8_t> &payload,
                                const SerializationContext &ser_ctx,
                                SchemaId &schema_id) {
    try {
        // First try to get schema ID from headers
        if (ser_ctx.headers.has_value()) {
            const auto &headers = ser_ctx.headers.value();

            std::string header_key;
            switch (ser_ctx.serde_type) {
                case SerdeType::Key:
                    header_key = KEY_SCHEMA_ID_HEADER;
                    break;
                case SerdeType::Value:
                    header_key = VALUE_SCHEMA_ID_HEADER;
                    break;
                default:
                    break;
            }

            if (!header_key.empty()) {
                auto header_value = headers.getLastHeaderValue(header_key);
                if (header_value.has_value() && !header_value.value().empty()) {
                    schema_id.readFromBytes(header_value.value());
                    return 0;  // No bytes consumed from payload
                }
            }
        }

        // Fall back to reading from payload prefix
        return schema_id.readFromBytes(payload);
    } catch (const std::exception &e) {
        throw SerializationError("Failed to deserialize schema ID: " +
                                 std::string(e.what()));
    }
}

size_t prefixSchemaIdDeserializer(const std::vector<uint8_t> &payload,
                                  const SerializationContext &ser_ctx,
                                  SchemaId &schema_id) {
    try {
        return schema_id.readFromBytes(payload);
    } catch (const std::exception &e) {
        throw SerializationError(
            "Failed to deserialize schema ID from prefix: " +
            std::string(e.what()));
    }
}

}  // namespace schemaregistry::serdes