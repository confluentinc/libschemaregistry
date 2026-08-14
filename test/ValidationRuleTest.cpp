/**
 * ValidationRuleTest
 * Tests for inline validation rules ("confluent:rules" / confluent.Meta rules)
 */

#include <gtest/gtest.h>

#include <functional>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "schemaregistry/rest/ClientConfiguration.h"
#include "schemaregistry/rest/MockSchemaRegistryClient.h"
#include "schemaregistry/rest/SchemaRegistryClient.h"
#include "schemaregistry/rest/model/Rule.h"
#include "schemaregistry/rest/model/RuleSet.h"
#include "schemaregistry/rest/model/Schema.h"
#include "schemaregistry/rules/cel/CelExecutor.h"
#include "schemaregistry/rules/cel/CelFieldExecutor.h"
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
#include "schemaregistry/serdes/json/JsonTypes.h"
#include "schemaregistry/serdes/json/JsonUtils.h"
#endif

#ifdef SCHEMAREGISTRY_USE_PROTOBUF
#include "schemaregistry/serdes/protobuf/ProtobufDeserializer.h"
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

std::vector<ValidationRuleError> validateAvro(
    const ::avro::GenericDatum &datum, bool fail_fast = false,
    const std::vector<nlohmann::json> &named_schemas = {}) {
    CelValidator validator;
    return schemaregistry::serdes::avro::utils::validateMessage(
        validator, nlohmann::json::parse(kAvroSchema), named_schemas, datum,
        fail_fast);
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

TEST(ValidationRuleTest, AvroEvaluatesRulesFromReferencedSchemas) {
    // The datum is built from the resolved schema, but the raw root schema only
    // names the referenced record, so its rules are reachable only through the
    // referenced schemas the walk is seeded with.
    const char *root = R"schema({
        "type": "record",
        "name": "Order",
        "namespace": "test",
        "fields": [
            {"name": "address", "type": "test.Address"}
        ]
    })schema";
    const char *referenced = R"schema({
        "type": "record",
        "name": "Address",
        "namespace": "test",
        "fields": [
            {"name": "zip", "type": "string",
             "confluent:rules": [
                {"name": "zip_digits",
                 "expr": "this.matches('^[0-9]{5}$') ? '' : 'zip must be 5 digits'"}
             ]}
        ]
    })schema";
    const char *resolved = R"schema({
        "type": "record",
        "name": "Order",
        "namespace": "test",
        "fields": [
            {"name": "address", "type": {
                "type": "record",
                "name": "Address",
                "fields": [{"name": "zip", "type": "string"}]
            }}
        ]
    })schema";

    auto schema = ::avro::compileJsonSchemaFromString(resolved);
    ::avro::GenericDatum datum(schema);
    auto &record = datum.value<::avro::GenericRecord>();
    record.fieldAt(0).value<::avro::GenericRecord>().setFieldAt(
        0, ::avro::GenericDatum(std::string("abc")));

    CelValidator validator;
    std::vector<nlohmann::json> named_schemas{
        nlohmann::json::parse(referenced)};
    auto violations = schemaregistry::serdes::avro::utils::validateMessage(
        validator, nlohmann::json::parse(root), named_schemas, datum, false);
    ASSERT_EQ(violations.size(), 1);
    EXPECT_EQ(violations[0].rule.name, "zip_digits");
    EXPECT_EQ(violations[0].field_path, "address.zip");
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

