/**
 * Custom OAuth Provider with user-provided token function
 */

#pragma once

#include <functional>
#include <string>

#include "schemaregistry/rest/OAuthProvider.h"

namespace schemaregistry::rest {

/**
 * Custom OAuth provider that delegates token fetching to a user-provided function.
 *
 * This allows users to implement custom token fetching logic without implementing
 * a full provider class.
 *
 * Example usage:
 * @code
 * auto provider = std::make_shared<CustomOAuthProvider>(
 *     []() { return fetch_token_from_my_idp(); },
 *     "lsrc-12345",
 *     "pool-67890"
 * );
 * @endcode
 *
 * Note: The function is called on every get_bearer_fields() call.
 * Users should implement their own caching and thread safety if needed.
 */
class CustomOAuthProvider : public OAuthProvider {
 public:
  /**
   * Function type that returns an access token string.
   * The function should handle all token fetching logic including:
   * - Fetching from custom OAuth endpoints
   * - Caching (if desired)
   * - Thread safety (if needed)
   * - Error handling
   *
   * @return Access token string
   * @throws std::runtime_error if unable to fetch token
   */
  using TokenFetchFunction = std::function<std::string()>;

  /**
   * Construct custom OAuth provider with user-provided token fetch function.
   *
   * @param fetch_fn Function that returns access token (required)
   * @param logical_cluster Schema Registry logical cluster ID (optional, required for Confluent Cloud)
   * @param identity_pool_id Identity pool ID(s) as comma-separated string (optional)
   * @throws std::invalid_argument if fetch_fn is empty
   */
  CustomOAuthProvider(TokenFetchFunction fetch_fn,
                     std::string logical_cluster = "",
                     std::string identity_pool_id = "");

  BearerFields get_bearer_fields() override;

 private:
  const TokenFetchFunction fetch_fn_;
  const std::string logical_cluster_;
  const std::string identity_pool_id_;
};

}  // namespace schemaregistry::rest
