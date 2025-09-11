/**
 * Mock Schema Registry Client
 * Synchronous C++ mock implementation for testing
 */

#include "schemaregistry/rest/MockSchemaRegistryClient.h"

#include <algorithm>
#include <iomanip>
#include <random>
#include <sstream>

namespace schemaregistry::rest {

// MockSchemaStore implementation

std::string MockSchemaStore::generateGuid() const {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);

    std::stringstream ss;
    ss << std::hex;
    for (int i = 0; i < 8; i++) ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 4; i++) ss << dis(gen);
    ss << "-4";
    for (int i = 0; i < 3; i++) ss << dis(gen);
    ss << "-";
    ss << dis(gen);
    for (int i = 0; i < 3; i++) ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 12; i++) ss << dis(gen);

    return ss.str();
}

void MockSchemaStore::setRegisteredSchema(
    const schemaregistry::rest::model::RegisteredSchema &schema) {
    auto subject = schema.getSubject().value_or("");

    // Update indexes
    if (schema.getId().has_value()) {
        schemaIdIndex[schema.getId().value()] = schema;
    }
    if (schema.getGuid().has_value()) {
        schemaGuidIndex[schema.getGuid().value()] = schema;
    }
    if (schema.getSchema().has_value()) {
        schemaIndex[schema.getSchema().value()] = schema;
    }

    // Update subject schemas
    auto it = schemas.find(subject);
    if (it != schemas.end()) {
        it->second.push_back(schema);
    } else {
        schemas[subject] = {schema};
    }
}

std::optional<schemaregistry::rest::model::Schema>
MockSchemaStore::getSchemaById(int32_t schemaId) const {
    auto it = schemaIdIndex.find(schemaId);
    if (it != schemaIdIndex.end()) {
        return it->second.toSchema();
    }
    return std::nullopt;
}

std::optional<schemaregistry::rest::model::Schema>
MockSchemaStore::getSchemaByGuid(const std::string &guid) const {
    auto it = schemaGuidIndex.find(guid);
    if (it != schemaGuidIndex.end()) {
        return it->second.toSchema();
    }
    return std::nullopt;
}

std::optional<schemaregistry::rest::model::RegisteredSchema>
MockSchemaStore::getRegisteredBySchema(
    const std::string &subject,
    const schemaregistry::rest::model::Schema &schema) const {
    auto it = schemas.find(subject);
    if (it != schemas.end()) {
        auto schemaStr = schema.getSchema().value_or("");
        for (const auto &rs : it->second) {
            if (rs.getSchema().value_or("") == schemaStr) {
                return rs;
            }
        }
    }
    return std::nullopt;
}

std::optional<schemaregistry::rest::model::RegisteredSchema>
MockSchemaStore::getRegisteredByVersion(const std::string &subject,
                                        int32_t version) const {
    auto it = schemas.find(subject);
    if (it != schemas.end()) {
        for (const auto &rs : it->second) {
            if (rs.getVersion().value_or(0) == version) {
                return rs;
            }
        }
    }
    return std::nullopt;
}

std::optional<schemaregistry::rest::model::RegisteredSchema>
MockSchemaStore::getLatestVersion(const std::string &subject) const {
    auto it = schemas.find(subject);
    if (it != schemas.end() && !it->second.empty()) {
        auto maxIt = std::max_element(
            it->second.begin(), it->second.end(),
            [](const schemaregistry::rest::model::RegisteredSchema &a,
               const schemaregistry::rest::model::RegisteredSchema &b) {
                return a.getVersion().value_or(0) < b.getVersion().value_or(0);
            });
        return *maxIt;
    }
    return std::nullopt;
}

std::optional<schemaregistry::rest::model::RegisteredSchema>
MockSchemaStore::getLatestWithMetadata(
    const std::string &subject,
    const std::unordered_map<std::string, std::string> &metadata) const {
    auto it = schemas.find(subject);
    if (it != schemas.end()) {
        for (const auto &rs : it->second) {
            if (hasMetadata(metadata, rs)) {
                return rs;
            }
        }
    }
    return std::nullopt;
}

std::vector<std::string> MockSchemaStore::getSubjects() const {
    std::vector<std::string> subjects;
    subjects.reserve(schemas.size());
    for (const auto &pair : schemas) {
        subjects.push_back(pair.first);
    }
    return subjects;
}

std::vector<int32_t> MockSchemaStore::getVersions(
    const std::string &subject) const {
    std::vector<int32_t> versions;
    auto it = schemas.find(subject);
    if (it != schemas.end()) {
        for (const auto &rs : it->second) {
            versions.push_back(rs.getVersion().value_or(0));
        }
    }
    return versions;
}

