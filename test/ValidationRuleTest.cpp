/**
 * ValidationRuleTest
 * Tests for inline validation rules ("confluent:rules" / confluent.Meta rules)
 */

#include <gtest/gtest.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "schemaregistry/rest/ClientConfiguration.h"
#include "schemaregistry/rest/SchemaRegistryClient.h"
#include "schemaregistry/rest/model/Rule.h"
#include "schemaregistry/rest/model/RuleSet.h"
#include "schemaregistry/rest/model/Schema.h"
#include "schemaregistry/rules/cel/CelExecutor.h"
#include "schemaregistry/rules/cel/CelValidator.h"
#include "schemaregistry/serdes/RuleRegistry.h"
#include "schemaregistry/serdes/SerdeConfig.h"
#include "schemaregistry/serdes/ValidationRule.h"
#include "schemaregistry/serdes/json/JsonValue.h"

#ifdef SCHEMAREGISTRY_USE_AVRO
#include <avro/Generic.hh>
#include <avro/ValidSchema.hh>

#include "schemaregistry/serdes/avro/AvroSerializer.h"
#include "schemaregistry/serdes/avro/AvroUtils.h"
#endif

#ifdef SCHEMAREGISTRY_USE_JSON
#include "schemaregistry/serdes/json/JsonSerializer.h"
#include "schemaregistry/serdes/json/JsonUtils.h"
#endif

#ifdef SCHEMAREGISTRY_USE_PROTOBUF
#include "schemaregistry/serdes/protobuf/ProtobufSerializer.h"
#include "schemaregistry/serdes/protobuf/ProtobufUtils.h"
#include "test/validation.pb.h"
#endif

using namespace schemaregistry::serdes;
using namespace schemaregistry::rest;
using namespace schemaregistry::rest::model;
using schemaregistry::rules::cel::CelValidator;

namespace {

ValidationRule rule(const std::string &name, const std::string &expr,
                    const std::string &doc = "") {
    return ValidationRule{name, doc, expr, ""};
}

std::shared_ptr<ISchemaRegistryClient> newMockClient() {
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    return SchemaRegistryClient::newClient(client_config);
}

// Locates a violation by rule name; returns nullptr when there is none.
const ValidationRuleError *findViolation(
    const std::vector<ValidationRuleError> &violations,
    const std::string &name) {
    for (const auto &violation : violations) {
        if (violation.rule.name == name) {
            return &violation;
        }
    }
    return nullptr;
}

SerializationContext valueContext(SerdeFormat format) {
    SerializationContext ctx;
    ctx.topic = "test";
    ctx.serde_type = SerdeType::Value;
    ctx.serde_format = format;
    return ctx;
}

}  // namespace

// --- CelValidator ---------------------------------------------------------

TEST(ValidationRuleTest, CelValidatorReturnsBool) {
    CelValidator validator;
    auto value = json::makeJsonValue(nlohmann::json{{"name", "alice"}});

    auto passing = validator.execute(rule("n", "this.name == 'alice'"), *value);
    ASSERT_TRUE(std::holds_alternative<bool>(passing));
    EXPECT_TRUE(std::get<bool>(passing));

    auto failing = validator.execute(rule("n", "this.name == 'bob'"), *value);
    ASSERT_TRUE(std::holds_alternative<bool>(failing));
    EXPECT_FALSE(std::get<bool>(failing));
}

TEST(ValidationRuleTest, CelValidatorReturnsMessage) {
    CelValidator validator;
    auto value = json::makeJsonValue(nlohmann::json{{"age", 3}});

    auto result = validator.execute(
        rule("n", "this.age >= 18 ? '' : 'must be an adult'"), *value);
    ASSERT_TRUE(std::holds_alternative<std::string>(result));
    EXPECT_EQ(std::get<std::string>(result), "must be an adult");
}

TEST(ValidationRuleTest, CelValidatorRejectsNonBooleanResult) {
    CelValidator validator;
    auto value = json::makeJsonValue(nlohmann::json{{"age", 3}});

    EXPECT_THROW(validator.execute(rule("n", "this.age"), *value), SerdeError);
}

