/**
 * Confluent Schema Registry Client
 * Schema store implementation for caching schema and registered schema
 * information
 */

#include "schemaregistry/rest/SchemaStore.h"

#include <functional>
#include <sstream>

namespace schemaregistry::rest {

SchemaStore::SchemaStore() {}

void SchemaStore::setSchema(const std::optional<std::string> &subject,
                            const std::optional<int32_t> &schemaId,
                            const std::optional<std::string> &schemaGuid,
                            const schemaregistry::rest::model::Schema &schema) {
    std::string subjectStr = subject.value_or("");

    if (schemaId.has_value()) {
        // Update schema id index
        schemaIdIndex[subjectStr][schemaId.value()] =
            std::make_pair(schemaGuid, schema);

        // Update schema index
        std::string schemaHash = createSchemaHash(schema);
        schemaIndex[subjectStr][schemaHash] = schemaId.value();
    }

    if (schemaGuid.has_value()) {
        // Update schema guid index
        schemaGuidIndex[schemaGuid.value()] = schema;
    }
}

void SchemaStore::setRegisteredSchema(
    const schemaregistry::rest::model::Schema &schema,
    const schemaregistry::rest::model::RegisteredSchema &rs) {
    std::string subjectStr = "";
    if (rs.getSubject().has_value()) {
        subjectStr = rs.getSubject().value();
    }

    std::string schemaHash = createSchemaHash(schema);

    // Update registered schema by ID index
    if (rs.getId().has_value()) {
        rsIdIndex[subjectStr][rs.getId().value()] = rs;
    }

    // Update registered schema by version index
    if (rs.getVersion().has_value()) {
        rsVersionIndex[subjectStr][rs.getVersion().value()] = rs;
    }

    // Update registered schema by schema index
    rsSchemaIndex[subjectStr][schemaHash] = rs;

    // Also update the schema store
    std::optional<std::string> guid;
    if (rs.getSchema().has_value()) {
        guid = rs.getSchema().value();
    }

    std::optional<std::string> subjectOpt;
    if (!subjectStr.empty()) {
        subjectOpt = subjectStr;
    }

    setSchema(subjectOpt, rs.getId(), guid, schema);
}

std::optional<
    std::pair<std::optional<std::string>, schemaregistry::rest::model::Schema>>
SchemaStore::getSchemaById(const std::string &subject, int32_t schemaId) const {
    auto subjectIt = schemaIdIndex.find(subject);
    if (subjectIt != schemaIdIndex.end()) {
        auto schemaIt = subjectIt->second.find(schemaId);
        if (schemaIt != subjectIt->second.end()) {
            return schemaIt->second;
        }
    }
    return std::nullopt;
}

std::optional<schemaregistry::rest::model::Schema> SchemaStore::getSchemaByGuid(
    const std::string &guid) const {
    auto it = schemaGuidIndex.find(guid);
    if (it != schemaGuidIndex.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<int32_t> SchemaStore::getIdBySchema(
    const std::string &subject,
    const schemaregistry::rest::model::Schema &schema) const {
    auto subjectIt = schemaIndex.find(subject);
    if (subjectIt != schemaIndex.end()) {
        std::string schemaHash = createSchemaHash(schema);
        auto schemaIt = subjectIt->second.find(schemaHash);
        if (schemaIt != subjectIt->second.end()) {
            return schemaIt->second;
        }
    }
    return std::nullopt;
}

std::optional<schemaregistry::rest::model::RegisteredSchema>
SchemaStore::getRegisteredBySchema(
    const std::string &subject,
    const schemaregistry::rest::model::Schema &schema) const {
    auto subjectIt = rsSchemaIndex.find(subject);
    if (subjectIt != rsSchemaIndex.end()) {
        std::string schemaHash = createSchemaHash(schema);
        auto schemaIt = subjectIt->second.find(schemaHash);
        if (schemaIt != subjectIt->second.end()) {
            return schemaIt->second;
        }
    }
    return std::nullopt;
}

std::optional<schemaregistry::rest::model::RegisteredSchema>
SchemaStore::getRegisteredByVersion(const std::string &subject,
                                    int32_t version) const {
    auto subjectIt = rsVersionIndex.find(subject);
    if (subjectIt != rsVersionIndex.end()) {
        auto versionIt = subjectIt->second.find(version);
        if (versionIt != subjectIt->second.end()) {
            return versionIt->second;
        }
    }
    return std::nullopt;
}

std::optional<schemaregistry::rest::model::RegisteredSchema>
SchemaStore::getRegisteredById(const std::string &subject,
                               int32_t schemaId) const {
    auto subjectIt = rsIdIndex.find(subject);
    if (subjectIt != rsIdIndex.end()) {
        auto idIt = subjectIt->second.find(schemaId);
        if (idIt != subjectIt->second.end()) {
            return idIt->second;
        }
    }
    return std::nullopt;
}

void SchemaStore::clear() {
    schemaIdIndex.clear();
    schemaGuidIndex.clear();
    schemaIndex.clear();
    rsIdIndex.clear();
    rsVersionIndex.clear();
    rsSchemaIndex.clear();
}

std::string SchemaStore::createSchemaHash(
    const schemaregistry::rest::model::Schema &schema) const {
    // Us the JSON representation of the schema to create a hash
    nlohmann::json j;
    to_json(j, schema);
    return j.dump();
}

}  // namespace schemaregistry::rest