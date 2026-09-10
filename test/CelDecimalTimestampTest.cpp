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
#include "schemaregistry/rules/cel/DecimalUtil.h"
#include "schemaregistry/serdes/ValidationRule.h"
#include "schemaregistry/serdes/json/JsonValue.h"
#include "schemaregistry/serdes/protobuf/ProtobufTypes.h"

using namespace schemaregistry::serdes;
using schemaregistry::rules::cel::CelValidator;

using schemaregistry::rules::cel::DecimalUtil;

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

// An exact div/sqrt result carries the reference's *preferred* scale rather than the
// quotient's own natural scale: `dividend.scale - divisor.scale` for divide, `scale / 2`
// truncated toward zero for square root. Trailing zeros are kept down to it and padded up to
// it, never stripped below.
//
// Division needs no code of ours - libmpdec's ideal exponent for divide is already the same
// quantity - so those cases are pinned rather than fixed. Square root is where libmpdec and
// the reference part company: the spec floors `exponent / 2` where BigDecimal truncates
// `scale / 2`, so they agree on an even scale and differ by one on an odd one. Natively
// `sqrt(9.0)` was "3.0" and `sqrt(16.000)` was "4.00".
TEST(CelDecimalTimestampTest, ExactDivAndSqrtCarryThePreferredScale) {
    // divide
    EXPECT_TRUE(evalBool(R"(string(decimals.div(decimal("10.0"), decimal("2.0"))) == "5")"));
    EXPECT_TRUE(evalBool(R"(string(decimals.div(decimal("10.0"), decimal("2"))) == "5.0")"));
    EXPECT_TRUE(evalBool(R"(string(decimals.div(decimal("6.0"), decimal("3"))) == "2.0")"));
    EXPECT_TRUE(evalBool(R"(string(decimals.div(decimal("10.00"), decimal("2"))) == "5.00")"));
    EXPECT_TRUE(evalBool(R"(string(decimals.div(decimal("1.000"), decimal("0.1"))) == "10.00")"));
    EXPECT_TRUE(evalBool(R"(string(decimals.div(decimal("-6.0"), decimal("3"))) == "-2.0")"));
    // ...but never below the exact quotient's own scale: 10/4 is 2.5 at a preferred 0.
    EXPECT_TRUE(evalBool(R"(string(decimals.div(decimal("10"), decimal("4"))) == "2.5")"));
    // sqrt, even scale: already agreed natively.
    EXPECT_TRUE(evalBool(R"(string(decimals.sqrt(decimal("4.00"))) == "2.0")"));
    EXPECT_TRUE(evalBool(R"(string(decimals.sqrt(decimal("100.0000"))) == "10.00")"));
    EXPECT_TRUE(evalBool(R"(string(decimals.sqrt(decimal("0.0001"))) == "0.01")"));
    // sqrt, odd scale: the divergence.
    EXPECT_TRUE(evalBool(R"(string(decimals.sqrt(decimal("9.0"))) == "3")"));
    EXPECT_TRUE(evalBool(R"(string(decimals.sqrt(decimal("400.0"))) == "20")"));
    EXPECT_TRUE(evalBool(R"(string(decimals.sqrt(decimal("16.000"))) == "4.0")"));
    // An inexact root is left at full precision - a trailing zero there is significant.
    EXPECT_TRUE(evalBool(
        R"(string(decimals.sqrt(decimal("2"))) == "1.4142135623730950488016887242096980786")"));
}

// The scale itself, not its rendering. `string()` is plain form, which reads the same at
// several scales - "0", "0" and "500" all hide the difference - but the scale is a field of
// the confluent.type.Decimal encoding, so it has to be asserted directly. It can be: the
// result is that message, so a rule can select `.scale` off it.
TEST(CelDecimalTimestampTest, PreferredScaleIsTheScaleNotTheRendering) {
    // A zero takes the preferred scale outright, in both directions - the reference returns
    // zeroValueOf(preferredScale). A strip loop guarded on a non-zero coefficient cannot
    // lower a zero's scale, which is why that case is handled separately.
    EXPECT_TRUE(evalBool(R"(decimals.sqrt(decimal("0")).scale == 0)"));
    EXPECT_TRUE(evalBool(R"(decimals.sqrt(decimal("0.0")).scale == 0)"));
    EXPECT_TRUE(evalBool(R"(decimals.sqrt(decimal("0.00")).scale == 1)"));
    EXPECT_TRUE(evalBool(R"(decimals.sqrt(decimal("0.000")).scale == 1)"));
    EXPECT_TRUE(evalBool(R"(decimals.div(decimal("0.00"), decimal("3")).scale == 2)"));
    EXPECT_TRUE(evalBool(R"(decimals.div(decimal("0"), decimal("3.00")).scale == -2)"));
    // Odd scale, and odd *negative* scale: -3/2 truncates toward zero to -1, not down to -2.
    EXPECT_TRUE(evalBool(R"(decimals.sqrt(decimal("9.0")).scale == 0)"));
    EXPECT_TRUE(evalBool(R"(decimals.sqrt(decimal("16.000")).scale == 1)"));
    EXPECT_TRUE(evalBool(R"(decimals.sqrt(decimal("4E+2")).scale == -1)"));
    EXPECT_TRUE(evalBool(R"(decimals.sqrt(decimal("1E+4")).scale == -2)"));
    EXPECT_TRUE(evalBool(R"(decimals.sqrt(decimal("250E+3")).scale == -1)"));
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
        // 1234 is four digits; `in` below compares precision too, so it has to be set.
        dec->set_precision(4);
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
    //
    // Note what that costs now that precision is written: `in` compares all three fields, so
    // membership is sensitive to the producer's precision as well as its scale. Two encodings
    // of one number differ under `in` for one more reason than before. `==` is unaffected -
    // it goes through the numeric override above.
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

// Cross-client parity: `timestamp-nanos` had no arm in fromAvroValue's logical-type switch, so it
// fell through to the base-type switch and reached CEL as a bare **int** of the raw nanosecond
// count. `timestamp(this.ts)` then read that as epoch *seconds* - 1.7e18 seconds - where Java
// (epochOf(value, 1_000_000_000L, 1L)) gives the correct instant. Millis and micros were handled,
// so only nanos was affected.
//
// Asserting against a fixed instant with sub-second digits is what makes this load-bearing: a
// regression cannot satisfy it by returning a plausible-looking timestamp.
TEST(CelDecimalTimestampTest, AvroLogicalTimestampNanosIntoCel) {
    const char *schema = R"json({
        "type": "record", "name": "TsNanosRecord",
        "confluent:rules": [
            {"name": "r",
             "expr": "timestamp(this.ts) == timestamp('2023-11-14T22:13:20.123456789Z')"}
        ],
        "fields": [
            {"name": "ts", "type": {"type": "long", "logicalType": "timestamp-nanos"}}
        ]
    })json";
    auto valid_schema = ::avro::compileJsonSchemaFromString(schema);
    ::avro::GenericDatum datum(valid_schema);
    datum.value<::avro::GenericRecord>().fieldAt(0).value<int64_t>() =
        1700000000123456789LL;

    CelValidator validator;
    auto violations = schemaregistry::serdes::avro::utils::validateMessage(
        validator, nlohmann::json::parse(schema), {}, datum, false);
    EXPECT_TRUE(violations.empty());
}

