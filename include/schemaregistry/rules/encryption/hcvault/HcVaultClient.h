/**
 * HcVaultClient
 * HashiCorp Vault KMS client implementation
 *
 * Implements Tink's KmsClient interface for HashiCorp Vault operations
 */

#pragma once

#include <memory>
#include <string>
#include <tuple>

#include "absl/strings/string_view.h"
#include "libvault/VaultClient.h"
#include "tink/aead.h"
#include "tink/kms_client.h"
#include "tink/util/statusor.h"

namespace schemaregistry::rules::encryption::hcvault {

/**
 * HashiCorp Vault KMS client that implements Tink's KmsClient interface
 *
 * This class provides the interface between Tink and HashiCorp Vault,
 * handling key URI validation and AEAD creation.
 */
class HcVaultKmsClient : public crypto::tink::KmsClient {
  public:
    /**
     * Constructor
     *
     * @param keyUriPrefix The key URI prefix this client supports
     * @param vaultClient Shared pointer to HashiCorp Vault client
     */
    HcVaultKmsClient(const std::string &keyUriPrefix,
                     std::shared_ptr<Vault::Client> vaultClient);

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
     * @param key_uri The HashiCorp Vault key URI
     * @return AEAD instance or error status
     */
    crypto::tink::util::StatusOr<std::unique_ptr<crypto::tink::Aead>> GetAead(
        absl::string_view key_uri) const override;

  private:
    std::string keyUriPrefix_;
    std::shared_ptr<Vault::Client> vaultClient_;

    struct VaultUrl {
        std::string scheme;
        std::string host;
        int port;
        std::string path;
    };

    /**
     * Parses a HashiCorp Vault URL to extract components
     *
     * @param url The URL to parse
     * @return VaultUrl structure with parsed components
     * @throws std::invalid_argument if URL format is invalid
     */
    static VaultUrl parseVaultUrl(const std::string &url);

    /**
     * Parses a key path to extract mount point and key name
     *
     * @param keyPath The key path to parse
     * @return Tuple containing (mountPath, keyName)
     * @throws std::invalid_argument if path format is invalid
     */
    static std::tuple<std::string, std::string> parseKeyPath(
        const std::string &keyPath);
};

}  // namespace schemaregistry::rules::encryption::hcvault