TEST(ValidationRuleTest, AvroInlineTagsQualifyRecordNames) {
    // The fullname of "foobar" in namespace "foo" is "foo.foobar". Treating the
    // namespace as an already-present prefix keys the tags under "foobar",
    // which never matches, and the tags - which drive field encryption - are
    // silently dropped.
    auto tags = schemaregistry::serdes::avro::utils::getInlineTags(
        nlohmann::json::parse(R"schema({
            "type": "record", "namespace": "foo", "name": "foobar",
            "fields": [{"name": "x", "type": "string", "confluent:tags": ["PII"]}]
        })schema"));
    ASSERT_EQ(tags.count("foo.foobar.x"), 1u)
        << "tags keyed under the wrong name";
    EXPECT_EQ(tags.at("foo.foobar.x").count("PII"), 1u);

    // A dotted name is already a fullname, so the namespace attribute is
    // ignored.
    auto dotted = schemaregistry::serdes::avro::utils::getInlineTags(
        nlohmann::json::parse(R"schema({
            "type": "record", "namespace": "x", "name": "a.B",
            "fields": [{"name": "y", "type": "string", "confluent:tags": ["PII"]}]
        })schema"));
    ASSERT_EQ(dotted.count("a.B.y"), 1u) << "namespace prepended to a fullname";
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

TEST(ValidationRuleTest, JsonOneOfPicksTheBranchTheValueSatisfies) {
    // Both branches are objects, so they cannot be told apart by JSON type:
    // they differ by the "kind" const. Evaluating both branches' rules would
    // reject a valid message.
    const char *schema = R"schema({
        "type": "object",
        "properties": {
            "payload": {
                "oneOf": [
                    {
                        "type": "object",
                        "properties": {
                            "kind": {"const": "a"},
                            "v": {"type": "integer",
                                  "confluent:rules": [{"name": "ruleA", "expr": "false"}]}
                        },
                        "required": ["kind"]
                    },
                    {
                        "type": "object",
                        "properties": {
                            "kind": {"const": "b"},
                            "v": {"type": "integer",
                                  "confluent:rules": [{"name": "ruleB", "expr": "false"}]}
                        },
                        "required": ["kind"]
                    }
                ]
            }
        }
    })schema";

    CelValidator validator;
    auto parsed = nlohmann::json::parse(schema);
    for (const auto &[kind, expected] :
         std::vector<std::pair<std::string, std::string>>{{"a", "ruleA"},
                                                          {"b", "ruleB"}}) {
        nlohmann::json value = {{"payload", {{"kind", kind}, {"v", 1}}}};
        auto violations = schemaregistry::serdes::json::utils::validateMessage(
            validator, parsed, value, false);
        ASSERT_EQ(violations.size(), 1u)
            << "kind " << kind << ": " << violations.size();
        EXPECT_EQ(violations[0].rule.name, expected);
        EXPECT_EQ(violations[0].field_path, "$.payload.v");
    }
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
    // serial has a positive rule; keep it valid so tests assert only what they
    // target.
    order.set_serial(1);
    return order;
}

/**
 * Rebuilds the validation proto's file into `pool`, with `mutate` applied to the
 * copy, so a test can pair a registered schema against the generated type the way
 * use.latest.version does. Returns the rebuilt ValidationOrder descriptor, which is
 * a different object from the generated one even where it describes the same fields.
 */
const google::protobuf::Descriptor *rebuiltValidationDescriptor(
    google::protobuf::DescriptorPool &pool,
    const std::function<void(google::protobuf::FileDescriptorProto &)> &mutate) {
    const auto *file = test::ValidationOrder::descriptor()->file();
    // Dependencies first, so the file's imports resolve in the new pool.
    std::function<void(const google::protobuf::FileDescriptor *)> add_dependency =
        [&](const google::protobuf::FileDescriptor *dependency) {
            for (int i = 0; i < dependency->dependency_count(); ++i) {
                add_dependency(dependency->dependency(i));
            }
            if (pool.FindFileByName(dependency->name()) != nullptr) {
                return;
            }
            google::protobuf::FileDescriptorProto proto;
            dependency->CopyTo(&proto);
            pool.BuildFile(proto);
        };
    for (int i = 0; i < file->dependency_count(); ++i) {
        add_dependency(file->dependency(i));
    }
    google::protobuf::FileDescriptorProto proto;
    file->CopyTo(&proto);
    mutate(proto);
    if (pool.BuildFile(proto) == nullptr) {
        return nullptr;
    }
    return pool.FindMessageTypeByName("test.ValidationOrder");
}

google::protobuf::DescriptorProto *messageOf(
    google::protobuf::FileDescriptorProto &proto, const std::string &name) {
    for (auto &message : *proto.mutable_message_type()) {
        if (message.name() == name) {
            return &message;
        }
    }
    return nullptr;
}

std::vector<ValidationRuleError> validateProto(
    const google::protobuf::Message &message, bool fail_fast = false) {
    CelValidator validator;
    // The message is already in the schema's terms here, so its own descriptor is
    // what carries the rules.
    return schemaregistry::serdes::protobuf::utils::validateMessage(
        validator, message, message.GetDescriptor(), fail_fast);
}

}  // namespace

