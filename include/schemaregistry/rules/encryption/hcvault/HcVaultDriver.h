/**
 * HcVaultDriver
 * HashiCorp Vault KMS implementation of the KmsDriver interface
 *
 * This is the C++ port of the Rust hcvault_driver.rs file
 */

#pragma once

#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>

#include "schemaregistry/rules/encryption/KmsDriver.h"
#include "tink/kms_client.h"
#include "tink/util/statusor.h"

namespace schemaregistry::rules::encryption::hcvault {

/**
 * HashiCorp Vault KMS driver implementation
 *
 * Provides HashiCorp Vault functionality by implementing the KmsDriver
 * interface and handling Vault-specific authentication and configuration.
 */
class HcVaultDriver : public KmsDriver {
  public:
    // Constants for configuration keys
    static constexpr const char *PREFIX = "hcvault://";
    static constexpr const char *TOKEN_ID = "token.id";
    static constexpr const char *NAMESPACE = "namespace";

    /**
     * Default constructor
     */
    HcVaultDriver();

    /**
     * Destructor
     */
    ~HcVaultDriver() override = default;

    /**
     * Get the HashiCorp Vault KMS key URL prefix
     *
     * @return The prefix "hcvault://"
     */
    const std::string &getKeyUrlPrefix() const override;

    /**
     * Create a new HashiCorp Vault KMS client instance
     *
     * @param conf Configuration parameters for the Vault KMS client
     * @param keyUrl The HashiCorp Vault key URL
     * @return A shared pointer to the created KMS client
     * @throws TinkError if client creation fails
     */
    std::shared_ptr<crypto::tink::KmsClient> newKmsClient(
        const std::unordered_map<std::string, std::string> &conf,
        const std::string &keyUrl) override;

    /**
     * Create a new instance of this driver
     *
     * @return A new HcVaultDriver instance
     */
    static std::unique_ptr<HcVaultDriver> create();

    /**
     * Register this driver with the global KMS driver registry
     */
    static void registerDriver();

  private:
    std::string prefix_;

    struct VaultUrl {
        std::string scheme;
        std::string host;
        int port;
        std::string path;
    };

    /**
     * Extract endpoint paths from the key path
     * The keyPath is expected to have the form "/{mount-path}/keys/{keyName}"
     *
     * @param keyPath The key path from the URL
     * @return A tuple containing (mountPath, keyName)
     * @throws TinkError if the path format is invalid
     */
    std::tuple<std::string, std::string> getEndpointPaths(
        const std::string &keyPath) const;

    /**
     * Validate that the key URI has the correct HashiCorp Vault prefix
     *
     * @param keyUri The key URI to validate
     * @return true if the URI is valid, false otherwise
     */
    bool isValidKeyUri(const std::string &keyUri) const;

    /**
     * Parse a Vault URL into its components
     *
     * @param url The URL to parse
     * @return VaultUrl structure with parsed components
     * @throws TinkError if the URL format is invalid
     */
    VaultUrl parseVaultUrl(const std::string &url) const;
};

}  // namespace schemaregistry::rules::encryption::hcvault