TEST(ValidationRuleTest, CelValidatorRejectsEmptyExpression) {
    CelValidator validator;
    auto value = json::makeJsonValue(nlohmann::json{{"age", 3}});

    EXPECT_THROW(validator.execute(rule("n", ""), *value), SerdeError);
}

TEST(ValidationRuleTest, FailedRuleBecomesAViolationWithItsCause) {
    CelValidator validator;
    auto value = json::makeJsonValue(nlohmann::json{{"age", 3}});
    std::vector<ValidationRuleError> violations;

    // A rule that cannot be evaluated is recorded as a violation rather than
    // aborting the walk.
    EXPECT_TRUE(evaluateValidationRule(validator, rule("bad", "this.missing"),
                                       *value, "$", violations));
    ASSERT_EQ(violations.size(), 1);
    EXPECT_EQ(violations[0].field_path, "$");
    EXPECT_FALSE(violations[0].cause.empty());
}

TEST(ValidationRuleTest, ViolationMessagePrefersDynamicThenDocThenExpr) {
    ValidationRuleError with_message{rule("r", "expr", "doc"), "a.b", "dynamic",
                                     ""};
    EXPECT_EQ(with_message.toString(), "a.b: r: dynamic");

    ValidationRuleError with_doc{rule("r", "expr", "doc"), "a.b", "", ""};
    EXPECT_EQ(with_doc.toString(), "a.b: r: doc");

    ValidationRuleError with_expr{rule("r", "expr"), "", "", ""};
    EXPECT_EQ(with_expr.toString(), "<root>: r: expr");

    ValidationRuleError unnamed{rule("", "expr"), "", "", "boom"};
    EXPECT_EQ(unnamed.toString(), "<root>: unnamed: expr (caused by: boom)");
}

TEST(ValidationRuleTest, AggregatedErrorListsEveryViolation) {
    std::vector<ValidationRuleError> violations = {
        {rule("a", "e1"), "x", "", ""}, {rule("b", "e2"), "y", "", ""}};
    try {
        raiseValidationViolations(violations);
        FAIL() << "expected ValidationRulesFailedError";
    } catch (const ValidationRulesFailedError &e) {
        EXPECT_EQ(e.getViolations().size(), 2);
        std::string message = e.what();
        EXPECT_NE(message.find("2 violations"), std::string::npos);
        EXPECT_NE(message.find("x: a: e1"), std::string::npos);
        EXPECT_NE(message.find("y: b: e2"), std::string::npos);
    }
}

TEST(ValidationRuleTest, NoViolationsRaisesNothing) {
    EXPECT_NO_THROW(raiseValidationViolations({}));
}

TEST(ValidationRuleTest, ParsesRulesFromSchemaProperty) {
    auto rules = parseValidationRules(nlohmann::json::parse(R"([
        {"name": "a", "doc": "d", "expr": "e", "sql": "s"},
        {"name": "b"},
        "not an object"
    ])"));
    ASSERT_EQ(rules.size(), 2);
    EXPECT_EQ(rules[0].name, "a");
    EXPECT_EQ(rules[0].doc, "d");
    EXPECT_EQ(rules[0].expr, "e");
    EXPECT_EQ(rules[0].sql, "s");
    EXPECT_EQ(rules[1].name, "b");
    EXPECT_TRUE(rules[1].expr.empty());

    EXPECT_TRUE(parseValidationRules(nlohmann::json::object()).empty());
}

TEST(ValidationRuleTest, ParsesExecutionMode) {
    EXPECT_EQ(parseValidationRulesExecution("DISABLED"),
              ValidationRulesExecution::Disabled);
    EXPECT_EQ(parseValidationRulesExecution("BEFORE_DOMAIN_RULES"),
              ValidationRulesExecution::BeforeDomainRules);
    EXPECT_EQ(parseValidationRulesExecution("AFTER_DOMAIN_RULES"),
              ValidationRulesExecution::AfterDomainRules);
    EXPECT_FALSE(parseValidationRulesExecution("nonsense").has_value());
}