// A field transform - the walk that drives CSFLE - has to descend into a nested message
// with that message's own descriptor. Walking it against the containing descriptor applies
// the parent's fields to the child, which the protobuf reflection API rejects with a fatal
// error rather than an exception.
TEST(ValidationRuleTest, ProtobufFieldTransformDescendsIntoNestedMessages) {
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = std::make_shared<MockSchemaRegistryClient>(client_config);

    std::unordered_map<std::string, std::string> rule_config;
    auto ser_conf = SerializerConfig(
        false, std::make_optional(SchemaSelector::useLatestVersion()), false,
        false, rule_config);

    test::ValidationOrder obj = protoOrder("ord-1234", 2, {"a", "b"}, "12345");

    Rule rule;
    rule.setName("test-cel");
    rule.setKind(Kind::Transform);
    rule.setMode(Mode::Write);
    rule.setType("CEL_FIELD");
    rule.setExpr("typeName == 'STRING' ; value + '-suffix'");
    RuleSet rule_set;
    rule_set.setDomainRules(std::vector<Rule>{rule});

    Schema schema;
    schema.setSchemaType("PROTOBUF");
    schema.setRuleSet(rule_set);
    schema.setSchema(
        protobuf::utils::schemaToString(obj.GetDescriptor()->file()));
    client->registerSchema("test-value", schema, false);

    auto rule_registry = std::make_shared<RuleRegistry>();
    rule_registry->registerExecutor(
        std::make_shared<schemaregistry::rules::cel::CelFieldExecutor>());

    schemaregistry::serdes::protobuf::ProtobufSerializer<test::ValidationOrder>
        ser(client, std::nullopt, rule_registry, ser_conf);
    auto ctx = valueContext(SerdeFormat::Protobuf);
    auto bytes = ser.serialize(ctx, obj);

    // The rule is write-only, so deserializing shows what was written.
    schemaregistry::serdes::protobuf::ProtobufDeserializer<test::ValidationOrder>
        deser(client, rule_registry, DeserializerConfig::createDefault());
    auto result = deser.deserialize(ctx, bytes);
    const auto *order = dynamic_cast<const test::ValidationOrder *>(result.get());
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->id(), "ord-1234-suffix");
    // The nested message's string field is only reached by descending with the
    // nested descriptor.
    EXPECT_EQ(order->address().zip(), "12345-suffix");
}

TEST(ValidationRuleTest, ProtobufMetaIsReadableFromGeneratedDescriptors) {
    // Everything the protobuf walker does depends on this, and so does field
    // encryption, which reads its tags from the same options. If the meta cannot be
    // read then no rule ever fires and no field is ever encrypted, both silently, so
    // assert it directly rather than only through a rule's outcome.
    const auto *desc = test::ValidationOrder::descriptor();
    auto message_meta = schemaregistry::serdes::protobuf::utils::getMessageMeta(desc);
    ASSERT_TRUE(message_meta.has_value());
    ASSERT_EQ(message_meta->rules_size(), 1);
    EXPECT_EQ(message_meta->rules(0).name(), "quantity_matches_items");

    const auto *field = desc->FindFieldByName("id");
    ASSERT_NE(field, nullptr);
    auto field_meta = schemaregistry::serdes::protobuf::utils::getFieldMeta(field);
    ASSERT_TRUE(field_meta.has_value());
    ASSERT_EQ(field_meta->rules_size(), 2);
    EXPECT_EQ(field_meta->rules(0).name(), "id_prefix");
}

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
    // A repeated field's own rules see the whole list, so the violation is reported
    // against the field rather than an element.
    EXPECT_EQ(violations[4].rule.name, "item_not_empty");
    EXPECT_EQ(violations[4].field_path, "items");
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
    // As for a repeated field, a map field's own rules see the whole map.
    EXPECT_EQ(violations[1].rule.name, "score_not_negative");
    EXPECT_EQ(violations[1].field_path, "scores");
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
    order.set_serial(1);
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

TEST(ValidationRuleTest, ProtobufAllowsDefaultedProto3Scalars) {
    // quantity and items are both at their proto3 defaults, so
    // "this.quantity == size(this.items)" holds; the message-level rule must be
    // able to see them rather than failing with "no such key".
    test::ValidationOrder order;
    order.set_id("ord-1234");
    order.set_serial(1);
    auto violations = validateProto(order);

    for (const auto &violation : violations) {
        EXPECT_TRUE(violation.cause.empty())
            << "no rule should fail to evaluate: " << violation.toString();
    }
    EXPECT_EQ(findViolation(violations, "quantity_matches_items"), nullptr);
    // quantity = 0 still legitimately fails "this > 0".
    ASSERT_EQ(violations.size(), 1);
    EXPECT_EQ(violations[0].rule.name, "positive_quantity");
}

