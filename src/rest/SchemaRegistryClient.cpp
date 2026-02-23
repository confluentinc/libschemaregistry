/**
 * Confluent Schema Registry Client
 * Synchronous C++ client implementation for interacting with Confluent Schema
 * Registry
 */

#include "schemaregistry/rest/SchemaRegistryClient.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "schemaregistry/rest/MockSchemaRegistryClient.h"
#include "schemaregistry/rest/model/Association.h"

using json = nlohmann::json;

namespace schemaregistry::rest {

SchemaRegistryClient::SchemaRegistryClient(
    std::shared_ptr<const schemaregistry::rest::ClientConfiguration> config)
    : restClient(std::make_shared<schemaregistry::rest::RestClient>(config)),
      store(std::make_shared<SchemaStore>()),
      storeMutex(std::make_shared<std::mutex>()),
      latestVersionCache(config->getCacheCapacity(),
                         std::chrono::seconds(config->getCacheLatestTtlSec())),
      latestWithMetadataCache(
          config->getCacheCapacity(),
          std::chrono::seconds(config->getCacheLatestTtlSec())) {
    if (config->getBaseUrls().empty()) {
        throw schemaregistry::rest::RestException("Base URL is required");
    }
}

SchemaRegistryClient::~SchemaRegistryClient() { close(); }

std::shared_ptr<ISchemaRegistryClient> SchemaRegistryClient::newClient(
    std::shared_ptr<const schemaregistry::rest::ClientConfiguration> config) {
    if (config->getBaseUrls().empty()) {
        throw schemaregistry::rest::RestException("Base URL is required");
    }

    const std::string url = config->getBaseUrls()[0];
    if (url.substr(0, 7) == "mock://") {
        return std::make_shared<MockSchemaRegistryClient>(config);
    }
    return std::make_shared<SchemaRegistryClient>(config);
}

std::shared_ptr<const schemaregistry::rest::ClientConfiguration>
SchemaRegistryClient::getConfiguration() const {
    return restClient->getConfiguration();
}

std::string SchemaRegistryClient::urlEncode(const std::string &str) const {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (char c : str) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << std::uppercase;
            escaped << '%' << std::setw(2) << int((unsigned char)c);
            escaped << std::nouppercase;
        }
    }

    return escaped.str();
}

std::string SchemaRegistryClient::createMetadataKey(
    const std::string &subject,
    const std::unordered_map<std::string, std::string> &metadata) const {
    std::ostringstream key;
    key << subject << "|";

    // Sort metadata for consistent key
    std::map<std::string, std::string> sortedMetadata(metadata.begin(),
                                                      metadata.end());
    for (const auto &pair : sortedMetadata) {
        key << pair.first << "=" << pair.second << "&";
    }

    return key.str();
}

std::string SchemaRegistryClient::sendHttpRequest(
    const std::string &path, const std::string &method,
    const std::vector<std::pair<std::string, std::string>> &query,
    const std::string &body) const {
    std::map<std::string, std::string> headers;
    headers.insert(std::make_pair("Content-Type", "application/json"));

    auto result =
        restClient->sendRequestUrls(path, method, query, headers, body);

    if (result.status_code >= 400) {
        std::string errorMsg = "HTTP Error " +
                               std::to_string(result.status_code) + ": " +
                               result.text;
        throw schemaregistry::rest::RestException(errorMsg, result.status_code);
    }

    return result.text;
}

schemaregistry::rest::model::RegisteredSchema
SchemaRegistryClient::parseRegisteredSchemaFromJson(
    const std::string &jsonStr) const {
    try {
        json j = json::parse(jsonStr);
        schemaregistry::rest::model::RegisteredSchema response;
        from_json(j, response);
        return response;
    } catch (const std::exception &e) {
        throw schemaregistry::rest::RestException(
            "Failed to parse registered schema from JSON: " +
            std::string(e.what()));
    }
}

schemaregistry::rest::model::ServerConfig
SchemaRegistryClient::parseConfigFromJson(const std::string &jsonStr) const {
    try {
        json j = json::parse(jsonStr);
        schemaregistry::rest::model::ServerConfig config;
        from_json(j, config);
        return config;
    } catch (const std::exception &e) {
        throw schemaregistry::rest::RestException(
            "Failed to parse config from JSON: " + std::string(e.what()));
    }
}

