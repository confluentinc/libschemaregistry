#pragma once

#include <memory>
#include <string>

#include "schemaregistry/rules/cel/CelExecutor.h"
#include "schemaregistry/serdes/ValidationRule.h"

namespace schemaregistry::rules::cel {

using namespace schemaregistry::serdes;

/**
 * Validation-rule executor backed by CEL. The rule expression is evaluated with
 * `this` bound to the value being validated and `now` bound to the current
 * time, and must resolve to a bool (false meaning the rule failed) or a string
 * (non-empty meaning the rule failed, with that string as the message).
 */
class CelValidator : public ValidationRuleExecutor {
  public:
    CelValidator();
    explicit CelValidator(std::shared_ptr<CelExecutor> executor);

    std::string getType() const override;

    ValidationRuleResult execute(const ValidationRule &rule,
                                 const SerdeValue &value) override;

    static void registerExecutor();

  private:
    std::shared_ptr<CelExecutor> executor_;
};

}  // namespace schemaregistry::rules::cel
