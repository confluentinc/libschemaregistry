/**
 * AzureKmsAeadTest
 * Tests for the pure, deterministic wire-format logic in AzureAead
 * (isValidVersion / extractVersion), which does not require any network
 * access or Azure Key Vault credentials.
 */

#include <gtest/gtest.h>

#include "schemaregistry/rules/encryption/azurekms/AzureKmsAead.h"

using schemaregistry::rules::encryption::azurekms::AzureAead;

namespace {
const std::string kVersionA(32, 'a');
}

TEST(AzureKmsAeadTest, IsValidVersionAcceptsExactly32HexChars) {
    EXPECT_TRUE(AzureAead::IsValidVersion(kVersionA));
    EXPECT_TRUE(AzureAead::IsValidVersion(std::string(32, 'F')));
}

TEST(AzureKmsAeadTest, IsValidVersionRejectsWrongLength) {
    EXPECT_FALSE(AzureAead::IsValidVersion("not-32-chars"));
    EXPECT_FALSE(AzureAead::IsValidVersion(""));
    EXPECT_FALSE(AzureAead::IsValidVersion(std::string(31, 'a')));
    EXPECT_FALSE(AzureAead::IsValidVersion(std::string(33, 'a')));
}

TEST(AzureKmsAeadTest, IsValidVersionRejectsNonHexCharacters) {
    std::string nonHex = "g" + kVersionA.substr(1);
    EXPECT_FALSE(AzureAead::IsValidVersion(nonHex));
}

TEST(AzureKmsAeadTest, ExtractVersionReturnsEmbeddedVersionForPrefixedCiphertext) {
    std::string ciphertext = "azure:v1:" + kVersionA + ":wrapped-bytes";
    EXPECT_EQ(kVersionA, AzureAead::ExtractVersion(ciphertext));
}

TEST(AzureKmsAeadTest, ExtractVersionReturnsEmptyForLegacyUnprefixedCiphertext) {
    EXPECT_EQ("", AzureAead::ExtractVersion("legacy-unprefixed-ciphertext"));
}

TEST(AzureKmsAeadTest, ExtractVersionReturnsEmptyWhenTooShort) {
    EXPECT_EQ("", AzureAead::ExtractVersion("azure:v1:short"));
}

TEST(AzureKmsAeadTest, ExtractVersionReturnsEmptyWhenMissingColonSeparator) {
    // 32 characters after the prefix but no trailing ':' before the payload.
    std::string ciphertext = "azure:v1:" + kVersionA + "Xwrapped-bytes";
    EXPECT_EQ("", AzureAead::ExtractVersion(ciphertext));
}
