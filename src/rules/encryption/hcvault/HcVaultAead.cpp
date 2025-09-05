/**
 * HcVaultAead implementation
 * HashiCorp Vault AEAD implementation
 */

#include "schemaregistry/rules/encryption/hcvault/HcVaultAead.h"

#include "absl/status/status.h"
#include "nlohmann/json.hpp"

namespace schemaregistry::rules::encryption::hcvault {

HcVaultAead::HcVaultAead(std::shared_ptr<Vault::Client> vaultClient,
                         const std::string &mountPoint,
                         const std::string &keyName)
    : vaultClient_(std::move(vaultClient)),
      mountPoint_(mountPoint),
      keyName_(keyName) {}

crypto::tink::util::StatusOr<std::string> HcVaultAead::Encrypt(
    absl::string_view plaintext, absl::string_view associated_data) const {
    try {
        // Convert plaintext to base64
        std::string plaintextBase64 =
            Vault::Base64::encode(std::string(plaintext));

        // Create encrypt request parameters
        Vault::Parameters parameters;
        parameters["plaintext"] = plaintextBase64;

        // Create path for encryption
        std::string path = keyName_;

        // Perform encryption using Vault transit engine
        Vault::Transit transit(*vaultClient_);
        auto response = transit.encrypt(Vault::Path{path}, parameters);

        if (!response) {
            return crypto::tink::util::Status(
                absl::StatusCode::kInternal,
                "HashiCorp Vault encryption failed: No response received");
        }

        // Parse response and extract ciphertext per libvault test example
        auto json = nlohmann::json::parse(response.value());
        if (!json.contains("data") || !json["data"].contains("ciphertext")) {
            return crypto::tink::util::Status(
                absl::StatusCode::kInternal,
                "HashiCorp Vault encryption failed: Unexpected response "
                "format");
        }
        return json["data"]["ciphertext"].get<std::string>();

    } catch (const std::exception &e) {
        return crypto::tink::util::Status(
            absl::StatusCode::kInternal,
            "HashiCorp Vault encryption failed: " + std::string(e.what()));
    }
}

crypto::tink::util::StatusOr<std::string> HcVaultAead::Decrypt(
    absl::string_view ciphertext, absl::string_view associated_data) const {
    try {
        // Create decrypt request parameters
        Vault::Parameters parameters;
        parameters["ciphertext"] = std::string(ciphertext);

        // Create path for decryption
        std::string path = keyName_;

        // Perform decryption using Vault transit engine
        Vault::Transit transit(*vaultClient_);
        auto response = transit.decrypt(Vault::Path{path}, parameters);

        if (!response) {
            return crypto::tink::util::Status(
                absl::StatusCode::kInternal,
                "HashiCorp Vault decryption failed: No response received");
        }

        // libvault transit.decrypt returns plaintext directly
        return response.value();

    } catch (const std::exception &e) {
        return crypto::tink::util::Status(
            absl::StatusCode::kInternal,
            "HashiCorp Vault decryption failed: " + std::string(e.what()));
    }
}

}  // namespace schemaregistry::rules::encryption::hcvault
