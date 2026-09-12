#include "schemaregistry/rules/cel/CelExecutor.h"

#include <any>
#include <regex>

#include "absl/strings/str_split.h"
#include "common/ast_proto.h"
#include "common/legacy_value.h"
#include "extensions/strings.h"
#include "eval/public/containers/container_backed_list_impl.h"
#include "eval/public/containers/container_backed_map_impl.h"
#include "runtime/activation.h"
#include "runtime/constant_folding.h"
#include "runtime/reference_resolver.h"
#include "runtime/regex_precompilation.h"
#include "runtime/runtime_options.h"
#include "runtime/standard_runtime_builder_factory.h"
#include "eval/public/structs/cel_proto_wrapper.h"
#include "extensions/math_ext.h"
#include "extensions/math_ext_macros.h"
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
// Pulls in avro/Generic.hh; rules-without-Avro is a supported build, so it follows the guard.
#include "schemaregistry/rules/cel/AvroResultWriter.h"
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

absl::StatusOr<std::unique_ptr<const ::cel::Runtime>>
CelExecutor::Impl::newRuleBuilder(google::protobuf::Arena *arena) {
    ::cel::RuntimeOptions options;
    options.enable_qualified_type_identifiers = true;
    options.enable_timestamp_duration_overflow_errors = true;
    options.enable_heterogeneous_equality = true;
    options.enable_empty_wrapper_null_unboxing = true;

    // CreateStandardRuntimeBuilder registers the whole standard set. Subsetting it - via the
    // bare CreateRuntimeBuilder plus the individual runtime/standard registrars - was tried in
    // order to take over `@in` for decimals, and does not work: with
    // enable_heterogeneous_equality the planner installs its own call handler for @in / in / _in_
    // (eval/compiler/flat_expr_builder.cc, HandleHeterogeneousEqualityIn) and emits a direct
    // interpretable, so membership never reaches the function registry at all. Note that _==_ is
    // treated differently there - the planner only intercepts it when no (any, any) overload is
    // registered, which is exactly the hole registerEquality uses. There is no equivalent
    // detection for @in, in 0.11 or in 0.16.
    auto builder_or = ::cel::CreateStandardRuntimeBuilder(
        google::protobuf::DescriptorPool::generated_pool(), options);
    if (!builder_or.ok()) {
        return builder_or.status();
    }
    ::cel::RuntimeBuilder builder = std::move(builder_or).value();

    // The modern equivalent of the legacy enable_qualified_identifier_rewrites option. The math
    // extension's functions are registered under namespaced names (math.abs, math.bitAnd), and
    // without this the parser's receiver-style call - `abs` with the target `math` - is never
    // folded into that name and every one of them fails to plan. kAlways, not
    // kCheckedExpressionOnly: these expressions are parse-only.
    auto status = ::cel::EnableReferenceResolver(
        builder, ::cel::ReferenceResolverEnabled::kAlways);
    if (!status.ok()) {
        return status;
    }
    // Replaces the legacy constant_folding / constant_arena and
    // enable_regex_precompilation options, which are builder steps in the modern API.
    status = ::cel::extensions::EnableConstantFolding(builder, arena);
    if (!status.ok()) {
        return status;
    }
    status = ::cel::extensions::EnableRegexPrecompilation(builder);
    if (!status.ok()) {
        return status;
    }

    status = ::cel::extensions::RegisterStringsFunctions(
        builder.function_registry(), options);
    if (!status.ok()) {
        return status;
    }
    status = ::cel::extensions::RegisterMathExtensionFunctions(
        builder.function_registry(), options);
    if (!status.ok()) {
        return status;
    }
    // Our custom extra functions. Still legacy CelFunction implementations, which register
    // directly into the modern registry - see RegisterExtraFuncs.
    status = RegisterExtraFuncs(builder.function_registry(), arena);
    if (!status.ok()) {
        return status;
    }

    return std::move(builder).Build();
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
        // A CONDITION's result is a verdict, not data, so it must not go through a result
        // writer - which is what the JVM does with
        // `if (ctx.rule().getKind() == RuleKind.CONDITION) return result;`, ahead of every
        // writer. Without the check a condition's bool was converted against the message it
        // was validating. That was harmless only while those conversions had a silent
        // fallback for a shape they did not recognise; once they began reporting the
        // mismatch, a condition over a decimal or timestamp field started failing as though
        // the rule had tried to write to it.
        if (ctx.getRule().getKind().value_or(Kind::Transform) == Kind::Condition) {
            return makeVerdict(msg, *result);
        }
        return toSerdeValue(ctx, msg, *result);
    }

    return nullptr;
}

