/**
 * Azure User-Assigned Managed Identity (UAMI) OAuth Provider
 */

#pragma once

#include <string>

#include "schemaregistry/rest/OAuthProvider.h"

namespace schemaregistry::rest {

/**
 * Azure User-Assigned Managed Identity (UAMI) OAuth provider.
 *
 * Fetches tokens from the Azure Instance Metadata Service (IMDS) using
 * a managed identity's client ID.
 */
class UamiOAuthProvider : public CachingOAuthProvider {
 public:
  struct Config : CacheConfig {
    // Azure IMDS base URL (default: standard Azure IMDS endpoint)
    std::string uami_url = "http://169.254.169.254/metadata/identity/oauth2/token";

    // Required: query string appended to uami_url (contains resource, client_id, api-version, etc.)
    std::string uami_endpoint_query;

    void validate() const;
  };

  explicit UamiOAuthProvider(const Config& config);

 protected:
  OAuthToken fetch_token() override;

 private:
  const Config config_;
};

}  // namespace schemaregistry::rest
