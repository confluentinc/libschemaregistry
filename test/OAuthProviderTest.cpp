/**
 * OAuthProviderTest
 * Tests for OAuth 2.0 provider implementations
 */

#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include "schemaregistry/rest/OAuthProvider.h"

using namespace schemaregistry::rest;

// =============================================================================
// StaticTokenProvider Tests
// =============================================================================

TEST(StaticTokenProviderTest, ReturnsCorrectFields) {
  auto provider = std::make_shared<StaticTokenProvider>(
      "test-token", "lsrc-123", "pool-456");

  auto fields = provider->get_bearer_fields();

  EXPECT_EQ(fields.access_token, "test-token");
  EXPECT_EQ(fields.logical_cluster, "lsrc-123");
  EXPECT_EQ(fields.identity_pool_id, "pool-456");
}

TEST(StaticTokenProviderTest, GetAccessTokenConvenience) {
  auto provider = std::make_shared<StaticTokenProvider>(
      "test-token", "lsrc-123", "pool-456");

  EXPECT_EQ(provider->get_access_token(), "test-token");
}

TEST(StaticTokenProviderTest, ThrowsOnEmptyToken) {
  EXPECT_THROW(
      StaticTokenProvider("", "lsrc-123", "pool-456"),
      std::invalid_argument);
}

TEST(StaticTokenProviderTest, ThrowsOnEmptyCluster) {
  EXPECT_THROW(
      StaticTokenProvider("token", "", "pool-456"),
      std::invalid_argument);
}

TEST(StaticTokenProviderTest, ThrowsOnEmptyPoolId) {
  EXPECT_THROW(
      StaticTokenProvider("token", "lsrc-123", ""),
      std::invalid_argument);
}

// =============================================================================
// OAuthToken Tests
// =============================================================================

TEST(OAuthTokenTest, DefaultConstructor) {
  OAuthToken token;
  EXPECT_FALSE(token.is_valid());
  EXPECT_TRUE(token.is_expired());
}

TEST(OAuthTokenTest, IsValidWithToken) {
  OAuthToken token;
  token.access_token = "test-token";
  EXPECT_TRUE(token.is_valid());
}

TEST(OAuthTokenTest, IsExpiredAtThreshold) {
  OAuthToken token;
  token.access_token = "test-token";
  token.expires_in_seconds = 3600;  // 1 hour

  // Set expiry well above threshold (20% remaining = 720 seconds)
  // Use 800 seconds to be safely above the threshold
  auto now = std::chrono::system_clock::now();
  token.expires_at = now + std::chrono::seconds(800);

  // With 800 seconds remaining, token should NOT be expired yet
  EXPECT_FALSE(token.is_expired(0.8));

  // Set expiry well below threshold - use 600 seconds (well past threshold)
  // This gives plenty of margin for test execution time
  now = std::chrono::system_clock::now();
  token.expires_at = now + std::chrono::seconds(600);

  // With only 600 seconds remaining (< 720 threshold), should be expired
  EXPECT_TRUE(token.is_expired(0.8));
}

TEST(OAuthTokenTest, IsNotExpiredBeforeThreshold) {
  OAuthToken token;
  token.access_token = "test-token";
  token.expires_in_seconds = 3600;  // 1 hour

  // Set expiry to 50% remaining (only 50% elapsed, before 80% threshold)
  auto now = std::chrono::system_clock::now();
  token.expires_at = now + std::chrono::seconds(1800);  // 30 minutes remaining

  // At 80% threshold, this token has only 50% elapsed, should NOT be expired yet
  EXPECT_FALSE(token.is_expired(0.8));
}

// =============================================================================
// OAuthClientProvider::Config Tests
// =============================================================================

TEST(OAuthClientConfigTest, ValidConfigPasses) {
  OAuthClientProvider::Config config;
  config.client_id = "client-id";
  config.client_secret = "client-secret";
  config.scope = "schema_registry";
  config.token_endpoint_url = "https://idp.example.com/token";
  config.logical_cluster = "lsrc-123";
  config.identity_pool_id = "pool-456";

  EXPECT_NO_THROW(config.validate());
}

