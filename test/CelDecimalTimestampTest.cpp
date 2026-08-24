/**
 * CelDecimalTimestampTest
 *
 * Tests the CEL Decimal (decimal / decimals.*) and Timestamp (timestamp)
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

// Avro is optional: SCHEMAREGISTRY_WITH_RULES forces Protobuf on but not Avro, so the
// Avro marshalling tests below are compiled only when Avro is enabled.
#ifdef SCHEMAREGISTRY_TEST_WITH_AVRO
#include <avro/Compiler.hh>
#include <avro/Generic.hh>
#include <avro/GenericDatum.hh>
#include <avro/ValidSchema.hh>

#include "schemaregistry/serdes/avro/AvroUtils.h"
#endif

#include "confluent/type/decimal.pb.h"
#include "google/protobuf/timestamp.pb.h"
#include "schemaregistry/rules/cel/CelValidator.h"
#include "schemaregistry/serdes/ValidationRule.h"
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

// Execute a rule that is expected to fail, asserting the error message contains
// the given fragment (a CEL error surfaces from execute() as a thrown exception).
testing::AssertionResult errContains(const std::string &expr, const std::string &fragment) {
    CelValidator validator;
    auto value = json::makeJsonValue(nlohmann::json{{"x", 1}});
    try {
        validator.execute(rule(expr), *value);
        return testing::AssertionFailure() << "expected an error, none thrown: " << expr;
    } catch (const std::exception &e) {
        std::string what = e.what();
        if (what.find(fragment) != std::string::npos) {
            return testing::AssertionSuccess();
        }
        return testing::AssertionFailure()
               << "error did not contain '" << fragment << "': " << what;
    }
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

// ITEM A: add/sub/mul (and neg/abs) are exact/uncapped like Java's BigDecimal — the
// result is NOT rounded to the 38-digit div/sqrt precision. Each result below has 39
// significant digits, so a 38-digit cap would drop the trailing "1" (…001 -> …000).
// (Values are kept below 2^127 so they still round-trip through the 16-byte
// confluent.type.Decimal wire form.)
TEST(CelDecimalTimestampTest, ArithmeticExactUncapped) {
    // add: 10^38 + 1 = a 39-significant-digit sum.
    EXPECT_TRUE(evalBool(
        R"(string(decimals.add(decimal("100000000000000000000000000000000000000"), decimal("1"))) == "100000000000000000000000000000000000001")"));
    // sub: (10^38 + 2) - 1 = 10^38 + 1 (39 significant digits).
    EXPECT_TRUE(evalBool(
        R"(string(decimals.sub(decimal("100000000000000000000000000000000000002"), decimal("1"))) == "100000000000000000000000000000000000001")"));
    // mul: (10^19 + 1)^2 = a 39-significant-digit product.
    EXPECT_TRUE(evalBool(
        R"(string(decimals.mul(decimal("10000000000000000001"), decimal("10000000000000000001"))) == "100000000000000000020000000000000000001")"));
    // neg/abs preserve the full coefficient of a >38-digit operand (no cap/rounding).
    EXPECT_TRUE(evalBool(
        R"(string(decimals.neg(decimals.add(decimal("100000000000000000000000000000000000000"), decimal("1")))) == "-100000000000000000000000000000000000001")"));
    EXPECT_TRUE(evalBool(
        R"(string(decimals.abs(decimals.sub(decimal("-100000000000000000000000000000000000002"), decimal("-1")))) == "100000000000000000000000000000000000001")"));
    // Contract check: div/sqrt still cap at 38 significant digits (HALF_UP) — unchanged.
    EXPECT_TRUE(evalBool(
        R"(string(decimals.div(decimal("2"), decimal("3"))) == "0.66666666666666666666666666666666666667")"));
}

// BUG K: decimals.mod must be exact like Java's BigDecimal.remainder (no MathContext).
// The remainder is computed from an integer quotient; when that quotient exceeds 38 digits
// the 38-digit context would trap (Division_impossible) and surface a CEL error. Using the
// exact (MaxContext) context makes mod succeed and return the true remainder.
// 10^40 mod 3 == 1 (10 ≡ 1 mod 3, so 10^40 ≡ 1), with an integer quotient of ~40 digits.
TEST(CelDecimalTimestampTest, ModExactLargeQuotient) {
    EXPECT_TRUE(evalBool(
        R"(decimals.eq(decimals.mod(decimal("1E40"), decimal("3")), decimal("1")))"));
    EXPECT_TRUE(evalBool(R"(string(decimals.mod(decimal("1E40"), decimal("3"))) == "1")"));
    // Normal (small-quotient) mod cases remain unchanged.
    EXPECT_TRUE(
        evalBool(R"(decimals.eq(decimals.mod(decimal("10"), decimal("3")), decimal("1")))"));
    EXPECT_TRUE(
        evalBool(R"(decimals.eq(decimals.mod(decimal("10.5"), decimal("3")), decimal("1.5")))"));
    // Remainder takes the sign of the dividend (SQL MOD semantics).
    EXPECT_TRUE(
        evalBool(R"(decimals.eq(decimals.mod(decimal("-10"), decimal("3")), decimal("-1")))"));
}

// ITEM F: a scale argument outside int32 range must raise an error (Java's requireIntScale
// / Math.toIntExact), not silently wrap to the low 32 bits. 3000000000 > INT32_MAX.
TEST(CelDecimalTimestampTest, ScaleArgOutOfIntRangeErrors) {
    EXPECT_TRUE(errContains(R"(decimals.round(decimal("1.5"), 3000000000))",
                            "scale out of int range"));
    EXPECT_TRUE(errContains(R"(decimals.trunc(decimal("1.5"), 3000000000))",
                            "scale out of int range"));
    EXPECT_TRUE(errContains(R"(decimals.eq(decimal(b"\x01", 3000000000), decimal("1")))",
                            "scale out of int range"));
}

// FIX 1: double(decimal) must distinguish underflow from overflow. std::stod threw
// std::out_of_range on BOTH, so tiny magnitudes wrongly became ±Infinity. strtod returns
// the nearest subnormal/0.0 on underflow and ±Infinity on overflow — matching Java's
// BigDecimal.doubleValue().
TEST(CelDecimalTimestampTest, DoubleUnderflowOverflow) {
    // Total underflow -> 0.0 (was +Infinity).
    EXPECT_TRUE(evalBool(R"(double(decimal("1e-400")) == 0.0)"));
    // Partial underflow -> a tiny positive subnormal, NOT 0.0 and NOT Infinity.
    EXPECT_TRUE(evalBool(R"(double(decimal("1e-320")) > 0.0)"));
    EXPECT_TRUE(evalBool(R"(double(decimal("1e-320")) < 1.0e-300)"));
    // Genuine overflow -> ±Infinity (larger/smaller than any finite double ~1.8e308).
    EXPECT_TRUE(evalBool(R"(double(decimal("1e400")) > 1.7e308)"));
    EXPECT_TRUE(evalBool(R"(double(decimal("-1e400")) < -1.7e308)"));
    // A representable magnitude still round-trips exactly.
    EXPECT_TRUE(evalBool(R"(double(decimal("100.50")) == 100.5)"));
}

// FIX 2: CEL `==` on two Decimals must be NUMERIC (scale-insensitive), matching decimals.eq
// — not proto-message equality (which is scale/encoding-sensitive: 2.0 has value=20/scale=1,
// 2.00 has value=200/scale=2, so message equality would report them unequal).
TEST(CelDecimalTimestampTest, DecimalNumericEquality) {
    EXPECT_TRUE(evalBool(R"(decimal("2.0") == decimal("2.00"))"));
    EXPECT_TRUE(evalBool(R"(decimal("2.0") == decimal("2.0"))"));
    EXPECT_FALSE(evalBool(R"(decimal("2.0") == decimal("2.1"))"));
    // != negates.
    EXPECT_FALSE(evalBool(R"(decimal("2.0") != decimal("2.00"))"));
    EXPECT_TRUE(evalBool(R"(decimal("2.0") != decimal("2.1"))"));
    // Consistent with decimals.eq for a differently-scaled equal pair.
    EXPECT_TRUE(evalBool(R"((decimal("2.0") == decimal("2.00")) == decimals.eq(decimal("2.0"), decimal("2.00")))"));
}

// Registering a custom `_==_`/`_!=_` disables cel-cpp's built-in equality step, so confirm
// standard heterogeneous equality for all the other types still works via our delegation to
// CelValueEqualImpl.
TEST(CelDecimalTimestampTest, EqualityStillWorksForOtherTypes) {
    EXPECT_TRUE(evalBool(R"(1 == 1)"));
    EXPECT_FALSE(evalBool(R"(1 == 2)"));
    EXPECT_TRUE(evalBool(R"(1 != 2)"));
    EXPECT_TRUE(evalBool(R"("abc" == "abc")"));
    EXPECT_FALSE(evalBool(R"("abc" == "abd")"));
    EXPECT_TRUE(evalBool(R"(true == true)"));
    EXPECT_TRUE(evalBool(R"(1 == 1.0)"));           // heterogeneous int/double
    EXPECT_TRUE(evalBool(R"([1, 2, 3] == [1, 2, 3])"));
    EXPECT_FALSE(evalBool(R"([1, 2] == [1, 2, 3])"));
    EXPECT_TRUE(evalBool(R"({"a": 1} == {"a": 1})"));
    EXPECT_TRUE(evalBool(R"(b"\x01\x02" == b"\x01\x02")"));
}

TEST(CelDecimalTimestampTest, DecimalFromBytesAndScale) {
    // 12.34 = unscaled 1234 (0x04D2) at scale 2.
    EXPECT_TRUE(evalBool(R"(decimals.eq(decimal(b"\x04\xd2", 2), decimal("12.34")))"));
}

// CROSS-CLIENT CONTRACT: the standard CEL `timestamp(<int>)` conversion interprets a bare
// integer as epoch SECONDS (never millis), matching cel-go / cel-java and cel-cpp's own
// `{kInt64}` overload in runtime/standard/type_conversion_functions.cc, which calls
// absl::FromUnixSeconds. This test pins that so the meaning can't drift to millis.
TEST(CelDecimalTimestampTest, TimestampBareIntIsEpochSeconds) {
    // 1700000000 seconds == 2023-11-14T22:13:20Z.
    EXPECT_TRUE(evalBool(R"(timestamp(1700000000) == timestamp("2023-11-14T22:13:20Z"))"));
    EXPECT_TRUE(evalBool(R"(string(timestamp(1700000000)) == "2023-11-14T22:13:20Z")"));
    // ...and is NOT the millis reading of the same integer (1700000000 ms == 1970-01-20T16:13:20Z).
    EXPECT_FALSE(evalBool(R"(timestamp(1700000000) == timestamp("1970-01-20T16:13:20Z"))"));
    // Component accessors agree.
    EXPECT_TRUE(evalBool(R"(timestamp(1700000000).getFullYear() == 2023)"));
    // Negative / pre-epoch ints go backwards from the epoch in seconds.
    EXPECT_TRUE(evalBool(R"(timestamp(-1) == timestamp("1969-12-31T23:59:59Z"))"));
    EXPECT_TRUE(evalBool(R"(timestamp(-86400) == timestamp("1969-12-31T00:00:00Z"))"));
    EXPECT_TRUE(evalBool(R"(timestamp(0) == timestamp("1970-01-01T00:00:00Z"))"));
    // A seconds value beyond year 9999 overflows (proof the argument is scaled as seconds,
    // not millis -- as millis this would be a valid 2001 timestamp).
    EXPECT_TRUE(errContains(R"(timestamp(999999999999) == timestamp(0))", "timestamp overflow"));
    // The two-argument timestamp(value, precision) form is unaffected by the above: it honors
    // the explicit precision, so each precision scales as named.
    EXPECT_TRUE(evalBool(R"(timestamp(1700000000, 0) == timestamp(1700000000))"));
    EXPECT_TRUE(evalBool(R"(timestamp(1700000000000, 3) == timestamp(1700000000))"));
    EXPECT_TRUE(evalBool(R"(timestamp(1700000000000000, 6) == timestamp(1700000000))"));
    EXPECT_TRUE(evalBool(R"(timestamp(1700000000000000000, 9) == timestamp(1700000000))"));
    // Sub-second precision survives, and the same integer differs across the two arities.
    EXPECT_TRUE(evalBool(
        R"(timestamp(1700000000123, 3) == timestamp("2023-11-14T22:13:20.123Z"))"));
    EXPECT_TRUE(evalBool(R"(timestamp(1700000000, 3) != timestamp(1700000000))"));
    // With the unit a number rather than a name, rejecting anything outside {0, 3, 6, 9} is
    // the only thing between a typo and a silently wrong instant.
    for (const char* expr : {R"(timestamp(1700000000, 1) == timestamp(0))",
                             R"(timestamp(1700000000, 2) == timestamp(0))",
                             R"(timestamp(1700000000, 4) == timestamp(0))",
                             R"(timestamp(1700000000, 7) == timestamp(0))",
                             R"(timestamp(1700000000, 10) == timestamp(0))",
                             R"(timestamp(1700000000, -3) == timestamp(0))"}) {
        EXPECT_TRUE(errContains(expr, "unknown precision")) << expr;
    }
    // The namespaced form is gone: `timestamp.of` no longer resolves, which cel-cpp reports
    // at compile time rather than as an evaluation error.
    EXPECT_TRUE(errContains(R"(timestamp.of(1700000000000, 3) == timestamp(1700000000))",
                            "No overload found"));
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

// Cross-client parity: a bare confluent.type.Decimal field is usable with decimals.*, ==,
// string() and double() with **no decimal(...) call** on it. The discriminating case is the
// scale-differing equality: a client comparing decimals by their protobuf encoding (unscaled
// bytes plus scale, field by field) answers false for decimal("12.340"), because 12.34 and
// 12.340 are the same number in two different encodings.
TEST(CelDecimalTimestampTest, ProtoDecimalNeedsNoConstructor) {
    auto evalDecimal = [](const std::string &expr) {
        // 12.34 = unscaled 1234 (0x04D2) at scale 2.
        auto dec = std::make_unique<confluent::type::Decimal>();
        dec->set_value(std::string("\x04\xd2", 2));
        dec->set_scale(2);
        CelValidator validator;
        auto value =
            protobuf::makeProtobufValue(protobuf::ProtobufVariant(std::move(dec)));
        auto result = validator.execute(rule(expr), *value);
        EXPECT_TRUE(std::holds_alternative<bool>(result)) << expr;
        return std::holds_alternative<bool>(result) && std::get<bool>(result);
    };

    // Bare: no constructor call on the field.
    EXPECT_TRUE(evalDecimal(R"(decimals.eq(this, decimal("12.34")))"));
    EXPECT_TRUE(evalDecimal(R"(decimals.gt(this, decimal("10.00")))"));
    // The wrapped form must keep working (decimal(...) re-entry).
    EXPECT_TRUE(evalDecimal(R"(decimals.eq(decimal(this), decimal("12.34")))"));
    // `==` is numeric on it: 12.34 equals 12.340 despite the differing scale.
    EXPECT_TRUE(evalDecimal(R"(this == decimal("12.340"))"));
    EXPECT_FALSE(evalDecimal(R"(this != decimal("12.340"))"));
    EXPECT_TRUE(evalDecimal(R"(decimals.lt(this, decimal("100")))"));
    // Negative control: a false comparison must still be false.
    EXPECT_FALSE(evalDecimal(R"(decimals.gt(this, decimal("100")))"));
    EXPECT_TRUE(evalDecimal(R"(string(this) == "12.34")"));
    EXPECT_TRUE(evalDecimal(R"(double(this) == 12.34)"));
}

// A Decimal inside a list or map compares numerically too, and `in` follows the same equality.
// Making the operands' own == numeric is not enough on its own: the general implementation
// recurses into containers with its own equality, so a Decimal nested one level deep was compared
// by its encoding and `[a] == [b]` disagreed with `a == b` on the very same values.
TEST(CelDecimalTimestampTest, ContainerEqualityIsNumericForNestedDecimals) {
    auto ev = [](const std::string &expr) {
        // 12.34 = unscaled 1234 (0x04D2) at scale 2.
        auto dec = std::make_unique<confluent::type::Decimal>();
        dec->set_value(std::string("\x04\xd2", 2));
        dec->set_scale(2);
        CelValidator validator;
        auto value =
            protobuf::makeProtobufValue(protobuf::ProtobufVariant(std::move(dec)));
        auto result = validator.execute(rule(expr), *value);
        EXPECT_TRUE(std::holds_alternative<bool>(result)) << expr;
        return std::holds_alternative<bool>(result) && std::get<bool>(result);
    };

    // 12.34 and 12.340 are the same number in two encodings.
    EXPECT_TRUE(ev(R"(this == decimal("12.340"))"));
    EXPECT_TRUE(ev(R"([this] == [decimal("12.340")])"));
    EXPECT_TRUE(ev(R"({'k': this} == {'k': decimal("12.340")})"));
    EXPECT_TRUE(ev(R"([[this]] == [[decimal("12.340")]])"));
    // `in` is NOT numeric here - cel-cpp's builtin @in cannot be taken over from the
    // registry (see registerEquality). Pinned as-is so the divergence from == is explicit,
    // and so this flips if membership is ever fixed.
    EXPECT_FALSE(ev(R"(this in [decimal("12.340")])"));
    EXPECT_TRUE(ev(R"(this in [decimal("12.34")])"));
    // Negative controls.
    EXPECT_FALSE(ev(R"([this] == [decimal("9")])"));
    EXPECT_FALSE(ev(R"([this] == [this, this])"));
    EXPECT_FALSE(ev(R"(this in [decimal("9")])"));
    // Decimal-free comparisons keep general semantics - the recursion is gated on a Decimal.
    EXPECT_TRUE(ev("[1, 2] == [1, 2]"));
    EXPECT_FALSE(ev("[1, 2] == [2, 1]"));
    EXPECT_TRUE(ev("{'a': 1} == {'a': 1}"));
    EXPECT_TRUE(ev("2 in [1, 2]"));
    EXPECT_FALSE(ev("3 in [1, 2]"));
    EXPECT_TRUE(ev("'x' in ['x', 'y']"));
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

#ifdef SCHEMAREGISTRY_TEST_WITH_AVRO
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
            {"name": "r", "expr": "timestamp(this.ts) < now"}
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

// Cross-client parity: an Avro `decimal` logical type is usable as a Decimal with **no
// `decimal(...)` call**, and the wrapped form keeps working alongside it. fromAvroValue applies
// the schema's scale and builds a confluent.type.Decimal message, which is this client's in-CEL
// decimal representation, so decimals.* (declared over {kMessage, kMessage}) accept it directly.
TEST(CelDecimalTimestampTest, AvroLogicalDecimalNeedsNoConstructor) {
    auto evalDecimal = [](const std::string &expr) {
        std::string schema = R"json({
            "type": "record", "name": "DecimalRecord",
            "confluent:rules": [
                {"name": "r", "expr": ")json" + expr + R"json("}
            ],
            "fields": [
                {"name": "amount", "type": {"type": "bytes", "logicalType": "decimal",
                                            "precision": 8, "scale": 2}}
            ]
        })json";
        auto valid_schema = ::avro::compileJsonSchemaFromString(schema);
        ::avro::GenericDatum datum(valid_schema);
        // 12.34 = unscaled 1234 = 0x04d2 at the schema's scale of 2.
        datum.value<::avro::GenericRecord>().fieldAt(0).value<std::vector<uint8_t>>() = {0x04,
                                                                                        0xd2};
        CelValidator validator;
        return schemaregistry::serdes::avro::utils::validateMessage(
                   validator, nlohmann::json::parse(schema), {}, datum, false)
            .empty();
    };

    // Bare: no constructor call on the field.
    EXPECT_TRUE(evalDecimal(R"(decimals.eq(this.amount, decimal(\"12.34\")))"));
    EXPECT_TRUE(evalDecimal(R"(decimals.gt(this.amount, decimal(\"10.00\")))"));
    // The wrapped form must keep working (decimal(...) re-entry).
    EXPECT_TRUE(evalDecimal(R"(decimals.eq(decimal(this.amount), decimal(\"12.34\")))"));
    // `==` is numeric on it: 12.34 equals 12.340 despite the differing scale. That holds only
    // because registerEquality overrides _==_ for a pair of Decimal messages — plain proto
    // message equality is structural and would answer false on the differing scale.
    EXPECT_TRUE(evalDecimal(R"(this.amount == decimal(\"12.340\"))"));
    // The schema's scale is applied, not guessed: as scale 0 this would be 1234.
    EXPECT_TRUE(evalDecimal(R"(decimals.lt(this.amount, decimal(\"100\")))"));
    // Negative control: a false comparison must fail.
    EXPECT_FALSE(evalDecimal(R"(decimals.gt(this.amount, decimal(\"100\")))"));
}

// Cross-client parity: an Avro timestamp logical type is usable as a timestamp with **no
// constructor call at all**. fromAvroValue converts it straight to a CEL timestamp, so it is
// comparable against `now` and carries the timestamp accessors. Every one of the seven clients
// has this test; the constructor is only needed for a plain numeric field whose unit the schema
// cannot supply.
TEST(CelDecimalTimestampTest, AvroLogicalTimestampNeedsNoConstructor) {
    auto evalTs = [](const std::string &expr, int64_t millis) {
        std::string schema = R"json({
            "type": "record", "name": "TsRecord",
            "confluent:rules": [
                {"name": "r", "expr": ")json" + expr + R"json("}
            ],
            "fields": [
                {"name": "ts", "type": {"type": "long", "logicalType": "timestamp-millis"}}
            ]
        })json";
        auto valid_schema = ::avro::compileJsonSchemaFromString(schema);
        ::avro::GenericDatum datum(valid_schema);
        datum.value<::avro::GenericRecord>().fieldAt(0).value<int64_t>() = millis;
        CelValidator validator;
        return schemaregistry::serdes::avro::utils::validateMessage(
                   validator, nlohmann::json::parse(schema), {}, datum, false)
            .empty();
    };

    const int64_t past = 1577836800000LL;    // 2020-01-01
    const int64_t future = 4102444800000LL;  // 2100-01-01
    // Bare comparison against `now`, plus the negative control that proves it really compares.
    EXPECT_TRUE(evalTs("this.ts < now", past));
    EXPECT_FALSE(evalTs("this.ts < now", future));
    // The schema's millis unit is applied, not guessed, and the accessors work directly.
    EXPECT_TRUE(evalTs("this.ts == timestamp(\\\"2023-11-14T22:13:20.123Z\\\")", 1700000000123LL));
    EXPECT_TRUE(evalTs("this.ts.getFullYear() == 2023", 1700000000123LL));
}
#endif  // SCHEMAREGISTRY_TEST_WITH_AVRO
