/**
 * AwsKmsDriver implementation
 * C++ port of the Rust aws_driver.rs file
 */

#include "schemaregistry/rules/encryption/awskms/AwsKmsDriver.h"

#include <cstdlib>
#include <sstream>
#include <vector>

#include "absl/base/call_once.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "aws/core/Aws.h"
#include "aws/core/auth/AWSCredentialsProvider.h"
#include "aws/core/auth/AWSCredentialsProviderChain.h"
#include "aws/core/client/ClientConfiguration.h"
#include "aws/identity-management/auth/STSAssumeRoleCredentialsProvider.h"
#include "aws/sts/STSClient.h"
#include "aws/sts/model/AssumeRoleRequest.h"
#include "schemaregistry/rules/encryption/EncryptionRegistry.h"
#include "schemaregistry/rules/encryption/awskms/AwsKmsClient.h"

static absl::once_flag aws_initialization_once;

namespace schemaregistry::rules::encryption::awskms {

void InitAwsApi() {
    Aws::SDKOptions options;
    Aws::InitAPI(options);
}

AwsKmsDriver::AwsKmsDriver() : prefix_(PREFIX) {}

const std::string &AwsKmsDriver::getKeyUrlPrefix() const { return prefix_; }

std::shared_ptr<crypto::tink::KmsClient> AwsKmsDriver::newKmsClient(
    const std::unordered_map<std::string, std::string> &conf,
    const std::string &keyUrl) {
    absl::call_once(aws_initialization_once, []() { InitAwsApi(); });
    if (!isValidKeyUri(keyUrl)) {
        throw TinkError("Invalid AWS KMS key URI: " + keyUrl);
    }

    try {
        // Build credentials based on configuration
        auto credentialsProvider = buildCredentials(conf, keyUrl);

        // Create the AWS KMS client using Tink's AwsKmsClient
        auto statusOrClient =
            crypto::tink::integration::awskms::AwsKmsClient::New(
                keyUrl, credentialsProvider);

        if (!statusOrClient.ok()) {
            throw TinkError("Failed to create AWS KMS client: " +
                            std::string(statusOrClient.status().message()));
        }

        return std::move(statusOrClient.value());

    } catch (const std::exception &e) {
        throw TinkError("Error creating AWS KMS client: " +
                        std::string(e.what()));
    }
}

std::unique_ptr<AwsKmsDriver> AwsKmsDriver::create() {
    return std::make_unique<AwsKmsDriver>();
}

void AwsKmsDriver::registerDriver() {
    auto driver = create();
    EncryptionRegistry::getInstance().registerKmsDriver(std::move(driver));
}

std::shared_ptr<Aws::Auth::AWSCredentialsProvider> AwsKmsDriver::buildCredentials(
    const std::unordered_map<std::string, std::string> &conf,
    const std::string &keyUrl) const {
    
    // Extract configuration values, falling back to environment variables
    std::string roleArn;
    auto roleArnIt = conf.find(ROLE_ARN);
    if (roleArnIt != conf.end()) {
        roleArn = roleArnIt->second;
    } else {
        const char* envRoleArn = std::getenv("AWS_ROLE_ARN");
        if (envRoleArn) {
            roleArn = envRoleArn;
        }
    }
    
    std::string roleSessionName;
    auto roleSessionNameIt = conf.find(ROLE_SESSION_NAME);
    if (roleSessionNameIt != conf.end()) {
        roleSessionName = roleSessionNameIt->second;
    } else {
        const char* envRoleSessionName = std::getenv("AWS_ROLE_SESSION_NAME");
        if (envRoleSessionName) {
            roleSessionName = envRoleSessionName;
        }
    }
    
    std::string roleExternalId;
    auto roleExternalIdIt = conf.find(ROLE_EXTERNAL_ID);
    if (roleExternalIdIt != conf.end()) {
        roleExternalId = roleExternalIdIt->second;
    } else {
        const char* envRoleExternalId = std::getenv("AWS_ROLE_EXTERNAL_ID");
        if (envRoleExternalId) {
            roleExternalId = envRoleExternalId;
        }
    }

    std::string roleWebIdentityTokenFile;
    const char* envRoleWebIdentityTokenFile = std::getenv("AWS_WEB_IDENTITY_TOKEN_FILE");
    if (envRoleWebIdentityTokenFile) {
        roleWebIdentityTokenFile = envRoleWebIdentityTokenFile;
    }
    
    std::string accessKeyId;
    auto accessKeyIt = conf.find(ACCESS_KEY_ID);
    if (accessKeyIt != conf.end()) {
        accessKeyId = accessKeyIt->second;
    }
    
    std::string secretAccessKey;
    auto secretKeyIt = conf.find(SECRET_ACCESS_KEY);
    if (secretKeyIt != conf.end()) {
        secretAccessKey = secretKeyIt->second;
    }
    
    std::string profile;
    auto profileIt = conf.find(PROFILE);
    if (profileIt != conf.end()) {
        profile = profileIt->second;
    }
    
    // Get region from key ARN
    std::string keyArn = keyUriToKeyArn(keyUrl);
    std::string region = getRegionFromKeyArn(keyArn);
    
    // Build base credentials provider
    std::shared_ptr<Aws::Auth::AWSCredentialsProvider> baseCredentials;
    
    if (!accessKeyId.empty() && !secretAccessKey.empty()) {
        // Use explicit access key and secret
        baseCredentials = std::make_shared<Aws::Auth::SimpleAWSCredentialsProvider>(
            accessKeyId, secretAccessKey);
    } else if (!profile.empty()) {
        // Use profile-based credentials
        baseCredentials = std::make_shared<Aws::Auth::ProfileConfigFileAWSCredentialsProvider>(
            profile.c_str());
    } else {
        // Use default credentials chain
        baseCredentials = std::make_shared<Aws::Auth::DefaultAWSCredentialsProviderChain>();
    }
    
    // If role ARN is specified, wrap with STS assume role provider
    // If roleWebIdentityTokenFile is set, use the DefaultCredentialsProvider
    if (!roleArn.empty() && roleWebIdentityTokenFile.empty()) {
        Aws::Client::ClientConfiguration stsConfig;
        stsConfig.region = region.c_str();

        auto stsClient = Aws::MakeShared<Aws::STS::STSClient>(
            "schemaregistry", baseCredentials, stsConfig);

        Aws::String session = roleSessionName.empty()
                                   ? Aws::String("schemaregistry-session")
                                   : Aws::String(roleSessionName.c_str());
        Aws::String extId = roleExternalId.empty() ? Aws::String("")
                                                   : Aws::String(roleExternalId.c_str());

        auto assumeProvider = Aws::MakeShared<Aws::Auth::STSAssumeRoleCredentialsProvider>(
            "schemaregistry",
            Aws::String(roleArn.c_str()),
            session,
            extId,
            Aws::Auth::DEFAULT_CREDS_LOAD_FREQ_SECONDS,
            stsClient);

        baseCredentials = assumeProvider;
    }
    
    return baseCredentials;
}


std::string AwsKmsDriver::keyUriToKeyArn(const std::string &keyUri) const {
    if (keyUri.find(PREFIX) != 0) {
        throw TinkError("Invalid key URI: " + keyUri);
    }
    return keyUri.substr(strlen(PREFIX));
}

std::string AwsKmsDriver::getRegionFromKeyArn(const std::string &keyArn) const {
    // An AWS key ARN is of the form:
    // arn:aws:kms:us-west-2:111122223333:key/1234abcd-12ab-34cd-56ef-1234567890ab

    std::vector<std::string> parts = absl::StrSplit(keyArn, ':');
    if (parts.size() < 6) {
        throw TinkError("Invalid key ARN format: " + keyArn);
    }

    // The region is the 4th component (index 3)
    return parts[3];
}

bool AwsKmsDriver::isValidKeyUri(const std::string &keyUri) const {
    if (keyUri.find(PREFIX) != 0) {
        return false;
    }

    try {
        std::string keyArn = keyUriToKeyArn(keyUri);
        getRegionFromKeyArn(keyArn);
        return true;
    } catch (const TinkError &) {
        return false;
    }
}

}  // namespace schemaregistry::rules::encryption::awskms