void MockSchemaStore::removeBySchema(
    const std::string &subject,
    const schemaregistry::rest::model::RegisteredSchema &registeredSchema) {
    auto it = schemas.find(subject);
    if (it != schemas.end()) {
        it->second.erase(
            std::remove_if(
                it->second.begin(), it->second.end(),
                [&registeredSchema](
                    const schemaregistry::rest::model::RegisteredSchema &rs) {
                    return rs.getSchema() == registeredSchema.getSchema();
                }),
            it->second.end());
    }
}

std::vector<int32_t> MockSchemaStore::removeBySubject(
    const std::string &subject) {
    std::vector<int32_t> versions;
    auto it = schemas.find(subject);
    if (it != schemas.end()) {
        for (const auto &rs : it->second) {
            versions.push_back(rs.getVersion().value_or(0));
            if (rs.getId().has_value()) {
                schemaIdIndex.erase(rs.getId().value());
            }
            if (rs.getSchema().has_value()) {
                schemaIndex.erase(rs.getSchema().value());
            }
        }
        schemas.erase(it);
    }
    return versions;
}

void MockSchemaStore::clear() {
    schemas.clear();
    schemaIdIndex.clear();
    schemaGuidIndex.clear();
    schemaIndex.clear();
}

bool MockSchemaStore::hasMetadata(
    const std::unordered_map<std::string, std::string> &metadata,
    const schemaregistry::rest::model::RegisteredSchema &rs) const {
    // Get the properties map from the registered schema's metadata
    std::map<std::string, std::string> props;
    auto rs_metadata = rs.getMetadata();
    if (rs_metadata.has_value()) {
        auto rs_props = rs_metadata.value().getProperties();
        if (rs_props.has_value()) {
            props = rs_props.value();
        }
    }

    // Check if all metadata key-value pairs exist and match in props
    return std::all_of(metadata.begin(), metadata.end(),
                       [&props](const auto &kv) {
                           auto it = props.find(kv.first);
                           return it != props.end() && it->second == kv.second;
                       });
}

// MockSchemaRegistryClient implementation

MockSchemaRegistryClient::MockSchemaRegistryClient(
    std::shared_ptr<const schemaregistry::rest::ClientConfiguration> config)
    : store(std::make_shared<MockSchemaStore>()),
      config(config),
      storeMutex(std::make_shared<std::mutex>()) {
    if (config->getBaseUrls().empty()) {
        throw schemaregistry::rest::RestException("Base URL is required");
    }
}

std::shared_ptr<const schemaregistry::rest::ClientConfiguration>
MockSchemaRegistryClient::getConfiguration() const {
    return config;
}

schemaregistry::rest::model::RegisteredSchema
MockSchemaRegistryClient::registerSchema(
    const std::string &subject,
    const schemaregistry::rest::model::Schema &schema, bool normalize) {
    std::lock_guard<std::mutex> lock(*storeMutex);

    // Check if schema already exists
    auto existing = store->getRegisteredBySchema(subject, schema);
    if (existing.has_value()) {
        return existing.value();
    }

    // Get latest version for the subject
    auto latest = store->getLatestVersion(subject);
    int32_t version =
        latest.has_value() ? latest.value().getVersion().value_or(0) + 1 : 1;

    // Create new registered schema
    schemaregistry::rest::model::RegisteredSchema registeredSchema(
        std::make_optional(store->nextSchemaId++),
        std::make_optional(store->generateGuid()), std::make_optional(subject),
        std::make_optional(version), schema);

    store->setRegisteredSchema(registeredSchema);

    return registeredSchema;
}

schemaregistry::rest::model::Schema MockSchemaRegistryClient::getBySubjectAndId(
    const std::optional<std::string> &subject, int32_t id,
    const std::optional<std::string> &format) {
    std::lock_guard<std::mutex> lock(*storeMutex);

    auto schema = store->getSchemaById(id);
    if (!schema.has_value()) {
        throw schemaregistry::rest::RestException("Schema not found");
    }

    return schema.value();
}

schemaregistry::rest::model::Schema MockSchemaRegistryClient::getByGuid(
    const std::string &guid, const std::optional<std::string> &format) {
    std::lock_guard<std::mutex> lock(*storeMutex);

    auto schema = store->getSchemaByGuid(guid);
    if (!schema.has_value()) {
        throw schemaregistry::rest::RestException("Schema not found");
    }

    return schema.value();
}

