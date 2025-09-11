#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "schemaregistry/serdes/Serde.h"
#include "schemaregistry/serdes/SerdeConfig.h"
#include "schemaregistry/serdes/SerdeError.h"
#include "schemaregistry/serdes/SerdeTypes.h"

namespace schemaregistry::serdes::json {

// Forward declarations
class JsonSerializer;

class JsonSerde;

/**
 * JSON serializer class template
 * Based on JsonSerializer from json.rs (converted to synchronous)
 */
class JsonSerializer {
  public:
    /**
     * Constructor
     */
    JsonSerializer(
        std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
        std::optional<schemaregistry::rest::model::Schema> schema,
        std::shared_ptr<RuleRegistry> rule_registry,
        const SerializerConfig &config);

    ~JsonSerializer();

    /**
     * Serialize a JSON value to bytes with schema validation
     * @param ctx Serialization context (topic, serde type, etc.)
     * @param value JSON value to serialize
     * @return Serialized bytes with schema ID header
     */
    std::vector<uint8_t> serialize(const SerializationContext &ctx,
                                   const nlohmann::json &value);

    /**
     * Close the serializer and cleanup resources
     */
    void close();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace schemaregistry::serdes::json