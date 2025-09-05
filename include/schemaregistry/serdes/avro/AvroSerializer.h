#pragma once

#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Avro C++ includes
#include <avro/Compiler.hh>
#include <avro/Decoder.hh>
#include <avro/Encoder.hh>
#include <avro/Generic.hh>
#include <avro/Specific.hh>
#include <avro/ValidSchema.hh>

// Project includes
#include "schemaregistry/rest/model/Schema.h"
#include "schemaregistry/serdes/Serde.h"
#include "schemaregistry/serdes/SerdeConfig.h"
#include "schemaregistry/serdes/SerdeError.h"
#include "schemaregistry/serdes/SerdeTypes.h"

namespace schemaregistry::serdes::avro {

// Forward declarations
class AvroSerde;
using SerializationContext = schemaregistry::serdes::SerializationContext;
using BaseSerializer = schemaregistry::serdes::BaseSerializer;

/**
 * Avro-specific serializer implementation
 * Converts objects to Avro binary format with schema registry integration
 */
class AvroSerializer {
  public:
    /**
     * Constructor for AvroSerializer
     * @param client Schema registry client for schema operations
     * @param schema Optional schema to use for serialization
     * @param rule_registry Optional rule registry for field transformations
     * @param config Serializer configuration
     */
    AvroSerializer(
        std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
        std::optional<schemaregistry::rest::model::Schema> schema,
        std::shared_ptr<RuleRegistry> rule_registry,
        const SerializerConfig &config);

    /**
     * Destructor
     */
    ~AvroSerializer();

    /**
     * Serialize a generic Avro datum to bytes
     * @param ctx Serialization context (topic, serde type, etc.)
     * @param datum Avro generic datum to serialize
     * @return Serialized bytes with schema ID header
     */
    std::vector<uint8_t> serialize(const SerializationContext &ctx,
                                   const ::avro::GenericDatum &datum);

    /**
     * Serialize a JSON value to Avro bytes
     * Uses the JSON to Avro conversion before serialization
     * @param ctx Serialization context
     * @param json_value JSON value to convert and serialize
     * @return Serialized bytes with schema ID header
     */
    std::vector<uint8_t> serializeJson(const SerializationContext &ctx,
                                       const nlohmann::json &json_value);

    /**
     * Close the serializer and cleanup resources
     */
    void close();

    /**
     * Compile a JSON schema string to an Avro ValidSchema, removing
     * confluent:tags properties This is a utility method that can be used by
     * applications to compile schemas
     * @param schema_str JSON schema string to compile
     * @return Compiled Avro ValidSchema
     */
    static ::avro::ValidSchema compileJsonSchema(const std::string &schema_str);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace schemaregistry::serdes::avro