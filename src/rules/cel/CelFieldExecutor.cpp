#include "schemaregistry/rules/cel/CelFieldExecutor.h"

#include "eval/public/containers/container_backed_list_impl.h"
#include "schemaregistry/rules/cel/CelExecutorImpl.h"
#include "schemaregistry/serdes/RuleRegistry.h"
#include "schemaregistry/serdes/avro/AvroTypes.h"
#include "schemaregistry/serdes/json/JsonTypes.h"
#include "schemaregistry/serdes/protobuf/ProtobufTypes.h"

namespace schemaregistry::rules::cel {

CelFieldExecutor::CelFieldExecutor() {
    // Create a proper CelExecutor instance like Rust version
    executor_ = std::make_shared<CelExecutor>();
}

CelFieldExecutor::CelFieldExecutor(std::shared_ptr<CelExecutor> executor)
    : executor_(std::move(executor)) {}

// Implement the required getType method from RuleBase
std::string CelFieldExecutor::getType() const { return "CEL_FIELD"; }

std::unique_ptr<SerdeValue> CelFieldExecutor::transformField(
    RuleContext &ctx, const SerdeValue &field_value) {
    auto field_ctx = ctx.currentField();
    if (!field_ctx || !field_ctx->isPrimitive()) {
        return field_value.clone();
    }

    // Only proceed if we have a valid executor
    if (!executor_) {
        return field_value.clone();
    }

    google::protobuf::Arena arena;
    absl::flat_hash_map<std::string, google::api::expr::runtime::CelValue> args;

    // Add field value like Rust version
    args.emplace("value",
                 executor_->impl_->fromSerdeValue(field_value, &arena));

    // Add field context information like Rust version
    auto *full_name_str = google::protobuf::Arena::Create<std::string>(
        &arena, field_ctx->getFullName());
    args.emplace("fullName", google::api::expr::runtime::CelValue::CreateString(
                                 full_name_str));

    auto *name_str = google::protobuf::Arena::Create<std::string>(
        &arena, field_ctx->getName());
    args.emplace("name",
                 google::api::expr::runtime::CelValue::CreateString(name_str));

    // Convert FieldType to string representation for typeName using existing
    // function
    auto *type_name_str = google::protobuf::Arena::Create<std::string>(
        &arena,
        schemaregistry::serdes::fieldTypeToString(field_ctx->getFieldType()));
    args.emplace("typeName", google::api::expr::runtime::CelValue::CreateString(
                                 type_name_str));

    // Create CEL list for tags like Rust version
    std::vector<google::api::expr::runtime::CelValue> tags_vec;

    for (const auto &tag : field_ctx->getTags()) {
        auto *tag_str =
            google::protobuf::Arena::Create<std::string>(&arena, tag);
        tags_vec.push_back(
            google::api::expr::runtime::CelValue::CreateString(tag_str));
    }

    // Create the CEL list for tags using proper API
    auto *list_impl = google::protobuf::Arena::Create<
        google::api::expr::runtime::ContainerBackedListImpl>(&arena, tags_vec);
    args.emplace("tags",
                 google::api::expr::runtime::CelValue::CreateList(list_impl));

    // Add containing message like Rust version
    args.emplace("message", executor_->impl_->fromSerdeValue(
                                field_ctx->getContainingMessage(), &arena));

    // Execute the CEL expression using the shared executor
    auto result = executor_->impl_->execute(ctx, field_value, args, &arena);
    if (result) {
        return result;
    }

    return field_value.clone();
}

void CelFieldExecutor::registerExecutor() {
    // Register this field executor with the global rule registry
    // This matches the Rust version:
    // crate::serdes::rule_registry::register_rule_executor(CelFieldExecutor::new());
    global_registry::registerRuleExecutor(std::make_shared<CelFieldExecutor>());
}

}  // namespace schemaregistry::rules::cel