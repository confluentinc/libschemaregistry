/**
 * AzureKmsClient implementation
 * Azure Key Vault KMS client implementation
 */

#include "schemaregistry/rules/encryption/azurekms/AzureKmsClient.h"

#include <algorithm>
#include <cctype>
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
    std::shared_ptr<Azure::Core::Credentials::TokenCredential> credential,
    std::unordered_map<std::string, std::string> conf)
    : keyUriPrefix_(keyUriPrefix),
      credential_(std::move(credential)),
      conf_(std::move(conf)) {}

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

        // Built from the raw (possibly versionless) keyVersion, exactly as
        // before this feature existed: used directly whenever the toggle is
        // off, and as Decrypt's fallback for legacy ciphertext with no
        // embedded version.
        auto defaultCryptoClient = std::make_shared<
            Azure::Security::KeyVault::Keys::Cryptography::CryptographyClient>(
            keyClient->GetCryptographyClient(keyName, keyVersion));

        bool saveVersion = false;
        auto saveVersionIt =
            conf_.find(AzureKmsDriver::ENCRYPT_AZURE_KEY_VERSION_SAVE);
        if (saveVersionIt != conf_.end()) {
            std::string value = saveVersionIt->second;
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });
            saveVersion = value == "true";
        }

        // Return the Azure AEAD implementation
        return std::make_unique<AzureAead>(keyClient, keyName, keyVersion,
                                           defaultCryptoClient, saveVersion);

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
    // or without version:
    // https://vault-name.vault.azure.net/keys/key-name
    // or with trailing slash:
    // https://vault-name.vault.azure.net/keys/key-name/
    std::regex urlRegex(R"(^(https://[^/]+)/keys/([^/]+)(?:/([^/]*))?/?$)");
    std::smatch matches;

    if (!std::regex_match(keyUrl, matches, urlRegex)) {
        throw std::invalid_argument("Invalid Azure Key Vault URL format: " +
                                    keyUrl);
    }

    // If version is not provided (matches[3] is empty), return empty string
    // Azure Key Vault will use the latest version when no version is specified
    std::string keyVersion = matches[3].matched ? matches[3].str() : "";

    return std::make_tuple(matches[1].str(), matches[2].str(), keyVersion);
}

}  // namespace schemaregistry::rules::encryption::azurekms
