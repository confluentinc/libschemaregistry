/**
 * AzureKmsClient implementation
 * Azure Key Vault KMS client implementation
 */

#include "schemaregistry/rules/encryption/azurekms/AzureKmsClient.h"

#include <regex>
#include <stdexcept>

#include "absl/status/status.h"
#include "azure/keyvault/keys.hpp"
#include "azure/keyvault/keys/cryptography/cryptography_client.hpp"
#include "schemaregistry/rules/encryption/azurekms/AzureKmsAead.h"
#include "schemaregistry/rules/encryption/azurekms/AzureKmsDriver.h"

namespace schemaregistry::rules::encryption::azurekms {

AzureKmsClient::AzureKmsClient(
    const std::string &keyUriPrefix,
    std::shared_ptr<Azure::Core::Credentials::TokenCredential> credential)
    : keyUriPrefix_(keyUriPrefix), credential_(std::move(credential)) {}

bool AzureKmsClient::DoesSupport(absl::string_view key_uri) const {
    std::string uri(key_uri);
    return uri.find(keyUriPrefix_) == 0;
}

crypto::tink::util::StatusOr<std::unique_ptr<crypto::tink::Aead>>
AzureKmsClient::GetAead(absl::string_view key_uri) const {
    if (!DoesSupport(key_uri)) {
        return crypto::tink::util::Status(absl::StatusCode::kInvalidArgument,
                                          "Key URI must start with prefix " +
                                              keyUriPrefix_ + ", but got " +
                                              std::string(key_uri));
    }

    try {
        // Strip the azure-kms:// prefix
        std::string uri = std::string(key_uri);
        if (uri.find(AzureKmsDriver::PREFIX) == 0) {
            uri = uri.substr(strlen(AzureKmsDriver::PREFIX));
        }

        // Parse the key information
        auto [vaultUrl, keyName, keyVersion] = parseKeyInfo(uri);

        // Create Azure Key Vault client
        auto keyClient =
            std::make_shared<Azure::Security::KeyVault::Keys::KeyClient>(
                vaultUrl, credential_);

        // Get the cryptography client for this specific key
        auto cryptoClient = std::make_shared<
            Azure::Security::KeyVault::Keys::Cryptography::CryptographyClient>(
            keyClient->GetCryptographyClient(keyName, keyVersion));

        // Return the Azure AEAD implementation
        return std::make_unique<AzureAead>(cryptoClient);

    } catch (const std::exception &e) {
        return crypto::tink::util::Status(
            absl::StatusCode::kInternal,
            "Failed to create Azure AEAD: " + std::string(e.what()));
    }
}

std::tuple<std::string, std::string, std::string> AzureKmsClient::parseKeyInfo(
    const std::string &keyUrl) {
    // Parse URL like:
    // https://vault-name.vault.azure.net/keys/key-name/key-version
    std::regex urlRegex(R"(^(https://[^/]+)/keys/([^/]+)/([^/]+)$)");
    std::smatch matches;

    if (!std::regex_match(keyUrl, matches, urlRegex)) {
        throw std::invalid_argument("Invalid Azure Key Vault URL format: " +
                                    keyUrl);
    }

    return std::make_tuple(matches[1].str(), matches[2].str(),
                           matches[3].str());
}

}  // namespace schemaregistry::rules::encryption::azurekms