TEST(ValidationRuleTest, ValidationIsDisabledByDefault) {
    EXPECT_EQ(SerializerConfig::createDefault().validation_rules_execution,
              ValidationRulesExecution::Disabled);
    EXPECT_FALSE(SerializerConfig::createDefault().validation_rules_fail_fast);
}

// --- Avro -----------------------------------------------------------------

#ifdef SCHEMAREGISTRY_USE_AVRO

namespace {

const char *kAvroSchema = R"schema({
    "type": "record",
    "name": "Order",
    "namespace": "test",
    "confluent:rules": [
        {"name": "quantity_matches_items",
         "expr": "this.quantity == size(this.items)"}
    ],
    "fields": [
        {"name": "id", "type": "string",
         "confluent:rules": [
            {"name": "id_prefix", "expr": "this.startsWith('ord-')"},
            {"name": "id_length", "expr": "size(this) > 4 ? '' : 'id is too short'"}
         ]},
        {"name": "quantity", "type": "int",
         "confluent:rules": [
            {"name": "positive_quantity", "doc": "quantity must be positive",
             "expr": "this > 0"}
         ]},
        {"name": "items", "type": {"type": "array", "items": "string"}},
        {"name": "note", "type": ["null", "string"],
         "confluent:rules": [
            {"name": "note_not_empty", "expr": "size(this) > 0"}
         ]},
        {"name": "address", "type": {
            "type": "record",
            "name": "Address",
            "fields": [
                {"name": "zip", "type": "string",
                 "confluent:rules": [
                    {"name": "zip_digits",
                     "expr": "this.matches('^[0-9]{5}$') ? '' : 'zip must be 5 digits'"}
                 ]}
            ]
        }}
    ]
})schema";

::avro::GenericDatum makeAvroOrder(const std::string &id, int32_t quantity,
                                   const std::vector<std::string> &items,
                                   const std::string &zip,
                                   bool with_note = false,
                                   const std::string &note = "") {
    auto schema =
        schemaregistry::serdes::avro::AvroSerializer::compileJsonSchema(
            kAvroSchema);
    ::avro::GenericDatum datum(schema);
    auto &record = datum.value<::avro::GenericRecord>();
    record.setFieldAt(0, ::avro::GenericDatum(id));
    record.setFieldAt(1, ::avro::GenericDatum(quantity));

    auto &items_datum = record.fieldAt(2);
    auto &array = items_datum.value<::avro::GenericArray>();
    for (const auto &item : items) {
        array.value().push_back(::avro::GenericDatum(item));
    }

    if (with_note) {
        auto &note_datum = record.fieldAt(3);
        note_datum.selectBranch(1);
        note_datum.value<std::string>() = note;
    }

    auto &address = record.fieldAt(4).value<::avro::GenericRecord>();
    address.setFieldAt(0, ::avro::GenericDatum(zip));
    return datum;
}

std::vector<ValidationRuleError> validateAvro(const ::avro::GenericDatum &datum,
                                              bool fail_fast = false) {
    CelValidator validator;
    return schemaregistry::serdes::avro::utils::validateMessage(
        validator, nlohmann::json::parse(kAvroSchema), datum, fail_fast);
}

}  // namespace

TEST(ValidationRuleTest, AvroValidRecordHasNoViolations) {
    auto datum = makeAvroOrder("ord-1234", 2, {"a", "b"}, "12345");
    EXPECT_TRUE(validateAvro(datum).empty());
}

