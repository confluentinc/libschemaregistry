#include "schemaregistry/rules/cel/CelValidator.h"

#include "absl/time/clock.h"
#include "eval/public/cel_value.h"
#include "google/protobuf/arena.h"
#include "schemaregistry/rules/cel/CelExecutorImpl.h"
#include "schemaregistry/serdes/RuleRegistry.h"
#include "schemaregistry/serdes/SerdeError.h"

namespace schemaregistry::rules::cel {

namespace {

std::string ruleName(const ValidationRule &rule) {
    return rule.name.empty() ? "unnamed" : rule.name;
}

}  // namespace

CelValidator::CelValidator() : executor_(std::make_shared<CelExecutor>()) {}

CelValidator::CelValidator(std::shared_ptr<CelExecutor> executor)
    : executor_(std::move(executor)) {}

std::string CelValidator::getType() const { return "CEL"; }

ValidationRuleResult CelValidator::execute(const ValidationRule &rule,
                                           const SerdeValue &value) {
    if (rule.expr.empty()) {
        throw SerdeError("Validation rule '" + ruleName(rule) +
                         "' has no expression");
    }
    if (!executor_) {
        throw SerdeError("Validation rule '" + ruleName(rule) +
                         "' has no CEL executor");
    }

    google::protobuf::Arena arena;
    absl::flat_hash_map<std::string, google::api::expr::runtime::CelValue> args;
    args.emplace("this", executor_->impl_->fromSerdeValue(value, &arena));
    args.emplace("now", google::api::expr::runtime::CelValue::CreateTimestamp(
                            absl::Now()));

    auto result = executor_->impl_->evaluate(rule.expr, args, &arena);
    if (!result) {
        throw SerdeError("Validation rule '" + ruleName(rule) +
                         "' produced no result");
    }

    if (result->IsError()) {
        const auto *error = result->ErrorOrDie();
        throw SerdeError(
            "Validation rule '" + ruleName(rule) +
            "' failed to evaluate: " + std::string(error->message()));
    }
    if (result->IsBool()) {
        return result->BoolOrDie();
    }
    if (result->IsString()) {
        return std::string(result->StringOrDie().value());
    }
    throw SerdeError("Validation rule '" + ruleName(rule) +
                     "' must return bool or string");
}

void CelValidator::registerExecutor() {
    global_registry::registerValidationRuleExecutor(
        std::make_shared<CelValidator>());
}

}  // namespace schemaregistry::rules::cel
