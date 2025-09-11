/**
 * AwsKmsDriver
 * AWS KMS implementation of the KmsDriver interface
 *
 * This is the C++ port of the Rust aws_driver.rs file
 */

#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "aws/core/auth/AWSCredentialsProvider.h"
#include "schemaregistry/rules/encryption/KmsDriver.h"
#include "tink/kms_client.h"
#include "tink/util/statusor.h"

namespace schemaregistry::rules::encryption::awskms {

/**
 * AWS KMS driver implementation
 *
 * Provides AWS KMS functionality by wrapping the Tink AWS KMS client
 * and handling AWS-specific credential configuration.
 */
class AwsKmsDriver : public KmsDriver {
  public:
    // Constants for configuration keys
    static constexpr const char *PREFIX = "aws-kms://";
    static constexpr const char *ACCESS_KEY_ID = "access.key.id";
    static constexpr const char *SECRET_ACCESS_KEY = "secret.access.key";
    static constexpr const char *PROFILE = "profile";
    static constexpr const char *ROLE_ARN = "role.arn";
    static constexpr const char *ROLE_SESSION_NAME = "role.session.name";
    static constexpr const char *ROLE_EXTERNAL_ID = "role.external.id";
    static constexpr const char *CREDENTIALS_PATH = "credentials.path";
    static constexpr const char *REGION = "region";

    /**
     * Default constructor
     */
    AwsKmsDriver();

    /**
     * Destructor
     */
    ~AwsKmsDriver() override = default;

    /**
     * Get the AWS KMS key URL prefix
     *
     * @return The prefix "aws-kms://"
     */
    const std::string &getKeyUrlPrefix() const override;

    /**
     * Create a new AWS KMS client instance
     *
     * @param conf Configuration parameters for the AWS KMS client
     * @param keyUrl The AWS KMS key URL
     * @return A shared pointer to the created KMS client
     * @throws TinkError if client creation fails
     */
    std::shared_ptr<crypto::tink::KmsClient> newKmsClient(
        const std::unordered_map<std::string, std::string> &conf,
        const std::string &keyUrl) override;

    /**
     * Create a new instance of this driver
     *
     * @return A new AwsKmsDriver instance
     */
    static std::unique_ptr<AwsKmsDriver> create();

    /**
     * Register this driver with the global KMS driver registry
     */
    static void registerDriver();

  private:
    std::string prefix_;

    /**
     * Build AWS credentials from the provided configuration map
     *
     * This method processes AWS credentials from the configuration and environment.
     * It supports:
     * - Explicit access key ID and secret access key
     * - AWS profile-based credentials
     * - Default AWS credentials chain
     * - Role assumption through environment variables
     *
     * @param conf Configuration parameters
     * @param keyUrl The AWS KMS key URL
     * @return Shared pointer to AWS credentials provider
     * @throws TinkError if configuration is invalid or credentials cannot be loaded
     */
    std::shared_ptr<Aws::Auth::AWSCredentialsProvider> buildCredentials(
        const std::unordered_map<std::string, std::string> &conf,
        const std::string &keyUrl) const;

    /**
     * Configure AWS environment variables based on the configuration
     *
     * @param conf Configuration parameters
     * @param keyUrl The AWS KMS key URL
     */
    void configureAwsEnvironment(
        const std::unordered_map<std::string, std::string> &conf,
        const std::string &keyUrl) const;

    /**
     * Convert a key URI to an AWS key ARN
     *
     * @param keyUri The key URI (e.g., "aws-kms://arn:aws:kms:...")
     * @return The AWS key ARN
     * @throws TinkError if the URI format is invalid
     */
    std::string keyUriToKeyArn(const std::string &keyUri) const;

    /**
     * Extract the AWS region from a key ARN
     *
     * @param keyArn The AWS key ARN
     * @return The AWS region string
     * @throws TinkError if the ARN format is invalid
     */
    std::string getRegionFromKeyArn(const std::string &keyArn) const;

    /**
     * Validate that the key URI has the correct AWS KMS prefix
     *
     * @param keyUri The key URI to validate
     * @return true if the URI is valid, false otherwise
     */
    bool isValidKeyUri(const std::string &keyUri) const;
};

}  // namespace schemaregistry::rules::encryption::awskms
