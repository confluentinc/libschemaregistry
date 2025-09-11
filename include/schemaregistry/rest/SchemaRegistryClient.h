/**
 * Confluent Schema Registry Client
 * Synchronous C++ client for interacting with Confluent Schema Registry
 */

#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "schemaregistry/rest/ClientConfiguration.h"
#include "schemaregistry/rest/ISchemaRegistryClient.h"
#include "schemaregistry/rest/RestClient.h"
#include "schemaregistry/rest/RestException.h"
#include "schemaregistry/rest/SchemaStore.h"
#include "schemaregistry/rest/TtlLruCache.h"
#include "schemaregistry/rest/model/RegisteredSchema.h"
#include "schemaregistry/rest/model/Schema.h"
#include "schemaregistry/rest/model/ServerConfig.h"

namespace schemaregistry::rest {

/**
 * Synchronous Schema Registry Client implementation
 */
class SchemaRegistryClient : public ISchemaRegistryClient {
  private:
    std::shared_ptr<schemaregistry::rest::RestClient> restClient;
    std::shared_ptr<SchemaStore> store;
    std::shared_ptr<std::mutex> storeMutex;

    // Caches for latest versions
    TtlLruCache<std::string, schemaregistry::rest::model::RegisteredSchema>
        latestVersionCache;
    TtlLruCache<std::string, schemaregistry::rest::model::RegisteredSchema>
        latestWithMetadataCache;

    // Helper methods
    std::string urlEncode(const std::string &str) const;

    std::string createMetadataKey(
        const std::string &subject,
        const std::unordered_map<std::string, std::string> &metadata) const;

    // JSON processing helpers
    schemaregistry::rest::model::Schema parseSchemaFromJson(
        const std::string &json) const;

    schemaregistry::rest::model::RegisteredSchema parseRegisteredSchemaFromJson(
        const std::string &json) const;

    schemaregistry::rest::model::ServerConfig parseConfigFromJson(
        const std::string &json) const;

    bool parseBoolFromJson(const std::string &json) const;

    std::vector<int32_t> parseIntArrayFromJson(const std::string &json) const;

    std::vector<std::string> parseStringArrayFromJson(
        const std::string &json) const;

    // HTTP request helpers
    std::string sendHttpRequest(
        const std::string &path, const std::string &method,
        const std::vector<std::pair<std::string, std::string>> &query = {},
        const std::string &body = "") const;

  public:
    /**
     * Constructor
     */
    SchemaRegistryClient(
        std::shared_ptr<const schemaregistry::rest::ClientConfiguration>
            config);

    /**
     * Destructor
     */
    ~SchemaRegistryClient() override;

    /**
     * Factory method to create a client instance
     * Returns MockSchemaRegistryClient for mock:// URLs, otherwise
     * SchemaRegistryClient
     */
    static std::shared_ptr<ISchemaRegistryClient> newClient(
        std::shared_ptr<const schemaregistry::rest::ClientConfiguration>
            config);

    /**
     * Get client configuration
     */
    std::shared_ptr<const schemaregistry::rest::ClientConfiguration>
    getConfiguration() const override;

    // Implement ISchemaRegistryClient methods
    schemaregistry::rest::model::RegisteredSchema registerSchema(
        const std::string &subject,
        const schemaregistry::rest::model::Schema &schema,
        bool normalize = false) override;

    schemaregistry::rest::model::Schema getBySubjectAndId(
        const std::optional<std::string> &subject, int32_t id,
        const std::optional<std::string> &format = std::nullopt) override;

    schemaregistry::rest::model::Schema getByGuid(
        const std::string &guid,
        const std::optional<std::string> &format = std::nullopt) override;

    schemaregistry::rest::model::RegisteredSchema getBySchema(
        const std::string &subject,
        const schemaregistry::rest::model::Schema &schema,
        bool normalize = false, bool deleted = false) override;

    schemaregistry::rest::model::RegisteredSchema getVersion(
        const std::string &subject, int32_t version, bool deleted = false,
        const std::optional<std::string> &format = std::nullopt) override;

    schemaregistry::rest::model::RegisteredSchema getLatestVersion(
        const std::string &subject,
        const std::optional<std::string> &format = std::nullopt) override;

    schemaregistry::rest::model::RegisteredSchema getLatestWithMetadata(
        const std::string &subject,
        const std::unordered_map<std::string, std::string> &metadata,
        bool deleted = false,
        const std::optional<std::string> &format = std::nullopt) override;

    std::vector<int32_t> getAllVersions(const std::string &subject) override;

    std::vector<std::string> getAllSubjects(bool deleted = false) override;

    std::vector<int32_t> deleteSubject(const std::string &subject,
                                       bool permanent = false) override;

    int32_t deleteSubjectVersion(const std::string &subject, int32_t version,
                                 bool permanent = false) override;

    bool testSubjectCompatibility(
        const std::string &subject,
        const schemaregistry::rest::model::Schema &schema) override;

    bool testCompatibility(
        const std::string &subject, int32_t version,
        const schemaregistry::rest::model::Schema &schema) override;

    schemaregistry::rest::model::ServerConfig getConfig(
        const std::string &subject) override;

    schemaregistry::rest::model::ServerConfig updateConfig(
        const std::string &subject,
        const schemaregistry::rest::model::ServerConfig &config) override;

    schemaregistry::rest::model::ServerConfig getDefaultConfig() override;

    schemaregistry::rest::model::ServerConfig updateDefaultConfig(
        const schemaregistry::rest::model::ServerConfig &config) override;

    void clearLatestCaches() override;

    void clearCaches() override;

    void close() override;
};

}  // namespace schemaregistry::rest