bool SchemaRegistryClient::parseBoolFromJson(const std::string &jsonStr) const {
    try {
        json j = json::parse(jsonStr);
        return j.get<bool>();
    } catch (const std::exception &e) {
        throw schemaregistry::rest::RestException(
            "Failed to parse boolean from JSON: " + std::string(e.what()));
    }
}

std::vector<int32_t> SchemaRegistryClient::parseIntArrayFromJson(
    const std::string &jsonStr) const {
    try {
        json j = json::parse(jsonStr);
        return j.get<std::vector<int32_t>>();
    } catch (const std::exception &e) {
        throw schemaregistry::rest::RestException(
            "Failed to parse int array from JSON: " + std::string(e.what()));
    }
}

std::vector<std::string> SchemaRegistryClient::parseStringArrayFromJson(
    const std::string &jsonStr) const {
    try {
        json j = json::parse(jsonStr);
        return j.get<std::vector<std::string>>();
    } catch (const std::exception &e) {
        throw schemaregistry::rest::RestException(
            "Failed to parse string array from JSON: " + std::string(e.what()));
    }
}

void SchemaRegistryClient::clearLatestCaches() {
    latestVersionCache.clear();
    latestWithMetadataCache.clear();
}

void SchemaRegistryClient::clearCaches() {
    clearLatestCaches();
    {
        std::lock_guard<std::mutex> lock(*storeMutex);
        store->clear();
    }
}

void SchemaRegistryClient::close() { clearCaches(); }

schemaregistry::rest::model::RegisteredSchema
SchemaRegistryClient::registerSchema(
    const std::string &subject,
    const schemaregistry::rest::model::Schema &schema, bool normalize) {
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(*storeMutex);
        auto registered = store->getRegisteredBySchema(subject, schema);
        if (registered.has_value()) {
            return registered.value();
        }
    }

    // Prepare request
    std::string path = "/subjects/" + urlEncode(subject) + "/versions";
    std::vector<std::pair<std::string, std::string>> query;
    query.emplace_back("normalize", normalize ? "true" : "false");

    // Serialize schema to JSON
    json j;
    to_json(j, schema);
    std::string body = j.dump();

    // Send request
    std::string responseBody = sendHttpRequest(path, "POST", query, body);

    // Parse response
    schemaregistry::rest::model::RegisteredSchema response =
        parseRegisteredSchemaFromJson(responseBody);

    // Update cache
    {
        std::lock_guard<std::mutex> lock(*storeMutex);
        schemaregistry::rest::model::Schema schemaKey;
        if (response.getSchema().has_value()) {
            schemaKey = response.toSchema();
        } else {
            schemaKey =
                schema;  // Use the input schema if no schema in response
        }
        store->setSchema(std::make_optional(subject), response.getId(),
                         response.getGuid(), schemaKey);
    }

    return response;
}

schemaregistry::rest::model::Schema SchemaRegistryClient::getBySubjectAndId(
    const std::optional<std::string> &subject, int32_t id,
    const std::optional<std::string> &format) {
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(*storeMutex);
        auto result = store->getSchemaById(subject.value_or(""), id);
        if (result.has_value()) {
            return result.value().second;
        }
    }

    // Prepare request
    std::string path = "/schemas/ids/" + std::to_string(id);
    std::vector<std::pair<std::string, std::string>> query;
    if (subject.has_value()) {
        query.emplace_back("subject", subject.value());
    }
    if (format.has_value()) {
        query.emplace_back("format", format.value());
    }

    // Send request
    std::string responseBody = sendHttpRequest(path, "GET", query);

    // Parse response
    schemaregistry::rest::model::RegisteredSchema response =
        parseRegisteredSchemaFromJson(responseBody);
    schemaregistry::rest::model::Schema schema = response.toSchema();

    // Update cache
    {
        std::lock_guard<std::mutex> lock(*storeMutex);
        store->setSchema(subject, std::make_optional(id), response.getGuid(),
                         schema);
    }

    return schema;
}

