#pragma once

#include "schemaregistry/rules/cel/CelExecutor.h"
#include "schemaregistry/serdes/Serde.h"

namespace schemaregistry::rules::cel {

class CelFieldExecutor : public FieldRuleExecutor {
  public:
    CelFieldExecutor();
    explicit CelFieldExecutor(std::shared_ptr<CelExecutor> executor);
    std::unique_ptr<SerdeValue> transformField(
        RuleContext &ctx, const SerdeValue &field_value) override;

    std::string getType() const override;

    static void registerExecutor();

  private:
    std::shared_ptr<CelExecutor> executor_;
};

}  // namespace schemaregistry::rules::cel