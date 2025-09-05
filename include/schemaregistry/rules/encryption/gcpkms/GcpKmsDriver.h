/**
 * GcpKmsDriver
 * Google Cloud KMS implementation of the KmsDriver interface
 *
 * This is the C++ port of the Rust gcp_driver.rs file
 */

#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "schemaregistry/rules/encryption/KmsDriver.h"
#include "tink/kms_client.h"
#include "tink/util/statusor.h"

namespace schemaregistry::rules::encryption::gcpkms {

/**
 * Google Cloud KMS driver implementation
 *
 * Provides GCP KMS functionality by wrapping the Tink GCP KMS client
 * and handling GCP-specific credential configuration.
 */
class GcpKmsDriver : public KmsDriver {
  public:
    // Constants for configuration keys
    static constexpr const char* PREFIX = "gcp-kms://";
    static constexpr const char* ACCOUNT_TYPE = "account.type";
    static constexpr const char* CLIENT_ID = "client.id";
    static constexpr const char* CLIENT_EMAIL = "client.email";
    static constexpr const char* PRIVATE_KEY_ID = "private.key.id";
    static constexpr const char* PRIVATE_KEY = "private.key";

    /**
     * Default constructor
     */
    GcpKmsDriver();

    /**
     * Get the key URL prefix that this driver handles
     * @return The prefix string "gcp-kms://"
     */
    const std::string& getKeyUrlPrefix() const override;

    /**
     * Create a new KMS client for the given configuration and key URL
     *
     * @param conf Configuration parameters including GCP credentials
     * @param keyUrl The GCP KMS key URL
     * @return A shared pointer to the KMS client
     * @throws TinkError if the client cannot be created
     */
    std::shared_ptr<crypto::tink::KmsClient> newKmsClient(
        const std::unordered_map<std::string, std::string>& conf,
        const std::string& keyUrl) override;

    /**
     * Create a new instance of this driver
     *
     * @return A new GcpKmsDriver instance
     */
    static std::unique_ptr<GcpKmsDriver> create();

    /**
     * Register this driver with the KMS driver registry
     */
    static void registerDriver();

  private:
    std::string prefix_;

    /**
     * Validate that the given URI is a valid GCP KMS key URI
     *
     * @param keyUri The key URI to validate
     * @return true if valid, false otherwise
     */
    bool isValidKeyUri(const std::string& keyUri) const;

    /**
     * Build credentials configuration from the provided configuration map
     *
     * This method processes GCP service account credentials from the
     * configuration. If specific credentials are not provided, it will use
     * default GCP credentials.
     *
     * @param conf Configuration parameters
     * @param keyUrl The GCP KMS key URL
     * @return Path to credentials file or empty string for default credentials
     * @throws TinkError if configuration is invalid
     */
    std::string buildCredentialsPath(
        const std::unordered_map<std::string, std::string>& conf,
        const std::string& keyUrl) const;

    /**
     * Create a temporary credentials file from the provided service account
     * configuration
     *
     * @param conf Configuration parameters containing service account details
     * @return Path to the temporary credentials file
     * @throws TinkError if credentials file creation fails
     */
    std::string createCredentialsFile(
        const std::unordered_map<std::string, std::string>& conf) const;

    /**
     * Check if all required service account parameters are present
     *
     * @param conf Configuration parameters
     * @return true if all required parameters are present
     */
    bool hasServiceAccountCredentials(
        const std::unordered_map<std::string, std::string>& conf) const;
};

/**
 * GCP service account credentials structure
 * Used for creating temporary credentials files
 */
struct GcpCredentials {
    std::string account_type;
    std::string private_key_id;
    std::string private_key;
    std::string client_email;
    std::string client_id;
};

}  // namespace schemaregistry::rules::encryption::gcpkms
