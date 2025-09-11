#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

#include "schemaregistry/serdes/SerdeTypes.h"

namespace schemaregistry::serdes {

// Forward declarations
struct SerializationContext;
class SchemaId;

/**
 * Configuration for serialization operations
 * Based on SerializerConfig from config.rs
 */
struct SerializerConfig {
    bool auto_register_schemas;
    std::optional<SchemaSelector> use_schema;
    bool normalize_schemas;
    bool validate;
    std::unordered_map<std::string, std::string> rule_config;
    SubjectNameStrategy subject_name_strategy;
    SchemaIdSerializer schema_id_serializer;

    // Constructors
    SerializerConfig();
    SerializerConfig(
        bool auto_register_schemas, std::optional<SchemaSelector> use_schema,
        bool normalize_schemas, bool validate,
        const std::unordered_map<std::string, std::string> &rule_config);

    // Default configuration factory
    static SerializerConfig createDefault();

    // Copy/move constructors and assignment operators
    SerializerConfig(const SerializerConfig &) = default;
    SerializerConfig(SerializerConfig &&) = default;
    SerializerConfig &operator=(const SerializerConfig &) = default;
    SerializerConfig &operator=(SerializerConfig &&) = default;
};

/**
 * Configuration for deserialization operations
 * Based on DeserializerConfig from config.rs
 */
struct DeserializerConfig {
    std::optional<SchemaSelector> use_schema;
    bool validate;
    std::unordered_map<std::string, std::string> rule_config;
    SubjectNameStrategy subject_name_strategy;
    SchemaIdDeserializer schema_id_deserializer;

    // Constructors
    DeserializerConfig();
    DeserializerConfig(
        std::optional<SchemaSelector> use_schema, bool validate,
        const std::unordered_map<std::string, std::string> &rule_config);

    // Default configuration factory
    static DeserializerConfig createDefault();

    // Copy/move constructors and assignment operators
    DeserializerConfig(const DeserializerConfig &) = default;
    DeserializerConfig(DeserializerConfig &&) = default;
    DeserializerConfig &operator=(const DeserializerConfig &) = default;
    DeserializerConfig &operator=(DeserializerConfig &&) = default;
};

/**
 * Default strategy functions (from config.rs and serde.rs)
 * These are declared here and defined in the implementation
 */

/**
 * Topic name strategy function
 * Maps to topic_name_strategy from serde.rs
 */
std::optional<std::string> topicNameStrategy(
    const std::string &topic, SerdeType serde_type,
    const std::optional<Schema> &schema);

/**
 * Prefix schema ID serializer
 * Maps to prefix_schema_id_serializer from serde.rs
 */
std::vector<uint8_t> prefixSchemaIdSerializer(
    const std::vector<uint8_t> &payload, const SerializationContext &ser_ctx,
    const SchemaId &schema_id);

/**
 * Header schema ID serializer
 * Maps to header_schema_id_serializer from serde.rs
 */
std::vector<uint8_t> headerSchemaIdSerializer(
    const std::vector<uint8_t> &payload, const SerializationContext &ser_ctx,
    const SchemaId &schema_id);

/**
 * Dual schema ID deserializer
 * Maps to dual_schema_id_deserializer from serde.rs
 */
size_t dualSchemaIdDeserializer(const std::vector<uint8_t> &payload,
                                const SerializationContext &ser_ctx,
                                SchemaId &schema_id);

/**
 * Prefix schema ID deserializer
 * Maps to prefix_schema_id_deserializer from serde.rs
 */
size_t prefixSchemaIdDeserializer(const std::vector<uint8_t> &payload,
                                  const SerializationContext &ser_ctx,
                                  SchemaId &schema_id);

}  // namespace schemaregistry::serdes