TEST(OAuthClientConfigTest, ThrowsOnMissingClientId) {
  OAuthClientProvider::Config config;
  // client_id is empty
  config.client_secret = "client-secret";
  config.scope = "schema_registry";
  config.token_endpoint_url = "https://idp.example.com/token";
  config.logical_cluster = "lsrc-123";
  config.identity_pool_id = "pool-456";

  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(OAuthClientConfigTest, ThrowsOnMissingClientSecret) {
  OAuthClientProvider::Config config;
  config.client_id = "client-id";
  // client_secret is empty
  config.scope = "schema_registry";
  config.token_endpoint_url = "https://idp.example.com/token";
  config.logical_cluster = "lsrc-123";
  config.identity_pool_id = "pool-456";

  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(OAuthClientConfigTest, ThrowsOnNegativeRetries) {
  OAuthClientProvider::Config config;
  config.client_id = "client-id";
  config.client_secret = "client-secret";
  config.scope = "schema_registry";
  config.token_endpoint_url = "https://idp.example.com/token";
  config.logical_cluster = "lsrc-123";
  config.identity_pool_id = "pool-456";
  config.max_retries = -1;  // Invalid

  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(OAuthClientConfigTest, ThrowsOnInvalidThreshold) {
  OAuthClientProvider::Config config;
  config.client_id = "client-id";
  config.client_secret = "client-secret";
  config.scope = "schema_registry";
  config.token_endpoint_url = "https://idp.example.com/token";
  config.logical_cluster = "lsrc-123";
  config.identity_pool_id = "pool-456";
  config.token_refresh_threshold = 1.1;  // Invalid (represents percentage)

  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(OAuthClientConfigTest, ThrowsOnBaseDelayExceedsMaxDelay) {
  OAuthClientProvider::Config config;
  config.client_id = "client-id";
  config.client_secret = "client-secret";
  config.scope = "schema_registry";
  config.token_endpoint_url = "https://idp.example.com/token";
  config.logical_cluster = "lsrc-123";
  config.identity_pool_id = "pool-456";
  config.retry_base_delay_ms = 5000;  // Base delay
  config.retry_max_delay_ms = 1000;   // Max delay less than base (invalid)

  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(OAuthClientConfigTest, DefaultValuesMatch) {
  OAuthClientProvider::Config config;

  // Check defaults match Python's values
  EXPECT_EQ(config.max_retries, 3);
  EXPECT_EQ(config.retry_base_delay_ms, 1000);
  EXPECT_EQ(config.retry_max_delay_ms, 20000);
  EXPECT_DOUBLE_EQ(config.token_refresh_threshold, 0.8);
  EXPECT_EQ(config.http_timeout_seconds, 30);
}

// =============================================================================
// OAuthProviderFactory Tests
// =============================================================================

TEST(OAuthProviderFactoryTest, CreatesStaticTokenProvider) {
  std::map<std::string, std::string> config = {
      {"bearer.auth.credentials.source", "STATIC_TOKEN"},
      {"bearer.auth.token", "test-token"},
      {"bearer.auth.logical.cluster", "lsrc-123"},
      {"bearer.auth.identity.pool.id", "pool-456"}
  };

  auto provider = OAuthProviderFactory::create(config);
  ASSERT_NE(provider, nullptr);

  auto fields = provider->get_bearer_fields();
  EXPECT_EQ(fields.access_token, "test-token");
  EXPECT_EQ(fields.logical_cluster, "lsrc-123");
  EXPECT_EQ(fields.identity_pool_id, "pool-456");
}

TEST(OAuthProviderFactoryTest, CreatesOAuthClientProvider) {
  std::map<std::string, std::string> config = {
      {"bearer.auth.credentials.source", "OAUTHBEARER"},
      {"bearer.auth.client.id", "client-id"},
      {"bearer.auth.client.secret", "client-secret"},
      {"bearer.auth.scope", "schema_registry"},
      {"bearer.auth.issuer.endpoint.url", "https://idp.example.com/token"},
      {"bearer.auth.logical.cluster", "lsrc-123"},
      {"bearer.auth.identity.pool.id", "pool-456"}
  };

  auto provider = OAuthProviderFactory::create(config);
  ASSERT_NE(provider, nullptr);

  // Can't test much without actual OAuth server, but creation should succeed
}

TEST(OAuthProviderFactoryTest, ThrowsOnMissingSource) {
  std::map<std::string, std::string> config = {
      {"bearer.auth.token", "test-token"}
  };

  EXPECT_THROW(OAuthProviderFactory::create(config), std::invalid_argument);
}

TEST(OAuthProviderFactoryTest, ThrowsOnInvalidSource) {
  std::map<std::string, std::string> config = {
      {"bearer.auth.credentials.source", "INVALID_SOURCE"}
  };

  EXPECT_THROW(OAuthProviderFactory::create(config), std::invalid_argument);
}

TEST(OAuthProviderFactoryTest, ThrowsOnMissingStaticToken) {
  std::map<std::string, std::string> config = {
      {"bearer.auth.credentials.source", "STATIC_TOKEN"},
      // Missing bearer.auth.token
      {"bearer.auth.logical.cluster", "lsrc-123"},
      {"bearer.auth.identity.pool.id", "pool-456"}
  };

  EXPECT_THROW(OAuthProviderFactory::create(config), std::invalid_argument);
}

TEST(OAuthProviderFactoryTest, ThrowsOnMissingOAuthClientId) {
  std::map<std::string, std::string> config = {
      {"bearer.auth.credentials.source", "OAUTHBEARER"},
      // Missing bearer.auth.client.id
      {"bearer.auth.client.secret", "client-secret"},
      {"bearer.auth.scope", "schema_registry"},
      {"bearer.auth.issuer.endpoint.url", "https://idp.example.com/token"},
      {"bearer.auth.logical.cluster", "lsrc-123"},
      {"bearer.auth.identity.pool.id", "pool-456"}
  };

  EXPECT_THROW(OAuthProviderFactory::create(config), std::invalid_argument);
}

// =============================================================================
// BearerFields Tests
// =============================================================================

TEST(BearerFieldsTest, DefaultConstructor) {
  BearerFields fields;
  EXPECT_TRUE(fields.access_token.empty());
  EXPECT_TRUE(fields.logical_cluster.empty());
  EXPECT_TRUE(fields.identity_pool_id.empty());
}

TEST(BearerFieldsTest, ParameterizedConstructor) {
  BearerFields fields("test-token", "lsrc-123", "pool-456");
  EXPECT_EQ(fields.access_token, "test-token");
  EXPECT_EQ(fields.logical_cluster, "lsrc-123");
  EXPECT_EQ(fields.identity_pool_id, "pool-456");
}
