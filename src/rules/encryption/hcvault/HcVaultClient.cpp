/**
 * HcVaultClient implementation
 * HashiCorp Vault KMS client implementation
 */

#include "schemaregistry/rules/encryption/hcvault/HcVaultClient.h"

#include <regex>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "absl/status/status.h"
#include "schemaregistry/rules/encryption/hcvault/HcVaultAead.h"
#include "schemaregistry/rules/encryption/hcvault/HcVaultDriver.h"

namespace schemaregistry::rules::encryption::hcvault {

HcVaultKmsClient::HcVaultKmsClient(const std::string &keyUriPrefix,
                                   std::shared_ptr<Vault::Client> vaultClient)
    : keyUriPrefix_(keyUriPrefix), vaultClient_(std::move(vaultClient)) {}

bool HcVaultKmsClient::DoesSupport(absl::string_view key_uri) const {
    std::string uri(key_uri);
    return uri.find(keyUriPrefix_) == 0;
}

crypto::tink::util::StatusOr<std::unique_ptr<crypto::tink::Aead>>
HcVaultKmsClient::GetAead(absl::string_view key_uri) const {
    if (!DoesSupport(key_uri)) {
        return crypto::tink::util::Status(absl::StatusCode::kInvalidArgument,
                                          "Key URI must start with prefix " +
                                              keyUriPrefix_ + ", but got " +
                                              std::string(key_uri));
    }

    try {
        // Strip the hcvault:// prefix
        std::string uri = std::string(key_uri);
        if (uri.find(HcVaultDriver::PREFIX) == 0) {
            uri = uri.substr(strlen(HcVaultDriver::PREFIX));
        }

        // Parse the URI to extract the path
        auto url = parseVaultUrl(uri);
        auto [mountPoint, keyName] = parseKeyPath(url.path);

        // Return the HashiCorp Vault AEAD implementation
        return std::make_unique<HcVaultAead>(vaultClient_, mountPoint, keyName);

    } catch (const std::exception &e) {
        return crypto::tink::util::Status(
            absl::StatusCode::kInternal,
            "Failed to create HashiCorp Vault AEAD: " + std::string(e.what()));
    }
}

HcVaultKmsClient::VaultUrl HcVaultKmsClient::parseVaultUrl(
    const std::string &url) {
    // Parse URL like: https://vault.example.com:8200/transit/keys/my-key
    std::regex urlRegex(R"(^(https?)://([^:/]+)(?::(\d+))?(/.*)?$)");
    std::smatch matches;

    if (!std::regex_match(url, matches, urlRegex)) {
        throw std::invalid_argument("Invalid HashiCorp Vault URL format: " +
                                    url);
    }

    VaultUrl parsed;
    parsed.scheme = matches[1].str();
    parsed.host = matches[2].str();
    parsed.port = matches[3].matched ? std::stoi(matches[3].str())
                                     : (parsed.scheme == "https" ? 443 : 80);
    parsed.path = matches[4].matched ? matches[4].str() : "/";

    return parsed;
}

std::tuple<std::string, std::string> HcVaultKmsClient::parseKeyPath(
    const std::string &keyPath) {
    // Parse path like: /transit/keys/my-key
    // Expected format: /{mount-path}/keys/{keyName}
    std::vector<std::string> parts;
    std::stringstream ss(keyPath);
    std::string part;

    while (std::getline(ss, part, '/')) {
        if (!part.empty()) {
            parts.push_back(part);
        }
    }

    if (parts.size() < 3 || parts[parts.size() - 2] != "keys") {
        throw std::invalid_argument("Invalid key path format: " + keyPath);
    }

    std::string mountPath = parts[0];
    std::string keyName = parts[parts.size() - 1];

    return std::make_tuple(mountPath, keyName);
}

}  // namespace schemaregistry::rules::encryption::hcvault
