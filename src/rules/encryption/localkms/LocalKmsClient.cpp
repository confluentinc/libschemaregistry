/**
 * LocalKmsClient
 * Implementation of local KMS client using Tink C++ APIs
 */

#include "schemaregistry/rules/encryption/localkms/LocalKmsClient.h"

#include <openssl/sha.h>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "tink/aead/aes_gcm_key_manager.h"
#include "tink/subtle/aes_gcm_boringssl.h"
#include "tink/subtle/hkdf.h"
#include "tink/util/status.h"
#include "tink/util/statusor.h"

namespace schemaregistry::rules::encryption::localkms {

namespace {
constexpr absl::string_view kPrefix = "local-kms://";
constexpr size_t kKeySize = 16;  // 128-bit key for AES-128-GCM
}  // namespace

LocalKmsClient::LocalKmsClient(const std::string &secret) : secret_(secret) {
    if (secret.empty()) {
        // Note: In a real implementation, we might want to throw an exception
        // here For now, we'll let the GetAead method handle the empty secret
        // case
    }
}

bool LocalKmsClient::DoesSupport(absl::string_view key_uri) const {
    return isValidKeyUri(key_uri);
}

absl::StatusOr<std::unique_ptr<crypto::tink::Aead>> LocalKmsClient::GetAead(
    absl::string_view key_uri) const {
    if (!DoesSupport(key_uri)) {
        return absl::InvalidArgumentError(absl::StrCat(
            "Key URI '", key_uri, "' not supported by LocalKmsClient"));
    }

    return GetPrimitive(secret_);
}

absl::StatusOr<std::unique_ptr<crypto::tink::Aead>>
LocalKmsClient::GetPrimitive(const std::string &secret) {
    if (secret.empty()) {
        return absl::InvalidArgumentError("Secret cannot be empty");
    }

    // Derive key using HKDF
    auto key_result = GetKey(secret);
    if (!key_result.ok()) {
        return key_result.status();
    }

    std::vector<uint8_t> key = std::move(*key_result);

    // Convert vector to string_view for AES-GCM
    absl::string_view key_view(reinterpret_cast<const char *>(key.data()),
                               key.size());

    // Create AES-GCM AEAD primitive using the derived key
    auto aes_gcm_result = crypto::tink::subtle::AesGcmBoringSsl::New(key_view);
    if (!aes_gcm_result.ok()) {
        return absl::InternalError(
            absl::StrCat("Failed to create AES-GCM primitive: ",
                         aes_gcm_result.status().message()));
    }

    return std::move(*aes_gcm_result);
}

absl::StatusOr<std::vector<uint8_t>> LocalKmsClient::GetKey(
    const std::string &secret) {
    if (secret.empty()) {
        return absl::InvalidArgumentError("Secret cannot be empty");
    }

    // Use HKDF with SHA-256 to derive a key from the secret
    // Parameters match the Rust implementation:
    // - Hash: SHA-256
    // - Salt: empty (nullptr)
    // - Info: empty
    // - Key length: 16 bytes (128 bits)

    auto hkdf_result = crypto::tink::subtle::Hkdf::ComputeHkdf(
        crypto::tink::subtle::HashType::SHA256,
        secret,   // Input key material (IKM)
        "",       // Salt (empty)
        "",       // Info (empty)
        kKeySize  // Output key length
    );

    if (!hkdf_result.ok()) {
        return absl::InternalError(absl::StrCat(
            "HKDF key derivation failed: ", hkdf_result.status().message()));
    }

    // Convert string to vector<uint8_t>
    const std::string &key_string = *hkdf_result;
    std::vector<uint8_t> key_bytes(key_string.begin(), key_string.end());

    return key_bytes;
}

bool LocalKmsClient::isValidKeyUri(absl::string_view key_uri) const {
    return key_uri.substr(0, kPrefix.size()) == kPrefix;
}

}  // namespace schemaregistry::rules::encryption::localkms