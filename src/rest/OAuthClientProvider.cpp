/**
 * OAuth 2.0 Client Credentials Provider Implementation
 */

#include "schemaregistry/rest/OAuthClientProvider.h"

#include <cpr/cpr.h>

#include <chrono>
#include <stdexcept>

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
  CacheConfig::validate();
}

// ============================================================================
// OAuthClientProvider Implementation
// ============================================================================

OAuthClientProvider::OAuthClientProvider(const Config& config)
    : CachingOAuthProvider(config), config_(config) {
  config_.validate();
}

OAuthToken OAuthClientProvider::fetch_token() {
  cpr::Payload payload{{"grant_type", "client_credentials"},
                       {"client_id", config_.client_id},
                       {"client_secret", config_.client_secret},
                       {"scope", config_.scope}};

  std::string response_text = fetch_with_retry(
      "OAuth",
      [&]() -> TokenResponse {
        auto r = cpr::Post(
            cpr::Url{config_.token_endpoint_url}, payload,
            cpr::Header{{"Content-Type", "application/x-www-form-urlencoded"}},
            cpr::Timeout{std::chrono::seconds(cache_config_.http_timeout_seconds)});
        return {static_cast<int>(r.status_code), r.text};
      });

  return parse_standard_token_response(response_text, "OAuth");
}

}  // namespace schemaregistry::rest
