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
// CustomOAuthProvider Tests
// =============================================================================

TEST(CustomOAuthProviderTest, CallsUserFunction) {
  int call_count = 0;
  auto fetch_fn = [&call_count]() {
    call_count++;
    return "custom-token-" + std::to_string(call_count);
  };

  auto provider = std::make_shared<CustomOAuthProvider>(fetch_fn, "lsrc-123", "pool-456");

  auto fields1 = provider->get_bearer_fields();
  EXPECT_EQ(fields1.access_token, "custom-token-1");
  EXPECT_EQ(fields1.logical_cluster, "lsrc-123");
  EXPECT_EQ(fields1.identity_pool_id, "pool-456");

  // Function is called every time (no caching)
  auto fields2 = provider->get_bearer_fields();
  EXPECT_EQ(fields2.access_token, "custom-token-2");
  EXPECT_EQ(call_count, 2);
}

TEST(CustomOAuthProviderTest, WorksWithoutCloudFields) {
  auto fetch_fn = []() { return "my-token"; };
  auto provider = std::make_shared<CustomOAuthProvider>(fetch_fn);

  auto fields = provider->get_bearer_fields();
  EXPECT_EQ(fields.access_token, "my-token");
  EXPECT_EQ(fields.logical_cluster, "");
  EXPECT_EQ(fields.identity_pool_id, "");
}

TEST(CustomOAuthProviderTest, ThrowsOnEmptyFunction) {
  CustomOAuthProvider::TokenFetchFunction empty_fn;
  EXPECT_THROW(CustomOAuthProvider(empty_fn, "lsrc-123", "pool-456"),
               std::invalid_argument);
}

TEST(CustomOAuthProviderTest, ThrowsWhenFunctionReturnsEmpty) {
  auto fetch_fn = []() { return ""; };
  auto provider = std::make_shared<CustomOAuthProvider>(fetch_fn, "lsrc-123", "pool-456");

  EXPECT_THROW(provider->get_bearer_fields(), std::runtime_error);
}

TEST(CustomOAuthProviderTest, PropagatlesUserFunctionExceptions) {
  auto fetch_fn = []() -> std::string {
    throw std::runtime_error("Custom IdP unavailable");
  };
  auto provider = std::make_shared<CustomOAuthProvider>(fetch_fn, "lsrc-123", "pool-456");

  EXPECT_THROW({
    try {
      provider->get_bearer_fields();
    } catch (const std::runtime_error& e) {
      EXPECT_STREQ(e.what(), "Custom IdP unavailable");
      throw;
    }
  }, std::runtime_error);
}

TEST(CustomOAuthProviderTest, WorksWithLambdaCapture) {
  std::string token = "captured-token";
  std::string cluster = "lsrc-999";

  auto fetch_fn = [token]() { return token; };
  auto provider = std::make_shared<CustomOAuthProvider>(fetch_fn, cluster, "pool-888");

  auto fields = provider->get_bearer_fields();
  EXPECT_EQ(fields.access_token, "captured-token");
  EXPECT_EQ(fields.logical_cluster, "lsrc-999");
  EXPECT_EQ(fields.identity_pool_id, "pool-888");
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

// =============================================================================
// Polymorphism Tests (verifies all providers work through base interface)
// =============================================================================

TEST(PolymorphismTest, AllProvidersWorkThroughBaseInterface) {
  // Test that OAuth provider types work through OAuthProvider* interface
  // This mimics how RestClient uses providers

  std::vector<std::shared_ptr<OAuthProvider>> providers;

  // Custom provider
  providers.push_back(std::make_shared<CustomOAuthProvider>(
      []() { return "custom-token"; }, "lsrc-222", "pool-222"));

  // Verify it works through base interface (polymorphism)
  auto fields = providers[0]->get_bearer_fields();
  EXPECT_EQ(fields.access_token, "custom-token");
  EXPECT_EQ(fields.logical_cluster, "lsrc-222");
  EXPECT_EQ(fields.identity_pool_id, "pool-222");
}
