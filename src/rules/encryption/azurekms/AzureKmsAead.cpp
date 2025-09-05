/**
 * AzureKmsAead implementation
 * Azure Key Vault AEAD implementation
 */

#include "schemaregistry/rules/encryption/azurekms/AzureKmsAead.h"

#include <vector>

#include "absl/status/status.h"
#include "azure/keyvault/keys/cryptography/cryptography_client_models.hpp"

namespace schemaregistry::rules::encryption::azurekms {

AzureAead::AzureAead(
    std::shared_ptr<
        Azure::Security::KeyVault::Keys::Cryptography::CryptographyClient>
        cryptoClient)
    : cryptoClient_(std::move(cryptoClient)) {}

crypto::tink::util::StatusOr<std::string> AzureAead::Encrypt(
    absl::string_view plaintext, absl::string_view associated_data) const {
    try {
        // Convert plaintext to vector<uint8_t>
        std::vector<uint8_t> plaintextBytes(plaintext.begin(), plaintext.end());

        // Create encrypt parameters using static factory method
        auto params = Azure::Security::KeyVault::Keys::Cryptography::
            EncryptParameters::RsaOaep256Parameters(plaintextBytes);

        // Perform encryption
        auto response = cryptoClient_->Encrypt(params);

        // Convert result to string
        const auto &result = response.Value.Ciphertext;
        return std::string(result.begin(), result.end());

    } catch (const std::exception &e) {
        return crypto::tink::util::Status(
            absl::StatusCode::kInternal,
            "Azure KMS encryption failed: " + std::string(e.what()));
    }
}

crypto::tink::util::StatusOr<std::string> AzureAead::Decrypt(
    absl::string_view ciphertext, absl::string_view associated_data) const {
    try {
        // Convert ciphertext to vector<uint8_t>
        std::vector<uint8_t> ciphertextBytes(ciphertext.begin(),
                                             ciphertext.end());

        // Create decrypt parameters using static factory method
        auto params = Azure::Security::KeyVault::Keys::Cryptography::
            DecryptParameters::RsaOaep256Parameters(ciphertextBytes);

        // Perform decryption
        auto response = cryptoClient_->Decrypt(params);

        // Convert result to string
        const auto &result = response.Value.Plaintext;
        return std::string(result.begin(), result.end());

    } catch (const std::exception &e) {
        return crypto::tink::util::Status(
            absl::StatusCode::kInternal,
            "Azure KMS decryption failed: " + std::string(e.what()));
    }
}

}  // namespace schemaregistry::rules::encryption::azurekms