TEST(ValidationRuleTest, AvroCollectsEveryViolation) {
    // Fails: id prefix, id length, quantity, record-level count, nested zip.
    auto datum = makeAvroOrder("x", 0, {"a"}, "abc");
    auto violations = validateAvro(datum);

    ASSERT_EQ(violations.size(), 5);
    EXPECT_EQ(violations[0].rule.name, "quantity_matches_items");
    EXPECT_EQ(violations[0].field_path, "");
    EXPECT_EQ(violations[1].rule.name, "id_prefix");
    EXPECT_EQ(violations[1].field_path, "id");
    EXPECT_EQ(violations[2].rule.name, "id_length");
    EXPECT_EQ(violations[2].message, "id is too short");
    EXPECT_EQ(violations[3].rule.name, "positive_quantity");
    EXPECT_EQ(violations[3].field_path, "quantity");
    EXPECT_EQ(violations[4].rule.name, "zip_digits");
    EXPECT_EQ(violations[4].field_path, "address.zip");
    EXPECT_EQ(violations[4].message, "zip must be 5 digits");
}

TEST(ValidationRuleTest, AvroFailFastStopsAtFirstViolation) {
    auto datum = makeAvroOrder("x", 0, {"a"}, "abc");
    EXPECT_EQ(validateAvro(datum, true).size(), 1);
}

TEST(ValidationRuleTest, AvroSkipsRulesOnNullFields) {
    // note is null, so note_not_empty must not be invoked.
    auto datum = makeAvroOrder("ord-1234", 2, {"a", "b"}, "12345");
    EXPECT_TRUE(validateAvro(datum).empty());

    auto with_note =
        makeAvroOrder("ord-1234", 2, {"a", "b"}, "12345", true, "");
    auto violations = validateAvro(with_note);
    ASSERT_EQ(violations.size(), 1);
    EXPECT_EQ(violations[0].rule.name, "note_not_empty");
    EXPECT_EQ(violations[0].field_path, "note");
}

TEST(ValidationRuleTest, AvroSerializerRejectsInvalidMessage) {
    auto client = newMockClient();
    auto ser_config = SerializerConfig::createDefault();
    ser_config.validation_rules_execution =
        ValidationRulesExecution::AfterDomainRules;

    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("AVRO"));
    schema.setSchema(std::make_optional<std::string>(kAvroSchema));

    auto rule_registry = std::make_shared<RuleRegistry>();
    rule_registry->registerValidationExecutor(std::make_shared<CelValidator>());

    schemaregistry::serdes::avro::AvroSerializer serializer(
        client, std::make_optional(schema), rule_registry, ser_config);
    auto ctx = valueContext(SerdeFormat::Avro);

    EXPECT_NO_THROW(serializer.serialize(
        ctx, makeAvroOrder("ord-1234", 2, {"a", "b"}, "12345")));
    EXPECT_THROW(
        serializer.serialize(ctx, makeAvroOrder("bad", 2, {"a", "b"}, "12345")),
        ValidationRulesFailedError);
}

TEST(ValidationRuleTest, AvroSerializerSkipsValidationWhenDisabled) {
    auto client = newMockClient();
    auto ser_config = SerializerConfig::createDefault();

    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("AVRO"));
    schema.setSchema(std::make_optional<std::string>(kAvroSchema));

    auto rule_registry = std::make_shared<RuleRegistry>();
    rule_registry->registerValidationExecutor(std::make_shared<CelValidator>());

    schemaregistry::serdes::avro::AvroSerializer serializer(
        client, std::make_optional(schema), rule_registry, ser_config);
    auto ctx = valueContext(SerdeFormat::Avro);

    EXPECT_NO_THROW(serializer.serialize(
        ctx, makeAvroOrder("bad", 2, {"a", "b"}, "12345")));
}

namespace {

// Registers a schema whose domain rule always fails, so that the order of the
// two failures tells us which phase ran first.
void registerSchemaWithFailingDomainRule(
    const std::shared_ptr<ISchemaRegistryClient> &client) {
    Rule cel_rule;
    cel_rule.setName(std::make_optional<std::string>("always-fails"));
    cel_rule.setKind(std::make_optional<Kind>(Kind::Condition));
    cel_rule.setMode(std::make_optional<Mode>(Mode::Write));
    cel_rule.setType(std::make_optional<std::string>("CEL"));
    cel_rule.setExpr(std::make_optional<std::string>("message.quantity > 100"));
    RuleSet rule_set;
    rule_set.setDomainRules(std::make_optional<std::vector<Rule>>({cel_rule}));

    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("AVRO"));
    schema.setSchema(std::make_optional<std::string>(kAvroSchema));
    schema.setRuleSet(std::make_optional<RuleSet>(rule_set));
    client->registerSchema("test-value", schema, false);
}

