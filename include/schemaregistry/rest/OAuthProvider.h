/**
 * OAuth 2.0 Provider for Schema Registry Client
 *
 * Implements OAuth 2.0 Client Credentials grant (RFC 6749 Section 4.4)
 * with automatic token caching and refresh.
 *
 * Authentication Methods (Mutually Exclusive):
 * ClientConfiguration supports three authentication methods:
 * - Basic Auth (API Key/Secret)
 * - OAuth Provider (this)
 * - Static Bearer Token (legacy)
 *
 * Setting any authentication method automatically clears the others.
 * Only one authentication method can be active at a time.
 */

#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>

namespace schemaregistry::rest {

/**
 * Bearer authentication fields required for Confluent Cloud Schema Registry.
 *
 * Confluent Cloud requires three fields:
 * - access_token: OAuth bearer token
 * - logical_cluster: Schema Registry logical cluster ID (e.g., "lsrc-12345"). Required for Confluent Cloud
 * - identity_pool_id: Identity pool ID (e.g., "pool-abcd"). Required for Confluent Cloud
 */
struct BearerFields {
  std::string access_token;
  std::string logical_cluster;
  std::string identity_pool_id;

  BearerFields() = default;
  BearerFields(std::string token, std::string cluster, std::string pool)
      : access_token(std::move(token)),
        logical_cluster(std::move(cluster)),
        identity_pool_id(std::move(pool)) {}
};

/**
 * Abstract base class for OAuth bearer authentication providers.
 */
class OAuthProvider {
 public:
  virtual ~OAuthProvider() = default;

  /**
   * Get bearer authentication fields for Schema Registry request.
   *
   * This method must be thread-safe. It may block to fetch/refresh tokens.
   *
   * @return BearerFields containing access token and cluster identifiers
   * @throws std::runtime_error if unable to obtain credentials
   */
  virtual BearerFields get_bearer_fields() = 0;

  /**
   * Get only the access token (convenience method).
   *
   * @return Access token string
   * @throws std::runtime_error if unable to obtain token
   */
  virtual std::string get_access_token() {
    return get_bearer_fields().access_token;
  }
};

/**
 * Static token provider. Uses a pre-obtained bearer token.
 *
 */
class StaticTokenProvider : public OAuthProvider {
 public:
  /**
   * Construct static token provider.
   *
   * @param token Bearer access token (required)
   * @param logical_cluster Schema Registry logical cluster ID (optional, required for Confluent Cloud)
   * @param identity_pool_id Identity pool ID (optional, required for Confluent Cloud)
   */
  StaticTokenProvider(std::string token, std::string logical_cluster = "",
                      std::string identity_pool_id = "");

  BearerFields get_bearer_fields() override;

 private:
  const BearerFields fields_;
};

/**
 * OAuth token with expiry tracking.
 */
struct OAuthToken {
  std::string access_token;
  std::chrono::system_clock::time_point expires_at; // Token expiry time
  int expires_in_seconds{0}; // Token lifetime in seconds

  OAuthToken() = default;

  bool is_valid() const { return !access_token.empty(); }

  bool is_expired(double threshold = 0.8) const {
    if (!is_valid()) return true;

    auto now = std::chrono::system_clock::now();
    // refresh buffer: remaining time before the actual expiry timestamp of the token
    // e.g. with threshold=0.8, we refresh when 80% of the lifetime has elapsed,
    // leaving 20% (0.2 * expires_in_seconds) as the refresh buffer.
    auto refresh_buffer = std::chrono::seconds(static_cast<int>(expires_in_seconds * (1 - threshold)));

    return (expires_at - refresh_buffer) < now;
  }
};

/**
 * OAuth 2.0 Client Credentials provider.
 *
 * Implements automatic token fetching using OAuth 2.0 Client Credentials
 * grant flow.
 */
class OAuthClientProvider : public OAuthProvider {
 public:
  /**
   * Configuration for OAuth Client Credentials flow.
   */
  struct Config {
    // Required OAuth parameters
    std::string client_id;
    std::string client_secret;
    std::string scope;
    std::string token_endpoint_url;

    // Optional Confluent Cloud parameters
    // Required for Confluent Cloud
    std::string logical_cluster;      // Schema Registry logical cluster ID (e.g., "lsrc-12345")
    std::string identity_pool_id;     // Identity pool ID (e.g., "pool-abcd")

    // Optional retry configuration
    int max_retries{3};
    int retry_base_delay_ms{1000};
    int retry_max_delay_ms{20000};

    // Optional token refresh behavior
    // Token is refreshed when elapsed time > threshold * total_lifetime
    // Default: 0.8 (refreshes when 80% of token lifetime has elapsed)
    double token_refresh_threshold{0.8};

    // Optional HTTP timeout
    int http_timeout_seconds{30};

    /**
     * Validate configuration has all required fields.
     *
     * @throws std::invalid_argument if configuration is invalid
     */
    void validate() const;
  };

  /**
   * Construct OAuth provider with configuration.
   *
   * @param config OAuth configuration (validated on construction)
   * @throws std::invalid_argument if configuration is invalid
   */
  explicit OAuthClientProvider(const Config& config);

  BearerFields get_bearer_fields() override;

  /**
   * Force token refresh on next access.
   */
  void invalidate_token();

 private:
  /**
   * Fetch new token from OAuth provider using Client Credentials grant.
   *
   * Performs exponential backoff with jitter on failures.
   *
   * @return Newly fetched OAuth token
   * @throws std::runtime_error if unable to fetch token after retries
   */
  OAuthToken fetch_token();

  const Config config_;

  mutable std::shared_mutex mutex_;  // Shared mutex for thread-safe token access
  OAuthToken token_;
};


/**
 * Factory for creating OAuth providers from configuration strings.
 *
 * Supports creating providers from key-value configuration maps similar
 * to other Confluent clients (Java, Python).
 *
 * Supported authentication methods:
 * - STATIC_TOKEN: Pre-obtained bearer token
 * - OAUTHBEARER: OAuth 2.0 Client Credentials flow
 */
class OAuthProviderFactory {
 public:
  /**
   * Create OAuth provider from configuration map.
   *
   * Required keys:
   * - bearer.auth.credentials.source: STATIC_TOKEN or OAUTHBEARER
   * Required keys for Confluent Cloud:
   * - bearer.auth.logical.cluster
   * - bearer.auth.identity.pool.id
   *
   * Additional required keys based on method:
   * For STATIC_TOKEN:
   * - bearer.auth.token (pre-obtained)
   * For OAUTHBEARER:
   * - bearer.auth.client.id
   * - bearer.auth.client.secret
   * - bearer.auth.scope
   * - bearer.auth.issuer.endpoint.url
   *
   * Optional keys
   *
   * @param config Configuration map
   * @return Shared pointer to OAuth provider
   * @throws std::invalid_argument if configuration is invalid or missing required keys
   */
  static std::shared_ptr<OAuthProvider> create(
      const std::map<std::string, std::string>& config);

 private:
  static std::shared_ptr<OAuthProvider> create_static_token_provider(
      const std::map<std::string, std::string>& config);

  static std::shared_ptr<OAuthProvider> create_oauth_provider(
      const std::map<std::string, std::string>& config);

  static std::string get_required_config(
      const std::map<std::string, std::string>& config, const std::string& key);
};

}  // namespace schemaregistry::rest