// Cross-client parity: an Avro `decimal` logical type is usable as a Decimal with **no
// `decimal(...)` call**, and the wrapped form keeps working alongside it. fromAvroValue applies
// the schema's scale and builds a confluent.type.Decimal message, which is this client's in-CEL
// decimal representation, so decimals.* (declared over {kMessage, kMessage}) accept it directly.
// An Avro `fixed` reaches CEL as bytes and an `enum` as its symbol name, matching the Java
// reference (GenericFixed -> CelByteString, GenericEnumSymbol -> String) and Rust. Neither had an
// arm in fromAvroValue's base-type switch, so both fell to its default and became CEL **null** -
// which made every comparison silently *false* rather than an error. For a validation rule that is
// the worst failure mode: `this.status == 'ACTIVE'` failed the record with nothing to show the rule
// itself was broken.
//
// Note a `fixed` carrying the `decimal` logical type was always handled by the logical-type switch,
// which is why only the bare forms were affected.
TEST(CelDecimalTimestampTest, AvroFixedAndEnumIntoCel) {
    auto eval = [](const std::string &expr) {
        std::string schema = R"json({
            "type": "record", "name": "FixedEnumRecord",
            "confluent:rules": [
                {"name": "r", "expr": ")json" + expr + R"json("}
            ],
            "fields": [
                {"name": "fx", "type": {"type": "fixed", "name": "F4", "size": 4}},
                {"name": "en", "type": {"type": "enum", "name": "E",
                                        "symbols": ["ACTIVE", "INACTIVE"]}}
            ]
        })json";
        auto valid_schema = ::avro::compileJsonSchemaFromString(schema);
        ::avro::GenericDatum datum(valid_schema);
        auto &rec = datum.value<::avro::GenericRecord>();
        rec.field("fx").value<::avro::GenericFixed>().value() =
            std::vector<uint8_t>{1, 2, 3, 4};
        rec.field("en").value<::avro::GenericEnum>().set("ACTIVE");
        CelValidator validator;
        return schemaregistry::serdes::avro::utils::validateMessage(
                   validator, nlohmann::json::parse(schema), {}, datum, false)
            .empty();
    };

    // fixed -> bytes
    EXPECT_TRUE(eval(R"(this.fx == b'\\x01\\x02\\x03\\x04')"));
    EXPECT_TRUE(eval("size(this.fx) == 4"));
    EXPECT_TRUE(eval("type(this.fx) == bytes"));
    // enum -> its symbol name (NOT an ordinal: a protobuf enum is an int, an Avro enum is a name)
    EXPECT_TRUE(eval(R"(this.en == 'ACTIVE')"));
    EXPECT_TRUE(eval(R"(this.en != 'INACTIVE')"));
    EXPECT_TRUE(eval("type(this.en) == string"));
    EXPECT_TRUE(eval(R"(this.en in ['ACTIVE', 'INACTIVE'])"));
    // A wrong comparison must be a clean false, and these would also have been false when the
    // fields read as null - so they are paired with the true cases above to be meaningful.
    EXPECT_FALSE(eval(R"(this.en == 'INACTIVE')"));
    EXPECT_FALSE(eval(R"(this.fx == b'\\x09')"));
}

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

