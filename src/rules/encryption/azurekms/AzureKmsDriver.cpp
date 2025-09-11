/**
 * AzureKmsDriver implementation
 * C++ port of the Rust azure_driver.rs file
 */

#include "schemaregistry/rules/encryption/azurekms/AzureKmsDriver.h"

#include <regex>
#include <stdexcept>
#include <tuple>

#include "azure/identity.hpp"
#include "schemaregistry/rules/encryption/EncryptionRegistry.h"
#include "schemaregistry/rules/encryption/azurekms/AzureKmsClient.h"

namespace schemaregistry::rules::encryption::azurekms {

// AzureKmsDriver implementation

AzureKmsDriver::AzureKmsDriver() : prefix_(PREFIX) {}

const std::string &AzureKmsDriver::getKeyUrlPrefix() const { return prefix_; }

std::shared_ptr<crypto::tink::KmsClient> AzureKmsDriver::newKmsClient(
    const std::unordered_map<std::string, std::string> &conf,
    const std::string &keyUrl) {
    if (!isValidKeyUri(keyUrl)) {
        throw TinkError("Invalid Azure KMS key URI: " + keyUrl);
    }

    try {
        // Create credential based on configuration
        std::shared_ptr<Azure::Core::Credentials::TokenCredential> credential;

        auto tenantIt = conf.find(TENANT_ID);
        auto clientIt = conf.find(CLIENT_ID);
        auto secretIt = conf.find(CLIENT_SECRET);

        if (tenantIt != conf.end() && clientIt != conf.end() &&
            secretIt != conf.end() && !tenantIt->second.empty() &&
            !clientIt->second.empty() && !secretIt->second.empty()) {
            // Use client secret credential
            credential =
                std::make_shared<Azure::Identity::ClientSecretCredential>(
                    tenantIt->second, clientIt->second, secretIt->second);
        } else {
            // Use default Azure credential
            credential =
                std::make_shared<Azure::Identity::DefaultAzureCredential>();
        }

        // Create and return the Azure KMS client
        return std::make_shared<AzureKmsClient>(keyUrl, credential);

    } catch (const std::exception &e) {
        throw TinkError("Error creating Azure KMS client: " +
                        std::string(e.what()));
    }
}

std::unique_ptr<AzureKmsDriver> AzureKmsDriver::create() {
    return std::make_unique<AzureKmsDriver>();
}

void AzureKmsDriver::registerDriver() {
    auto driver = create();
    EncryptionRegistry::getInstance().registerKmsDriver(std::move(driver));
}

std::tuple<std::string, std::string, std::string> AzureKmsDriver::getKeyInfo(
    const std::string &keyUrl) const {
    // Remove azure-kms:// prefix if present
    std::string url = keyUrl;
    if (url.find(PREFIX) == 0) {
        url = url.substr(strlen(PREFIX));
    }

    // Parse URL like:
    // https://vault-name.vault.azure.net/keys/key-name/key-version
    std::regex urlRegex(R"(^(https://[^/]+)/keys/([^/]+)/([^/]+)$)");
    std::smatch matches;

    if (!std::regex_match(url, matches, urlRegex)) {
        throw TinkError("Invalid Azure Key Vault URL format: " + keyUrl);
    }

    return std::make_tuple(matches[1].str(), matches[2].str(),
                           matches[3].str());
}

bool AzureKmsDriver::isValidKeyUri(const std::string &keyUri) const {
    if (keyUri.find(PREFIX) != 0) {
        return false;
    }

    try {
        getKeyInfo(keyUri);
        return true;
    } catch (const TinkError &) {
        return false;
    }
}

}  // namespace schemaregistry::rules::encryption::azurekms