TEST(ValidationRuleTest, ProtobufUsesTheSelectedSchemasDescriptor) {
    auto client = std::make_shared<MockSchemaRegistryClient>(
        std::make_shared<const ClientConfiguration>(
            std::vector<std::string>{"mock://"}));

    test::ValidationOrder invalid = protoOrder("bad", 2, {"a", "b"}, "12345");
    Schema schema;
    schema.setSchemaType("PROTOBUF");
    schema.setSchema(
        protobuf::utils::schemaToString(invalid.GetDescriptor()->file()));
    client->registerSchema("test-value", schema, false);

    auto ser_config = SerializerConfig(
        false, std::make_optional(SchemaSelector::useLatestVersion()), false,
        false, std::unordered_map<std::string, std::string>{});
    ser_config.validation_rules_execution =
        ValidationRulesExecution::AfterDomainRules;

    auto rule_registry = std::make_shared<RuleRegistry>();
    rule_registry->registerValidationExecutor(std::make_shared<CelValidator>());

    schemaregistry::serdes::protobuf::ProtobufSerializer<test::ValidationOrder>
        serializer(client, std::nullopt, rule_registry, ser_config);
    auto ctx = valueContext(SerdeFormat::Protobuf);

    // Rules are read from the descriptor parsed out of the selected registry
    // schema, not from the caller's compiled-in one.
    EXPECT_THROW(serializer.serialize(ctx, invalid),
                 ValidationRulesFailedError);
    EXPECT_NO_THROW(serializer.serialize(
        ctx, protoOrder("ord-1234", 2, {"a", "b"}, "12345")));
}

