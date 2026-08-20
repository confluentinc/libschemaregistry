/**
 * CelDecimalTimestampTest
 *
 * Tests the CEL Decimal (decimal / decimals.*) and Timestamp (timestamp.of)
 * function families, plus marshalling each of the four schema-side shapes into
 * CEL: an Avro logical timestamp, a Protobuf WKT timestamp, an Avro logical
 * decimal, and a Protobuf confluent.type.Decimal.
 */

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include <avro/Compiler.hh>
#include <avro/Generic.hh>
#include <avro/GenericDatum.hh>
#include <avro/ValidSchema.hh>

#include "confluent/type/decimal.pb.h"
#include "google/protobuf/timestamp.pb.h"
#include "schemaregistry/rules/cel/CelValidator.h"
#include "schemaregistry/serdes/ValidationRule.h"
#include "schemaregistry/serdes/avro/AvroUtils.h"
#include "schemaregistry/serdes/json/JsonValue.h"
#include "schemaregistry/serdes/protobuf/ProtobufTypes.h"

using namespace schemaregistry::serdes;
using schemaregistry::rules::cel::CelValidator;

namespace {

ValidationRule rule(const std::string &expr) {
    return ValidationRule{"r", "", expr, ""};
}

// Evaluate a boolean CEL rule against a dummy JSON value (for expressions that
// don't reference `this`).
bool evalBool(const std::string &expr) {
    CelValidator validator;
    auto value = json::makeJsonValue(nlohmann::json{{"x", 1}});
    auto result = validator.execute(rule(expr), *value);
    EXPECT_TRUE(std::holds_alternative<bool>(result)) << expr;
    return std::holds_alternative<bool>(result) && std::get<bool>(result);
}

}  // namespace

// ---- Decimal operators (cross-client pinned values) ----

TEST(CelDecimalTimestampTest, DecimalOperators) {
    EXPECT_TRUE(evalBool(R"(decimals.gt(decimal("12.34"), decimal("10.00")))"));
    EXPECT_FALSE(evalBool(R"(decimals.lt(decimal("12.34"), decimal("10.00")))"));
    EXPECT_TRUE(evalBool(R"(decimals.eq(decimal("1.50"), decimal("1.5")))"));
    EXPECT_TRUE(evalBool(
        R"(decimals.eq(decimals.add(decimal("12.34"), decimal("1.66")), decimal("14.00")))"));
    EXPECT_TRUE(
        evalBool(R"(decimals.eq(decimals.sub(decimal("5"), decimal("3")), decimal("2")))"));
    EXPECT_TRUE(
        evalBool(R"(decimals.eq(decimals.mul(decimal("1.5"), decimal("2")), decimal("3.0")))"));
    EXPECT_TRUE(
        evalBool(R"(decimals.eq(decimals.mod(decimal("10"), decimal("3")), decimal("1")))"));
    EXPECT_TRUE(evalBool(R"(decimals.eq(decimals.sqrt(decimal("144")), decimal("12")))"));
    EXPECT_TRUE(evalBool(
        R"(decimals.eq(decimals.greatest(decimal("2.5"), decimal("9.99")), decimal("9.99")))"));
    EXPECT_TRUE(evalBool(
        R"(decimals.eq(decimals.least(decimal("2.5"), decimal("9.99")), decimal("2.5")))"));
    EXPECT_TRUE(evalBool(R"(decimals.eq(decimals.neg(decimal("2.5")), decimal("-2.5")))"));
    EXPECT_TRUE(evalBool(R"(decimals.eq(decimals.abs(decimal("-2.5")), decimal("2.5")))"));
    EXPECT_TRUE(evalBool(R"(decimals.sign(decimal("-2.5")) == -1)"));
    EXPECT_TRUE(evalBool(R"(double(decimal("100.50")) == 100.5)"));
}

TEST(CelDecimalTimestampTest, DecimalStringForms) {
    // Division: exact terminates, non-terminating rounds to 38 significant digits HALF_UP.
    EXPECT_TRUE(evalBool(R"(string(decimals.div(decimal("1"), decimal("8"))) == "0.125")"));
    EXPECT_TRUE(evalBool(R"(string(decimals.div(decimal("12"), decimal("1"))) == "12")"));
    EXPECT_TRUE(evalBool(
        R"(string(decimals.div(decimal("1"), decimal("99"))) == "0.010101010101010101010101010101010101010")"));
    EXPECT_TRUE(evalBool(
        R"(string(decimals.div(decimal("2"), decimal("3"))) == "0.66666666666666666666666666666666666667")"));
    EXPECT_TRUE(evalBool(
        R"(string(decimals.sqrt(decimal("2"))) == "1.4142135623730950488016887242096980786")"));
    // Rounding family (Flink-aligned).
    EXPECT_TRUE(evalBool(R"(string(decimals.round(decimal("2.567"), 2)) == "2.57")"));
    EXPECT_TRUE(evalBool(R"(string(decimals.trunc(decimal("2.567"), 2)) == "2.56")"));
    EXPECT_TRUE(evalBool(R"(string(decimals.floor(decimal("2.9"))) == "2")"));
    EXPECT_TRUE(evalBool(R"(string(decimals.ceil(decimal("2.1"))) == "3")"));
    // string(Decimal) is plain (never scientific); scale preserved.
    EXPECT_TRUE(evalBool(R"(string(decimal("1.50")) == "1.50")"));
}

