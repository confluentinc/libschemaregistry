#include "schemaregistry/rules/cel/CelExecutor.h"

#include <regex>

#include "absl/strings/str_split.h"
#include "eval/public/activation.h"
#include "eval/public/builtin_func_registrar.h"
#include "eval/public/cel_expr_builder_factory.h"
#include "eval/public/containers/container_backed_list_impl.h"
#include "eval/public/containers/container_backed_map_impl.h"
#include "eval/public/string_extension_func_registrar.h"
#include "eval/public/structs/cel_proto_wrapper.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "nlohmann/json.hpp"
#include "parser/parser.h"
#include "schemaregistry/rules/cel/CelExecutorImpl.h"
#include "schemaregistry/rules/cel/CelUtils.h"
#include "schemaregistry/rules/cel/ExtraFunc.h"
#include "schemaregistry/serdes/RuleRegistry.h"
#include "schemaregistry/serdes/SerdeError.h"
#ifdef SCHEMAREGISTRY_USE_AVRO
#include "schemaregistry/serdes/avro/AvroTypes.h"
#endif
#include "schemaregistry/serdes/json/JsonTypes.h"
#include "schemaregistry/serdes/protobuf/ProtobufTypes.h"

namespace schemaregistry::rules::cel {

using namespace schemaregistry::serdes;

// Impl constructor implementations
CelExecutor::Impl::Impl() {
    // Try to initialize the CEL runtime
    auto runtime_result = newRuleBuilder(&arena_);
    if (!runtime_result.ok()) {
        throw SerdeError("Failed to create CEL runtime: " +
                         std::string(runtime_result.status().message()));
    }
    runtime_ = std::move(runtime_result.value());
}

// CelExecutor constructor implementations
CelExecutor::CelExecutor() : impl_(std::make_unique<Impl>()) {}

// Destructor
CelExecutor::~CelExecutor() = default;

// Move constructor
CelExecutor::CelExecutor(CelExecutor &&) noexcept = default;

// Move assignment
CelExecutor &CelExecutor::operator=(CelExecutor &&) noexcept = default;

// Implement the required getType method
std::string CelExecutor::getType() const { return "CEL"; }

absl::StatusOr<
    std::unique_ptr<google::api::expr::runtime::CelExpressionBuilder>>
CelExecutor::Impl::newRuleBuilder(google::protobuf::Arena *arena) {
    google::api::expr::runtime::InterpreterOptions options;
    options.enable_qualified_type_identifiers = true;
    options.enable_timestamp_duration_overflow_errors = true;
    options.enable_empty_wrapper_null_unboxing = true;
    options.enable_regex_precompilation = true;
    options.constant_folding = true;
    options.constant_arena = arena;

    std::unique_ptr<google::api::expr::runtime::CelExpressionBuilder> builder =
        google::api::expr::runtime::CreateCelExpressionBuilder(options);
    auto register_status = google::api::expr::runtime::RegisterBuiltinFunctions(
        builder->GetRegistry(), options);
    if (!register_status.ok()) {
        return register_status;
    }
    register_status =
        google::api::expr::runtime::RegisterStringExtensionFunctions(
            builder->GetRegistry());
    if (!register_status.ok()) {
        return register_status;
    }
    // Register our custom extra functions
    register_status = RegisterExtraFuncs(*builder->GetRegistry(), arena);
    if (!register_status.ok()) {
        return register_status;
    }
    return builder;
}

std::unique_ptr<SerdeValue> CelExecutor::transform(
    schemaregistry::serdes::RuleContext &ctx, const SerdeValue &msg) {
    google::protobuf::Arena arena;

    absl::flat_hash_map<std::string, google::api::expr::runtime::CelValue> args;
    args.emplace("message", impl_->fromSerdeValue(msg, &arena));

    return impl_->execute(ctx, msg, args, &arena);
}

std::unique_ptr<SerdeValue> CelExecutor::Impl::execute(
    schemaregistry::serdes::RuleContext &ctx, const SerdeValue &msg,
    const absl::flat_hash_map<std::string, google::api::expr::runtime::CelValue>
        &args,
    google::protobuf::Arena *arena) {
    // Get the expression from the rule context
    const Rule &rule = ctx.getRule();

    auto expr_opt = rule.getExpr();
    if (!expr_opt.has_value()) {
        throw SerdeError("rule does not contain an expression");
    }

    std::string expr = expr_opt.value();
    if (expr.empty()) {
        throw SerdeError("rule does not contain an expression");
    }

    // Split expression on semicolon to handle guard expressions like Rust
    // version
    std::vector<absl::string_view> parts = absl::StrSplit(expr, ";");

    if (parts.size() > 1) {
        // Handle guard expression
        absl::string_view guard = parts[0];
        if (!guard.empty()) {
            auto guard_result =
                executeRule(ctx, msg, std::string(guard), args, arena);
            if (guard_result) {
                // Check if guard evaluates to false - if so, return copy of
                // original message
                if (guard_result->IsBool() && !guard_result->BoolOrDie()) {
                    // Return copy using the msg's clone method
                    return msg.clone();
                }
            }
        }
        // Use the second part as the main expression
        expr = std::string(parts[1]);
    }

    // Execute the main expression
    auto result = executeRule(ctx, msg, expr, args, arena);
    if (result) {
        return toSerdeValue(msg, *result);
    }

    return nullptr;
}

std::unique_ptr<google::api::expr::runtime::CelValue>
CelExecutor::Impl::executeRule(
    RuleContext &ctx, const SerdeValue &msg, const std::string &expr,
    const absl::flat_hash_map<std::string, google::api::expr::runtime::CelValue>
        &args,
    google::protobuf::Arena *arena) {
    // Get or compile the expression (with caching)
    auto parsed_expr_status = getOrCompileExpression(expr);
    if (!parsed_expr_status.ok()) {
        throw SerdeError("CEL expression compilation failed: " +
                         std::string(parsed_expr_status.status().message()));
    }
    auto parsed_expr = parsed_expr_status.value();

    // Create activation context and add all arguments
    google::api::expr::runtime::Activation activation;
    for (const auto &pair : args) {
        activation.InsertValue(pair.first, pair.second);
    }

    // Evaluate the expression using the passed arena
    auto eval_status = parsed_expr->Evaluate(activation, arena);
    if (!eval_status.ok()) {
        throw SerdeError("CEL evaluation failed: " +
                         std::string(eval_status.status().message()));
    }

    return std::make_unique<google::api::expr::runtime::CelValue>(
        eval_status.value());
}

absl::StatusOr<std::shared_ptr<google::api::expr::runtime::CelExpression>>
CelExecutor::Impl::getOrCompileExpression(const std::string &expr) {
    // Thread-safe cache lookup
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = expression_cache_.find(expr);
        if (it != expression_cache_.end()) {
            // Return shared_ptr from cache
            return it->second;
        }
    }

