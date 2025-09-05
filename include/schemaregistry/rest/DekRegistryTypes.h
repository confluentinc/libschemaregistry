/**
 * DekRegistryTypes
 * Common types and hash functions for DEK Registry Client
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "schemaregistry/rest/model/Dek.h"

namespace schemaregistry::rest {

/**
 * Key ID for KEK caching
 */
struct KekId {
    std::string name;
    bool deleted;

    bool operator==(const KekId &other) const {
        return name == other.name && deleted == other.deleted;
    }
};

/**
 * Key ID for DEK caching
 */
struct DekId {
    std::string kek_name;
    std::string subject;
    int32_t version;
    schemaregistry::rest::model::Algorithm algorithm;
    bool deleted;

    bool operator==(const DekId &other) const {
        return kek_name == other.kek_name && subject == other.subject &&
               version == other.version && algorithm == other.algorithm &&
               deleted == other.deleted;
    }
};

}  // namespace schemaregistry::rest

// Hash specializations for std::unordered_map (implementations in
// DekRegistryTypes.cpp)
namespace std {
template <>
struct hash<schemaregistry::rest::KekId> {
    std::size_t operator()(const schemaregistry::rest::KekId &k) const;
};

template <>
struct hash<schemaregistry::rest::DekId> {
    std::size_t operator()(const schemaregistry::rest::DekId &k) const;
};
}  // namespace std