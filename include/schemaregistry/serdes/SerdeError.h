#pragma once

#include <memory>
#include <stdexcept>
#include <string>

// Include for Rule type
#include "schemaregistry/rest/model/Rule.h"

namespace schemaregistry::serdes {

/**
 * Base exception class for all serialization/deserialization errors
 * Based on the SerdeError enum from serde.rs
 */
class SerdeError : public std::runtime_error {
  public:
    explicit SerdeError(const std::string &message)
        : std::runtime_error(message) {}
    virtual ~SerdeError() = default;
};

/**
 * Rule execution errors
 * Maps to SerdeError::Rule variant
 */
class RuleError : public SerdeError {
  public:
    explicit RuleError(const std::string &message)
        : SerdeError("Rule failed: " + message) {}
};

/**
 * Rule condition errors
 * Maps to SerdeError::RuleCondition variant
 */
class RuleConditionError : public SerdeError {
  private:
    std::shared_ptr<schemaregistry::rest::model::Rule> rule_;

  public:
    explicit RuleConditionError(
        std::shared_ptr<schemaregistry::rest::model::Rule> rule,
        const std::string &message = "Rule condition failed");

    std::shared_ptr<schemaregistry::rest::model::Rule> getRule() const {
        return rule_;
    }
};

/**
 * REST API errors
 * Maps to SerdeError::Rest variant
 */
class RestError : public SerdeError {
  public:
    explicit RestError(const std::string &message)
        : SerdeError("REST error: " + message) {}
};

/**
 * General serialization errors
 * Maps to SerdeError::Serialization variant
 */
class SerializationError : public SerdeError {
  public:
    explicit SerializationError(const std::string &message)
        : SerdeError("Serde error: " + message) {}
};

/**
 * Cryptographic/Tink errors
 * Maps to SerdeError::Tink variant
 */
class TinkError : public SerdeError {
  public:
    explicit TinkError(const std::string &message)
        : SerdeError("Tink error: " + message) {}
};

/**
 * UUID parsing errors
 * Based on uuid parsing errors in serde.rs
 */
class UuidError : public SerdeError {
  public:
    explicit UuidError(const std::string &message)
        : SerdeError("UUID error: " + message) {}
};

/**
 * IO errors
 * Based on IO operations in serde.rs
 */
class IoError : public SerdeError {
  public:
    explicit IoError(const std::string &message)
        : SerdeError("IO error: " + message) {}
};

/**
 * Utility functions for error handling
 */
namespace error_utils {
/**
 * Create serialization error from string message
 */
SerializationError createSerializationError(const std::string &message);

/**
 * Create rule error from rule type and message
 */
RuleError createRuleError(const std::string &rule_type,
                          const std::string &message);

/**
 * Create UUID error from parsing failure
 */
UuidError createUuidError(const std::string &invalid_uuid);

/**
 * Create IO error from operation description
 */
IoError createIoError(const std::string &operation, const std::string &details);
}  // namespace error_utils

}  // namespace schemaregistry::serdes