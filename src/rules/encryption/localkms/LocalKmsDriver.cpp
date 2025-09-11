/**
 * LocalKmsDriver
 * Implementation of local KMS driver
 */

#include "schemaregistry/rules/encryption/localkms/LocalKmsDriver.h"

#include <cstdlib>
#include <string>

#include "schemaregistry/rules/encryption/EncryptionRegistry.h"
#include "schemaregistry/rules/encryption/KmsDriver.h"

namespace schemaregistry::rules::encryption::localkms {

const std::string &LocalKmsDriver::getKeyUrlPrefix() const {
    return keyUrlPrefix_;
}

std::shared_ptr<crypto::tink::KmsClient> LocalKmsDriver::newKmsClient(
    const std::unordered_map<std::string, std::string> &conf,
    const std::string &keyUrl) {
    // Validate that this driver supports the key URL
    if (keyUrl.find(keyUrlPrefix_) != 0) {
        throw TinkError("Key URL '" + keyUrl +
                        "' not supported by LocalKmsDriver. " +
                        "Expected prefix: " + keyUrlPrefix_);
    }

    // Resolve the secret from configuration or environment
    std::string secret = resolveSecret(conf);

    if (secret.empty()) {
        throw TinkError(std::string("Cannot load secret for LocalKmsClient. ") +
                        "Provide secret via configuration key '" +
                        std::string(getSecretConfigKey()) +
                        "' or environment variable '" +
                        std::string(getSecretEnvVar()) + "'");
    }

    // Create and return the LocalKmsClient
    try {
        return std::make_shared<LocalKmsClient>(secret);
    } catch (const std::exception &e) {
        throw TinkError("Failed to create LocalKmsClient: " +
                        std::string(e.what()));
    }
}

std::unique_ptr<LocalKmsDriver> LocalKmsDriver::create() {
    return std::make_unique<LocalKmsDriver>();
}

void LocalKmsDriver::registerDriver() {
    auto driver = create();
    EncryptionRegistry::getInstance().registerKmsDriver(std::move(driver));
}

std::string LocalKmsDriver::resolveSecret(
    const std::unordered_map<std::string, std::string> &conf) const {
    // First, try to get secret from configuration
    auto configKey = std::string(getSecretConfigKey());
    auto it = conf.find(configKey);
    if (it != conf.end() && !it->second.empty()) {
        return it->second;
    }

    // If not found in config, try environment variable
    auto envVar = std::string(getSecretEnvVar());
    std::string envSecret = getEnvVar(envVar);
    if (!envSecret.empty()) {
        return envSecret;
    }

    // Return empty string if secret not found anywhere
    return "";
}

std::string LocalKmsDriver::getEnvVar(const std::string &name) const {
    const char *value = std::getenv(name.c_str());
    return value ? std::string(value) : std::string();
}

}  // namespace schemaregistry::rules::encryption::localkms