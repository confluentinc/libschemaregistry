/**
 * CelVariantTest
 *
 * Tests the CEL Variant function family (variant / variants.*), plus marshalling
 * the two schema-side shapes into CEL: an Avro confluent.type.Variant record and a
 * Protobuf confluent.type.Variant message. Mirrors CelDecimalTimestampTest.
 */

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef SCHEMAREGISTRY_TEST_WITH_AVRO
#include <avro/Compiler.hh>
#include <avro/Generic.hh>
#include <avro/GenericDatum.hh>
#include <avro/ValidSchema.hh>

#include "schemaregistry/serdes/avro/AvroUtils.h"
#endif

#include "confluent/type/variant.pb.h"
#include "schemaregistry/rules/cel/CelValidator.h"
#include "schemaregistry/serdes/Variant.h"
#include "schemaregistry/serdes/ValidationRule.h"
#include "schemaregistry/serdes/json/JsonValue.h"
#include "schemaregistry/serdes/protobuf/ProtobufTypes.h"

using namespace schemaregistry::serdes;
using schemaregistry::rules::cel::CelValidator;

namespace {

// A JSON document exercising objects, arrays, an explicit null, and nesting.
constexpr const char *kDoc =
    R"({"name":"alice","age":30,"explicit":null,"nested":{"x":1},"scores":[10,20,30]})";

ValidationRule rule(const std::string &expr) {
    return ValidationRule{"r", "", expr, ""};
}

// Evaluate a boolean CEL rule with `this` bound to a JSON string value (so
// variants.parseJson(this) turns it into a Variant).
bool evalWith(const std::string &expr, const std::string &jsonStr) {
    CelValidator validator;
    auto value = json::makeJsonValue(nlohmann::json(jsonStr));
    auto result = validator.execute(rule(expr), *value);
    EXPECT_TRUE(std::holds_alternative<bool>(result)) << expr;
    return std::holds_alternative<bool>(result) && std::get<bool>(result);
}

}  // namespace

// ---- variants.* over a parsed JSON string bound as `this` ----

TEST(CelVariantTest, VariantFunctions) {
    EXPECT_TRUE(evalWith("variants.type(variants.parseJson(this)) == 'object'", kDoc));
    EXPECT_TRUE(evalWith(
        "variants.as(variants.field(variants.parseJson(this), 'name'), 'string') == 'alice'",
        kDoc));
    EXPECT_TRUE(evalWith(
        "variants.as(variants.field(variants.parseJson(this), 'age'), 'int') == 30", kDoc));
    // A missing field is CEL null (absent); an explicit JSON null is a present variant-null.
    EXPECT_TRUE(
        evalWith("variants.field(variants.parseJson(this), 'missing') == null", kDoc));
    EXPECT_TRUE(evalWith(
        "variants.isNull(variants.field(variants.parseJson(this), 'explicit'))", kDoc));
    EXPECT_TRUE(evalWith(
        "!variants.isNull(variants.field(variants.parseJson(this), 'missing'))", kDoc));
    EXPECT_TRUE(evalWith(
        "variants.as(variants.path(variants.parseJson(this), '$.nested.x'), 'int') == 1",
        kDoc));
    EXPECT_TRUE(evalWith(
        "variants.as(variants.index(variants.field(variants.parseJson(this), 'scores'), 2), "
        "'int') == 30",
        kDoc));
    // tryAs returns CEL null on a type mismatch (age is an int, not a string).
    EXPECT_TRUE(evalWith(
        "variants.tryAs(variants.field(variants.parseJson(this), 'age'), 'string') == null",
        kDoc));
    EXPECT_TRUE(evalWith(
        R"(variants.toJson(variants.field(variants.parseJson(this), 'nested')) == '{"x":1}')",
        kDoc));
}

// ---- Marshalling: the two schema-side shapes into CEL ----

TEST(CelVariantTest, ProtoConfluentTypeVariantIntoCel) {
    // A confluent.type.Variant proto message bound as `this`; variant(dyn) unwraps it.
    auto parsed = Variant::parseJson(kDoc);
    auto msg = std::make_unique<confluent::type::Variant>();
    msg->set_metadata(std::string(parsed.metadataBytes().begin(),
                                  parsed.metadataBytes().end()));
    msg->set_value(std::string(parsed.valueBytes().begin(), parsed.valueBytes().end()));
    CelValidator validator;
    auto value = protobuf::makeProtobufValue(protobuf::ProtobufVariant(std::move(msg)));
    auto result = validator.execute(
        rule("variants.as(variants.field(variant(this), 'age'), 'int') == 30"), *value);
    ASSERT_TRUE(std::holds_alternative<bool>(result));
    EXPECT_TRUE(std::get<bool>(result));
}

#ifdef SCHEMAREGISTRY_TEST_WITH_AVRO
TEST(CelVariantTest, AvroVariantRecordIntoCel) {
    // A confluent.type.Variant record field: fromAvroValue surfaces it as a Variant
    // message, so variant(this.data) / variants.* see a first-class Variant.
    const char *schema = R"json({
        "type": "record", "name": "Holder",
        "confluent:rules": [
            {"name": "r",
             "expr": "variants.as(variants.field(variant(this.data), 'name'), 'string') == 'alice'"}
        ],
        "fields": [
            {"name": "data", "type": {
                "type": "record", "name": "confluent.type.Variant",
                "fields": [
                    {"name": "metadata", "type": "bytes"},
                    {"name": "value", "type": "bytes"}
                ]
            }}
        ]
    })json";
    auto valid_schema = ::avro::compileJsonSchemaFromString(schema);
    ::avro::GenericDatum datum(valid_schema);
    auto &holder = datum.value<::avro::GenericRecord>();
    auto &dataRec = holder.fieldAt(0).value<::avro::GenericRecord>();

    auto parsed = Variant::parseJson(R"({"name":"alice","age":30})");
    dataRec.field("metadata").value<std::vector<uint8_t>>() = parsed.metadataBytes();
    dataRec.field("value").value<std::vector<uint8_t>>() = parsed.valueBytes();

    CelValidator validator;
    auto violations = schemaregistry::serdes::avro::utils::validateMessage(
        validator, nlohmann::json::parse(schema), {}, datum, false);
    EXPECT_TRUE(violations.empty());
}
#endif