schemaregistry::rest::model::Schema SchemaRegistryClient::getByGuid(
    const std::string &guid, const std::optional<std::string> &format) {
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(*storeMutex);
        auto result = store->getSchemaByGuid(guid);
        if (result.has_value()) {
            return result.value();
        }
    }

    // Prepare request
    std::string path = "/schemas/guids/" + urlEncode(guid);
    std::vector<std::pair<std::string, std::string>> query;
    if (format.has_value()) {
        query.emplace_back("format", format.value());
    }

    // Send request
    std::string responseBody = sendHttpRequest(path, "GET", query);

    // Parse response
    schemaregistry::rest::model::RegisteredSchema response =
        parseRegisteredSchemaFromJson(responseBody);
    schemaregistry::rest::model::Schema schema = response.toSchema();

    // Update cache
    {
        std::lock_guard<std::mutex> lock(*storeMutex);
        store->setSchema(std::nullopt, response.getId(),
                         std::make_optional(guid), schema);
    }

    return schema;
}

schemaregistry::rest::model::RegisteredSchema SchemaRegistryClient::getBySchema(
    const std::string &subject,
    const schemaregistry::rest::model::Schema &schema, bool normalize,
    bool deleted) {
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(*storeMutex);
        auto result = store->getRegisteredBySchema(subject, schema);
        if (result.has_value()) {
            return result.value();
        }
    }

    // Prepare request
    std::string path = "/subjects/" + urlEncode(subject);
    std::vector<std::pair<std::string, std::string>> query;
    query.emplace_back("normalize", normalize ? "true" : "false");
    query.emplace_back("deleted", deleted ? "true" : "false");

    // Serialize schema to JSON
    json j;
    to_json(j, schema);
    std::string body = j.dump();

    // Send request
    std::string responseBody = sendHttpRequest(path, "POST", query, body);

    // Parse response
    schemaregistry::rest::model::RegisteredSchema response =
        parseRegisteredSchemaFromJson(responseBody);

    // Update cache
    {
        std::lock_guard<std::mutex> lock(*storeMutex);
        // Ensure the schema matches the input
        schemaregistry::rest::model::RegisteredSchema rs(
            response.getId(), response.getGuid(), response.getSubject(),
            response.getVersion(), schema);
        store->setRegisteredSchema(schema, rs);
    }

    return response;
}

schemaregistry::rest::model::RegisteredSchema SchemaRegistryClient::getVersion(
    const std::string &subject, int32_t version, bool deleted,
    const std::optional<std::string> &format) {
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(*storeMutex);
        auto result = store->getRegisteredByVersion(subject, version);
        if (result.has_value()) {
            return result.value();
        }
    }

    // Prepare request
    std::string path = "/subjects/" + urlEncode(subject) + "/versions/" +
                       std::to_string(version);
    std::vector<std::pair<std::string, std::string>> query;
    query.emplace_back("deleted", deleted ? "true" : "false");
    if (format.has_value()) {
        query.emplace_back("format", format.value());
    }

    // Send request
    std::string responseBody = sendHttpRequest(path, "GET", query);

    // Parse response
    schemaregistry::rest::model::RegisteredSchema response =
        parseRegisteredSchemaFromJson(responseBody);

    // Update cache
    {
        std::lock_guard<std::mutex> lock(*storeMutex);
        schemaregistry::rest::model::Schema schema = response.toSchema();
        store->setRegisteredSchema(schema, response);
    }

    return response;
}

schemaregistry::rest::model::RegisteredSchema
SchemaRegistryClient::getLatestVersion(
    const std::string &subject, const std::optional<std::string> &format) {
    // Check cache first
    auto cached = latestVersionCache.get(subject);
    if (cached.has_value()) {
        return cached.value();
    }

    // Prepare request
    std::string path = "/subjects/" + urlEncode(subject) + "/versions/latest";
    std::vector<std::pair<std::string, std::string>> query;
    if (format.has_value()) {
        query.emplace_back("format", format.value());
    }

    // Send request
    std::string responseBody = sendHttpRequest(path, "GET", query);

    // Parse response
    schemaregistry::rest::model::RegisteredSchema response =
        parseRegisteredSchemaFromJson(responseBody);

    // Update cache
    latestVersionCache.put(subject, response);

    return response;
}

