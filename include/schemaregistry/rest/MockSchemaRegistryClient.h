/**
 * Mock Schema Registry Client
 * Synchronous C++ mock implementation for testing
 */

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "schemaregistry/rest/ClientConfiguration.h"
#include "schemaregistry/rest/RestException.h"
#include "schemaregistry/rest/SchemaRegistryClient.h"
#include "schemaregistry/rest/model/RegisteredSchema.h"
#include "schemaregistry/rest/model/Schema.h"
#include "schemaregistry/rest/model/ServerConfig.h"

namespace schemaregistry::rest {

class MockSchemaStore {
  private:
    // Maps subject -> list of registered schemas
    std::unordered_map<
        std::string, std::vector<schemaregistry::rest::model::RegisteredSchema>>
        schemas;

    // Maps schema id -> registered schema
    std::unordered_map<int32_t, schemaregistry::rest::model::RegisteredSchema>
        schemaIdIndex;

    // Maps guid -> registered schema
    std::unordered_map<std::string,
                       schemaregistry::rest::model::RegisteredSchema>
        schemaGuidIndex;

    // Maps schema content -> registered schema
    std::unordered_map<std::string,
                       schemaregistry::rest::model::RegisteredSchema>
        schemaIndex;

    int32_t nextSchemaId = 1;

  public:
    MockSchemaStore() = default;
    ~MockSchemaStore() = default;

    void setRegisteredSchema(
        const schemaregistry::rest::model::RegisteredSchema &schema);

    std::optional<schemaregistry::rest::model::Schema> getSchemaById(
        int32_t schemaId) const;

    std::optional<schemaregistry::rest::model::Schema> getSchemaByGuid(
        const std::string &guid) const;

    std::optional<schemaregistry::rest::model::RegisteredSchema>
    getRegisteredBySchema(
        const std::string &subject,
        const schemaregistry::rest::model::Schema &schema) const;

    std::optional<schemaregistry::rest::model::RegisteredSchema>
    getRegisteredByVersion(const std::string &subject, int32_t version) const;

    std::optional<schemaregistry::rest::model::RegisteredSchema>
    getLatestVersion(const std::string &subject) const;

    std::optional<schemaregistry::rest::model::RegisteredSchema>
    getLatestWithMetadata(
        const std::string &subject,
        const std::unordered_map<std::string, std::string> &metadata) const;

    std::vector<std::string> getSubjects() const;

    std::vector<int32_t> getVersions(const std::string &subject) const;

    void removeBySchema(
        const std::string &subject,
        const schemaregistry::rest::model::RegisteredSchema &registeredSchema);

    std::vector<int32_t> removeBySubject(const std::string &subject);

    void clear();

    friend class MockSchemaRegistryClient;

  private:
    bool hasMetadata(
        const std::unordered_map<std::string, std::string> &metadata,
        const schemaregistry::rest::model::RegisteredSchema &rs) const;

    std::string generateGuid() const;
};

/**
 * Mock Schema Registry Client for testing
 */
class MockSchemaRegistryClient : public ISchemaRegistryClient {
  private:
    std::shared_ptr<MockSchemaStore> store;
    std::shared_ptr<const schemaregistry::rest::ClientConfiguration> config;
    std::shared_ptr<std::mutex> storeMutex;

  public:
    explicit MockSchemaRegistryClient(
        std::shared_ptr<const schemaregistry::rest::ClientConfiguration>
            config);

    virtual ~MockSchemaRegistryClient() = default;

    virtual std::shared_ptr<const schemaregistry::rest::ClientConfiguration>
    getConfiguration() const override;

    // Schema operations
    virtual schemaregistry::rest::model::RegisteredSchema registerSchema(
        const std::string &subject,
        const schemaregistry::rest::model::Schema &schema,
        bool normalize = false) override;

    virtual schemaregistry::rest::model::Schema getBySubjectAndId(
        const std::optional<std::string> &subject, int32_t id,
        const std::optional<std::string> &format = std::nullopt) override;

    virtual schemaregistry::rest::model::Schema getByGuid(
        const std::string &guid,
        const std::optional<std::string> &format = std::nullopt) override;

    virtual schemaregistry::rest::model::RegisteredSchema getBySchema(
        const std::string &subject,
        const schemaregistry::rest::model::Schema &schema,
        bool normalize = false, bool deleted = false) override;

    virtual schemaregistry::rest::model::RegisteredSchema getVersion(
        const std::string &subject, int32_t version, bool deleted = false,
        const std::optional<std::string> &format = std::nullopt) override;

    virtual schemaregistry::rest::model::RegisteredSchema getLatestVersion(
        const std::string &subject,
        const std::optional<std::string> &format = std::nullopt) override;

    virtual schemaregistry::rest::model::RegisteredSchema getLatestWithMetadata(
        const std::string &subject,
        const std::unordered_map<std::string, std::string> &metadata,
        bool deleted = false,
        const std::optional<std::string> &format = std::nullopt) override;

    virtual std::vector<int32_t> getAllVersions(
        const std::string &subject) override;

    virtual std::vector<std::string> getAllSubjects(
        bool deleted = false) override;

    virtual std::vector<int32_t> deleteSubject(const std::string &subject,
                                               bool permanent = false) override;

    virtual int32_t deleteSubjectVersion(const std::string &subject,
                                         int32_t version,
                                         bool permanent = false) override;

    // Compatibility operations
    virtual bool testSubjectCompatibility(
        const std::string &subject,
        const schemaregistry::rest::model::Schema &schema) override;

    virtual bool testCompatibility(
        const std::string &subject, int32_t version,
        const schemaregistry::rest::model::Schema &schema) override;

    // Config operations
    virtual schemaregistry::rest::model::ServerConfig getConfig(
        const std::string &subject) override;

    virtual schemaregistry::rest::model::ServerConfig updateConfig(
        const std::string &subject,
        const schemaregistry::rest::model::ServerConfig &config) override;

    virtual schemaregistry::rest::model::ServerConfig getDefaultConfig()
        override;

    virtual schemaregistry::rest::model::ServerConfig updateDefaultConfig(
        const schemaregistry::rest::model::ServerConfig &config) override;

    // Cache operations
    virtual void clearLatestCaches() override;

    virtual void clearCaches() override;

    virtual void close() override;
};

}  // namespace schemaregistry::rest