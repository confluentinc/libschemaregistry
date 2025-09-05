/**
 * AzureKmsDriver
 * Azure Key Vault KMS implementation of the KmsDriver interface
 *
 * This is the C++ port of the Rust azure_driver.rs file
 */

#pragma once

#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>

#include "schemaregistry/rules/encryption/KmsDriver.h"
#include "tink/kms_client.h"
#include "tink/util/statusor.h"

namespace schemaregistry::rules::encryption::azurekms {

/**
 * Azure Key Vault KMS driver implementation
 *
 * Provides Azure Key Vault functionality by implementing the KmsDriver
 * interface and handling Azure-specific credential configuration.
 */
class AzureKmsDriver : public KmsDriver {
  public:
    // Constants for configuration keys
    static constexpr const char *PREFIX = "azure-kms://";
    static constexpr const char *TENANT_ID = "tenant.id";
    static constexpr const char *CLIENT_ID = "client.id";
    static constexpr const char *CLIENT_SECRET = "client.secret";

    /**
     * Default constructor
     */
    AzureKmsDriver();

    /**
     * Destructor
     */
    ~AzureKmsDriver() override = default;

    /**
     * Get the Azure Key Vault KMS key URL prefix
     *
     * @return The prefix "azure-kms://"
     */
    const std::string &getKeyUrlPrefix() const override;

    /**
     * Create a new Azure Key Vault KMS client instance
     *
     * @param conf Configuration parameters for the Azure KMS client
     * @param keyUrl The Azure Key Vault key URL
     * @return A shared pointer to the created KMS client
     * @throws TinkError if client creation fails
     */
    std::shared_ptr<crypto::tink::KmsClient> newKmsClient(
        const std::unordered_map<std::string, std::string> &conf,
        const std::string &keyUrl) override;

    /**
     * Create a new instance of this driver
     *
     * @return A new AzureKmsDriver instance
     */
    static std::unique_ptr<AzureKmsDriver> create();

    /**
     * Register this driver with the global KMS driver registry
     */
    static void registerDriver();

  private:
    std::string prefix_;

    /**
     * Extract key information from Azure Key Vault URL
     *
     * @param keyUrl The Azure Key Vault key URL
     * @return A tuple containing (vaultUrl, keyName, keyVersion)
     * @throws TinkError if the URL format is invalid
     */
    std::tuple<std::string, std::string, std::string> getKeyInfo(
        const std::string &keyUrl) const;

    /**
     * Validate that the key URI has the correct Azure KMS prefix
     *
     * @param keyUri The key URI to validate
     * @return true if the URI is valid, false otherwise
     */
    bool isValidKeyUri(const std::string &keyUri) const;
};

}  // namespace schemaregistry::rules::encryption::azurekms
