/**
 * AzureKmsAead implementation
 * Azure Key Vault AEAD implementation
 */

#include "schemaregistry/rules/encryption/azurekms/AzureKmsAead.h"

#include <algorithm>
#include <cctype>
#include <vector>

#include "absl/status/status.h"
#include "azure/keyvault/keys/cryptography/cryptography_client_models.hpp"

namespace schemaregistry::rules::encryption::azurekms {

namespace {

const std::string kVersionPrefix = "azure:v1:";
constexpr size_t kVersionLength = 32;
// +1 for the ':' separating the version from the raw ciphertext bytes.
const size_t kHeaderLength = kVersionPrefix.size() + kVersionLength + 1;

}  // namespace

bool AzureAead::IsValidVersion(const std::string &version) {
    if (version.size() != kVersionLength) {
        return false;
    }
    return std::all_of(version.begin(), version.end(), [](unsigned char c) {
        return std::isxdigit(c);
    });
}

std::string AzureAead::ExtractVersion(const std::string &ciphertext) {
    if (ciphertext.size() < kHeaderLength ||
        ciphertext.compare(0, kVersionPrefix.size(), kVersionPrefix) != 0 ||
        ciphertext[kHeaderLength - 1] != ':') {
        return "";
    }
    return ciphertext.substr(kVersionPrefix.size(), kVersionLength);
}

AzureAead::AzureAead(
    std::shared_ptr<Azure::Security::KeyVault::Keys::KeyClient> keyClient,
    std::string keyName, std::string keyVersion,
    std::shared_ptr<
        Azure::Security::KeyVault::Keys::Cryptography::CryptographyClient>
        defaultCryptoClient,
    bool saveVersion)
    : keyClient_(std::move(keyClient)),
      keyName_(std::move(keyName)),
      keyVersion_(std::move(keyVersion)),
      defaultCryptoClient_(std::move(defaultCryptoClient)),
      saveVersion_(saveVersion) {}

Azure::Security::KeyVault::Keys::Cryptography::CryptographyClient
AzureAead::cryptoClientForVersion(const std::string &version) const {
    return keyClient_->GetCryptographyClient(keyName_, version);
}

crypto::tink::util::StatusOr<std::string> AzureAead::Encrypt(
    absl::string_view plaintext, absl::string_view associated_data) const {
    try {
        std::vector<uint8_t> plaintextBytes(plaintext.begin(), plaintext.end());
        auto params = Azure::Security::KeyVault::Keys::Cryptography::
            EncryptParameters::RsaOaep256Parameters(plaintextBytes);

        if (!saveVersion_) {
            auto response = defaultCryptoClient_->Encrypt(params);
            const auto &result = response.Value.Ciphertext;
            return std::string(result.begin(), result.end());
        }

        // Resolve the currently latest key version and use it to build a
        // version-specific client for this encrypt call.
        auto keyResponse = keyClient_->GetKey(keyName_);
        const std::string &resolvedId = keyResponse.Value.Id();
        auto lastSlash = resolvedId.find_last_of('/');
        if (lastSlash == std::string::npos) {
            return crypto::tink::util::Status(
                absl::StatusCode::kInternal,
                "Resolved Azure Key Vault key id is missing a version "
                "segment: " +
                    resolvedId);
        }
        std::string version = resolvedId.substr(lastSlash + 1);
        if (!IsValidVersion(version)) {
            // Mirrors Decrypt's own validation: a DEK this method wraps must
            // always be one this same class can later unwrap.
            return crypto::tink::util::Status(
                absl::StatusCode::kInternal,
                "kms key version '" + version + "' must be a " +
                    std::to_string(kVersionLength) +
                    "-character hex string; cannot be embedded in a "
                    "fixed-width azure:v1: prefix");
        }

        auto client = cryptoClientForVersion(version);
        auto response = client.Encrypt(params);
        const auto &ciphertext = response.Value.Ciphertext;

        std::string output;
        output.reserve(kVersionPrefix.size() + version.size() + 1 +
                       ciphertext.size());
        output.append(kVersionPrefix);
        output.append(version);
        output.push_back(':');
        output.append(ciphertext.begin(), ciphertext.end());
        return output;

    } catch (const std::exception &e) {
        return crypto::tink::util::Status(
            absl::StatusCode::kInternal,
            "Azure KMS encryption failed: " + std::string(e.what()));
    }
}

crypto::tink::util::StatusOr<std::string> AzureAead::Decrypt(
    absl::string_view ciphertext, absl::string_view associated_data) const {
    try {
        std::string ciphertextStr(ciphertext);
        std::string version = ExtractVersion(ciphertextStr);

        if (version.empty()) {
            std::vector<uint8_t> wrappedBytes(ciphertextStr.begin(),
                                              ciphertextStr.end());
            auto params = Azure::Security::KeyVault::Keys::Cryptography::
                DecryptParameters::RsaOaep256Parameters(wrappedBytes);
            auto response = defaultCryptoClient_->Decrypt(params);
            const auto &result = response.Value.Plaintext;
            return std::string(result.begin(), result.end());
        }

        if (!IsValidVersion(version)) {
            // Encrypted key material is unauthenticated at this layer, so a
            // corrupted or tampered value could otherwise smuggle arbitrary
            // characters (e.g. '/') into the key name/version passed to the
            // Azure SDK below.
            return crypto::tink::util::Status(
                absl::StatusCode::kInternal,
                "ciphertext carries an invalid azure:v1: key version: '" +
                    version + "'");
        }
        std::string wrapped = ciphertextStr.substr(kHeaderLength);
        std::vector<uint8_t> wrappedBytes(wrapped.begin(), wrapped.end());
        auto params = Azure::Security::KeyVault::Keys::Cryptography::
            DecryptParameters::RsaOaep256Parameters(wrappedBytes);
        auto client = cryptoClientForVersion(version);
        auto response = client.Decrypt(params);
        const auto &result = response.Value.Plaintext;
        return std::string(result.begin(), result.end());

    } catch (const std::exception &e) {
        return crypto::tink::util::Status(
            absl::StatusCode::kInternal,
            "Azure KMS decryption failed: " + std::string(e.what()));
    }
}

}  // namespace schemaregistry::rules::encryption::azurekms
