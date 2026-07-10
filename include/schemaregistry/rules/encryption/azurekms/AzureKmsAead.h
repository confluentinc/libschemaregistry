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
#include "azure/keyvault/keys.hpp"
#include "azure/keyvault/keys/cryptography/cryptography_client.hpp"
#include "tink/aead.h"
#include "tink/util/statusor.h"

namespace schemaregistry::rules::encryption::azurekms {

/**
 * Azure Key Vault AEAD implementation that wraps Azure Key Vault operations
 *
 * Unlike AWS KMS and GCP KMS, Azure Key Vault wrap/unwrap operations are
 * scoped to an explicit key version and do not embed that version in the
 * ciphertext. When saveVersion is enabled (see
 * AzureKmsDriver::ENCRYPT_AZURE_KEY_VERSION_SAVE), Encrypt makes its output
 * self-describing by prepending the exact version that produced it:
 * "azure:v1:" + 32-character key version + ":" + raw ciphertext bytes.
 *
 * Decrypt always checks for this prefix regardless of the current saveVersion
 * value, since a DEK wrapped while the toggle was on must remain decryptable
 * even after it is turned back off.
 */
class AzureAead : public crypto::tink::Aead {
  public:
    /**
     * Constructor
     *
     * @param keyClient Shared pointer to the Azure Key Vault key client,
     *                  used to resolve the current key version (when
     *                  saveVersion is enabled) and to build a
     *                  CryptographyClient for an explicit version (used by
     *                  Decrypt to target whichever version is embedded in
     *                  already-wrapped ciphertext)
     * @param keyName Name of the key within the vault
     * @param keyVersion The version parsed from the (possibly versionless)
     *                   kek URI; used directly when saveVersion is off, and
     *                   as Decrypt's fallback for legacy ciphertext with no
     *                   embedded version
     * @param defaultCryptoClient Cryptography client built from keyName and
     *                            keyVersion as-is (cheap: does not itself
     *                            make a network call)
     * @param saveVersion Whether to make Encrypt's output self-describing
     */
    AzureAead(
        std::shared_ptr<Azure::Security::KeyVault::Keys::KeyClient> keyClient,
        std::string keyName, std::string keyVersion,
        std::shared_ptr<
            Azure::Security::KeyVault::Keys::Cryptography::CryptographyClient>
            defaultCryptoClient,
        bool saveVersion);

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

    /**
     * Returns true if value is exactly 32 hex characters, the only shape
     * that can be embedded in (and later parsed back out of) the
     * fixed-width "azure:v1:" prefix. Used to validate both a freshly
     * resolved version (in Encrypt) and one extracted from ciphertext (in
     * Decrypt), since the encrypted key material is unauthenticated at this
     * layer and could be corrupted or tampered with. Exposed for testing.
     */
    static bool IsValidVersion(const std::string &version);

    /**
     * Returns the embedded version if ciphertext carries the "azure:v1:"
     * prefix (see class doc), or an empty string if it does not (e.g. a
     * legacy DEK wrapped before ENCRYPT_AZURE_KEY_VERSION_SAVE was enabled
     * on its KEK, or the toggle is not set). Exposed for testing.
     */
    static std::string ExtractVersion(const std::string &ciphertext);

  private:
    std::shared_ptr<Azure::Security::KeyVault::Keys::KeyClient> keyClient_;
    std::string keyName_;
    std::string keyVersion_;
    std::shared_ptr<
        Azure::Security::KeyVault::Keys::Cryptography::CryptographyClient>
        defaultCryptoClient_;
    bool saveVersion_;

    /**
     * Builds a CryptographyClient for an explicit version. Used both by
     * Encrypt (once it has resolved the current version) and Decrypt (to
     * target whichever version is embedded in already-wrapped ciphertext) --
     * there is only one place that knows how to turn a version into a
     * client.
     */
    Azure::Security::KeyVault::Keys::Cryptography::CryptographyClient
    cryptoClientForVersion(const std::string &version) const;
};

}  // namespace schemaregistry::rules::encryption::azurekms