// The coefficient goes out at whatever width it needs. `confluent.type.Decimal.value` is a
// variable-length bytes field and the JVM fills it from `BigInteger.toByteArray()`, which has
// no ceiling; this codec used to read the coefficient out of an `mpd_uint128_triple_t` and
// refuse anything past signed 128 bits, on both sides.
//
// That bound was not an edge: CEL's decimal precision is 38 significant digits, and adding or
// multiplying two values at that precision crosses it immediately. Measured against the JDK:
//
//   38 nines                      precision 38  16 bytes  round-trips
//   38 nines + 38 nines           precision 39  17 bytes  round-trips
//   38 nines * 38 nines           precision 76  32 bytes  round-trips
//   2^127                         precision 39  17 bytes  round-trips
//
// so `decimals.add` on two values at the documented precision was an error here and ordinary
// there. What the old bound was really guarding against - a magnitude whose top bit is set
// reading back as its own negation, 2^127 becoming -2^127 - is now handled by prepending the
// zero byte `BigInteger.toByteArray()` prepends, rather than by refusing the value.
TEST(CelDecimalTimestampTest, AWideCoefficientRoundTripsAtAnyWidth) {
    // The old boundary, from both sides of it.
    EXPECT_TRUE(evalBool(
        "string(decimal(\"170141183460469231731687303715884105727\")) == "
        "\"170141183460469231731687303715884105727\""));
    EXPECT_TRUE(evalBool(
        "string(decimal(\"170141183460469231731687303715884105728\")) == "
        "\"170141183460469231731687303715884105728\""));
    EXPECT_TRUE(evalBool(
        "string(decimal(\"-170141183460469231731687303715884105728\")) == "
        "\"-170141183460469231731687303715884105728\""));
    EXPECT_TRUE(evalBool(
        "string(decimal(\"340282366920938463463374607431768211455\")) == "
        "\"340282366920938463463374607431768211455\""));
    EXPECT_TRUE(evalBool(
        "string(decimal(\"-340282366920938463463374607431768211455\")) == "
        "\"-340282366920938463463374607431768211455\""));

    // Exact arithmetic at CEL's own precision, which is what made the bound reachable.
    EXPECT_TRUE(evalBool(
        "string(decimals.add(decimal(\"99999999999999999999999999999999999999\"), "
        "decimal(\"99999999999999999999999999999999999999\"))) == "
        "\"199999999999999999999999999999999999998\""));
    EXPECT_TRUE(evalBool(
        "string(decimals.mul(decimal(\"99999999999999999999999999999999999999\"), "
        "decimal(\"99999999999999999999999999999999999999\"))) == "
        "\"9999999999999999999999999999999999999800000000000000000000000000000000000001\""));

    // The small and awkward widths, which the two's-complement trimming has to get exactly
    // right: -1 is one byte, -128 is one byte (not 0xFF,0x80), -256 is two.
    EXPECT_TRUE(evalBool("string(decimal(\"-1\")) == \"-1\""));
    EXPECT_TRUE(evalBool("string(decimal(\"-128\")) == \"-128\""));
    EXPECT_TRUE(evalBool("string(decimal(\"-129\")) == \"-129\""));
    EXPECT_TRUE(evalBool("string(decimal(\"-256\")) == \"-256\""));
    EXPECT_TRUE(evalBool("string(decimal(\"127\")) == \"127\""));
    EXPECT_TRUE(evalBool("string(decimal(\"128\")) == \"128\""));
    EXPECT_TRUE(evalBool("string(decimal(\"0\")) == \"0\""));
    EXPECT_TRUE(evalBool("string(decimal(\"0.00\")) == \"0.00\""));
    // And a wide coefficient with a scale, so the two are independent.
    EXPECT_TRUE(evalBool(
        "string(decimal(\"3402823669209384634633746074317682114.55\")) == "
        "\"3402823669209384634633746074317682114.55\""));
}

// `confluent.type.Decimal.precision` is the unscaled value's digit count, which is what
// BigDecimal.precision() reports. This client set it nowhere, so every decimal it produced
// carried 0 - a value the reference cannot produce, since precision() is never less than 1
// (zero's precision is 1) - and a JVM consumer rewrites such a message on its next touch.
// Measured against the JDK: new BigDecimal("12.34").precision() is 4, ("12.340") is 5,
// ("0") is 1.
TEST(CelDecimalTimestampTest, PrecisionIsTheUnscaledDigitCount) {
    EXPECT_TRUE(evalBool(R"(decimal("12.34").precision == 4u)"));
    EXPECT_TRUE(evalBool(R"(decimal("1").precision == 1u)"));
    EXPECT_TRUE(evalBool(R"(decimal("0").precision == 1u)"));
    EXPECT_TRUE(evalBool(R"(decimal("-999.5").precision == 4u)"));
    // Trailing zeros count: 12.340 is unscaled 12340, five digits, not four.
    EXPECT_TRUE(evalBool(R"(decimal("12.340").precision == 5u)"));
    // It tracks the coefficient the encoder actually wrote, at any width.
    EXPECT_TRUE(evalBool(
        R"(decimals.mul(decimal("99999999999999999999999999999999999999"), )"
        R"(decimal("99999999999999999999999999999999999999")).precision == 76u)"));
    // Negative controls: 0 is what the old code wrote for everything.
    EXPECT_FALSE(evalBool(R"(decimal("12.34").precision == 0u)"));
    EXPECT_FALSE(evalBool(R"(decimal("12.34").precision == 2u)"));

    // Reading ignores it, as every non-Java client does: a message whose declared precision
    // disagrees with its coefficient is read for its value and scale alone, not rounded to
    // the declared digits the way BigDecimal(unscaled, scale, MathContext(precision)) would.
    // Declared 2 against a four-digit coefficient: 12.34 stays 12.34, it does not become 12.
    auto readWithDeclaredPrecision = [](uint32_t precision) {
        auto dec = std::make_unique<confluent::type::Decimal>();
        dec->set_value(std::string("\x04\xd2", 2));  // unscaled 1234
        dec->set_scale(2);
        dec->set_precision(precision);
        CelValidator validator;
        auto value =
            protobuf::makeProtobufValue(protobuf::ProtobufVariant(std::move(dec)));
        auto result = validator.execute(rule(R"(string(decimal(this)))"), *value);
        return std::get<std::string>(result);
    };
    EXPECT_EQ(readWithDeclaredPrecision(4), "12.34");
    EXPECT_EQ(readWithDeclaredPrecision(2), "12.34");
    EXPECT_EQ(readWithDeclaredPrecision(0), "12.34");
}

// `decimals.mod` must be exact at any width. The Rust client computed it as `trunc(a/b) * b`
// through a division capped at its library's default 100-digit precision, and returned a
// silently wrong residual past that - 1e101 mod 3 came back as 10. libmpdec's mpd_qrem is
// exact, so this client was never affected; pinned so it stays that way, and because the
// existing coverage stopped at 1E40 (41 digits) and would not have caught it.
//
// Measured on the JDK: 10^k mod 3 is 1 for every k, and 10^200 mod 7 is 2 (10^6 = 1 mod 7,
// 200 mod 6 = 2).
TEST(CelDecimalTimestampTest, ModIsExactPastAHundredDigits) {
    for (const char *k : {"99", "100", "101", "200", "10000"}) {
        const std::string expr = std::string("string(decimals.mod(decimal(\"1e") + k +
                                 "\"), decimal(\"3\"))) == \"1\"";
        EXPECT_TRUE(evalBool(expr)) << "1e" << k << " mod 3 must be exactly 1";
    }
    EXPECT_TRUE(evalBool("string(decimals.mod(decimal(\"1e200\"), decimal(\"7\"))) == \"2\""));
    EXPECT_TRUE(evalBool("string(decimals.mod(decimal(\"-1e101\"), decimal(\"3\"))) == \"-1\""));
    EXPECT_TRUE(evalBool("string(decimals.mod(decimal(\"12.34\"), decimal(\"1.5\"))) == \"0.34\""));
}

