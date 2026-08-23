#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "schemaregistry/serdes/SerdeError.h"
#include "schemaregistry/serdes/SerdeTypes.h"

namespace schemaregistry::serdes {

/**
 * Determines when inline validation rules run, relative to domain rule
 * transformations.
 */
enum class ValidationRulesExecution {
    Disabled,
    BeforeDomainRules,
    AfterDomainRules
};

/**
 * Parse a ValidationRulesExecution from its string form ("DISABLED",
 * "BEFORE_DOMAIN_RULES", "AFTER_DOMAIN_RULES"). Returns nullopt when the string
 * names no known mode.
 */
std::optional<ValidationRulesExecution> parseValidationRulesExecution(
    const std::string &value);

/**
 * The schema property (Avro) / keyword (JSON Schema) that holds inline
 * validation rules.
 */
constexpr const char *VALIDATION_RULES_PROP = "confluent:rules";

/**
 * An inline validation rule (a CHECK constraint) declared on a schema, either
 * on a record/message/object or on one of its fields.
 */
struct ValidationRule {
    std::string name;
    std::string doc;
    std::string expr;
    std::string sql;
};

/**
 * A single inline validation rule failure, located at field_path within the
 * message that was validated.
 */
struct ValidationRuleError {
    ValidationRule rule;
    std::string field_path;
    /**
     * Optional dynamic error message returned by the rule itself — set when the
     * rule expression returned a non-empty string explaining the failure (e.g.
     * "x > 0 ? '' : 'x must be positive'"). Empty when the failure was a plain
     * false or an evaluation error.
     */
    std::string message;
    /** Executor failure text, when the rule could not be evaluated at all. */
    std::string cause;

    std::string toString() const;
};

/**
 * Aggregates every inline validation rule failure found while walking a
 * message.
 */
class ValidationRulesFailedError : public SerdeError {
  public:
    explicit ValidationRulesFailedError(
        std::vector<ValidationRuleError> violations);

    const std::vector<ValidationRuleError> &getViolations() const {
        return violations_;
    }

  private:
    std::vector<ValidationRuleError> violations_;
};

/**
 * The outcome of evaluating a validation rule: either a bool (false meaning the
 * rule failed) or a string (non-empty meaning the rule failed, with that string
 * as the failure message).
 */
using ValidationRuleResult = std::variant<bool, std::string>;

/**
 * Evaluates a single inline validation rule against a value.
 */
class ValidationRuleExecutor {
  public:
    virtual ~ValidationRuleExecutor() = default;

    /**
     * Get the type identifier for this executor
     */
    virtual std::string getType() const = 0;

    /**
     * Evaluate the rule against value. Throws SerdeError when the rule cannot
     * be compiled or evaluated, or when it resolves to something other than a
     * bool or a string.
     */
    virtual ValidationRuleResult execute(const ValidationRule &rule,
                                         const SerdeValue &value) = 0;
};

/**
 * Parse a "confluent:rules" property value — a list of objects with
 * name/doc/expr/sql keys. Anything that is not such a list yields no rules;
 * malformed entries within the list are skipped.
 */
std::vector<ValidationRule> parseValidationRules(const nlohmann::json &prop);

/**
 * Evaluate one inline validation rule, appending a ValidationRuleError to
 * violations when it fails. A rule that cannot be evaluated is itself recorded
 * as a violation so the walk can continue.
 *
 * @return true when a violation was recorded
 */
bool evaluateValidationRule(ValidationRuleExecutor &executor,
                            const ValidationRule &rule, const SerdeValue &value,
                            const std::string &path,
                            std::vector<ValidationRuleError> &violations);

/**
 * Throw a ValidationRulesFailedError listing every violation, or return
 * normally when there are none.
 */
void raiseValidationViolations(std::vector<ValidationRuleError> violations);

/**
 * Append a field name to a dotted validation path.
 */
std::string appendValidationPath(const std::string &path,
                                 const std::string &name);

}  // namespace schemaregistry::serdes