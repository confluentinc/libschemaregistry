/**
 * AzureKmsAead
 * Azure Key Vault AEAD implementation
 *
 * Provides AEAD functionality using Azure Key Vault cryptography operations
 */

#pragma once

#include <memory>
#include <string>

#include "absl/strings/string_view.h"
#include "azure/keyvault/keys/cryptography/cryptography_client.hpp"
#include "tink/aead.h"
#include "tink/util/statusor.h"

namespace schemaregistry::rules::encryption::azurekms {

/**
 * Azure Key Vault AEAD implementation that wraps Azure Key Vault operations
 *
 * This class implements the Tink AEAD interface using Azure Key Vault's
 * cryptography client for encryption and decryption operations.
 */
class AzureAead : public crypto::tink::Aead {
  public:
    /**
     * Constructor
     *
     * @param cryptoClient Shared pointer to Azure Key Vault cryptography client
     */
    explicit AzureAead(
        std::shared_ptr<
            Azure::Security::KeyVault::Keys::Cryptography::CryptographyClient>
            cryptoClient);

    /**
     * Encrypts plaintext using Azure Key Vault
     *
     * @param plaintext The data to encrypt
     * @param associated_data Additional authenticated data (currently unused)
     * @return Encrypted ciphertext or error status
     */
    crypto::tink::util::StatusOr<std::string> Encrypt(
        absl::string_view plaintext,
        absl::string_view associated_data) const override;

    /**
     * Decrypts ciphertext using Azure Key Vault
     *
     * @param ciphertext The data to decrypt
     * @param associated_data Additional authenticated data (currently unused)
     * @return Decrypted plaintext or error status
     */
    crypto::tink::util::StatusOr<std::string> Decrypt(
        absl::string_view ciphertext,
        absl::string_view associated_data) const override;

  private:
    std::shared_ptr<
        Azure::Security::KeyVault::Keys::Cryptography::CryptographyClient>
        cryptoClient_;
};

}  // namespace schemaregistry::rules::encryption::azurekms
