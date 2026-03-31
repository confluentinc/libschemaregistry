/**
 * Azure UAMI OAuth Provider Implementation
 */

#include "schemaregistry/rest/UamiOAuthProvider.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <stdexcept>
#include <string>

using json = nlohmann::json;

namespace schemaregistry::rest {

// ============================================================================
// UamiOAuthProvider::Config Implementation
// ============================================================================

void UamiOAuthProvider::Config::validate() const {
  if (uami_endpoint_query.empty()) {
    throw std::invalid_argument("uami_endpoint_query is required");
  }
  CacheConfig::validate();
}

// ============================================================================
// UamiOAuthProvider Implementation
// ============================================================================

UamiOAuthProvider::UamiOAuthProvider(const Config& config)
    : CachingOAuthProvider(config), config_(config) {
  config_.validate();
}

OAuthToken UamiOAuthProvider::fetch_token() {
  std::string url = config_.uami_url + config_.uami_endpoint_query;

  std::string response_text = fetch_with_retry(
      "UAMI",
      [&]() -> TokenResponse {
        auto r = cpr::Get(
            cpr::Url{url},
            cpr::Header{{"Metadata", "true"}},
            cpr::Timeout{std::chrono::seconds(cache_config_.http_timeout_seconds)});
        return {static_cast<int>(r.status_code), r.text};
      });

  auto json_response = json::parse(response_text);

  if (!json_response.contains("access_token")) {
    throw std::runtime_error(
        "UAMI token response missing 'access_token' field");
  }

  OAuthToken token;
  token.access_token = json_response["access_token"].get<std::string>();
  // Azure IMDS returns expires_in as a string (e.g., "3599")
  if (json_response.contains("expires_in")) {
    auto& val = json_response["expires_in"];
    if (val.is_string()) {
      token.expires_in_seconds = std::stoi(val.get<std::string>());
    } else {
      token.expires_in_seconds = val.get<int>();
    }
  } else {
    token.expires_in_seconds = 3600;
  }
  token.expires_at = std::chrono::system_clock::now() +
                     std::chrono::seconds(token.expires_in_seconds);
  return token;
}

}  // namespace schemaregistry::rest