// Variant `==` is equality of the encoding: the metadata bytes and the standalone value bytes.
// Falling through to CelValueEqualImpl is exactly that comparison, so this client needs no arm
// of its own; these cases pin it. Sound but incomplete - equal bytes mean equal values, but one
// value has many encodings.
TEST(CelDecimalTimestampTest, VariantEqualityIsOverTheEncoding) {
    EXPECT_TRUE(evalBool("variants.parseJson(\"1\") == variants.parseJson(\"1\")"));
    EXPECT_FALSE(evalBool("variants.parseJson(\"1\") != variants.parseJson(\"1\")"));
    EXPECT_TRUE(evalBool("variants.parseJson(\"{}\") == variants.parseJson(\"{}\")"));
    EXPECT_TRUE(evalBool(
        "variants.parseJson(\"{\\\"a\\\":1}\") == variants.parseJson(\"{\\\"a\\\":1}\")"));
    EXPECT_FALSE(evalBool("variants.parseJson(\"1\") == variants.parseJson(\"2\")"));
    // Incomplete, as documented: an int and a double are two encodings, and JSON reads any
    // fractional number as a double.
    EXPECT_FALSE(evalBool("variants.parseJson(\"1\") == variants.parseJson(\"1.0\")"));
    // Containers recurse with the same equality.
    EXPECT_TRUE(evalBool("[variants.parseJson(\"1\")] == [variants.parseJson(\"1\")]"));
    EXPECT_TRUE(evalBool(
        "{'k': variants.parseJson(\"1\")} == {'k': variants.parseJson(\"1\")}"));
    EXPECT_FALSE(evalBool("[variants.parseJson(\"1\")] == [variants.parseJson(\"2\")]"));
    // A Variant is not equal to a non-Variant.
    EXPECT_FALSE(evalBool("variants.parseJson(\"1\") == 1"));
    // Navigation: a field reached the same way from the same document.
    EXPECT_TRUE(evalBool(
        "variants.field(variants.parseJson(\"{\\\"a\\\":1}\"), \"a\") == "
        "variants.field(variants.parseJson(\"{\\\"a\\\":1}\"), \"a\")"));
    // And decimals stay numeric, which is the other half of this override.
    EXPECT_TRUE(evalBool("decimal(\"2.0\") == decimal(\"2.00\")"));
    EXPECT_TRUE(evalBool("[decimal(\"2.0\")] == [decimal(\"2.00\")]"));
    // Decimal-free, Variant-free comparisons keep general semantics.
    EXPECT_TRUE(evalBool("[1, 2] == [1, 2]"));
    EXPECT_FALSE(evalBool("[1, 2] == [2, 1]"));
}

// The rounding family must not cap total precision: BigDecimal.setScale takes no MathContext,
// so the JVM's round/trunc/floor/ceil rescale and never shorten the coefficient. That became
// load-bearing once the coefficient stopped being capped at 128 bits - `decimals.add` on two
// operands at CEL's own 38-digit precision already yields 39 digits and `decimals.mul` yields
// 76, so wide values now reach these functions routinely.
//
// They are correct today for a reason that is easy to lose: mpd_qrescale "ignores precision,
// emax, emin, but uses the rounding mode" (mpdecimal's own comment on _mpd_qrescale), and
// mpd_qfloor/mpd_qceil likewise. So the 38-digit context these were handed never capped
// anything. `quantize` *does* observe prec - it is why the Python client's own rescale needs
// an unbounded context - so a switch to it, or a genuinely bounded context here, would start
// truncating silently. These cases are what would catch that.
TEST(CelDecimalTimestampTest, TheRoundingFamilyDoesNotCapPrecision) {
    const std::string n38 = "99999999999999999999999999999999999999";           // 38 digits
    const std::string sum = "199999999999999999999999999999999999998";          // 39
    const std::string product =
        "9999999999999999999999999999999999999800000000000000000000000000000000000001";  // 76
    const std::string mul = "decimals.mul(decimal(\"" + n38 + "\"), decimal(\"" + n38 + "\"))";
    const std::string add = "decimals.add(decimal(\"" + n38 + "\"), decimal(\"" + n38 + "\"))";

    // The operands that make the rest reachable at all.
    EXPECT_TRUE(evalBool("string(" + add + ") == \"" + sum + "\""));
    EXPECT_TRUE(evalBool("string(" + mul + ") == \"" + product + "\""));

    // A no-op rescale of a wide value.
    EXPECT_TRUE(evalBool("string(decimals.round(" + add + ", 0)) == \"" + sum + "\""));
    EXPECT_TRUE(evalBool("string(decimals.round(" + mul + ", 0)) == \"" + product + "\""));
    EXPECT_TRUE(evalBool("string(decimals.trunc(" + mul + ", 0)) == \"" + product + "\""));
    EXPECT_TRUE(evalBool("string(decimals.floor(" + mul + ")) == \"" + product + "\""));
    EXPECT_TRUE(evalBool("string(decimals.ceil(" + mul + ")) == \"" + product + "\""));

    // Rescaling to a *finer* scale, which grows the coefficient past 38 digits: setScale(5)
    // zero-pads on the JVM, giving 43 digits, and round(mul, 2) gives 78.
    EXPECT_TRUE(evalBool("string(decimals.round(decimal(\"" + n38 + "\"), 1)) == \"" +
                         n38 + ".0\""));
    EXPECT_TRUE(evalBool("string(decimals.round(decimal(\"" + n38 + "\"), 5)) == \"" +
                         n38 + ".00000\""));
    EXPECT_TRUE(evalBool("string(decimals.round(" + mul + ", 2)) == \"" + product + ".00\""));

    // Rounding a wide value that actually has a fraction to drop, which is the only shape
    // that makes floor/ceil rescale rather than short-circuit.
    EXPECT_TRUE(evalBool("string(decimals.round(decimal(\"" + sum + ".5\"), 0)) == "
                         "\"199999999999999999999999999999999999999\""));
    EXPECT_TRUE(evalBool("string(decimals.floor(decimal(\"" + sum + ".5\"))) == \"" +
                         sum + "\""));
    EXPECT_TRUE(evalBool("string(decimals.ceil(decimal(\"" + sum + ".5\"))) == "
                         "\"199999999999999999999999999999999999999\""));
    EXPECT_TRUE(evalBool("string(decimals.trunc(decimal(\"" + sum + ".5\"))) == \"" +
                         sum + "\""));
    EXPECT_TRUE(evalBool("string(decimals.floor(decimal(\"" + product + ".5\"))) == \"" +
                         product + "\""));

    // Flink's TRUNCATE early-return still holds at these widths: a target scale at or finer
    // than the input's is a no-op, so no zero-padding.
    EXPECT_TRUE(evalBool("string(decimals.trunc(decimal(\"" + n38 + "\"), 5)) == \"" +
                         n38 + "\""));
}

