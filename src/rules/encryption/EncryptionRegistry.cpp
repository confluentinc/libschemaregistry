/**
 * EncryptionRegistry
 * Implementation of global registry for KMS drivers and clients
 */

#include "schemaregistry/rules/encryption/EncryptionRegistry.h"

#include <algorithm>
#include <stdexcept>

#include "tink/kms_clients.h"

namespace schemaregistry::rules::encryption {

EncryptionRegistry &EncryptionRegistry::getInstance() {
    // Meyer's singleton - thread-safe in C++11 and later
    static EncryptionRegistry instance;
    return instance;
}

void EncryptionRegistry::registerKmsDriver(std::shared_ptr<KmsDriver> driver) {
    if (!driver) {
        throw TinkError("Cannot register null KMS driver");
    }

    std::lock_guard<std::mutex> lock(driversMutex_);
    drivers_.push_back(driver);
}

void EncryptionRegistry::clearKmsDrivers() {
    std::lock_guard<std::mutex> lock(driversMutex_);
    drivers_.clear();
}

std::shared_ptr<KmsDriver> EncryptionRegistry::getKmsDriver(
    const std::string &keyUri) {
    if (keyUri.empty()) {
        throw TinkError("Key URI cannot be empty");
    }

    std::lock_guard<std::mutex> lock(driversMutex_);

    for (const auto &driver : drivers_) {
        if (keyUri.find(driver->getKeyUrlPrefix()) == 0) {
            return driver;
        }
    }

    throw TinkError("KMS driver supporting " + keyUri + " not found");
}

void EncryptionRegistry::registerKmsClient(
    std::shared_ptr<crypto::tink::KmsClient> client) {
    if (!client) {
        throw TinkError("Cannot register null KMS client");
    }

    // Register in our local registry
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        clients_.push_back(client);
    }

    // Also register in Tink's global registry for compatibility
    // Note: Tink's Add method takes a unique_ptr, but we need to keep our
    // shared_ptr We'll rely on our local registry for lookups to maintain
    // shared ownership
}

void EncryptionRegistry::clearKmsClients() {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    clients_.clear();

    // Note: We cannot clear Tink's global registry as it doesn't provide a
    // clear method This is a limitation when integrating with Tink's global
    // state
}

std::shared_ptr<crypto::tink::KmsClient> EncryptionRegistry::getKmsClient(
    const std::string &keyUri) {
    if (keyUri.empty()) {
        throw TinkError("Key URI cannot be empty");
    }

    // First check our local registry
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);

        for (const auto &client : clients_) {
            if (client->DoesSupport(keyUri)) {
                return client;
            }
        }
    }

    // Fallback to Tink's global registry
    auto tinkResult = crypto::tink::KmsClients::Get(keyUri);
    if (tinkResult.ok()) {
        // Tink returns a raw pointer, but we need to be careful about ownership
        // Since Tink maintains ownership, we can't safely convert to shared_ptr
        // without risking double-deletion. For now, we'll throw an error
        // suggesting to use our registry instead.
        throw TinkError(
            "KMS client found in Tink registry but not accessible "
            "via shared_ptr. "
            "Please register clients using "
            "EncryptionRegistry::registerKmsClient()");
    }

    throw TinkError("KMS client supporting " + keyUri + " not found");
}

// Convenience functions for easier access (moved from header)
void clearKmsDrivers() { EncryptionRegistry::getInstance().clearKmsDrivers(); }

std::shared_ptr<KmsDriver> getKmsDriver(const std::string &keyUri) {
    return EncryptionRegistry::getInstance().getKmsDriver(keyUri);
}

void registerKmsClient(std::shared_ptr<crypto::tink::KmsClient> client) {
    EncryptionRegistry::getInstance().registerKmsClient(client);
}

void clearKmsClients() { EncryptionRegistry::getInstance().clearKmsClients(); }

std::shared_ptr<crypto::tink::KmsClient> getKmsClient(
    const std::string &keyUri) {
    return EncryptionRegistry::getInstance().getKmsClient(keyUri);
}

}  // namespace schemaregistry::rules::encryption