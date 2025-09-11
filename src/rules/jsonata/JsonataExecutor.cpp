#include "schemaregistry/rules/jsonata/JsonataExecutor.h"

#include <regex>

#include "jsonata/Jsonata.h"
#include "schemaregistry/serdes/RuleRegistry.h"
#include "schemaregistry/serdes/Serde.h"

namespace schemaregistry::rules::jsonata {

using namespace schemaregistry::serdes;

JsonataExecutor::JsonataExecutor() {}

// Implement the required getType method
std::string JsonataExecutor::getType() const { return "JSONATA"; }

std::unique_ptr<SerdeValue> JsonataExecutor::transform(
    schemaregistry::serdes::RuleContext &ctx, const SerdeValue &msg) {
    auto expr = ctx.getRule().getExpr();
    if (!expr) {
        throw SerdeError("Rule does not contain an expression");
    }
    auto jsonata = ::jsonata::Jsonata(expr.value());
    auto value = msg.asJson();
    auto result = jsonata.evaluate(value);
    return SerdeValue::newJson(msg.getFormat(), result);
}

void JsonataExecutor::registerExecutor() {
    // Register this executor with the global rule registry
    // This matches the Rust version:
    // crate::serdes::rule_registry::register_rule_executor(JsonataExecutor::new());
    global_registry::registerRuleExecutor(std::make_shared<JsonataExecutor>());
}

}  // namespace schemaregistry::rules::jsonata