schemaregistry::rest::model::RegisteredSchema
SchemaRegistryClient::getLatestWithMetadata(
    const std::string &subject,
    const std::unordered_map<std::string, std::string> &metadata, bool deleted,
    const std::optional<std::string> &format) {
    // Check cache first
    std::string cacheKey = createMetadataKey(subject, metadata);
    auto cached = latestWithMetadataCache.get(cacheKey);
    if (cached.has_value()) {
        return cached.value();
    }

    // Prepare request
    std::string path = "/subjects/" + urlEncode(subject) + "/metadata";
    std::vector<std::pair<std::string, std::string>> query;
    query.emplace_back("deleted", deleted ? "true" : "false");
    if (format.has_value()) {
        query.emplace_back("format", format.value());
    }

    // Add metadata to query
    for (const auto &pair : metadata) {
        query.emplace_back("key", pair.first);
        query.emplace_back("value", pair.second);
    }

    // Send request
    std::string responseBody = sendHttpRequest(path, "GET", query);

    // Parse response
    schemaregistry::rest::model::RegisteredSchema response =
        parseRegisteredSchemaFromJson(responseBody);

    // Update cache
    latestWithMetadataCache.put(cacheKey, response);

    return response;
}

std::vector<int32_t> SchemaRegistryClient::getAllVersions(
    const std::string &subject) {
    // Prepare request
    std::string path = "/subjects/" + urlEncode(subject) + "/versions";

    // Send request
    std::string responseBody = sendHttpRequest(path, "GET");

    // Parse response
    return parseIntArrayFromJson(responseBody);
}

std::vector<std::string> SchemaRegistryClient::getAllSubjects(bool deleted) {
    // Prepare request
    std::string path = "/subjects";
    std::vector<std::pair<std::string, std::string>> query;
    query.emplace_back("deleted", deleted ? "true" : "false");

    // Send request
    std::string responseBody = sendHttpRequest(path, "GET", query);

    // Parse response
    return parseStringArrayFromJson(responseBody);
}

std::vector<int32_t> SchemaRegistryClient::deleteSubject(
    const std::string &subject, bool permanent) {
    // Prepare request
    std::string path = "/subjects/" + urlEncode(subject);
    std::vector<std::pair<std::string, std::string>> query;
    query.emplace_back("permanent", permanent ? "true" : "false");

    // Send request
    std::string responseBody = sendHttpRequest(path, "DELETE", query);

    // Parse response
    return parseIntArrayFromJson(responseBody);
}

int32_t SchemaRegistryClient::deleteSubjectVersion(const std::string &subject,
                                                   int32_t version,
                                                   bool permanent) {
    // Prepare request
    std::string path = "/subjects/" + urlEncode(subject) + "/versions/" +
                       std::to_string(version);
    std::vector<std::pair<std::string, std::string>> query;
    query.emplace_back("permanent", permanent ? "true" : "false");

    // Send request
    std::string responseBody = sendHttpRequest(path, "DELETE", query);

    // Parse response
    try {
        json j = json::parse(responseBody);
        return j.get<int32_t>();
    } catch (const std::exception &e) {
        throw schemaregistry::rest::RestException(
            "Failed to parse version from JSON: " + std::string(e.what()));
    }
}

bool SchemaRegistryClient::testSubjectCompatibility(
    const std::string &subject,
    const schemaregistry::rest::model::Schema &schema) {
    // Prepare request
    std::string path =
        "/compatibility/subjects/" + urlEncode(subject) + "/versions/latest";

    // Serialize schema to JSON
    json j;
    to_json(j, schema);
    std::string body = j.dump();

    // Send request
    std::string responseBody = sendHttpRequest(path, "POST", {}, body);

    // Parse response
    try {
        json response = json::parse(responseBody);
        return response.get<bool>();
    } catch (const std::exception &e) {
        throw schemaregistry::rest::RestException(
            "Failed to parse compatibility response from JSON: " +
            std::string(e.what()));
    }
}

bool SchemaRegistryClient::testCompatibility(
    const std::string &subject, int32_t version,
    const schemaregistry::rest::model::Schema &schema) {
    // Prepare request
    std::string path = "/compatibility/subjects/" + urlEncode(subject) +
                       "/versions/" + std::to_string(version);

    // Serialize schema to JSON
    json j;
    to_json(j, schema);
    std::string body = j.dump();

    // Send request
    std::string responseBody = sendHttpRequest(path, "POST", {}, body);

    // Parse response
    try {
        json response = json::parse(responseBody);
        return response.get<bool>();
    } catch (const std::exception &e) {
        throw schemaregistry::rest::RestException(
            "Failed to parse compatibility response from JSON: " +
            std::string(e.what()));
    }
}

schemaregistry::rest::model::ServerConfig SchemaRegistryClient::getConfig(
    const std::string &subject) {
    // Prepare request
    std::string path = "/config/" + urlEncode(subject);

    // Send request
    std::string responseBody = sendHttpRequest(path, "GET");

    // Parse response
    return parseConfigFromJson(responseBody);
}

