/**
 * OAuth 2.0 Provider Implementation
 */

#include "schemaregistry/rest/OAuthProvider.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>

using json = nlohmann::json;

namespace schemaregistry::rest {

// ============================================================================
// StaticTokenProvider Implementation
// ============================================================================

StaticTokenProvider::StaticTokenProvider(std::string token,
                                         std::string logical_cluster,
                                         std::string identity_pool_id)
    : fields_(std::move(token), std::move(logical_cluster),
              std::move(identity_pool_id)) {
  if (fields_.access_token.empty()) {
    throw std::invalid_argument("access_token cannot be empty");
  }
  if (fields_.logical_cluster.empty()) {
    throw std::invalid_argument("logical_cluster cannot be empty");
  }
  if (fields_.identity_pool_id.empty()) {
    throw std::invalid_argument("identity_pool_id cannot be empty");
  }
}

BearerFields StaticTokenProvider::get_bearer_fields() { return fields_; }

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
  if (logical_cluster.empty()) {
    throw std::invalid_argument("logical_cluster is required");
  }
  if (identity_pool_id.empty()) {
    throw std::invalid_argument("identity_pool_id is required");
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
  std::lock_guard<std::mutex> lock(mutex_);

  // Check if we need to fetch/refresh token
  if (!token_.is_valid() ||
      token_.is_expired(config_.token_refresh_threshold)) {
    fetch_token();
  }

  return BearerFields{token_.access_token, config_.logical_cluster,
                      config_.identity_pool_id};
}

void OAuthClientProvider::invalidate_token() {
  std::lock_guard<std::mutex> lock(mutex_);
  token_ = OAuthToken{};
}

void OAuthClientProvider::fetch_token() {
  // Build form-encoded body for OAuth client credentials grant
  cpr::Payload payload{{"grant_type", "client_credentials"},
                       {"client_id", config_.client_id},
                       {"client_secret", config_.client_secret},
                       {"scope", config_.scope}};

  // Retry with exponential backoff
  for (int attempt = 0; attempt <= config_.max_retries; ++attempt) {
    try {
      auto response = cpr::Post(
          cpr::Url{config_.token_endpoint_url}, payload,
          cpr::Header{{"Content-Type", "application/x-www-form-urlencoded"}},
          cpr::Timeout{std::chrono::seconds(config_.http_timeout_seconds)});

      // Check for success
      if (response.status_code >= 200 && response.status_code < 300) {
        // Parse JSON response
        auto json_response = json::parse(response.text);

        if (!json_response.contains("access_token")) {
          throw std::runtime_error(
              "OAuth token response missing 'access_token' field");
        }

        token_.access_token = json_response["access_token"].get<std::string>();

        // Parse expires_in (optional, default to 3600 seconds = 1 hour)
        if (json_response.contains("expires_in")) {
          token_.expires_in_seconds = json_response["expires_in"].get<int>();
        } else {
          token_.expires_in_seconds = 3600;  // Default 1 hour
        }

        // Calculate expiry time
        token_.expires_at = std::chrono::system_clock::now() +
                            std::chrono::seconds(token_.expires_in_seconds);

        return;  // Success!
      }

      // Non-2xx response
      std::ostringstream error_msg;
      error_msg << "OAuth token request failed with status "
                << response.status_code;
      if (!response.text.empty()) {
        error_msg << ": " << response.text;
      }

      // Last attempt, throw error
      if (attempt >= config_.max_retries) {
        throw std::runtime_error(error_msg.str());
      }

      // Retriable error, apply backoff
      int delay_ms = calculate_backoff_delay(attempt);
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

    } catch (const json::exception& e) {
      std::ostringstream error_msg;
      error_msg << "Failed to parse OAuth token response: " << e.what();
      if (attempt >= config_.max_retries) {
        throw std::runtime_error(error_msg.str());
      }

      int delay_ms = calculate_backoff_delay(attempt);
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

    } catch (const std::exception& e) {
      // Network error or other exception
      if (attempt >= config_.max_retries) {
        std::ostringstream error_msg;
        error_msg << "Failed to fetch OAuth token after " << config_.max_retries
                  << " attempts: " << e.what();
        throw std::runtime_error(error_msg.str());
      }

      int delay_ms = calculate_backoff_delay(attempt);
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
  }
}

int OAuthClientProvider::calculate_backoff_delay(int attempt) const {
  // Exponential backoff: base * (2^attempt)
  int exponential_delay = config_.retry_base_delay_ms * (1 << attempt);
  int capped_delay = std::min(exponential_delay, config_.retry_max_delay_ms);

  // Apply full jitter to prevent thundering herd
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, capped_delay);

  return dis(gen);
}


// ============================================================================
// OAuthProviderFactory Implementation
// ============================================================================

std::shared_ptr<OAuthProvider> OAuthProviderFactory::create(
    const std::map<std::string, std::string>& config) {
  std::string source =
      get_required_config(config, "bearer.auth.credentials.source");

  if (source == "STATIC_TOKEN") {
    return create_static_token_provider(config);
  } else if (source == "OAUTHBEARER") {
    return create_oauth_provider(config);
  } else {
    throw std::invalid_argument(
        "Invalid bearer.auth.credentials.source: " + source +
        ". Must be STATIC_TOKEN or OAUTHBEARER");
  }
}

std::shared_ptr<OAuthProvider>
OAuthProviderFactory::create_static_token_provider(
    const std::map<std::string, std::string>& config) {
  std::string token = get_required_config(config, "bearer.auth.token");
  std::string logical_cluster =
      get_required_config(config, "bearer.auth.logical.cluster");
  std::string identity_pool_id =
      get_required_config(config, "bearer.auth.identity.pool.id");

  return std::make_shared<StaticTokenProvider>(
      std::move(token), std::move(logical_cluster),
      std::move(identity_pool_id));
}

std::shared_ptr<OAuthProvider> OAuthProviderFactory::create_oauth_provider(
    const std::map<std::string, std::string>& config) {
  OAuthClientProvider::Config oauth_config;

  oauth_config.client_id = get_required_config(config, "bearer.auth.client.id");
  oauth_config.client_secret =
      get_required_config(config, "bearer.auth.client.secret");
  oauth_config.scope = get_required_config(config, "bearer.auth.scope");
  oauth_config.token_endpoint_url =
      get_required_config(config, "bearer.auth.issuer.endpoint.url");
  oauth_config.logical_cluster =
      get_required_config(config, "bearer.auth.logical.cluster");
  oauth_config.identity_pool_id =
      get_required_config(config, "bearer.auth.identity.pool.id");

  return std::make_shared<OAuthClientProvider>(oauth_config);
}


std::string OAuthProviderFactory::get_required_config(
    const std::map<std::string, std::string>& config, const std::string& key) {
  auto it = config.find(key);
  if (it == config.end()) {
    throw std::invalid_argument("Missing required configuration key: " + key);
  }
  if (it->second.empty()) {
    throw std::invalid_argument("Configuration key cannot be empty: " + key);
  }
  return it->second;
}

std::string OAuthProviderFactory::get_optional_config(
    const std::map<std::string, std::string>& config, const std::string& key,
    const std::string& default_value) {
  auto it = config.find(key);
  return (it != config.end()) ? it->second : default_value;
}

}  // namespace schemaregistry::rest
