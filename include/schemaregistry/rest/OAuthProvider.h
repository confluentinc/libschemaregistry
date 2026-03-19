/**
 * OAuth 2.0 Provider for Schema Registry Client
 *
 * Base classes for OAuth bearer authentication. For concrete providers see:
 * - OAuthClientProvider.h (Client Credentials grant)
 * - CustomOAuthProvider.h (user-provided token function)
 * - UamiOAuthProvider.h (Azure UAMI via IMDS)
 *
 * Authentication Methods (Mutually Exclusive):
 * ClientConfiguration supports three authentication methods:
 * - Basic Auth (API Key/Secret)
 * - OAuth Provider
 * - Static Bearer Token
 *
 * Setting any authentication method automatically clears the others.
 * Only one authentication method can be active at a time.
 */

#pragma once

#include <chrono>
#include <functional>
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
 * Base class for OAuth providers with automatic token caching and refresh.
 *
 * Provides thread-safe token caching using double-checked locking with
 * shared_mutex. Subclasses implement fetch_token() for the actual token
 * retrieval (HTTP call + parsing + retry).
 */
class CachingOAuthProvider : public OAuthProvider {
 public:
  struct CacheConfig {
    std::string logical_cluster;
    std::string identity_pool_id;

    int max_retries{3};
    int retry_base_delay_ms{1000};
    int retry_max_delay_ms{20000};
    double token_refresh_threshold{0.8};
    int http_timeout_seconds{30};

    void validate() const;
  };

  BearerFields get_bearer_fields() override;

  /**
   * Force token refresh on next access.
   */
  void invalidate_token();

 protected:
  explicit CachingOAuthProvider(const CacheConfig& cache_config);

  /**
   * Fetch a new token from the token endpoint.
   *
   * Subclasses implement the HTTP request and response parsing.
   *
   * @return Newly fetched OAuthToken
   * @throws std::runtime_error if unable to fetch token after retries
   */
  virtual OAuthToken fetch_token() = 0;

  /**
   * Token response from an HTTP request, used by fetch_with_retry.
   */
  struct TokenResponse {
    int status_code{0};
    std::string text;
  };

  /**
   * Execute an HTTP request with retry and exponential backoff.
   *
   * Retries on network errors and retriable HTTP status codes.
   * Returns the successful response body for the caller to parse.
   *
   * @param provider_name Name for error messages (e.g., "OAuth", "UAMI")
   * @param make_request Function that performs the HTTP request
   * @return Response body text on success (2xx)
   * @throws std::runtime_error if unable to get a successful response after retries
   */
  std::string fetch_with_retry(
      const std::string& provider_name,
      const std::function<TokenResponse()>& make_request);

  /**
   * Parse a standard OAuth token response (access_token string, expires_in int).
   *
   * @param json_text Raw JSON response body
   * @param provider_name Name for error messages
   * @return Parsed OAuthToken
   */
  static OAuthToken parse_standard_token_response(
      const std::string& json_text, const std::string& provider_name);

  const CacheConfig cache_config_;

 private:
  mutable std::shared_mutex mutex_;
  OAuthToken token_;
};

}  // namespace schemaregistry::rest
