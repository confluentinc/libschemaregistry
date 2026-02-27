/**
 * DekRegistryTypes
 * Common types and hash functions for DEK Registry Client
 */

#pragma once

#include <cstdint>
#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/hash/hash.h"

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

    template <typename H>
    friend H AbslHashValue(H state, const KekId &key) {
        return H::combine(std::move(state), key.name, key.deleted);
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

    template <typename H>
    friend H AbslHashValue(H state, const DekId &key) {
        return H::combine(std::move(state), key.kek_name, key.subject,
                          key.version, key.algorithm, key.deleted);
    }
};

}  // namespace schemaregistry::rest

