/**
 * LocalKmsDriver
 * Local KMS driver implementation - C++ port of Rust local_driver.rs
 */

#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "schemaregistry/rules/encryption/KmsDriver.h"
#include "schemaregistry/rules/encryption/localkms/LocalKmsClient.h"
#include "tink/kms_client.h"

namespace schemaregistry::rules::encryption::localkms {

/**
 * Local KMS driver implementation
 *
 * This driver creates LocalKmsClient instances for local KMS operations.
 * It handles configuration and environment variable resolution for secrets.
 *
 * This is the C++ equivalent of the Rust LocalKmsDriver implementation.
 */
class LocalKmsDriver : public KmsDriver {
  public:
    /**
     * Default constructor
     */
    LocalKmsDriver() = default;

    /**
     * Destructor
     */
    ~LocalKmsDriver() override = default;

    // KmsDriver interface implementation

    /**
     * Get the key URL prefix that this driver supports
     *
     * @return The URL prefix "local-kms://"
     */
    const std::string &getKeyUrlPrefix() const override;

    /**
     * Create a new KMS client instance
     *
     * @param conf Configuration parameters for the KMS client
     * @param keyUrl The specific key URL that the client will handle
     * @return A shared pointer to the created LocalKmsClient
     * @throws TinkError if client creation fails (e.g., missing secret)
     */
    std::shared_ptr<crypto::tink::KmsClient> newKmsClient(
        const std::unordered_map<std::string, std::string> &conf,
        const std::string &keyUrl) override;

    /**
     * Create a new instance of this driver
     *
     * @return A new LocalKmsDriver instance
     */
    static std::unique_ptr<LocalKmsDriver> create();

    /**
     * Register this driver with the global encryption registry
     *
     * This is a convenience method that creates a new driver instance
     * and registers it with the EncryptionRegistry.
     */
    static void registerDriver();

    /**
     * Get the configuration key name for the secret
     *
     * @return The configuration key "secret"
     */
    static constexpr absl::string_view getSecretConfigKey() { return "secret"; }

    /**
     * Get the environment variable name for the secret
     *
     * @return The environment variable name "LOCAL_SECRET"
     */
    static constexpr absl::string_view getSecretEnvVar() {
        return "LOCAL_SECRET";
    }

  private:
    mutable std::string keyUrlPrefix_{"local-kms://"};

    /**
     * Resolve the secret from configuration or environment
     *
     * @param conf Configuration map
     * @return The secret string, or empty if not found
     */
    std::string resolveSecret(
        const std::unordered_map<std::string, std::string> &conf) const;

    /**
     * Get environment variable value
     *
     * @param name Environment variable name
     * @return The value if found, empty string otherwise
     */
    std::string getEnvVar(const std::string &name) const;
};

}  // namespace schemaregistry::rules::encryption::localkms