// confluent.type.Decimal.scale is a signed int32, and the scale is the negated exponent, so a
// wide exponent wrapped it: 1e-2147483648 needs scale 2147483648, which wrapped to -2147483648
// and turned a vanishingly small number into an enormous one (it compared >= 1). The bug this
// catches is the *wrapping*, and that is what the assertions are about.
//
// The band itself is this client's own, not a portable contract. The exponent range is
// delegated to each native library and documented as a per-client limitation: libmpdec here,
// decimal.js in JS, apd in Go (which caps at 100000, far narrower). What is portable is that
// a value outside the range becomes a rule error rather than a silently different number. It
// happens to coincide with the JVM's here because every C++ decimal is carried as a
// confluent.type.Decimal message, whose int32 scale is exactly BigDecimal's - measured,
// BigDecimal refuses a literal at +/-2147483648 and takes +/-2147483647.
TEST(CelDecimalTimestampTest, WideExponentIsRefusedNotWrapped) {
    EXPECT_TRUE(evalBool("decimals.lt(decimal(\"1e-2147483647\"), decimal(\"1\"))"));
    EXPECT_TRUE(evalBool("decimals.gt(decimal(\"1e2147483647\"), decimal(\"1\"))"));
    EXPECT_TRUE(errContains("decimals.lt(decimal(\"1e-2147483648\"), decimal(\"1\"))",
                            "does not fit the confluent.type.Decimal int32 scale"));
    EXPECT_TRUE(errContains("decimals.lt(decimal(\"1e2147483648\"), decimal(\"1\"))",
                            "does not fit the confluent.type.Decimal int32 scale"));
}

// The width ceiling. libmpdec has no useful bound of its own - MAX_PREC is around 1e18 digits,
// so it tries - and width failure is resource exhaustion, which cannot be turned into a rule
// error after the fact. Java is the only client in the family that fails cleanly on width
// (ArithmeticException at 646456993 digits); DecimalUtil::kSaneWidth stands in for that as a
// bound rather than as a model of BigDecimal's domain, so the accepted/rejected split here is
// this client's and is deliberately tighter than the JVM's.
//
// The dividing line is not arithmetic vs. rescale - it is whether the operation has to build a
// positional form. Measured in the sibling Python client on the shared libmpdec, peak RSS,
// operands 1e2147483647 and 3: mul, div, <, ==, min, neg, abs all 13 MB; add 1738 MB, sub
// 1738 MB, remainder 1733 MB; add(1e2147483647, 1e-2147483647) 3125 MB. So three of six
// arithmetic operations reach a multi-GB allocation from one expression over two operands each
// cheap to construct, and the other three cost nothing at any width.
TEST(CelDecimalTimestampTest, AlignmentWidthIsRefused) {
    const std::string wide = "decimal(\"1e2147483647\")";
    const std::string tiny = "decimal(\"1e-2147483647\")";
    // add/sub expand the narrower operand into the wider one's frame.
    EXPECT_TRUE(errContains("decimals.add(" + wide + ", decimal(\"1\"))", "aligning the operands"));
    EXPECT_TRUE(errContains("decimals.sub(" + wide + ", decimal(\"1\"))", "aligning the operands"));
    EXPECT_TRUE(errContains("decimals.add(" + tiny + ", decimal(\"1\"))", "aligning the operands"));
    EXPECT_TRUE(errContains("decimals.add(" + wide + ", " + tiny + ")", "aligning the operands"));
    EXPECT_TRUE(errContains("decimals.sub(" + wide + ", " + tiny + ")", "aligning the operands"));
    // remainder, via the integral quotient it has to produce on the way.
    EXPECT_TRUE(errContains("decimals.mod(" + wide + ", decimal(\"3\"))",
                            "the integral quotient"));
    EXPECT_TRUE(errContains("decimals.mod(" + wide + ", " + tiny + ")",
                            "the integral quotient"));
    EXPECT_TRUE(errContains("decimals.mod(decimal(\"1.5\"), " + tiny + ")",
                            "the integral quotient"));
}

