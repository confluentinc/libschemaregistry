#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "schemaregistry/rest/ISchemaRegistryClient.h"
#include "schemaregistry/serdes/Serde.h"
#include "schemaregistry/serdes/SerdeConfig.h"
#include "schemaregistry/serdes/SerdeError.h"
#include "schemaregistry/serdes/SerdeTypes.h"
#include "schemaregistry/serdes/json/JsonSerializer.h"  // For JsonSerde

namespace schemaregistry::serdes::json {

// Forward declarations
class JsonDeserializer;
using SerializationContext = schemaregistry::serdes::SerializationContext;

/**
 * JSON deserializer class template
 * Based on JsonDeserializer from json.rs (converted to synchronous)
 */
class JsonDeserializer {
  public:
    /**
     * Constructor
     */
    JsonDeserializer(
        std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
        std::shared_ptr<RuleRegistry> rule_registry,
        const DeserializerConfig &config);

    ~JsonDeserializer();

    /**
     * Deserialize bytes to JSON object with schema validation and migration
     * @param ctx Serialization context (topic, serde type, etc.)
     * @param data Serialized bytes with schema ID header
     * @return Deserialized JSON object
     */
    nlohmann::json deserialize(const SerializationContext &ctx,
                               const std::vector<uint8_t> &data);

    /**
     * Close the deserializer and cleanup resources
     */
    void close();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace schemaregistry::serdes::json