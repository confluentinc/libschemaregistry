/**
 * Custom OAuth Provider Implementation
 */

#include "schemaregistry/rest/CustomOAuthProvider.h"

#include <stdexcept>

namespace schemaregistry::rest {

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
  std::string access_token = fetch_fn_();

  if (access_token.empty()) {
    throw std::runtime_error("Custom token fetch function returned empty token");
  }

  return BearerFields{std::move(access_token), logical_cluster_, identity_pool_id_};
}

}  // namespace schemaregistry::rest