// Expanding a *zero* is free, so the aligned frame is set by the operands that actually have
// digits - several of these turn on which operand expands rather than on how far apart the
// scales are. A zero also keeps whatever scale it was built with, so its adjusted exponent says
// nothing about the cost, which is what an earlier estimate got wrong. Every row measured on
// libmpdec and on the JDK, which agree throughout:
//
//   0E+2e9 + 0E-2e9      free, 1 digit           precision 1
//   0E+2e9 + 1           free, 1 digit           precision 1, scale 0 (the zero expands)
//   0E+2e9 mod 1E-2e9    free, 1 digit           precision 1
//   1 + 0E-2e9           1601 MB, 2e9+1 digits   ArithmeticException  (the *one* expands)
TEST(CelDecimalTimestampTest, ExpandingAZeroOperandIsFree) {
    const std::string zeroCoarse = "decimal(\"0E+2000000000\")";
    const std::string zeroFine = "decimal(\"0E-2000000000\")";
    EXPECT_TRUE(evalBool(
        "decimals.eq(decimals.add(" + zeroCoarse + ", " + zeroFine + "), decimal(\"0\"))"));
    EXPECT_TRUE(evalBool(
        "decimals.eq(decimals.sub(" + zeroCoarse + ", " + zeroFine + "), decimal(\"0\"))"));
    EXPECT_TRUE(evalBool(
        "decimals.eq(decimals.add(" + zeroCoarse + ", decimal(\"1\")), decimal(\"1\"))"));
    EXPECT_TRUE(evalBool("decimals.eq(decimals.mod(" + zeroCoarse +
                         ", decimal(\"1E-2000000000\")), decimal(\"0\"))"));
    EXPECT_TRUE(evalBool("decimals.eq(decimals.mod(" + zeroFine +
                         ", decimal(\"1E+2000000000\")), decimal(\"0\"))"));
    EXPECT_TRUE(evalBool("decimals.eq(decimals.mod(decimal(\"0\"), decimal(\"3\")), decimal(\"0\"))"));
    // The row that must still be refused: here the *one* expands into the zero's scale. A
    // blanket zero exemption would have let this through.
    EXPECT_TRUE(errContains("decimals.add(decimal(\"1\"), " + zeroFine + ") != decimal(\"0\")",
                            "aligning the operands"));
}

// The must-fail twin: everything that does not align stays unguarded at any width, and
// alignment that stays narrow is accepted however extreme both operands are. A guard on the
// operands' own magnitudes rather than on their difference would refuse all of these.
TEST(CelDecimalTimestampTest, TheCheapOperationsStayUnguarded) {
    const std::string wide = "decimal(\"1e2147483647\")";
    const std::string tiny = "decimal(\"1e-2147483647\")";
    // Comparison short-circuits on the adjusted exponent.
    EXPECT_TRUE(evalBool("decimals.lt(" + tiny + ", " + wide + ")"));
    EXPECT_TRUE(evalBool("decimals.gt(" + wide + ", " + tiny + ")"));
    EXPECT_FALSE(evalBool("decimals.eq(" + wide + ", " + tiny + ")"));
    // div holds the coefficient to the context precision and lets the exponent absorb the
    // difference, so it costs nothing however far apart the operands are.
    // div holds the coefficient to the context precision and lets the exponent absorb the
    // difference, so it costs nothing however far apart the operands are. It is refused here
    // only because the *result's* exponent, -4294967294, is outside the int32 scale that
    // carries every decimal in this client - not by any width bound. Java agrees: the
    // equivalent divide gives a scale no `int` can hold.
    EXPECT_TRUE(errContains("decimals.eq(decimals.div(" + tiny + ", " + wide + "), decimal(\"0\"))",
                            "int32 scale"));
    // A legitimate division at a wide exponent, which is what makes the widened Emax/Emin on
    // context() load-bearing. Default-constructed, its Emax is 999999 - narrower than the
    // exponent this client's own constructor accepts - so this was `[Overflow]` where Java
    // and Python both return a result.
    EXPECT_TRUE(evalBool("decimals.gt(decimals.div(decimal(\"1e1000000\"), decimal(\"1\")), "
                         "decimal(\"1\"))"));
    EXPECT_TRUE(evalBool("decimals.gt(decimals.sqrt(decimal(\"1e1000000\")), decimal(\"1\"))"));
    // mul is exact and cheap at any width. It is refused here only where its *result* needs a
    // scale no int32 can carry, and that comes from wrapping the value back into a
    // confluent.type.Decimal message, not from a width bound.
    EXPECT_TRUE(errContains("decimals.mul(" + wide + ", " + wide + ") == " + wide,
                            "int32 scale"));
    EXPECT_TRUE(evalBool("decimals.eq(decimals.mul(" + wide + ", " + tiny + "), decimal(\"1\"))"));
    // Alignment that stays narrow because the exponents are close.
    EXPECT_TRUE(evalBool("decimals.gt(decimals.add(" + wide + ", " + wide + "), decimal(\"1\"))"));
    EXPECT_TRUE(evalBool(
        "decimals.gt(decimals.sub(" + wide + ", decimal(\"1e2147483646\")), decimal(\"0\"))"));
    // remainder whose integral quotient is small, however far apart the operands are.
    EXPECT_TRUE(evalBool("decimals.eq(decimals.mod(" + tiny + ", " + wide + "), " + tiny + ")"));
    EXPECT_TRUE(evalBool(
        "decimals.eq(decimals.mod(" + wide + ", decimal(\"1e2147483000\")), decimal(\"0\"))"));
    // And ordinary arithmetic, unchanged.
    EXPECT_TRUE(evalBool("string(decimals.add(decimal(\"12.34\"), decimal(\"1.5\"))) == \"13.84\""));
    EXPECT_TRUE(evalBool("string(decimals.mod(decimal(\"1E40\"), decimal(\"3\"))) == \"1\""));
}

// Rescaling is the second width site, and the one-argument forms are rescales too: `round(x)`,
// `floor(x)` and `ceil(x)` target scale 0, so a value with a large negative exponent is a
// multi-GB allocation from a rule that names no scale at all. floor/ceil do not go through
// roundTo, so they carry the bound separately.
TEST(CelDecimalTimestampTest, RescaleWidthIsRefused) {
    // Only *expanding* a scale costs anything - the coefficient grows by the difference - so
    // the cases are values whose integer part has to be built. Scale 0 against a large
    // positive exponent is the one-argument forms' version of that.
    const std::string tall = "decimal(\"1e20000000\")";
    EXPECT_TRUE(errContains("decimals.round(" + tall + ") != decimal(\"0\")", "a scale of 0"));
    EXPECT_TRUE(errContains("decimals.floor(" + tall + ") != decimal(\"0\")",
                            "rounding to an integer"));
    EXPECT_TRUE(errContains("decimals.ceil(" + tall + ") != decimal(\"0\")",
                            "rounding to an integer"));
    EXPECT_TRUE(errContains("decimals.round(decimal(\"1.23\"), 100000000) != decimal(\"0\")",
                            "a scale of 100000000"));
    // trunc is absent on purpose: its `scale >= current scale` early return - Java's
    // `intScale >= v.scale()` - means it only ever coarsens, so it cannot reach an expanding
    // rescale by any argument.
}

