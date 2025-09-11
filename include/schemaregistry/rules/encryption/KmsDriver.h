/**
 * KmsDriver
 * Abstract interface for KMS drivers - C++ equivalent of Rust KmsDriver trait
 */

#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "tink/kms_client.h"
#include "tink/util/statusor.h"

namespace schemaregistry::rules::encryption {

/**
 * Exception type for KMS-related errors
 */
class TinkError : public std::exception {
  public:
    explicit TinkError(const std::string &message) : message_(message) {}

    const char *what() const noexcept override { return message_.c_str(); }

  private:
    std::string message_;
};

/**
 * Abstract interface for KMS drivers
 *
 * This is the C++ equivalent of the Rust KmsDriver trait.
 * Implementations provide KMS-specific functionality for different providers
 * (AWS KMS, GCP KMS, Azure Key Vault, etc.)
 */
class KmsDriver {
  public:
    virtual ~KmsDriver() = default;

    /**
     * Get the key URL prefix that this driver supports
     *
     * @return The URL prefix (e.g., "aws-kms://", "gcp-kms://")
     */
    virtual const std::string &getKeyUrlPrefix() const = 0;

    /**
     * Create a new KMS client instance
     *
     * @param conf Configuration parameters for the KMS client
     * @param keyUrl The specific key URL that the client will handle
     * @return A shared pointer to the created KMS client
     * @throws TinkError if client creation fails
     */
    virtual std::shared_ptr<crypto::tink::KmsClient> newKmsClient(
        const std::unordered_map<std::string, std::string> &conf,
        const std::string &keyUrl) = 0;
};

}  // namespace schemaregistry::rules::encryption