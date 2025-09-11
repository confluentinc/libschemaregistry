/**
 * AzureKmsClient
 * Azure Key Vault KMS client implementation
 *
 * Implements Tink's KmsClient interface for Azure Key Vault operations
 */

#pragma once

#include <memory>
#include <string>
#include <tuple>

#include "absl/strings/string_view.h"
#include "azure/core/credentials/credentials.hpp"
#include "tink/aead.h"
#include "tink/kms_client.h"
#include "tink/util/statusor.h"

namespace schemaregistry::rules::encryption::azurekms {

/**
 * Azure Key Vault KMS client that implements Tink's KmsClient interface
 *
 * This class provides the interface between Tink and Azure Key Vault,
 * handling key URI validation and AEAD creation.
 */
class AzureKmsClient : public crypto::tink::KmsClient {
  public:
    /**
     * Constructor
     *
     * @param keyUriPrefix The key URI prefix this client supports
     * @param credential Azure credentials for authentication
     */
    AzureKmsClient(
        const std::string &keyUriPrefix,
        std::shared_ptr<Azure::Core::Credentials::TokenCredential> credential);

    /**
     * Checks if this client supports the given key URI
     *
     * @param key_uri The key URI to check
     * @return true if supported, false otherwise
     */
    bool DoesSupport(absl::string_view key_uri) const override;

    /**
     * Creates an AEAD instance for the given key URI
     *
     * @param key_uri The Azure Key Vault key URI
     * @return AEAD instance or error status
     */
    crypto::tink::util::StatusOr<std::unique_ptr<crypto::tink::Aead>> GetAead(
        absl::string_view key_uri) const override;

  private:
    std::string keyUriPrefix_;
    std::shared_ptr<Azure::Core::Credentials::TokenCredential> credential_;

    /**
     * Parses an Azure Key Vault URL to extract key information
     *
     * @param keyUrl The key URL to parse
     * @return Tuple containing (vaultUrl, keyName, keyVersion)
     * @throws std::invalid_argument if URL format is invalid
     */
    static std::tuple<std::string, std::string, std::string> parseKeyInfo(
        const std::string &keyUrl);
};

}  // namespace schemaregistry::rules::encryption::azurekms