    // Compile the expression using the runtime builder
    if (!runtime_) {
        return absl::FailedPreconditionError("CEL runtime not initialized");
    }

    auto pexpr_or = google::api::expr::parser::Parse(expr);
    if (!pexpr_or.ok()) {
        return pexpr_or.status();
    }
    auto pexpr = std::move(pexpr_or).value();
    auto expr_or =
        runtime_->CreateExpression(&pexpr.expr(), &pexpr.source_info());
    if (!expr_or.ok()) {
        return expr_or.status();
    }

    auto compiled_expr = std::move(expr_or.value());

    // Cache the compiled expression using shared_ptr
    std::shared_ptr<google::api::expr::runtime::CelExpression> shared_expr(
        compiled_expr.release());
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        expression_cache_[expr] = shared_expr;
    }

    return shared_expr;
}

google::api::expr::runtime::CelValue CelExecutor::Impl::fromSerdeValue(
    const SerdeValue &value, google::protobuf::Arena *arena) {
    switch (value.getFormat()) {
        case SerdeFormat::Json: {
            auto json_value = schemaregistry::serdes::json::asJson(value);
            return utils::fromJsonValue(json_value, arena);
        }
#ifdef SCHEMAREGISTRY_USE_AVRO
        case SerdeFormat::Avro: {
            auto avro_value = schemaregistry::serdes::avro::asAvro(value);
            return utils::fromAvroValue(avro_value, arena);
        }
#endif
        case SerdeFormat::Protobuf: {
            auto &proto_variant =
                schemaregistry::serdes::protobuf::asProtobuf(value);
            return utils::fromProtobufValue(proto_variant, arena);
        }
        default:
            return google::api::expr::runtime::CelValue::CreateNull();
    }
}

std::unique_ptr<SerdeValue> CelExecutor::Impl::toSerdeValue(
    const SerdeValue &original,
    const google::api::expr::runtime::CelValue &cel_value) {
    switch (original.getFormat()) {
        case SerdeFormat::Json: {
            auto original_json = schemaregistry::serdes::json::asJson(original);
            auto converted_json = utils::toJsonValue(original_json, cel_value);
            return schemaregistry::serdes::json::makeJsonValue(converted_json);
        }
#ifdef SCHEMAREGISTRY_USE_AVRO
        case SerdeFormat::Avro: {
            auto original_avro = schemaregistry::serdes::avro::asAvro(original);
            auto converted_avro = utils::toAvroValue(original_avro, cel_value);
            return schemaregistry::serdes::avro::makeAvroValue(converted_avro);
        }
#endif
        case SerdeFormat::Protobuf: {
            auto &proto_variant =
                schemaregistry::serdes::protobuf::asProtobuf(original);
            auto converted_proto_variant =
                utils::toProtobufValue(proto_variant, cel_value);
            return schemaregistry::serdes::protobuf::makeProtobufValue(
                std::move(converted_proto_variant));
        }
        default:
            // For unknown formats, return a copy of the original
            return original.clone();
    }
}

void CelExecutor::registerExecutor() {
    // Register this executor with the global rule registry
    // This matches the Rust version:
    // crate::serdes::rule_registry::register_rule_executor(CelExecutor::new());
    global_registry::registerRuleExecutor(std::make_shared<CelExecutor>());
}

}  // namespace schemaregistry::rules::cel