// Coarsening a scale is free at any distance: the coefficient shrinks to a single digit
// rather than growing. Measured on the shared libmpdec, all instant and all one digit wide:
// 1.23 at scale -1000000 / -100000000 / -2000000000, and 1e-1000000 and 1e-100000000 at
// scale 0. Java agrees - BigDecimal("1.23").setScale(-100000000) is precision 1 - so an
// `abs(shift) + digits` estimate, which is what this guard first carried, refused every one
// of them wrongly.
TEST(CelDecimalTimestampTest, CoarseningAScaleIsNeverRefused) {
    const std::string deep = "decimal(\"1e-20000000\")";
    EXPECT_TRUE(evalBool("decimals.eq(decimals.round(" + deep + "), decimal(\"0\"))"));
    EXPECT_TRUE(evalBool("decimals.eq(decimals.trunc(" + deep + "), decimal(\"0\"))"));
    EXPECT_TRUE(evalBool("decimals.eq(decimals.floor(" + deep + "), decimal(\"0\"))"));
    EXPECT_TRUE(evalBool("decimals.eq(decimals.ceil(" + deep + "), decimal(\"1\"))"));
    EXPECT_TRUE(evalBool(
        "decimals.eq(decimals.round(decimal(\"1e-100000000\")), decimal(\"0\"))"));
    EXPECT_TRUE(evalBool(
        "decimals.eq(decimals.round(decimal(\"1.23\"), -100000000), decimal(\"0\"))"));
    EXPECT_TRUE(evalBool(
        "decimals.eq(decimals.round(decimal(\"1.23\"), -1000000000), decimal(\"0\"))"));
    // Far enough to leave the int32 scale domain, which is refused when the result is wrapped
    // back into a confluent.type.Decimal - by the scale field, not by any width bound.
    EXPECT_TRUE(errContains("decimals.round(decimal(\"1.23\"), -2147483648) != decimal(\"0\")",
                            "int32 scale"));
}

// The coefficient arriving through the (bytes, scale) constructor is the fourth width site,
// and the only one not reachable from a CEL literal: it takes a 4 MB `bytes` value to cross
// the ceiling, which a rule cannot spell but a `fixed` decimal field could carry. Checked
// from the byte count before the digits are built - one byte is about 2.41 decimal digits -
// so this asserts the arithmetic the constructor uses rather than routing 4 MB through CEL.
TEST(CelDecimalTimestampTest, AWideCoefficientIsRefused) {
    // 4300 digits is about 1785 bytes, so a literal is enough to cross it - no need to route
    // megabytes through CEL.
    EXPECT_TRUE(errContains("decimal(b\"" + std::string(2000, 'A') + "\", 0) != decimal(\"0\")",
                            "the coefficient"));
    // And the same ceiling on the way out, which is the one that matters: a rescale can
    // produce a coefficient far wider than anything a literal can express.
    // `decimals.round(x, 1000000)` used to grind here for minutes rather than fail.
    EXPECT_TRUE(errContains("decimals.round(decimal(\"1.23\"), 1000000) != decimal(\"0\")",
                            "the coefficient"));
    EXPECT_TRUE(errContains("decimals.round(decimal(\"1.23\"), 5000) != decimal(\"0\")",
                            "the coefficient"));
    // The scale on the bytes path costs nothing: it only sets the exponent, so an extreme
    // scale on a small coefficient is fine and the width guard fires later, where the digits
    // are needed.
    EXPECT_TRUE(evalBool("decimals.eq(decimal(b\"\\x04\\xd2\", 2), decimal(\"12.34\"))"));
    EXPECT_TRUE(evalBool("decimals.lt(decimal(b\"\\x01\", 2147483647), decimal(\"1\"))"));
    // Just inside the ceiling, both directions, and it round-trips.
    EXPECT_TRUE(evalBool("decimals.round(decimal(\"1.23\"), 4000) != decimal(\"0\")"));
    EXPECT_TRUE(evalBool("decimals.eq(decimal(b\"" + std::string(1700, 'A') +
                         "\", 0), decimal(b\"" + std::string(1700, 'A') + "\", 0))"));
}

// An order of magnitude under the ceiling, the same expressions answer - including the zero
// exemption, which the width formula needs because rescaling a zero never expands anything
// (its result stays compact) and BigDecimal agrees: new BigDecimal(BigInteger.ZERO,
// 2147483647) is precision 1. Without it these are false rejections.
TEST(CelDecimalTimestampTest, RescaleWithinTheCeilingStillAnswers) {
    EXPECT_TRUE(evalBool("string(decimals.round(decimal(\"2.5\"))) == \"3\""));
    EXPECT_TRUE(evalBool("string(decimals.floor(decimal(\"-1.5\"))) == \"-2\""));
    EXPECT_TRUE(evalBool("string(decimals.ceil(decimal(\"1.5\"))) == \"2\""));
    EXPECT_TRUE(evalBool("string(decimals.trunc(decimal(\"-1.9\"))) == \"-1\""));
    // Zero at an extreme scale: exempt from the rescale bound in both directions.
    EXPECT_TRUE(evalBool("decimals.eq(decimals.round(decimal(b\"\", 2147483647), 0), decimal(\"0\"))"));
    EXPECT_TRUE(evalBool("decimals.eq(decimals.round(decimal(\"0\"), 2147483647), decimal(\"0\"))"));
    EXPECT_TRUE(evalBool("decimals.eq(decimals.floor(decimal(b\"\", 2147483647)), decimal(\"0\"))"));
    EXPECT_TRUE(evalBool("decimals.eq(decimals.ceil(decimal(b\"\", 2147483647)), decimal(\"0\"))"));
    // An ordinary coefficient through the bytes constructor, and an extreme scale on a small
    // one - which only sets the exponent.
    EXPECT_TRUE(evalBool("decimals.eq(decimal(b\"\\x04\\xd2\", 2), decimal(\"12.34\"))"));
    EXPECT_TRUE(evalBool("decimals.lt(decimal(b\"\\x01\", 2147483647), decimal(\"1\"))"));
}

