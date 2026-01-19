/**
 * Utility functions for retry backoff and error handling.
 */

#pragma once

#include <chrono>
#include <cstdint>

namespace schemaregistry::rest::utils {

/**
 * Check if an HTTP status code represents a retriable error.
 *
 * Retriable errors are transient failures that may succeed on retry:
 * - 408 Request Timeout
 * - 429 Too Many Requests
 * - 5xx Server Errors (500, 502, 503, 504, etc.)
 *
 * Non-retriable errors are permanent failures (4xx client errors like
 * 400, 401, 403, 404) that won't succeed on retry.
 *
 * @param status_code HTTP status code
 * @return true if the error is retriable, false otherwise
 */
bool isRetriable(int status_code);

/**
 * Calculate exponential backoff delay with overflow protection and jitter.
 *
 * Calculates delay as: base * (2^attempt) with full jitter.
 * Protects against integer overflow for large attempt values.
 *
 * @param initial_backoff_ms Base delay in milliseconds
 * @param retry_attempt Retry attempt number (0-indexed)
 * @param max_backoff Maximum delay (cap for exponential growth)
 * @return Delay in milliseconds with jitter applied (range: 0 to calculated delay)
 */
std::chrono::milliseconds calculateExponentialBackoff(
    std::uint32_t initial_backoff_ms, std::uint32_t retry_attempt,
    std::chrono::milliseconds max_backoff);

}  // namespace schemaregistry::rest::utils
