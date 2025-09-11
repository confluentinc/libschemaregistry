#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>

#include "nlohmann/json.hpp"
#include "schemaregistry/serdes/Serde.h"
#include "schemaregistry/serdes/SerdeError.h"

namespace schemaregistry::rules::jsonata {

using namespace schemaregistry::serdes;

class JsonataExecutor : public RuleExecutor {
  public:
    JsonataExecutor();

    std::unique_ptr<SerdeValue> transform(
        schemaregistry::serdes::RuleContext &ctx,
        const SerdeValue &msg) override;

    std::string getType() const override;

    static void registerExecutor();
};

}  // namespace schemaregistry::rules::jsonata