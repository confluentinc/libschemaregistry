/**
 * LocalKmsClient
 * Local KMS client implementation using local secrets - C++ port of Rust
 * local_client.rs
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "tink/aead.h"
#include "tink/kms_client.h"
#include "tink/util/statusor.h"

namespace schemaregistry::rules::encryption::localkms {

/**
 * Local KMS client that provides KMS functionality using local secrets
 *
 * This class implements a KMS client that doesn't rely on external KMS
 * services. Instead, it derives encryption keys from a local secret using HKDF
 * and creates AES-GCM AEAD primitives for encryption/decryption operations.
 *
 * This is the C++ equivalent of the Rust LocalKmsClient implementation.
 */
class LocalKmsClient : public crypto::tink::KmsClient {
  public:
    /**
     * Constructor
     *
     * @param secret The local secret to use for key derivation
     */
    explicit LocalKmsClient(const std::string &secret);

    /**
     * Destructor
     */
    ~LocalKmsClient() override = default;

    // KmsClient interface implementation

    /**
     * Check if this client supports the given key URI
     *
     * @param key_uri The key URI to check
     * @return true if the key URI is supported (starts with "local-kms://")
     */
    bool DoesSupport(absl::string_view key_uri) const override;

    /**
     * Get an AEAD primitive for the given key URI
     *
     * @param key_uri The key URI (must start with "local-kms://")
     * @return StatusOr containing the AEAD primitive or an error
     */
    absl::StatusOr<std::unique_ptr<crypto::tink::Aead>> GetAead(
        absl::string_view key_uri) const override;

    /**
     * Static method to create an AEAD primitive from a secret
     *
     * @param secret The secret to derive the key from
     * @return StatusOr containing the AEAD primitive or an error
     */
    static absl::StatusOr<std::unique_ptr<crypto::tink::Aead>> GetPrimitive(
        const std::string &secret);

    /**
     * Static method to derive a key from a secret using HKDF
     *
     * @param secret The input secret
     * @return StatusOr containing the derived key bytes or an error
     */
    static absl::StatusOr<std::vector<uint8_t>> GetKey(
        const std::string &secret);

    /**
     * Get the key URL prefix supported by this client
     *
     * @return The prefix "local-kms://"
     */
    static constexpr absl::string_view GetPrefix() { return "local-kms://"; }

  private:
    std::string secret_;

    // Helper method to validate key URI format
    bool isValidKeyUri(absl::string_view key_uri) const;
};

}  // namespace schemaregistry::rules::encryption::localkms