TEST(ValidationRuleTest, ProtobufPreservesUnsignedValues) {
    // A uint64 above int64 max narrowed to a signed value reads as negative, so
    // a "this > 0" rule would reject a perfectly valid serial number.
    test::ValidationOrder order = protoOrder("ord-1234", 1, {"a"}, "12345");
    order.set_serial(std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(validateProto(order).size(), 0u);

    order.set_serial(0);
    auto violations = validateProto(order);
    ASSERT_EQ(violations.size(), 1u);
    EXPECT_EQ(violations[0].rule.name, "serial_positive");
}

TEST(ValidationRuleTest, MessageLevelRulesSeeUnsignedFieldsAndMaps) {
    test::ValidationMessageLevel message;
    message.set_serial(std::numeric_limits<uint64_t>::max());
    (*message.mutable_scores())["ok"] = 0;

    auto violations = validateProto(message);
    for (const auto &violation : violations) {
        EXPECT_TRUE(violation.cause.empty())
            << "message-level rule failed to evaluate: "
            << violation.toString();
    }
    EXPECT_EQ(violations.size(), 0u);

    // and the rule really does run: a zero serial fails it
    message.set_serial(0);
    auto failures = validateProto(message);
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_EQ(failures[0].rule.name, "sees_serial_and_scores");
}


// A rule that binds `this` to a nested message needs that message in the schema's
// terms, not just the top-level one: a rule's environment is built from the
// registered schema, so `this.renamed_zip` cannot read a field the caller's type
// calls `zip`. Renaming a field at the same number is a compatible change.
TEST(ValidationRuleTest, ProtobufNestedMessageRuleSeesSchemaNamesUnderARename) {
    google::protobuf::DescriptorPool pool;
    const auto *schema_descriptor = rebuiltValidationDescriptor(
        pool, [](google::protobuf::FileDescriptorProto &proto) {
            auto *address = messageOf(proto, "ValidationAddress");
            ASSERT_NE(address, nullptr);
            for (auto &field : *address->mutable_field()) {
                if (field.number() == 1) {
                    field.set_name("renamed_zip");
                    field.set_json_name("renamedZip");
                    field.clear_options();
                }
            }
            auto *meta = address->mutable_options()->MutableExtension(
                confluent::message_meta);
            meta->clear_rules();
            auto *rule = meta->add_rules();
            rule->set_name("zip_present");
            rule->set_expr("size(this.renamed_zip) > 0");
        });
    ASSERT_NE(schema_descriptor, nullptr);

    auto order = protoOrder("ord-1234", 1, {"a"}, "12345");
    CelValidator validator;
    auto violations = schemaregistry::serdes::protobuf::utils::validateMessage(
        validator, order, schema_descriptor, false);

    EXPECT_TRUE(violations.empty()) << violations.size() << " violations";
}

// Adding a field is the most ordinary compatible change there is, so the registered
// schema can declare one the generated type has never heard of - and a
// message-level rule can reference it, expecting the schema's default. That only
// works if the message is read through the schema, so a field with no counterpart
// is itself a reason to re-read, even when every shared field agrees.
TEST(ValidationRuleTest, ProtobufMessageRuleSeesAFieldOnlyTheSchemaDeclares) {
    google::protobuf::DescriptorPool pool;
    const auto *schema_descriptor = rebuiltValidationDescriptor(
        pool, [](google::protobuf::FileDescriptorProto &proto) {
            auto *order = messageOf(proto, "ValidationOrder");
            ASSERT_NE(order, nullptr);
            auto *added = order->add_field();
            added->set_name("added");
            added->set_json_name("added");
            added->set_number(99);
            added->set_type(google::protobuf::FieldDescriptorProto::TYPE_STRING);
            added->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
            auto *meta = order->mutable_options()->MutableExtension(
                confluent::message_meta);
            meta->clear_rules();
            auto *rule = meta->add_rules();
            rule->set_name("added_default");
            rule->set_expr("this.added == ''");
        });
    ASSERT_NE(schema_descriptor, nullptr);

    auto order = protoOrder("ord-1234", 1, {"a"}, "12345");
    CelValidator validator;
    auto violations = schemaregistry::serdes::protobuf::utils::validateMessage(
        validator, order, schema_descriptor, false);

    EXPECT_TRUE(violations.empty()) << violations.size() << " violations";
}

// A field-level rule on a repeated or map field is evaluated once, with the whole
// collection bound to `this` - matching the JVM client - rather than once per element.
// A rule about the elements is therefore written as a comprehension over them.
TEST(ValidationRuleTest, ProtobufCollectionRulesSeeTheWholeCollection) {
    auto order = protoOrder("ord-1234", 2, {"a", "b"}, "12345");
    (*order.mutable_scores())["ok"] = 1;
    // size(this) is only answerable if the whole list is bound.
    EXPECT_TRUE(validateProto(order).empty());

    // One empty element fails the comprehension, and the violation is reported against
    // the field, once.
    auto with_empty = protoOrder("ord-1234", 2, {"a", ""}, "12345");
    auto violations = validateProto(with_empty);
    ASSERT_EQ(violations.size(), 1);
    EXPECT_EQ(violations[0].rule.name, "item_not_empty");
    EXPECT_EQ(violations[0].field_path, "items");
    EXPECT_TRUE(violations[0].cause.empty());

    // An empty map is bound as an empty map, not skipped and not an error.
    auto no_scores = protoOrder("ord-1234", 2, {"a", "b"}, "12345");
    EXPECT_TRUE(validateProto(no_scores).empty());

    // A negative entry fails the comprehension over the map's keys.
    auto negative = protoOrder("ord-1234", 2, {"a", "b"}, "12345");
    (*negative.mutable_scores())["bad"] = -1;
    auto map_violations = validateProto(negative);
    ASSERT_EQ(map_violations.size(), 1);
    EXPECT_EQ(map_violations[0].rule.name, "score_not_negative");
    EXPECT_EQ(map_violations[0].field_path, "scores");
    EXPECT_TRUE(map_violations[0].cause.empty());
}

// A field with explicit presence that is unset has nothing to transform, and writing a
// value back would materialize it: an absent message would become present, carrying a
// transformed default.
TEST(ValidationRuleTest, ProtobufFieldTransformLeavesAbsentFieldsAbsent) {
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = std::make_shared<MockSchemaRegistryClient>(client_config);

    std::unordered_map<std::string, std::string> rule_config;
    auto ser_conf = SerializerConfig(
        false, std::make_optional(SchemaSelector::useLatestVersion()), false,
        false, rule_config);

    // address is absent.
    test::ValidationOrder obj;
    obj.set_id("ord-1234");
    obj.set_quantity(2);
    obj.add_items("a");
    obj.set_serial(1);

    Rule rule;
    rule.setName("test-cel");
    rule.setKind(Kind::Transform);
    rule.setMode(Mode::Write);
    rule.setType("CEL_FIELD");
    rule.setExpr("typeName == 'STRING' ; value + '-suffix'");
    RuleSet rule_set;
    rule_set.setDomainRules(std::vector<Rule>{rule});

    Schema schema;
    schema.setSchemaType("PROTOBUF");
    schema.setRuleSet(rule_set);
    schema.setSchema(
        protobuf::utils::schemaToString(obj.GetDescriptor()->file()));
    client->registerSchema("test-value", schema, false);

    auto rule_registry = std::make_shared<RuleRegistry>();
    rule_registry->registerExecutor(
        std::make_shared<schemaregistry::rules::cel::CelFieldExecutor>());

    schemaregistry::serdes::protobuf::ProtobufSerializer<test::ValidationOrder>
        ser(client, std::nullopt, rule_registry, ser_conf);
    auto ctx = valueContext(SerdeFormat::Protobuf);
    auto bytes = ser.serialize(ctx, obj);

    schemaregistry::serdes::protobuf::ProtobufDeserializer<test::ValidationOrder>
        deser(client, rule_registry, DeserializerConfig::createDefault());
    auto result = deser.deserialize(ctx, bytes);
    const auto *order = dynamic_cast<const test::ValidationOrder *>(result.get());
    ASSERT_NE(order, nullptr);
    EXPECT_FALSE(order->has_address()) << "the absent message was materialized";
    // The fields that are present are still transformed.
    EXPECT_EQ(order->id(), "ord-1234-suffix");
}

// A map field arrives as a list of entry messages, so the transform walk reaches the
// entry's key as well as its value. A key is part of the map's identity rather than a
// value to transform - rewriting it moves the entry - and the validation walk never
// evaluates anything on a key either.
TEST(ValidationRuleTest, ProtobufFieldTransformLeavesMapKeysAlone) {
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = std::make_shared<MockSchemaRegistryClient>(client_config);

    std::unordered_map<std::string, std::string> rule_config;
    auto ser_conf = SerializerConfig(
        false, std::make_optional(SchemaSelector::useLatestVersion()), false,
        false, rule_config);

    test::ValidationOrder obj = protoOrder("ord-1234", 2, {"a"}, "12345");
    (*obj.mutable_scores())["k1"] = 1;

    Rule rule;
    rule.setName("test-cel");
    rule.setKind(Kind::Transform);
    rule.setMode(Mode::Write);
    rule.setType("CEL_FIELD");
    rule.setExpr("typeName == 'STRING' ; value + '-suffix'");
    RuleSet rule_set;
    rule_set.setDomainRules(std::vector<Rule>{rule});

    Schema schema;
    schema.setSchemaType("PROTOBUF");
    schema.setRuleSet(rule_set);
    schema.setSchema(
        protobuf::utils::schemaToString(obj.GetDescriptor()->file()));
    client->registerSchema("test-value", schema, false);

    auto rule_registry = std::make_shared<RuleRegistry>();
    rule_registry->registerExecutor(
        std::make_shared<schemaregistry::rules::cel::CelFieldExecutor>());

    schemaregistry::serdes::protobuf::ProtobufSerializer<test::ValidationOrder>
        ser(client, std::nullopt, rule_registry, ser_conf);
    auto ctx = valueContext(SerdeFormat::Protobuf);
    auto bytes = ser.serialize(ctx, obj);

    schemaregistry::serdes::protobuf::ProtobufDeserializer<test::ValidationOrder>
        deser(client, rule_registry, DeserializerConfig::createDefault());
    auto result = deser.deserialize(ctx, bytes);
    const auto *order = dynamic_cast<const test::ValidationOrder *>(result.get());
    ASSERT_NE(order, nullptr);
    ASSERT_EQ(order->scores().size(), 1);
    EXPECT_EQ(order->scores().begin()->first, "k1")
        << "the map key was rewritten by the transform";
    // The string fields that are values are still transformed.
    EXPECT_EQ(order->id(), "ord-1234-suffix");
}

#endif  // SCHEMAREGISTRY_USE_PROTOBUF

#ifdef SCHEMAREGISTRY_USE_JSON
namespace {

// A root whose text is the same whichever schema it references. What its "$ref"
// resolves to is decided entirely by the reference list on the Schema.
constexpr const char *kSharedRoot = R"schema({
    "type": "object",
    "properties": {"payload": {"$ref": "ref"}}
})schema";