std::shared_ptr<RuleRegistry> newValidatingRegistry() {
    auto rule_registry = std::make_shared<RuleRegistry>();
    rule_registry->registerExecutor(
        std::make_shared<schemaregistry::rules::cel::CelExecutor>());
    rule_registry->registerValidationExecutor(std::make_shared<CelValidator>());
    return rule_registry;
}

SerializerConfig latestVersionConfig(ValidationRulesExecution execution) {
    auto config = SerializerConfig(
        false, std::make_optional(SchemaSelector::useLatestVersion()), true,
        false, std::unordered_map<std::string, std::string>{});
    config.validation_rules_execution = execution;
    return config;
}

}  // namespace

TEST(ValidationRuleTest, AvroSerializerRunsValidationBeforeDomainRules) {
    auto client = newMockClient();
    registerSchemaWithFailingDomainRule(client);

    schemaregistry::serdes::avro::AvroSerializer serializer(
        client, std::nullopt, newValidatingRegistry(),
        latestVersionConfig(ValidationRulesExecution::BeforeDomainRules));
    auto ctx = valueContext(SerdeFormat::Avro);

    // Both the inline rules and the domain rule fail; validation first means
    // the validation failure is what surfaces.
    EXPECT_THROW(
        serializer.serialize(ctx, makeAvroOrder("bad", 2, {"a", "b"}, "12345")),
        ValidationRulesFailedError);
}

TEST(ValidationRuleTest, AvroSerializerRunsValidationAfterDomainRules) {
    auto client = newMockClient();
    registerSchemaWithFailingDomainRule(client);

    schemaregistry::serdes::avro::AvroSerializer serializer(
        client, std::nullopt, newValidatingRegistry(),
        latestVersionConfig(ValidationRulesExecution::AfterDomainRules));
    auto ctx = valueContext(SerdeFormat::Avro);

    // Same message, but the domain rule now runs first, so its failure is what
    // surfaces instead of the validation failure.
    try {
        serializer.serialize(ctx, makeAvroOrder("bad", 2, {"a", "b"}, "12345"));
        FAIL() << "expected the domain rule to fail";
    } catch (const SerdeError &e) {
        EXPECT_EQ(dynamic_cast<const ValidationRulesFailedError *>(&e),
                  nullptr);
    }
}

#endif  // SCHEMAREGISTRY_USE_AVRO

// --- JSON Schema ----------------------------------------------------------

#ifdef SCHEMAREGISTRY_USE_JSON

namespace {

const char *kJsonSchema = R"schema({
    "type": "object",
    "confluent:rules": [
        {"name": "quantity_matches_items",
         "expr": "this.quantity == size(this.items)"}
    ],
    "properties": {
        "id": {
            "type": "string",
            "confluent:rules": [
                {"name": "id_prefix", "expr": "this.startsWith('ord-')"}
            ]
        },
        "quantity": {
            "type": "integer",
            "confluent:rules": [
                {"name": "positive_quantity", "expr": "this > 0"}
            ]
        },
        "items": {"type": "array", "items": {"type": "string"}},
        "address": {"$ref": "#/definitions/Address"}
    },
    "definitions": {
        "Address": {
            "type": "object",
            "properties": {
                "zip": {
                    "type": "string",
                    "confluent:rules": [
                        {"name": "zip_digits",
                         "expr": "this.matches('^[0-9]{5}$') ? '' : 'zip must be 5 digits'"}
                    ]
                }
            }
        }
    }
})schema";