std::unique_ptr<SerdeValue> CelExecutor::Impl::makeVerdict(
    const SerdeValue &msg,
    const google::api::expr::runtime::CelValue &result) {
    // Only the verdict itself is carried over, in the message's own format so that the
    // caller's asBool() reads it. A non-bool result stays a failure, as on the JVM, where
    // anything that is not Boolean.TRUE fails the rule.
    const bool verdict = result.IsBool() && result.BoolOrDie();
    switch (msg.getFormat()) {
        case SerdeFormat::Json:
            return schemaregistry::serdes::json::makeJsonValue(
                nlohmann::json(verdict));
#ifdef SCHEMAREGISTRY_USE_AVRO
        case SerdeFormat::Avro:
            return schemaregistry::serdes::avro::makeAvroValue(
                ::avro::GenericDatum(verdict));
#endif
        case SerdeFormat::Protobuf:
            return schemaregistry::serdes::protobuf::makeProtobufValue(
                schemaregistry::serdes::protobuf::ProtobufVariant(verdict));
        default:
            return msg.clone();
    }
}

std::unique_ptr<google::api::expr::runtime::CelValue>
CelExecutor::Impl::executeRule(
    RuleContext &ctx, const SerdeValue &msg, const std::string &expr,
    const absl::flat_hash_map<std::string, google::api::expr::runtime::CelValue>
        &args,
    google::protobuf::Arena *arena) {
    return evaluate(expr, args, arena);
}

std::unique_ptr<google::api::expr::runtime::CelValue>
CelExecutor::Impl::evaluate(
    const std::string &expr,
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

    // Bindings arrive as legacy CelValue from the format converters in CelUtils, so each is
    // adapted on the way in and the result adapted back on the way out. Supported for
    // protobuf-backed values, which is all this client produces.
    ::cel::Activation activation;
    for (const auto &pair : args) {
        auto modern = ::cel::ModernValue(arena, pair.second);
        if (!modern.ok()) {
            throw SerdeError("CEL binding conversion failed for '" + pair.first +
                             "': " + std::string(modern.status().message()));
        }
        activation.InsertOrAssignValue(pair.first, std::move(modern).value());
    }

    // Note the argument order: the modern Evaluate takes the arena first.
    auto eval_status = parsed_expr->Evaluate(arena, activation);
    if (!eval_status.ok()) {
        throw SerdeError("CEL evaluation failed: " +
                         std::string(eval_status.status().message()));
    }

    auto legacy = ::cel::LegacyValue(arena, eval_status.value());
    if (!legacy.ok()) {
        throw SerdeError("CEL result conversion failed: " +
                         std::string(legacy.status().message()));
    }
    auto value = std::make_unique<google::api::expr::runtime::CelValue>(
        std::move(legacy).value());
    // cel-cpp reports a *runtime* failure as an error Value carrying an OK status - a failed
    // conversion, an unresolved overload - so the eval_status check above does not see it.
    // Left unchecked, the error CelValue reached toAvroValue, which has no arm for it and
    // returns its input unchanged: a message-level condition then neither passed, failed nor
    // errored and the record went out as it came in (the only silent wrong answer in the
    // C8/C9 sweep), and a field rule over a null quietly passed.
    if (value->IsError()) {
        const absl::Status *err = value->ErrorOrDie();
        throw SerdeError("CEL evaluation failed: " +
                         std::string(err != nullptr ? err->message() : "unknown error"));
    }
    return value;
}

absl::StatusOr<std::shared_ptr<const ::cel::Program>>
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

    // math.greatest and math.least are macros rather than registry functions -
    // they take a variable number of arguments - so the parser has to know them
    // as well. Parsing with an explicit macro list replaces the default set, so
    // the standard macros (has, all, exists, exists_one, map, filter) are
    // carried along with them.
    static const std::vector<::cel::Macro> *kMacros = [] {
        auto *macros = new std::vector<::cel::Macro>(::cel::Macro::AllMacros());
        auto math = ::cel::extensions::math_macros();
        macros->insert(macros->end(), math.begin(), math.end());
        return macros;
    }();

    auto pexpr_or = google::api::expr::parser::ParseWithMacros(expr, *kMacros);
    if (!pexpr_or.ok()) {
        return pexpr_or.status();
    }
    auto pexpr = std::move(pexpr_or).value();
    // The modern runtime plans from a cel::Ast rather than from the parsed protobuf, so the
    // parse result is converted rather than passed by pointer.
    auto ast_or = ::cel::CreateAstFromParsedExpr(pexpr.expr(), &pexpr.source_info());
    if (!ast_or.ok()) {
        return ast_or.status();
    }
    auto expr_or = runtime_->CreateProgram(std::move(ast_or).value());
    if (!expr_or.ok()) {
        return expr_or.status();
    }

    std::shared_ptr<const ::cel::Program> shared_expr(
        std::move(expr_or).value().release());
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
    schemaregistry::serdes::RuleContext &ctx, const SerdeValue &original,
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
            // With the field's schema, the result is converted against the slot rather than
            // against the datum: a union resolves to the branch that accepts it, so a rule can
            // fill a null branch - where the datum's own type is null and says nothing.
            auto field_ctx = ctx.currentField();
            if (field_ctx.has_value()) {
                const auto *node = std::any_cast<::avro::NodePtr>(
                    &field_ctx->getFieldDescriptor());
                if (node != nullptr && *node != nullptr) {
                    return schemaregistry::serdes::avro::makeAvroValue(
                        utils::avroValueForField(field_ctx->getFullName(), *node,
                                                 original_avro, cel_value));
                }
            }
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
