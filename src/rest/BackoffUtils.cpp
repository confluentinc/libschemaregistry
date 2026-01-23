/**
 * Utility functions for retry backoff and error handling.
 */

#include "schemaregistry/rest/BackoffUtils.h"

#include <limits>
#include <random>

namespace schemaregistry::rest::utils {

bool isRetriable(int status_code) {
  return status_code == 408      // REQUEST_TIMEOUT
         || status_code == 429   // TOO_MANY_REQUESTS
         || status_code == 500   // INTERNAL_SERVER_ERROR
         || status_code == 502   // BAD_GATEWAY
         || status_code == 503   // SERVICE_UNAVAILABLE
         || status_code == 504;  // GATEWAY_TIMEOUT
}

std::chrono::milliseconds calculateExponentialBackoff(
    std::uint32_t initial_backoff_ms, std::uint32_t retry_attempt,
    std::chrono::milliseconds max_backoff) {
  // Calculate 2^retry_attempt * initial_backoff_ms with overflow protection
  std::uint64_t backoff_ms;
  if (retry_attempt >= 32 ||
      (1ULL << retry_attempt) >
          std::numeric_limits<std::uint32_t>::max() / initial_backoff_ms) {
    // Overflow would occur, use max_backoff
    backoff_ms = static_cast<std::uint64_t>(max_backoff.count());
  } else {
    backoff_ms = static_cast<std::uint64_t>((1ULL << retry_attempt) *
                                            initial_backoff_ms);
    if (backoff_ms > static_cast<std::uint64_t>(max_backoff.count())) {
      backoff_ms = static_cast<std::uint64_t>(max_backoff.count());
    }
  }

  // Apply jitter (random factor between 0.0 and 1.0)
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  double jitter = dist(gen);

  return std::chrono::milliseconds(static_cast<long>(backoff_ms * jitter));
}

}  // namespace schemaregistry::rest::utils
