#include "schemaregistry/serdes/ValidationRule.h"

#include <sstream>

namespace schemaregistry::serdes {

std::optional<ValidationRulesExecution> parseValidationRulesExecution(
    const std::string &value) {
    if (value == "DISABLED") {
        return ValidationRulesExecution::Disabled;
    }
    if (value == "BEFORE_DOMAIN_RULES") {
        return ValidationRulesExecution::BeforeDomainRules;
    }
    if (value == "AFTER_DOMAIN_RULES") {
        return ValidationRulesExecution::AfterDomainRules;
    }
    return std::nullopt;
}

std::string ValidationRuleError::toString() const {
    std::ostringstream out;
    out << (field_path.empty() ? "<root>" : field_path) << ": "
        << (rule.name.empty() ? "unnamed" : rule.name) << ": ";
    // Prefer the dynamic message returned by the rule itself; fall back to the
    // rule's authored doc / SQL / CEL expression in that order.
    if (!message.empty()) {
        out << message;
    } else if (!rule.doc.empty()) {
        out << rule.doc;
    } else if (!rule.sql.empty()) {
        out << rule.sql;
    } else {
        out << rule.expr;
    }
    if (!cause.empty()) {
        out << " (caused by: " << cause << ")";
    }
    return out.str();
}

namespace {

std::string buildMessage(const std::vector<ValidationRuleError> &violations) {
    if (violations.empty()) {
        return "Validation rule failed (no detail)";
    }
    std::ostringstream out;
    out << "Validation rule failed (" << violations.size()
        << (violations.size() == 1 ? " violation):" : " violations):");
    for (const auto &violation : violations) {
        out << "\n  - " << violation.toString();
    }
    return out.str();
}

std::string stringProp(const nlohmann::json &entry, const std::string &key) {
    auto it = entry.find(key);
    if (it == entry.end() || !it->is_string()) {
        return "";
    }
    return it->get<std::string>();
}

}  // namespace

ValidationRulesFailedError::ValidationRulesFailedError(
    std::vector<ValidationRuleError> violations)
    : SerdeError(buildMessage(violations)),
      violations_(std::move(violations)) {}

std::vector<ValidationRule> parseValidationRules(const nlohmann::json &prop) {
    std::vector<ValidationRule> rules;
    if (!prop.is_array()) {
        return rules;
    }
    for (const auto &entry : prop) {
        if (!entry.is_object()) {
            continue;
        }
        rules.push_back(ValidationRule{
            stringProp(entry, "name"), stringProp(entry, "doc"),
            stringProp(entry, "expr"), stringProp(entry, "sql")});
    }
    return rules;
}

bool evaluateValidationRule(ValidationRuleExecutor &executor,
                            const ValidationRule &rule, const SerdeValue &value,
                            const std::string &path,
                            std::vector<ValidationRuleError> &violations) {
    ValidationRuleResult result;
    try {
        result = executor.execute(rule, value);
    } catch (const std::exception &e) {
        violations.push_back(
            ValidationRuleError{rule, path, "", std::string(e.what())});
        return true;
    }
    if (std::holds_alternative<bool>(result)) {
        if (!std::get<bool>(result)) {
            violations.push_back(ValidationRuleError{rule, path, "", ""});
            return true;
        }
        return false;
    }
    const std::string &message = std::get<std::string>(result);
    if (!message.empty()) {
        violations.push_back(ValidationRuleError{rule, path, message, ""});
        return true;
    }
    return false;
}

void raiseValidationViolations(std::vector<ValidationRuleError> violations) {
    if (violations.empty()) {
        return;
    }
    throw ValidationRulesFailedError(std::move(violations));
}

std::string appendValidationPath(const std::string &path,
                                 const std::string &name) {
    return path.empty() ? name : path + "." + name;
}

}  // namespace schemaregistry::serdes
