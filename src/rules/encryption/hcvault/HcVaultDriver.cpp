/**
 * HcVaultDriver implementation
 * C++ port of the Rust hcvault_driver.rs file
 */

#include "schemaregistry/rules/encryption/hcvault/HcVaultDriver.h"

#include <cstdlib>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <vector>

#include "libvault/VaultClient.h"
#include "schemaregistry/rules/encryption/EncryptionRegistry.h"
#include "schemaregistry/rules/encryption/hcvault/HcVaultClient.h"

namespace schemaregistry::rules::encryption::hcvault {

// HcVaultDriver implementation

HcVaultDriver::HcVaultDriver() : prefix_(PREFIX) {}

const std::string &HcVaultDriver::getKeyUrlPrefix() const { return prefix_; }

std::shared_ptr<crypto::tink::KmsClient> HcVaultDriver::newKmsClient(
    const std::unordered_map<std::string, std::string> &conf,
    const std::string &keyUrl) {
    if (!isValidKeyUri(keyUrl)) {
        throw TinkError("Invalid HashiCorp Vault KMS key URI: " + keyUrl);
    }

    try {
        // Get token from configuration or environment
        std::string token;
        auto tokenIt = conf.find(TOKEN_ID);
        if (tokenIt != conf.end() && !tokenIt->second.empty()) {
            token = tokenIt->second;
        } else {
            const char *envToken = std::getenv("VAULT_TOKEN");
            if (envToken) {
                token = envToken;
            }
        }

        if (token.empty()) {
            throw TinkError(
                "Cannot load Vault token from configuration or environment");
        }

        // Get namespace from configuration or environment
        std::string namespace_;
        auto namespaceIt = conf.find(NAMESPACE);
        if (namespaceIt != conf.end() && !namespaceIt->second.empty()) {
            namespace_ = namespaceIt->second;
        } else {
            const char *envNamespace = std::getenv("VAULT_NAMESPACE");
            if (envNamespace) {
                namespace_ = envNamespace;
            }
        }

        // Parse the key URL to extract vault connection details
        std::string uri = keyUrl;
        if (uri.find(PREFIX) == 0) {
            uri = uri.substr(strlen(PREFIX));
        }

        auto url = parseVaultUrl(uri);

        Vault::HttpErrorCallback httpErrorCallback = [&](std::string err) {
            std::cout << "Error accessing Hashicorp Vault: " << err
                      << std::endl;
        };

        // Create Vault client configuration
        auto configBuilder =
            Vault::ConfigBuilder()
                .withHost(Vault::Host{url.host})
                .withPort(Vault::Port{std::to_string(url.port)});

        if (url.scheme == "https") {
            configBuilder = configBuilder.withTlsEnabled(true);
        } else {
            configBuilder = configBuilder.withTlsEnabled(false);
        }

        if (!namespace_.empty()) {
            configBuilder =
                configBuilder.withNamespace(Vault::Namespace{namespace_});
        }

        auto config = configBuilder.build();

        // Create authentication strategy with token
        auto authStrategy = Vault::TokenStrategy{Vault::Token{token}};

        // Create the Vault client
        auto vaultClient = std::make_shared<Vault::Client>(config, authStrategy,
                                                           httpErrorCallback);

        // Create and return the HashiCorp Vault KMS client
        return std::make_shared<HcVaultKmsClient>(keyUrl, vaultClient);

    } catch (const std::exception &e) {
        throw TinkError("Error creating HashiCorp Vault KMS client: " +
                        std::string(e.what()));
    }
}

std::unique_ptr<HcVaultDriver> HcVaultDriver::create() {
    return std::make_unique<HcVaultDriver>();
}

void HcVaultDriver::registerDriver() {
    auto driver = create();
    EncryptionRegistry::getInstance().registerKmsDriver(std::move(driver));
}

std::tuple<std::string, std::string> HcVaultDriver::getEndpointPaths(
    const std::string &keyPath) const {
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
        throw TinkError("Invalid key path format: " + keyPath);
    }

    std::string mountPath = parts[0];
    std::string keyName = parts[parts.size() - 1];

    return std::make_tuple(mountPath, keyName);
}

bool HcVaultDriver::isValidKeyUri(const std::string &keyUri) const {
    if (keyUri.find(PREFIX) != 0) {
        return false;
    }

    try {
        std::string uri = keyUri.substr(strlen(PREFIX));
        auto url = parseVaultUrl(uri);
        getEndpointPaths(url.path);
        return true;
    } catch (const TinkError &) {
        return false;
    }
}

// Helper method for parsing vault URLs (used by both driver and client)
HcVaultDriver::VaultUrl HcVaultDriver::parseVaultUrl(
    const std::string &url) const {
    // Parse URL like: https://vault.example.com:8200/transit/keys/my-key
    std::regex urlRegex(R"(^(https?)://([^:/]+)(?::(\d+))?(/.*)?$)");
    std::smatch matches;

    if (!std::regex_match(url, matches, urlRegex)) {
        throw TinkError("Invalid HashiCorp Vault URL format: " + url);
    }

    VaultUrl parsed;
    parsed.scheme = matches[1].str();
    parsed.host = matches[2].str();
    parsed.port = matches[3].matched ? std::stoi(matches[3].str())
                                     : (parsed.scheme == "https" ? 443 : 80);
    parsed.path = matches[4].matched ? matches[4].str() : "/";

    return parsed;
}

}  // namespace schemaregistry::rules::encryption::hcvault