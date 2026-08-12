#pragma once

#include <any>
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "JsonValue.h"
#include "schemaregistry/rest/ISchemaRegistryClient.h"
#include "schemaregistry/serdes/SerdeError.h"
#include "schemaregistry/serdes/SerdeTypes.h"

namespace schemaregistry::serdes::json {

using Resolver = std::function<jsoncons::ojson(const jsoncons::uri &)>;

/**
 * JSON schema caching and validation class
 * Based on JsonSerde struct from json.rs (converted to synchronous)
 */
class JsonSerde {
  public:
    JsonSerde();
    ~JsonSerde() = default;

    // Schema parsing and caching
    std::shared_ptr<jsoncons::jsonschema::json_schema<jsoncons::ojson>>
    getParsedSchema(
        const schemaregistry::rest::model::Schema &schema,
        std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client);

    /**
     * The schema's JSON with its references resolved and inlined, parsed once
     * and cached. Inline validation walks the schema JSON rather than the
     * compiled schema, and needs references flattened so that rules declared in
     * a referenced schema are reachable; without that they would be silently
     * skipped.
     */
    std::shared_ptr<const nlohmann::json> getSchemaJson(
        const schemaregistry::rest::model::Schema &schema,
        std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client);

    // Clear caches
    void clear();

  private:
    // Cache for parsed schemas: Schema -> json_schema
    std::unordered_map<
        std::string,
        std::shared_ptr<jsoncons::jsonschema::json_schema<jsoncons::ojson>>>
        parsed_schemas_cache_;

    // Cache for raw schema JSON: schema string -> parsed json
    std::unordered_map<std::string, std::shared_ptr<const nlohmann::json>>
        schema_json_cache_;

    mutable std::mutex cache_mutex_;

    // Helper methods
    void resolveNamedSchema(
        const schemaregistry::rest::model::Schema &schema,
        std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
        std::unordered_map<std::string, std::string> references);
};

/**
 * JSON referencing errors
 * Maps to SerdeError::JsonReferencing variant
 */
class JsonReferencingError : public SerdeError {
  public:
    explicit JsonReferencingError(const std::string &message)
        : SerdeError("JSON referencing error: " + message) {}
    explicit JsonReferencingError() : SerdeError("JSON referencing error") {}
};

/**
 * JSON serialization errors
 * Maps to SerdeError::Json variant
 */
class JsonError : public SerdeError {
  public:
    explicit JsonError(const std::string &message)
        : SerdeError("JSON serde error: " + message) {}
};

/**
 * JSON validation errors
 * Maps to SerdeError::JsonValidation variant
 */
class JsonValidationError : public SerdeError {
  public:
    explicit JsonValidationError(const std::string &message)
        : SerdeError("JSON validation error: " + message) {}
};

jsoncons::ojson asOJson(const SerdeValue &value);

std::unique_ptr<SerdeValue> makeJsonValue(const jsoncons::ojson &value);

std::unique_ptr<SerdeValue> makeJsonValue(jsoncons::ojson &&value);

}  // namespace schemaregistry::serdes::json
