/**
 * GcpKmsDriver implementation
 * C++ port of the Rust gcp_driver.rs file
 */

#include "schemaregistry/rules/encryption/gcpkms/GcpKmsDriver.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "schemaregistry/rules/encryption/EncryptionRegistry.h"
#include "schemaregistry/rules/encryption/gcpkms/GcpKmsClient.h"

namespace schemaregistry::rules::encryption::gcpkms {

GcpKmsDriver::GcpKmsDriver() : prefix_(PREFIX) {}

const std::string& GcpKmsDriver::getKeyUrlPrefix() const { return prefix_; }

std::shared_ptr<crypto::tink::KmsClient> GcpKmsDriver::newKmsClient(
    const std::unordered_map<std::string, std::string>& conf,
    const std::string& keyUrl) {
    if (!isValidKeyUri(keyUrl)) {
        throw TinkError("Invalid GCP KMS key URI: " + keyUrl);
    }

    try {
        // Build credentials path from configuration
        std::string credentialsPath = buildCredentialsPath(conf, keyUrl);

        // Create GCP KMS client using Tink's GCP integration
        auto clientResult =
            crypto::tink::integration::gcpkms::GcpKmsClient::New(
                keyUrl, credentialsPath);

        if (!clientResult.ok()) {
            throw TinkError("Failed to create GCP KMS client: " +
                            std::string(clientResult.status().message()));
        }

        return std::move(clientResult.value());

    } catch (const std::exception& e) {
        throw TinkError("Error creating GCP KMS client: " +
                        std::string(e.what()));
    }
}

std::unique_ptr<GcpKmsDriver> GcpKmsDriver::create() {
    return std::make_unique<GcpKmsDriver>();
}

void GcpKmsDriver::registerDriver() {
    auto driver = create();
    EncryptionRegistry::getInstance().registerKmsDriver(std::move(driver));
}

bool GcpKmsDriver::isValidKeyUri(const std::string& keyUri) const {
    // Check if the URI starts with the GCP KMS prefix
    if (keyUri.find(PREFIX) != 0) {
        return false;
    }

    // Extract the key name part (after the prefix)
    std::string keyName = keyUri.substr(strlen(PREFIX));
    if (keyName.empty()) {
        return false;
    }

    // Basic validation: should contain projects, locations, keyRings,
    // cryptoKeys Expected format:
    // projects/PROJECT_ID/locations/LOCATION/keyRings/KEY_RING/cryptoKeys/KEY_NAME
    std::vector<std::string> parts = absl::StrSplit(keyName, '/');

    // Should have at least 8 parts: projects, PROJECT_ID, locations, LOCATION,
    // keyRings, KEY_RING, cryptoKeys, KEY_NAME
    if (parts.size() < 8) {
        return false;
    }

    // Check required path components
    return parts[0] == "projects" && parts[2] == "locations" &&
           parts[4] == "keyRings" && parts[6] == "cryptoKeys";
}

std::string GcpKmsDriver::buildCredentialsPath(
    const std::unordered_map<std::string, std::string>& conf,
    const std::string& keyUrl) const {
    // If we have service account credentials in the config, create a temporary
    // file
    if (hasServiceAccountCredentials(conf)) {
        return createCredentialsFile(conf);
    }

    // Check for explicit credentials path in configuration
    auto pathIt = conf.find("credentials.path");
    if (pathIt != conf.end() && !pathIt->second.empty()) {
        return pathIt->second;
    }

    // Check for GOOGLE_APPLICATION_CREDENTIALS environment variable
    const char* envCreds = std::getenv("GOOGLE_APPLICATION_CREDENTIALS");
    if (envCreds != nullptr && strlen(envCreds) > 0) {
        return std::string(envCreds);
    }

    // Return empty string to use default credentials
    return "";
}

std::string GcpKmsDriver::createCredentialsFile(
    const std::unordered_map<std::string, std::string>& conf) const {
    // Extract service account parameters
    auto accountTypeIt = conf.find(ACCOUNT_TYPE);
    auto clientIdIt = conf.find(CLIENT_ID);
    auto clientEmailIt = conf.find(CLIENT_EMAIL);
    auto privateKeyIdIt = conf.find(PRIVATE_KEY_ID);
    auto privateKeyIt = conf.find(PRIVATE_KEY);

    std::string accountType =
        accountTypeIt != conf.end() ? accountTypeIt->second : "service_account";

    // Create JSON credentials content
    std::ostringstream json;
    json << "{\n";
    json << "  \"type\": \"" << accountType << "\",\n";
    json << "  \"client_id\": \"" << clientIdIt->second << "\",\n";
    json << "  \"client_email\": \"" << clientEmailIt->second << "\",\n";
    json << "  \"private_key_id\": \"" << privateKeyIdIt->second << "\",\n";
    json << "  \"private_key\": \"" << privateKeyIt->second << "\"\n";
    json << "}";

    // Create temporary file
    std::string tempPath =
        "/tmp/gcp_creds_" + std::to_string(std::time(nullptr)) + ".json";

    std::ofstream file(tempPath);
    if (!file.is_open()) {
        throw TinkError("Failed to create temporary credentials file: " +
                        tempPath);
    }

    file << json.str();
    file.close();

    if (file.fail()) {
        throw TinkError("Failed to write credentials to file: " + tempPath);
    }

    return tempPath;
}

bool GcpKmsDriver::hasServiceAccountCredentials(
    const std::unordered_map<std::string, std::string>& conf) const {
    // Check if all required service account parameters are present
    auto clientIdIt = conf.find(CLIENT_ID);
    auto clientEmailIt = conf.find(CLIENT_EMAIL);
    auto privateKeyIdIt = conf.find(PRIVATE_KEY_ID);
    auto privateKeyIt = conf.find(PRIVATE_KEY);

    return clientIdIt != conf.end() && !clientIdIt->second.empty() &&
           clientEmailIt != conf.end() && !clientEmailIt->second.empty() &&
           privateKeyIdIt != conf.end() && !privateKeyIdIt->second.empty() &&
           privateKeyIt != conf.end() && !privateKeyIt->second.empty();
}

}  // namespace schemaregistry::rules::encryption::gcpkms
