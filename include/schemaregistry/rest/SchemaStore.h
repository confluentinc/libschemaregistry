/**
 * Confluent Schema Registry Client
 * Schema store for caching schema and registered schema information
 */

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "schemaregistry/rest/model/RegisteredSchema.h"
#include "schemaregistry/rest/model/Schema.h"

namespace schemaregistry::rest {

/**
 * Schema store for caching schema and registered schema information
 */
class SchemaStore {
  private:
    // Maps subject -> schema_id -> (guid, schema)
    std::unordered_map<
        std::string,
        std::unordered_map<int32_t,
                           std::pair<std::optional<std::string>,
                                     schemaregistry::rest::model::Schema>>>
        schemaIdIndex;

    // Maps guid -> schema
    std::unordered_map<std::string, schemaregistry::rest::model::Schema>
        schemaGuidIndex;

    // Maps subject -> schema -> schema_id
    std::unordered_map<std::string, std::unordered_map<std::string, int32_t>>
        schemaIndex;

    // Maps subject -> schema_id -> registered_schema
    std::unordered_map<
        std::string,
        std::unordered_map<int32_t,
                           schemaregistry::rest::model::RegisteredSchema>>
        rsIdIndex;

    // Maps subject -> version -> registered_schema
    std::unordered_map<
        std::string,
        std::unordered_map<int32_t,
                           schemaregistry::rest::model::RegisteredSchema>>
        rsVersionIndex;

    // Maps subject -> schema -> registered_schema
    std::unordered_map<
        std::string,
        std::unordered_map<std::string,
                           schemaregistry::rest::model::RegisteredSchema>>
        rsSchemaIndex;

  public:
    SchemaStore();

    ~SchemaStore() = default;

    /**
     * Set schema information
     */
    void setSchema(const std::optional<std::string> &subject,
                   const std::optional<int32_t> &schemaId,
                   const std::optional<std::string> &schemaGuid,
                   const schemaregistry::rest::model::Schema &schema);

    /**
     * Set registered schema information
     */
    void setRegisteredSchema(
        const schemaregistry::rest::model::Schema &schema,
        const schemaregistry::rest::model::RegisteredSchema &rs);

    /**
     * Get schema by subject and id
     */
    std::optional<std::pair<std::optional<std::string>,
                            schemaregistry::rest::model::Schema>>
    getSchemaById(const std::string &subject, int32_t schemaId) const;

    /**
     * Get schema by guid
     */
    std::optional<schemaregistry::rest::model::Schema> getSchemaByGuid(
        const std::string &guid) const;

    /**
     * Get schema id by subject and schema
     */
    std::optional<int32_t> getIdBySchema(
        const std::string &subject,
        const schemaregistry::rest::model::Schema &schema) const;

    /**
     * Get registered schema by subject and schema
     */
    std::optional<schemaregistry::rest::model::RegisteredSchema>
    getRegisteredBySchema(
        const std::string &subject,
        const schemaregistry::rest::model::Schema &schema) const;

    /**
     * Get registered schema by subject and version
     */
    std::optional<schemaregistry::rest::model::RegisteredSchema>
    getRegisteredByVersion(const std::string &subject, int32_t version) const;

    /**
     * Get registered schema by subject and id
     */
    std::optional<schemaregistry::rest::model::RegisteredSchema>
    getRegisteredById(const std::string &subject, int32_t schemaId) const;

    /**
     * Clear all cached data
     */
    void clear();

  private:
    // Helper to create a string hash for schemas
    std::string createSchemaHash(
        const schemaregistry::rest::model::Schema &schema) const;
};

}  // namespace schemaregistry::rest