std::vector<ValidationRuleError> validateJson(const nlohmann::json &value,
                                              bool fail_fast = false) {
    CelValidator validator;
    return schemaregistry::serdes::json::utils::validateMessage(
        validator, nlohmann::json::parse(kJsonSchema), value, fail_fast);
}

nlohmann::json jsonOrder(const std::string &id, int quantity,
                         const std::vector<std::string> &items,
                         const std::string &zip) {
    return nlohmann::json{{"id", id},
                          {"quantity", quantity},
                          {"items", items},
                          {"address", {{"zip", zip}}}};
}

}  // namespace

TEST(ValidationRuleTest, JsonValidObjectHasNoViolations) {
    EXPECT_TRUE(
        validateJson(jsonOrder("ord-1", 2, {"a", "b"}, "12345")).empty());
}

TEST(ValidationRuleTest, JsonCollectsEveryViolationWithDollarRootedPaths) {
    auto violations = validateJson(jsonOrder("x", 0, {"a"}, "abc"));

    // Properties are walked in the instance's key order, so assert on the set
    // of violations rather than their sequence.
    ASSERT_EQ(violations.size(), 4);
    ASSERT_NE(findViolation(violations, "quantity_matches_items"), nullptr);
    EXPECT_EQ(findViolation(violations, "quantity_matches_items")->field_path,
              "$");
    ASSERT_NE(findViolation(violations, "id_prefix"), nullptr);
    EXPECT_EQ(findViolation(violations, "id_prefix")->field_path, "$.id");
    ASSERT_NE(findViolation(violations, "positive_quantity"), nullptr);
    EXPECT_EQ(findViolation(violations, "positive_quantity")->field_path,
              "$.quantity");
    // Resolved through "$ref".
    ASSERT_NE(findViolation(violations, "zip_digits"), nullptr);
    EXPECT_EQ(findViolation(violations, "zip_digits")->field_path,
              "$.address.zip");
}

TEST(ValidationRuleTest, JsonFailFastStopsAtFirstViolation) {
    EXPECT_EQ(validateJson(jsonOrder("x", 0, {"a"}, "abc"), true).size(), 1);
}

TEST(ValidationRuleTest, JsonSkipsAbsentAndNullProperties) {
    nlohmann::json value{{"quantity", 1}, {"items", {"a"}}};
    EXPECT_TRUE(validateJson(value).empty());

    value["id"] = nullptr;
    EXPECT_TRUE(validateJson(value).empty());
}

TEST(ValidationRuleTest, JsonSerializerRejectsInvalidMessage) {
    auto client = newMockClient();
    auto ser_config = SerializerConfig::createDefault();
    ser_config.validation_rules_execution =
        ValidationRulesExecution::AfterDomainRules;

    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("JSON"));
    schema.setSchema(std::make_optional<std::string>(kJsonSchema));

    auto rule_registry = std::make_shared<RuleRegistry>();
    rule_registry->registerValidationExecutor(std::make_shared<CelValidator>());

    schemaregistry::serdes::json::JsonSerializer serializer(
        client, std::make_optional(schema), rule_registry, ser_config);
    auto ctx = valueContext(SerdeFormat::Json);

    EXPECT_NO_THROW(
        serializer.serialize(ctx, jsonOrder("ord-1", 2, {"a", "b"}, "12345")));
    EXPECT_THROW(
        serializer.serialize(ctx, jsonOrder("bad", 2, {"a", "b"}, "12345")),
        ValidationRulesFailedError);
}

#endif  // SCHEMAREGISTRY_USE_JSON

// --- Protobuf -------------------------------------------------------------

#ifdef SCHEMAREGISTRY_USE_PROTOBUF

namespace {

test::ValidationOrder protoOrder(const std::string &id, int32_t quantity,
                                 const std::vector<std::string> &items,
                                 const std::string &zip) {
    test::ValidationOrder order;
    order.set_id(id);
    order.set_quantity(quantity);
    for (const auto &item : items) {
        order.add_items(item);
    }
    order.mutable_address()->set_zip(zip);
    return order;
}

std::vector<ValidationRuleError> validateProto(
    const google::protobuf::Message &message, bool fail_fast = false) {
    CelValidator validator;
    return schemaregistry::serdes::protobuf::utils::validateMessage(
        validator, message, fail_fast);
}

}  // namespace

