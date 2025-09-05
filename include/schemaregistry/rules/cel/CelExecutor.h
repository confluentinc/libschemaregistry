#pragma once

#include <memory>

#include "schemaregistry/serdes/Serde.h"

namespace schemaregistry::rules::cel {

using namespace schemaregistry::serdes;

class CelExecutor : public RuleExecutor {
  public:
    CelExecutor();
    ~CelExecutor();

    // Rule out copy constructor and assignment operator for PIMPL
    CelExecutor(const CelExecutor &) = delete;
    CelExecutor &operator=(const CelExecutor &) = delete;

    // Allow move constructor and assignment operator
    CelExecutor(CelExecutor &&) noexcept;
    CelExecutor &operator=(CelExecutor &&) noexcept;

    std::unique_ptr<SerdeValue> transform(
        schemaregistry::serdes::RuleContext &ctx,
        const SerdeValue &msg) override;

    std::string getType() const override;

    static void registerExecutor();

  private:
    friend class CelFieldExecutor;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace schemaregistry::rules::cel