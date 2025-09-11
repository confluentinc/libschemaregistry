/**
 * WildcardMatcher
 * Utility for matching text against wildcard patterns
 */

#include "schemaregistry/serdes/WildcardMatcher.h"

#include <stdexcept>

namespace schemaregistry::serdes {

namespace {

/**
 * Helper function to handle escape sequences in wildcard patterns
 *
 * @param dst The destination string being built
 * @param src The source pattern string
 * @param i Current position in the source string (after the backslash)
 * @return Pair of (updated destination string, updated position)
 */
std::pair<std::string, size_t> doubleSlashes(const std::string &dst,
                                             const std::string &src, size_t i) {
    std::string result = dst;

    if (i < src.length()) {
        char nextChar = src[i];
        // The backslash escapes the next character, making it literal
        // We need to escape it for regex if it's a regex special character
        if (nextChar == '.' || nextChar == '+' || nextChar == '{' ||
            nextChar == '}' || nextChar == '(' || nextChar == ')' ||
            nextChar == '|' || nextChar == '^' || nextChar == '$' ||
            nextChar == '*' || nextChar == '?') {
            result.push_back('\\');
            result.push_back(nextChar);
        } else {
            result.push_back(nextChar);
        }
        i++;
    } else {
        // A backslash at the very end is treated as a literal backslash
        result.push_back('\\');
        result.push_back('\\');
    }

    return std::make_pair(result, i);
}

}  // anonymous namespace

std::string wildcardToRegexp(const std::string &pattern,
                             const std::string &separator) {
    std::string dst;

    // Replace **<separator>* with ** to handle the special case
    std::string pat = "**" + separator + "*";
    std::string src = pattern;

    // Replace all occurrences of **<separator>* with **
    size_t pos = 0;
    while ((pos = src.find(pat, pos)) != std::string::npos) {
        src.replace(pos, pat.length(), "**");
        pos += 2;  // Length of "**"
    }

    size_t i = 0;
    size_t size = src.length();

    while (i < size) {
        char c = src[i];
        i++;

        if (c == '*') {
            // One char lookahead for **
            if (i < size && src[i] == '*') {
                dst += ".*";
                i++;
            } else {
                dst += "[^" + separator + "]*";
            }
        } else if (c == '?') {
            dst += "[^" + separator + "]";
        } else if (c == '.' || c == '+' || c == '{' || c == '}' || c == '(' ||
                   c == ')' || c == '|' || c == '^' || c == '$') {
            // These need to be escaped in regular expressions
            dst.push_back('\\');
            dst.push_back(c);
        } else if (c == '\\') {
            auto result = doubleSlashes(dst, src, i);
            dst = result.first;
            i = result.second;
        } else {
            dst.push_back(c);
        }
    }

    return dst;
}

bool wildcardMatch(const std::string &text, const std::string &matcher) {
    try {
        std::string regexPattern = wildcardToRegexp(matcher, ".");
        std::regex pattern(regexPattern);
        std::smatch match;

        if (std::regex_search(text, match, pattern)) {
            // Check if the match covers the entire string
            return match.position() == 0 && match.length() == text.length();
        }

        return false;
    } catch (const std::regex_error &e) {
        // If regex compilation fails, return false
        return false;
    }
}

}  // namespace schemaregistry::serdes