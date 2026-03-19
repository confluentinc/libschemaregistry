/**
 * OAuth 2.0 Base Provider Implementation
 */

#include "schemaregistry/rest/OAuthProvider.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "schemaregistry/rest/BackoffUtils.h"

namespace schemaregistry::rest {

namespace {

std::string join_with_commas(const std::vector<std::string>& values) {
  if (values.empty()) return "";

  size_t total_size = 0;
  for (const auto& s : values) total_size += s.size();
  total_size += values.size() - 1;

  std::string joined;
  joined.reserve(total_size);

  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) joined += ",";
    joined += values[i];
  }
  return joined;
}

}  // namespace

// ============================================================================
// CachingOAuthProvider::CacheConfig Implementation
// ============================================================================

void CachingOAuthProvider::CacheConfig::validate() const {
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

void CachingOAuthProvider::CacheConfig::set_identity_pool_ids(
    const std::vector<std::string>& pool_ids) {
  identity_pool_id = join_with_commas(pool_ids);
}

// ============================================================================
// CachingOAuthProvider Implementation
// ============================================================================

CachingOAuthProvider::CachingOAuthProvider(const CacheConfig& cache_config)
    : cache_config_(cache_config) {}

BearerFields CachingOAuthProvider::get_bearer_fields() {
  {
    std::shared_lock<std::shared_mutex> read_lock(mutex_);
    if (token_.is_valid() &&
        !token_.is_expired(cache_config_.token_refresh_threshold)) {
      return BearerFields{token_.access_token, cache_config_.logical_cluster,
                          cache_config_.identity_pool_id};
    }
  }

  std::unique_lock<std::shared_mutex> write_lock(mutex_);
  if (token_.is_valid() &&
      !token_.is_expired(cache_config_.token_refresh_threshold)) {
    return BearerFields{token_.access_token, cache_config_.logical_cluster,
                        cache_config_.identity_pool_id};
  }

  OAuthToken new_token = fetch_token();
  token_ = std::move(new_token);

  return BearerFields{token_.access_token, cache_config_.logical_cluster,
                      cache_config_.identity_pool_id};
}

void CachingOAuthProvider::invalidate_token() {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  token_ = OAuthToken{};
}

std::string CachingOAuthProvider::fetch_with_retry(
    const std::string& provider_name,
    const std::function<TokenResponse()>& make_request) {
  int attempt = 0;
  while (true) {
    auto response = make_request();

    if (response.status_code >= 200 && response.status_code < 300) {
      return response.text;
    }

    bool should_retry = false;
    if (response.status_code == 0) {
      should_retry = true;
    } else if (response.status_code >= 400) {
      should_retry = utils::isRetriable(response.status_code);
    }

    if (!should_retry || attempt >= cache_config_.max_retries) {
      std::ostringstream error_msg;
      error_msg << provider_name << " token request failed with status "
                << response.status_code;
      if (!response.text.empty()) {
        error_msg << ": " << response.text;
      }
      throw std::runtime_error(error_msg.str());
    }

    auto delay = utils::calculateExponentialBackoff(
        cache_config_.retry_base_delay_ms, attempt,
        std::chrono::milliseconds(cache_config_.retry_max_delay_ms));
    std::this_thread::sleep_for(delay);
    attempt++;
  }
}

OAuthToken CachingOAuthProvider::parse_standard_token_response(
    const std::string& json_text, const std::string& provider_name) {
  using json = nlohmann::json;
  auto json_response = json::parse(json_text);

  if (!json_response.contains("access_token")) {
    throw std::runtime_error(
        provider_name + " token response missing 'access_token' field");
  }

  OAuthToken token;
  token.access_token = json_response["access_token"].get<std::string>();
  token.expires_in_seconds = json_response.value("expires_in", 3600);
  token.expires_at = std::chrono::system_clock::now() +
                     std::chrono::seconds(token.expires_in_seconds);
  return token;
}

}  // namespace schemaregistry::rest