// Two referenced schemas that differ only in the rule they declare.
std::string refSchemaRequiring(const std::string &prefix,
                               const std::string &rule_name) {
    return R"schema({
        "type": "object",
        "properties": {"code": {"type": "string",
            "confluent:rules": [{"name": ")schema" +
           rule_name + R"schema(",
             "expr": "this.startsWith(')schema" +
           prefix + R"schema(') ? '' : 'wrong prefix'"}]}}
    })schema";
}

Schema rootReferencing(const std::string &subject) {
    SchemaReference ref;
    ref.setName(std::make_optional<std::string>("ref"));
    ref.setSubject(std::make_optional<std::string>(subject));
    ref.setVersion(std::make_optional<int32_t>(1));

    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("JSON"));
    schema.setSchema(std::make_optional<std::string>(kSharedRoot));
    schema.setReferences(
        std::make_optional<std::vector<SchemaReference>>({ref}));
    return schema;
}

}  // namespace

TEST(ValidationRuleTest, JsonSchemaCacheSeparatesSchemasByTheirReferences) {
    auto client = newMockClient();
    for (const auto &subject : {std::string("ref-a"), std::string("ref-b")}) {
        Schema ref_schema;
        ref_schema.setSchemaType(std::make_optional<std::string>("JSON"));
        ref_schema.setSchema(std::make_optional<std::string>(
            refSchemaRequiring(subject == "ref-a" ? "A" : "B",
                               subject == "ref-a" ? "code_a" : "code_b")));
        client->registerSchema(subject, ref_schema, false);
    }

    // One serde, so both lookups go through the same cache. The two schemas
    // agree on every byte of their text and differ only in what they reference,
    // which is exactly the pair a text-keyed cache would conflate.
    schemaregistry::serdes::json::JsonSerde serde;
    auto flattened_a = serde.getSchemaJson(rootReferencing("ref-a"), client);
    auto flattened_b = serde.getSchemaJson(rootReferencing("ref-b"), client);

    ASSERT_NE(flattened_a, nullptr);
    ASSERT_NE(flattened_b, nullptr);
    EXPECT_NE(flattened_a, flattened_b)
        << "the second schema was served the first one's flattened document";
    EXPECT_NE(flattened_a->dump().find("code_a"), std::string::npos);
    EXPECT_NE(flattened_b->dump().find("code_b"), std::string::npos);
    EXPECT_EQ(flattened_b->dump().find("code_a"), std::string::npos)
        << "rules from the other schema's reference leaked in";

    // The same schema still hits the cache rather than re-resolving.
    EXPECT_EQ(serde.getSchemaJson(rootReferencing("ref-a"), client),
              flattened_a);
}
#endif