TEST(ValidationRuleTest, ProtobufValidMessageHasNoViolations) {
    EXPECT_TRUE(
        validateProto(protoOrder("ord-1234", 2, {"a", "b"}, "12345")).empty());
}

TEST(ValidationRuleTest, ProtobufCollectsEveryViolation) {
    auto order = protoOrder("x", -1, {"a", ""}, "abc");
    auto violations = validateProto(order);

    // Fields are walked in declaration order: id, quantity, items, address.
    ASSERT_EQ(violations.size(), 6);
    EXPECT_EQ(violations[0].rule.name, "quantity_matches_items");
    EXPECT_EQ(violations[0].field_path, "");
    EXPECT_EQ(violations[1].rule.name, "id_prefix");
    EXPECT_EQ(violations[1].field_path, "id");
    EXPECT_EQ(violations[2].rule.name, "id_length");
    EXPECT_EQ(violations[2].message, "id is too short");
    EXPECT_EQ(violations[3].rule.name, "positive_quantity");
    EXPECT_EQ(violations[3].field_path, "quantity");
    // Repeated fields are validated element by element.
    EXPECT_EQ(violations[4].rule.name, "item_not_empty");
    EXPECT_EQ(violations[4].field_path, "items[1]");
    EXPECT_EQ(violations[5].rule.name, "zip_digits");
    EXPECT_EQ(violations[5].field_path, "address.zip");
}

TEST(ValidationRuleTest, ProtobufValidatesNestedMessagesAndMapValues) {
    auto order = protoOrder("ord-1234", 1, {"a"}, "abc");
    (*order.mutable_scores())["good"] = 1;
    (*order.mutable_scores())["bad"] = -1;
    auto violations = validateProto(order);

    ASSERT_EQ(violations.size(), 2);
    EXPECT_EQ(violations[0].rule.name, "zip_digits");
    EXPECT_EQ(violations[0].field_path, "address.zip");
    EXPECT_EQ(violations[1].rule.name, "score_not_negative");
    EXPECT_EQ(violations[1].field_path, "scores[\"bad\"]");
}

TEST(ValidationRuleTest, ProtobufFailFastStopsAtFirstViolation) {
    EXPECT_EQ(validateProto(protoOrder("x", -1, {"a", ""}, "abc"), true).size(),
              1);
}

TEST(ValidationRuleTest, ProtobufSkipsRulesOnUnsetMessageFields) {
    // address is unset, so the nested zip rule must not be invoked.
    test::ValidationOrder order;
    order.set_id("ord-1234");
    order.set_quantity(1);
    order.add_items("a");
    EXPECT_TRUE(validateProto(order).empty());
}

TEST(ValidationRuleTest, ProtobufSerializerRejectsInvalidMessage) {
    auto client = newMockClient();
    auto ser_config = SerializerConfig::createDefault();
    ser_config.validation_rules_execution =
        ValidationRulesExecution::AfterDomainRules;

    auto rule_registry = std::make_shared<RuleRegistry>();
    rule_registry->registerValidationExecutor(std::make_shared<CelValidator>());

    schemaregistry::serdes::protobuf::ProtobufSerializer<test::ValidationOrder>
        serializer(client, std::nullopt, rule_registry, ser_config);
    auto ctx = valueContext(SerdeFormat::Protobuf);

    EXPECT_NO_THROW(serializer.serialize(
        ctx, protoOrder("ord-1234", 2, {"a", "b"}, "12345")));
    EXPECT_THROW(
        serializer.serialize(ctx, protoOrder("bad", 2, {"a", "b"}, "12345")),
        ValidationRulesFailedError);
}

#endif  // SCHEMAREGISTRY_USE_PROTOBUF
