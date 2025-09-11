/**
 * WildcardMatcher
 * Utility for matching text against wildcard patterns
 */

#pragma once

#include <regex>
#include <string>
#include <utility>

namespace schemaregistry::serdes {

/**
 * Match text against a wildcard pattern
 *
 * Supports:
 * - '*' matches any sequence of characters except separator
 * - '**' matches any sequence of characters including separator
 * - '?' matches any single character except separator
 *
 * @param text The text to match against
 * @param matcher The wildcard pattern to match
 * @return true if the text matches the pattern exactly
 */
bool wildcardMatch(const std::string &text, const std::string &matcher);

/**
 * Convert a wildcard pattern to a regular expression
 *
 * @param pattern The wildcard pattern
 * @param separator The separator character (default ".")
 * @return The regular expression string
 */
std::string wildcardToRegexp(const std::string &pattern,
                             const std::string &separator = ".");

}  // namespace schemaregistry::serdes