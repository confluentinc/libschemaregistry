/**
 * OAuth 2.0 Provider Implementation
 */

#include "schemaregistry/rest/OAuthProvider.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "schemaregistry/rest/BackoffUtils.h"

using json = nlohmann::json;

namespace schemaregistry::rest {

// ============================================================================
// OAuthClientProvider::Config Implementation
// ============================================================================

void OAuthClientProvider::Config::validate() const {
  if (client_id.empty()) {
    throw std::invalid_argument("client_id is required");
  }
  if (client_secret.empty()) {
    throw std::invalid_argument("client_secret is required");
  }
  if (scope.empty()) {
    throw std::invalid_argument("scope is required");
  }
  if (token_endpoint_url.empty()) {
    throw std::invalid_argument("token_endpoint_url is required");
  }
  if (max_retries < 0) {
    throw std::invalid_argument("max_retries must be non-negative");
  }
  if (retry_base_delay_ms <= 0) {
    throw std::invalid_argument("retry_base_delay_ms must be positive");
  }
  if (retry_max_delay_ms <= 0) {
    throw std::invalid_argument("retry_max_delay_ms must be positive");
  }
  if (retry_base_delay_ms > retry_max_delay_ms) {
    throw std::invalid_argument(
        "retry_base_delay_ms must not exceed retry_max_delay_ms");
  }
  if (token_refresh_threshold < 0.0 || token_refresh_threshold > 1.0) {
    throw std::invalid_argument(
        "token_refresh_threshold must be between 0.0 and 1.0");
  }
  if (http_timeout_seconds <= 0) {
    throw std::invalid_argument("http_timeout_seconds must be positive");
  }
}

// ============================================================================
// OAuthClientProvider Implementation
// ============================================================================

OAuthClientProvider::OAuthClientProvider(const Config& config)
    : config_(config) {
  config_.validate();
}

BearerFields OAuthClientProvider::get_bearer_fields() {
  // Check if existing lock is valid
  // Use shared lock for concurrent reads
  {
    std::shared_lock<std::shared_mutex> read_lock(mutex_);
    if (token_.is_valid() &&
        !token_.is_expired(config_.token_refresh_threshold)) {
      return BearerFields{token_.access_token, config_.logical_cluster,
                          config_.identity_pool_id};
    }
  }

  // Refresh token if expired or not present
  // Use exclusive lock to prevent multiple threads fetching new token simultaneously
  std::unique_lock<std::shared_mutex> write_lock(mutex_);
  if (token_.is_valid() &&
      !token_.is_expired(config_.token_refresh_threshold)) {
    return BearerFields{token_.access_token, config_.logical_cluster,
                        config_.identity_pool_id};
  }

  OAuthToken new_token = fetch_token();
  token_ = std::move(new_token);

  return BearerFields{token_.access_token, config_.logical_cluster,
                      config_.identity_pool_id};
}

void OAuthClientProvider::invalidate_token() {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  token_ = OAuthToken{};
}

OAuthToken OAuthClientProvider::fetch_token() {
  // Build form-encoded body for OAuth client credentials grant
  cpr::Payload payload{{"grant_type", "client_credentials"},
                       {"client_id", config_.client_id},
                       {"client_secret", config_.client_secret},
                       {"scope", config_.scope}};

  int attempt = 0;
  while (true) {
    auto response = cpr::Post(
        cpr::Url{config_.token_endpoint_url}, payload,
        cpr::Header{{"Content-Type", "application/x-www-form-urlencoded"}},
        cpr::Timeout{std::chrono::seconds(config_.http_timeout_seconds)});

    // Success: parse and return token
    if (response.status_code >= 200 && response.status_code < 300) {
      try {
        auto json_response = json::parse(response.text);

        if (!json_response.contains("access_token")) {
          throw std::runtime_error(
              "OAuth token response missing 'access_token' field");
        }

        OAuthToken token;
        token.access_token = json_response["access_token"].get<std::string>();
        token.expires_in_seconds = json_response.value("expires_in", 3600);
        token.expires_at = std::chrono::system_clock::now() +
                           std::chrono::seconds(token.expires_in_seconds);
        return token;
      } catch (const json::exception& e) {
        // JSON parsing error on 2xx - not retriable
        throw std::runtime_error(
            "Failed to parse OAuth token response: " + std::string(e.what()));
      }
    }

    // Check if we should retry
    bool should_retry = false;
    if (response.status_code == 0) {
      // Network error - always retriable
      should_retry = true;
    } else if (response.status_code >= 400) {
      should_retry = utils::isRetriable(response.status_code);
    }

    if (!should_retry || attempt >= config_.max_retries) {
      std::ostringstream error_msg;
      error_msg << "OAuth token request failed with status "
                << response.status_code;
      if (!response.text.empty()) {
        error_msg << ": " << response.text;
      }
      throw std::runtime_error(error_msg.str());
    }

    // Retry with backoff
    auto delay = utils::calculateExponentialBackoff(
        config_.retry_base_delay_ms, attempt,
        std::chrono::milliseconds(config_.retry_max_delay_ms));
    std::this_thread::sleep_for(delay);
    attempt++;
  }
}

// ============================================================================
// CustomOAuthProvider Implementation
// ============================================================================

CustomOAuthProvider::CustomOAuthProvider(TokenFetchFunction fetch_fn,
                                         std::string logical_cluster,
                                         std::string identity_pool_id)
    : fetch_fn_(std::move(fetch_fn)),
      logical_cluster_(std::move(logical_cluster)),
      identity_pool_id_(std::move(identity_pool_id)) {
  if (!fetch_fn_) {
    throw std::invalid_argument("fetch_fn cannot be empty");
  }
}

BearerFields CustomOAuthProvider::get_bearer_fields() {
  // Call the custom function to get access token
  std::string access_token = fetch_fn_();

  if (access_token.empty()) {
    throw std::runtime_error("Custom token fetch function returned empty token");
  }

  return BearerFields{std::move(access_token), logical_cluster_, identity_pool_id_};
}

}  // namespace schemaregistry::rest