#ifdef SCHEMAREGISTRY_USE_JSON
namespace {

std::shared_ptr<ISchemaRegistryClient> clientWithJsonFieldRule(
    const std::string &expr, const std::string &schema_json) {
    auto client = newMockClient();
    Rule rule;
    rule.setName("fieldRule");
    rule.setKind(Kind::Transform);
    rule.setMode(Mode::Write);
    rule.setType("CEL_FIELD");
    rule.setTags(std::vector<std::string>{"PII"});
    rule.setExpr(expr);
    RuleSet rule_set;
    rule_set.setDomainRules(std::vector<Rule>{rule});

    Schema schema;
    schema.setSchemaType("JSON");
    schema.setRuleSet(rule_set);
    schema.setSchema(std::make_optional<std::string>(schema_json));
    client->registerSchema("test-value", schema, false);
    return client;
}

schemaregistry::serdes::json::JsonSerializer jsonSerializerFor(
    std::shared_ptr<ISchemaRegistryClient> client) {
    auto rule_registry = std::make_shared<RuleRegistry>();
    rule_registry->registerExecutor(
        std::make_shared<schemaregistry::rules::cel::CelFieldExecutor>());
    auto ser_config = SerializerConfig::createDefault();
    ser_config.auto_register_schemas = false;
    ser_config.use_schema = SchemaSelector::useLatestVersion();
    return schemaregistry::serdes::json::JsonSerializer(
        client, std::nullopt, rule_registry, ser_config);
}

}  // namespace

// A field transform that fails must reach the caller. transformFields used to wrap the
// per-field call, the pointer write and the whole walk in catch blocks with empty bodies,
// so a failing rule left the field with its original value and the serializer reported
// success - for a field-encryption rule, emitting the plaintext.
TEST(ValidationRuleTest, JsonFieldTransformFailureIsNotSwallowed) {
    auto client = clientWithJsonFieldRule("noSuchFunction(value)", R"schema({
        "type": "object",
        "properties": {"code": {"type": "string", "confluent:tags": ["PII"]}}
    })schema");
    auto ser = jsonSerializerFor(client);
    auto ctx = valueContext(SerdeFormat::Json);

    nlohmann::json value = {{"code", "secret"}};
    EXPECT_THROW(ser.serialize(ctx, value), std::exception);
}