// Rendering is the third width site, and it does not come from a rescale: div holds its
// coefficient to 38 digits while its exponent runs free. `double(Decimal)` parses the same
// plain form, so it carries the same bound. No zero shortcut here, unlike the rescale guard -
// a zero at an extreme scale renders as that many zeros.
TEST(CelDecimalTimestampTest, RenderingAWidePlainFormIsRefused) {
    EXPECT_TRUE(errContains("string(decimal(\"1e2147483647\")) == \"\"", "the plain form"));
    EXPECT_TRUE(errContains("string(decimal(\"1e-2147483647\")) == \"\"", "the plain form"));
    EXPECT_TRUE(errContains("string(decimal(b\"\", 2147483647)) == \"\"", "the plain form"));
    EXPECT_TRUE(errContains("double(decimal(\"1e2147483647\")) == 0.0", "the plain form"));
    // Still renders below the ceiling, coefficient and exponent independently.
    EXPECT_TRUE(evalBool("string(decimal(\"12.34\")) == \"12.34\""));
    EXPECT_TRUE(evalBool("string(decimal(\"1e1000000\")) != \"\""));
    EXPECT_TRUE(evalBool("string(decimal(\"1e-1000000\")) != \"\""));
}

// double(Decimal) parses the plain-notation rendering, which always writes '.'. strtod reads
// its radix character from LC_NUMERIC, so a comma-radix locale in the host process stopped the
// parse at the dot and double(decimal("100.50")) returned 100. std::from_chars ignores the
// locale by definition. The over/underflow behaviour must survive the change: Java's
// BigDecimal.doubleValue() gives 1e-400 -> 0.0, 1e-320 -> a subnormal, +/-1e400 -> +/-Infinity.
TEST(CelDecimalTimestampTest, DoubleOfDecimalIsLocaleIndependent) {
    EXPECT_TRUE(evalBool("double(decimal(\"100.50\")) == 100.5"));
    EXPECT_TRUE(evalBool("double(decimal(\"-100.50\")) == -100.5"));
    EXPECT_TRUE(evalBool("double(decimal(\"1e-400\")) == 0.0"));
    EXPECT_TRUE(evalBool("double(decimal(\"1e400\")) > 1.0e308"));
    EXPECT_TRUE(evalBool("double(decimal(\"-1e400\")) < -1.0e308"));
    // A subnormal must stay a subnormal rather than collapsing to zero or blowing up.
    EXPECT_TRUE(evalBool("double(decimal(\"1e-320\")) > 0.0"));
    EXPECT_TRUE(evalBool("double(decimal(\"1e-320\")) < 1.0e-300"));
}

// A non-finite bareword is rewritten before nlohmann sees it, and the scanner retries at every
// byte. With only a trailing-boundary check, `1NaN` matched `NaN` at offset 1 and was rewritten
// to the *valid* number `10.0` - malformed input parsed as a wrong value rather than being
// rejected. Jackson refuses these, so a bareword may only begin where a JSON value may begin.
TEST(CelDecimalTimestampTest, NonFiniteBarewordChecksBothBoundaries) {
    // Rejected: tryParseJson yields CEL null, so toJson does not return a string.
    for (const char *bad : {"1NaN", "1Infinity", "NaN1", "NaNny"}) {
        const std::string expr =
            std::string("variants.toJson(variants.tryParseJson(\"") + bad + "\")) == \"\"";
        EXPECT_FALSE(evalBool(expr)) << bad << " must not parse";
    }
    // Still accepted, at each position a value may start.
    EXPECT_TRUE(evalBool("variants.toJson(variants.tryParseJson(\"NaN\")) == \"NaN\""));
    EXPECT_TRUE(
        evalBool("variants.toJson(variants.tryParseJson(\"-Infinity\")) == \"-Infinity\""));
    EXPECT_TRUE(evalBool("variants.toJson(variants.tryParseJson(\"[NaN]\")) == \"[NaN]\""));
    EXPECT_TRUE(
        evalBool("variants.toJson(variants.tryParseJson(\"[1,NaN]\")) == \"[1,NaN]\""));
}

// The two-argument timestamp(value, precision) never checked the CEL/protobuf timestamp range,
// so timestamp(INT64_MAX, 0) produced an out-of-range absl::Time and handed it back as a
// timestamp. Java's instantOfEpoch enforces 0001-01-01T00:00:00Z..9999-12-31T23:59:59.999999999Z
// for every precision ("Timestamp out of range: ..."), which is what the one-argument
// constructor already did here.
TEST(CelDecimalTimestampTest, TwoArgTimestampChecksTheCelRange) {
    // In range at each supported precision.
    EXPECT_TRUE(evalBool("timestamp(1700000000, 0) == timestamp(\"2023-11-14T22:13:20Z\")"));
    EXPECT_TRUE(evalBool("timestamp(1700000000123, 3) == timestamp(\"2023-11-14T22:13:20.123Z\")"));
    // The exact bounds: 0001-01-01T00:00:00Z and 9999-12-31T23:59:59Z.
    EXPECT_TRUE(evalBool("timestamp(-62135596800, 0) == timestamp(\"0001-01-01T00:00:00Z\")"));
    EXPECT_TRUE(evalBool("timestamp(253402300799, 0) == timestamp(\"9999-12-31T23:59:59Z\")"));

    // One second past each bound, and the extremes at every precision.
    EXPECT_TRUE(errContains("timestamp(-62135596801, 0) == timestamp(0)", "out of range"));
    EXPECT_TRUE(errContains("timestamp(253402300800, 0) == timestamp(0)", "out of range"));
    for (const char *expr : {"timestamp(9223372036854775807, 0) == timestamp(0)",
                             "timestamp(-9223372036854775808, 0) == timestamp(0)",
                             "timestamp(9223372036854775807, 3) == timestamp(0)"}) {
        EXPECT_TRUE(errContains(expr, "out of range")) << expr;
    }
}