schemaregistry::rest::model::ServerConfig SchemaRegistryClient::updateConfig(
    const std::string &subject,
    const schemaregistry::rest::model::ServerConfig &config) {
    // Prepare request
    std::string path = "/config/" + urlEncode(subject);

    // Serialize config to JSON
    json j;
    to_json(j, config);
    std::string body = j.dump();

    // Send request
    std::string responseBody = sendHttpRequest(path, "PUT", {}, body);

    // Parse response
    return parseConfigFromJson(responseBody);
}

schemaregistry::rest::model::ServerConfig
SchemaRegistryClient::getDefaultConfig() {
    // Prepare request
    std::string path = "/config";

    // Send request
    std::string responseBody = sendHttpRequest(path, "GET");

    // Parse response
    return parseConfigFromJson(responseBody);
}

schemaregistry::rest::model::ServerConfig
SchemaRegistryClient::updateDefaultConfig(
    const schemaregistry::rest::model::ServerConfig &config) {
    // Prepare request
    std::string path = "/config";

    // Serialize config to JSON
    json j;
    to_json(j, config);
    std::string body = j.dump();

    // Send request
    std::string responseBody = sendHttpRequest(path, "PUT", {}, body);

    // Parse response
    return parseConfigFromJson(responseBody);
}

std::vector<schemaregistry::rest::model::Association>
SchemaRegistryClient::getAssociationsByResourceName(
    const std::string &resource_name, const std::string &resource_namespace,
    const std::string &resource_type,
    const std::vector<std::string> &association_types,
    const std::string &lifecycle, int32_t offset, int32_t limit) {
    // Prepare request
    std::string path = "/associations";
    std::vector<std::pair<std::string, std::string>> query;
    query.emplace_back("resourceName", resource_name);
    if (!resource_namespace.empty()) {
        query.emplace_back("resourceNamespace", resource_namespace);
    }
    if (!resource_type.empty()) {
        query.emplace_back("resourceType", resource_type);
    }
    for (const auto &assoc_type : association_types) {
        query.emplace_back("associationType", assoc_type);
    }
    if (!lifecycle.empty()) {
        query.emplace_back("lifecycle", lifecycle);
    }
    if (offset > 0) {
        query.emplace_back("offset", std::to_string(offset));
    }
    if (limit >= 0) {
        query.emplace_back("limit", std::to_string(limit));
    }

    // Send request
    std::string responseBody = sendHttpRequest(path, "GET", query);

    // Parse response
    json j = json::parse(responseBody);
    std::vector<schemaregistry::rest::model::Association> associations;
    for (const auto &item : j) {
        schemaregistry::rest::model::Association assoc;
        from_json(item, assoc);
        associations.push_back(assoc);
    }
    return associations;
}

schemaregistry::rest::model::AssociationResponse
SchemaRegistryClient::createAssociation(
    const schemaregistry::rest::model::AssociationCreateOrUpdateRequest
        &request) {
    // Prepare request
    std::string path = "/associations";

    // Serialize request to JSON
    json j;
    to_json(j, request);
    std::string body = j.dump();

    // Send request
    std::string responseBody = sendHttpRequest(path, "POST", {}, body);

    // Parse response
    json response_json = json::parse(responseBody);
    schemaregistry::rest::model::AssociationResponse response;
    from_json(response_json, response);
    return response;
}

void SchemaRegistryClient::deleteAssociations(
    const std::string &resource_id,
    const std::optional<std::string> &resource_type,
    const std::optional<std::vector<std::string>> &association_types,
    bool cascade_lifecycle) {
    // Prepare request
    std::string path = "/associations/resources/" + urlEncode(resource_id);
    std::vector<std::pair<std::string, std::string>> query;

    if (resource_type.has_value()) {
        query.emplace_back("resourceType", resource_type.value());
    }
    if (association_types.has_value()) {
        for (const auto &assoc_type : association_types.value()) {
            query.emplace_back("associationType", assoc_type);
        }
    }
    query.emplace_back("cascadeLifecycle",
                       cascade_lifecycle ? "true" : "false");

    // Send request (DELETE returns no content)
    sendHttpRequest(path, "DELETE", query);
}

}  // namespace schemaregistry::rest