// A transform that succeeds still has to write its result back.
TEST(ValidationRuleTest, JsonFieldTransformResultIsWrittenBack) {
    auto client = clientWithJsonFieldRule("value + '-x'", R"schema({
        "type": "object",
        "properties": {"code": {"type": "string", "confluent:tags": ["PII"]}}
    })schema");
    auto ser = jsonSerializerFor(client);
    auto ctx = valueContext(SerdeFormat::Json);

    auto bytes = ser.serialize(ctx, nlohmann::json{{"code", "a"}});
    std::string payload(bytes.begin() + 5, bytes.end());
    EXPECT_NE(payload.find("a-x"), std::string::npos) << payload;
}

// getFieldType must not guess String for a schema that declares no usable type: a rule
// keyed on the field's type would match the wrong fields, and an encryption rule would
// treat a number or an object as text.
TEST(ValidationRuleTest, JsonFieldTypeIsDerivedFromTheSchema) {
    using schemaregistry::serdes::json::utils::schema_navigation::getFieldType;
    auto typeOf = [](const char *json) {
        return getFieldType(jsoncons::ojson::parse(json));
    };

    EXPECT_EQ(typeOf(R"({"type":"string"})"), FieldType::String);
    EXPECT_EQ(typeOf(R"({"type":"integer"})"), FieldType::Int);
    EXPECT_EQ(typeOf(R"({"type":"number"})"), FieldType::Double);
    EXPECT_EQ(typeOf(R"({"type":"boolean"})"), FieldType::Boolean);
    EXPECT_EQ(typeOf(R"({"type":"array"})"), FieldType::Array);
    // An object with declared properties is a record; without them, an open map.
    EXPECT_EQ(typeOf(R"({"type":"object","properties":{"a":{"type":"string"}}})"),
              FieldType::Record);
    EXPECT_EQ(typeOf(R"({"type":"object"})"), FieldType::Map);
    // A nullable field is the type that is not null.
    EXPECT_EQ(typeOf(R"({"type":["string","null"]})"), FieldType::String);
    EXPECT_EQ(typeOf(R"({"type":["null","integer"]})"), FieldType::Int);
    // Genuinely ambiguous, so no single type to act on.
    EXPECT_EQ(typeOf(R"({"type":["string","integer"]})"), FieldType::Combined);
    EXPECT_EQ(typeOf(R"({"anyOf":[{"type":"string"},{"type":"integer"}]})"),
              FieldType::Combined);
    EXPECT_EQ(typeOf(R"({"type":"string","enum":["a","b"]})"), FieldType::Enum);
    // No type at all: a record if it declares properties, otherwise nothing to act on -
    // and never String, which is what it used to return.
    EXPECT_EQ(typeOf(R"({"properties":{"a":{"type":"string"}}})"), FieldType::Record);
    EXPECT_EQ(typeOf(R"({"$ref":"#/$defs/Other"})"), FieldType::Null);
    EXPECT_EQ(typeOf(R"({"type":"unrecognized"})"), FieldType::Null);
}
#endif

#ifdef SCHEMAREGISTRY_USE_PROTOBUF
// cel-cpp is handed the message itself, not a map of its fields, so the engine answers from
// the descriptor. A well-known type is then the value it wraps rather than a map of seconds
// and nanos, and these rules have an overload at all.
TEST(ValidationRuleTest, ProtobufWellKnownTypesBindAsTheValueTheyWrap) {
    test::ValidationWellKnown obj;
    obj.mutable_created_at()->set_seconds(1600000000);
    obj.mutable_name()->set_value("widget");
    EXPECT_TRUE(validateProto(obj).empty());
}

TEST(ValidationRuleTest, ProtobufWellKnownTypeRulesStillFire) {
    // Epoch is not after epoch, and an empty string fails size() > 0.
    test::ValidationWellKnown obj;
    obj.mutable_created_at()->set_seconds(0);
    obj.mutable_name()->set_value("");
    auto violations = validateProto(obj);
    EXPECT_NE(findViolation(violations, "created_after_epoch"), nullptr);
    EXPECT_NE(findViolation(violations, "name_not_empty"), nullptr);
}

// has() reports protobuf presence: a proto3 scalar left at its default is unset. Built as a
// map the key was always there, so has() was unconditionally true.
TEST(ValidationRuleTest, ProtobufHasFollowsProtobufPresence) {
    test::ValidationPresence unset;
    auto violations = validateProto(unset);
    EXPECT_NE(findViolation(violations, "quantity_unset"), nullptr)
        << "has() reported an unwritten field as set";

    test::ValidationPresence written;
    written.set_quantity(5);
    EXPECT_TRUE(validateProto(written).empty())
        << "has() reported a written field as unset";
}
#endif
