/**
 * IDekRegistryClient
 * Interface for Data Encryption Key (DEK) Registry Client
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "schemaregistry/rest/ClientConfiguration.h"
#include "schemaregistry/rest/model/CreateDekRequest.h"
#include "schemaregistry/rest/model/CreateKekRequest.h"
#include "schemaregistry/rest/model/Dek.h"
#include "schemaregistry/rest/model/Kek.h"

namespace schemaregistry::rest {

/**
 * Interface for DEK Registry Client
 */
class IDekRegistryClient {
  public:
    virtual ~IDekRegistryClient() = default;

    /**
     * Get client configuration
     */
    virtual std::shared_ptr<const schemaregistry::rest::ClientConfiguration>
    getConfiguration() const = 0;

    /**
     * Register a new Key Encryption Key (KEK)
     */
    virtual schemaregistry::rest::model::Kek registerKek(
        const schemaregistry::rest::model::CreateKekRequest &request) = 0;

    /**
     * Register a new Data Encryption Key (DEK)
     */
    virtual schemaregistry::rest::model::Dek registerDek(
        const std::string &kek_name,
        const schemaregistry::rest::model::CreateDekRequest &request) = 0;

    /**
     * Get a KEK by name
     */
    virtual schemaregistry::rest::model::Kek getKek(const std::string &name,
                                                    bool deleted = false) = 0;

    /**
     * Get a DEK by KEK name, subject, algorithm, and version
     */
    virtual schemaregistry::rest::model::Dek getDek(
        const std::string &kek_name, const std::string &subject,
        const std::optional<schemaregistry::rest::model::Algorithm> &algorithm =
            std::nullopt,
        const std::optional<int32_t> &version = std::nullopt,
        bool deleted = false) = 0;

    /**
     * Set the key material for a DEK
     */
    virtual schemaregistry::rest::model::Dek setDekKeyMaterial(
        const std::string &kek_name, const std::string &subject,
        const std::optional<schemaregistry::rest::model::Algorithm> &algorithm =
            std::nullopt,
        const std::optional<int32_t> &version = std::nullopt,
        bool deleted = false,
        const std::vector<uint8_t> &key_material_bytes = {}) = 0;

    /**
     * Clear all caches
     */
    virtual void clearCaches() = 0;

    /**
     * Close the client
     */
    virtual void close() = 0;
};

}  // namespace schemaregistry::rest