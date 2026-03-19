/**
 * OAuth 2.0 Client Credentials Provider
 */

#pragma once

#include "schemaregistry/rest/OAuthProvider.h"

namespace schemaregistry::rest {

/**
 * OAuth 2.0 Client Credentials provider.
 *
 * Implements automatic token fetching using OAuth 2.0 Client Credentials
 * grant flow.
 */
class OAuthClientProvider : public CachingOAuthProvider {
 public:
  /**
   * Configuration for OAuth Client Credentials flow.
   */
  struct Config : CacheConfig {
    // Required OAuth parameters
    std::string client_id;
    std::string client_secret;
    std::string scope;
    std::string token_endpoint_url;

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

 protected:
  OAuthToken fetch_token() override;

 private:
  const Config config_;
};

}  // namespace schemaregistry::rest
