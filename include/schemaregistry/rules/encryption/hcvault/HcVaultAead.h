/**
 * HcVaultAead
 * HashiCorp Vault AEAD implementation
 *
 * Provides AEAD functionality using HashiCorp Vault transit operations
 */

#pragma once

#include <memory>
#include <string>

#include "absl/strings/string_view.h"
#include "libvault/VaultClient.h"
#include "tink/aead.h"
#include "tink/util/statusor.h"

namespace schemaregistry::rules::encryption::hcvault {

/**
 * HashiCorp Vault AEAD implementation that wraps Vault transit operations
 *
 * This class implements the Tink AEAD interface using HashiCorp Vault's
 * transit encryption engine for encryption and decryption operations.
 */
class HcVaultAead : public crypto::tink::Aead {
  public:
    /**
     * Constructor
     *
     * @param vaultClient Shared pointer to HashiCorp Vault client
     * @param mountPoint The mount point for the transit engine
     * @param keyName The name of the encryption key
     */
    HcVaultAead(std::shared_ptr<Vault::Client> vaultClient,
                const std::string &mountPoint, const std::string &keyName);

    /**
     * Encrypts plaintext using HashiCorp Vault transit engine
     *
     * @param plaintext The data to encrypt
     * @param associated_data Additional authenticated data (currently unused)
     * @return Encrypted ciphertext or error status
     */
    crypto::tink::util::StatusOr<std::string> Encrypt(
        absl::string_view plaintext,
        absl::string_view associated_data) const override;

    /**
     * Decrypts ciphertext using HashiCorp Vault transit engine
     *
     * @param ciphertext The data to decrypt
     * @param associated_data Additional authenticated data (currently unused)
     * @return Decrypted plaintext or error status
     */
    crypto::tink::util::StatusOr<std::string> Decrypt(
        absl::string_view ciphertext,
        absl::string_view associated_data) const override;

  private:
    std::shared_ptr<Vault::Client> vaultClient_;
    std::string mountPoint_;
    std::string keyName_;
};

}  // namespace schemaregistry::rules::encryption::hcvault