schemaregistry::rest::model::RegisteredSchema
MockSchemaRegistryClient::getBySchema(
    const std::string &subject,
    const schemaregistry::rest::model::Schema &schema, bool normalize,
    bool deleted) {
    std::lock_guard<std::mutex> lock(*storeMutex);

    auto rs = store->getRegisteredBySchema(subject, schema);
    if (!rs.has_value()) {
        throw schemaregistry::rest::RestException("Schema not found");
    }

    return rs.value();
}

schemaregistry::rest::model::RegisteredSchema
MockSchemaRegistryClient::getVersion(const std::string &subject,
                                     int32_t version, bool deleted,
                                     const std::optional<std::string> &format) {
    std::lock_guard<std::mutex> lock(*storeMutex);

    auto rs = store->getRegisteredByVersion(subject, version);
    if (!rs.has_value()) {
        throw schemaregistry::rest::RestException("Schema not found");
    }

    return rs.value();
}

schemaregistry::rest::model::RegisteredSchema
MockSchemaRegistryClient::getLatestVersion(
    const std::string &subject, const std::optional<std::string> &format) {
    std::lock_guard<std::mutex> lock(*storeMutex);

    auto rs = store->getLatestVersion(subject);
    if (!rs.has_value()) {
        throw schemaregistry::rest::RestException("Schema not found");
    }

    return rs.value();
}

schemaregistry::rest::model::RegisteredSchema
MockSchemaRegistryClient::getLatestWithMetadata(
    const std::string &subject,
    const std::unordered_map<std::string, std::string> &metadata, bool deleted,
    const std::optional<std::string> &format) {
    std::lock_guard<std::mutex> lock(*storeMutex);

    auto rs = store->getLatestWithMetadata(subject, metadata);
    if (!rs.has_value()) {
        throw schemaregistry::rest::RestException("Schema not found");
    }

    return rs.value();
}

std::vector<int32_t> MockSchemaRegistryClient::getAllVersions(
    const std::string &subject) {
    std::lock_guard<std::mutex> lock(*storeMutex);
    return store->getVersions(subject);
}

std::vector<std::string> MockSchemaRegistryClient::getAllSubjects(
    bool deleted) {
    std::lock_guard<std::mutex> lock(*storeMutex);
    return store->getSubjects();
}

std::vector<int32_t> MockSchemaRegistryClient::deleteSubject(
    const std::string &subject, bool permanent) {
    std::lock_guard<std::mutex> lock(*storeMutex);
    return store->removeBySubject(subject);
}

int32_t MockSchemaRegistryClient::deleteSubjectVersion(
    const std::string &subject, int32_t version, bool permanent) {
    std::lock_guard<std::mutex> lock(*storeMutex);

    auto rs = store->getRegisteredByVersion(subject, version);
    if (!rs.has_value()) {
        throw schemaregistry::rest::RestException("Schema not found");
    }

    store->removeBySchema(subject, rs.value());
    return rs.value().getVersion().value_or(0);
}

bool MockSchemaRegistryClient::testSubjectCompatibility(
    const std::string &subject,
    const schemaregistry::rest::model::Schema &schema) {
    // For mock implementation, always return true
    return true;
}

bool MockSchemaRegistryClient::testCompatibility(
    const std::string &subject, int32_t version,
    const schemaregistry::rest::model::Schema &schema) {
    // For mock implementation, always return true
    return true;
}

schemaregistry::rest::model::ServerConfig MockSchemaRegistryClient::getConfig(
    const std::string &subject) {
    return schemaregistry::rest::model::ServerConfig();
}

schemaregistry::rest::model::ServerConfig
MockSchemaRegistryClient::updateConfig(
    const std::string &subject,
    const schemaregistry::rest::model::ServerConfig &config) {
    return schemaregistry::rest::model::ServerConfig();
}

schemaregistry::rest::model::ServerConfig
MockSchemaRegistryClient::getDefaultConfig() {
    return schemaregistry::rest::model::ServerConfig();
}

schemaregistry::rest::model::ServerConfig
MockSchemaRegistryClient::updateDefaultConfig(
    const schemaregistry::rest::model::ServerConfig &config) {
    return schemaregistry::rest::model::ServerConfig();
}

void MockSchemaRegistryClient::clearLatestCaches() {
    // Mock implementation - no caches to clear
}

void MockSchemaRegistryClient::clearCaches() {
    std::lock_guard<std::mutex> lock(*storeMutex);
    store->clear();
}

void MockSchemaRegistryClient::close() { clearCaches(); }

}  // namespace schemaregistry::rest