TEST(CelDecimalTimestampTest, DecimalFromBytesAndScale) {
    // 12.34 = unscaled 1234 (0x04D2) at scale 2.
    EXPECT_TRUE(evalBool(R"(decimals.eq(decimal(b"\x04\xd2", 2), decimal("12.34")))"));
}

// ---- is* validators (member-style) ----

TEST(CelDecimalTimestampTest, IsValidators) {
    EXPECT_TRUE(evalBool(R"("foo@bar.com".isEmail())"));
    EXPECT_FALSE(evalBool(R"("not-an-email".isEmail())"));
    EXPECT_TRUE(evalBool(R"("example.com".isHostname())"));
    EXPECT_TRUE(evalBool(R"("192.168.0.1".isIpv4())"));
    EXPECT_FALSE(evalBool(R"("192.168.0.1".isIpv6())"));
    EXPECT_TRUE(evalBool(R"("::1".isIpv6())"));
    EXPECT_TRUE(evalBool(R"("https://example.com/x".isUri())"));
    EXPECT_TRUE(evalBool(R"("12345678-1234-1234-1234-123456789012".isUuid())"));
    EXPECT_FALSE(evalBool(R"("nope".isUuid())"));
}

// ---- Marshalling: the four schema-side shapes into CEL ----

TEST(CelDecimalTimestampTest, ProtoConfluentTypeDecimalIntoCel) {
    // A confluent.type.Decimal message: 12.34 = unscaled 1234 (0x04D2) at scale 2.
    auto dec = std::make_unique<confluent::type::Decimal>();
    dec->set_value(std::string("\x04\xd2", 2));
    dec->set_scale(2);
    CelValidator validator;
    auto value = protobuf::makeProtobufValue(protobuf::ProtobufVariant(std::move(dec)));
    auto result =
        validator.execute(rule(R"(decimals.gt(decimal(this), decimal("10.00")))"), *value);
    ASSERT_TRUE(std::holds_alternative<bool>(result));
    EXPECT_TRUE(std::get<bool>(result));
}

TEST(CelDecimalTimestampTest, ProtoWktTimestampIntoCel) {
    auto ts = std::make_unique<google::protobuf::Timestamp>();
    ts->set_seconds(1577836800);  // 2020-01-01T00:00:00Z
    CelValidator validator;
    auto value = protobuf::makeProtobufValue(protobuf::ProtobufVariant(std::move(ts)));
    auto result = validator.execute(rule("this < now"), *value);
    ASSERT_TRUE(std::holds_alternative<bool>(result));
    EXPECT_TRUE(std::get<bool>(result));
}

TEST(CelDecimalTimestampTest, AvroLogicalDecimalIntoCel) {
    const char *schema = R"json({
        "type": "record", "name": "DecimalRecord",
        "confluent:rules": [
            {"name": "r", "expr": "decimals.gt(decimal(this.amount), decimal(\"10.00\"))"}
        ],
        "fields": [
            {"name": "amount", "type": {"type": "bytes", "logicalType": "decimal",
                                        "precision": 8, "scale": 2}}
        ]
    })json";
    auto valid_schema = ::avro::compileJsonSchemaFromString(schema);
    ::avro::GenericDatum datum(valid_schema);
    auto &record = datum.value<::avro::GenericRecord>();
    record.fieldAt(0).value<std::vector<uint8_t>>() = {0x04, 0xd2};  // 12.34

    CelValidator validator;
    auto violations = schemaregistry::serdes::avro::utils::validateMessage(validator, nlohmann::json::parse(schema), {},
                                                   datum, false);
    EXPECT_TRUE(violations.empty());
}

TEST(CelDecimalTimestampTest, AvroLogicalTimestampIntoCel) {
    const char *schema = R"json({
        "type": "record", "name": "TsRecord",
        "confluent:rules": [
            {"name": "r", "expr": "timestamp.of(this.ts) < now"}
        ],
        "fields": [
            {"name": "ts", "type": {"type": "long", "logicalType": "timestamp-millis"}}
        ]
    })json";
    auto valid_schema = ::avro::compileJsonSchemaFromString(schema);
    ::avro::GenericDatum datum(valid_schema);
    auto &record = datum.value<::avro::GenericRecord>();
    record.fieldAt(0).value<int64_t>() = 1577836800000LL;  // 2020-01-01

    CelValidator validator;
    auto violations = schemaregistry::serdes::avro::utils::validateMessage(validator, nlohmann::json::parse(schema), {},
                                                   datum, false);
    EXPECT_TRUE(violations.empty());
}
