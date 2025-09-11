#include "schemaregistry/rules/encryption/FieldEncryptionExecutor.h"

#include "schemaregistry/serdes/RuleRegistry.h"

namespace schemaregistry::rules::encryption {

using namespace schemaregistry::serdes;

// Field encryption executor implementation
FieldEncryptionExecutor::FieldEncryptionExecutor(std::shared_ptr<Clock> clock)
    : executor_(clock) {}

void FieldEncryptionExecutor::configure(
    std::shared_ptr<const ClientConfiguration> client_config,
    const std::unordered_map<std::string, std::string> &rule_config) {
    executor_.configure(client_config, rule_config);
}

std::string FieldEncryptionExecutor::getType() const { return "ENCRYPT"; }

void FieldEncryptionExecutor::close() { executor_.close(); }

std::unique_ptr<SerdeValue> FieldEncryptionExecutor::transformField(
    RuleContext &ctx, const SerdeValue &field_value) {
    auto transform = executor_.newTransform(ctx);
    auto field_ctx = ctx.currentField();
    if (!field_ctx) {
        throw SerdeError("no field context");
    }

    return transform->transform(ctx, field_ctx->getFieldType(), field_value);
}

IDekRegistryClient *FieldEncryptionExecutor::getClient() const {
    return executor_.getClient();
}

void FieldEncryptionExecutor::registerExecutor() {
    global_registry::registerRuleExecutor(
        std::make_shared<FieldEncryptionExecutor>(nullptr));
}

}  // namespace schemaregistry::rules::encryption