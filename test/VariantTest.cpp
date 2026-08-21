/**
 * VariantTest
 *
 * Tests the self-contained Spark/Parquet Variant binary codec: parseJson /
 * toJson round-trips, getVariantType per type, scalar getters, object/array
 * navigation (including misses), element counts, decimal string formatting, the
 * standalone re-encoding of navigated sub-variants, and malformed-JSON errors.
 */

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "schemaregistry/serdes/Variant.h"

using schemaregistry::serdes::Variant;
using schemaregistry::serdes::VariantException;
using schemaregistry::serdes::VariantType;

namespace {

Variant parse(const std::string &json) { return Variant::parseJson(json); }

}  // namespace

// ---- round-trip: toJson(parseJson(x)) == x ----

TEST(VariantTest, RoundTripScalars) {
    EXPECT_EQ(parse("null").toJson(), "null");
    EXPECT_EQ(parse("true").toJson(), "true");
    EXPECT_EQ(parse("false").toJson(), "false");
    EXPECT_EQ(parse("42").toJson(), "42");
    EXPECT_EQ(parse("-1").toJson(), "-1");
    EXPECT_EQ(parse("1000000").toJson(), "1000000");
    EXPECT_EQ(parse("\"hello\"").toJson(), "\"hello\"");
    EXPECT_EQ(parse("1.5").toJson(), "1.5");
    EXPECT_EQ(parse("2.0").toJson(), "2.0");
}

TEST(VariantTest, RoundTripObjectAndArray) {
    EXPECT_EQ(parse("{\"x\":1}").toJson(), "{\"x\":1}");
    // Object fields are key-sorted on write.
    EXPECT_EQ(parse("{\"b\":2,\"a\":1}").toJson(), "{\"a\":1,\"b\":2}");
    EXPECT_EQ(parse("[1,2,3]").toJson(), "[1,2,3]");
    EXPECT_EQ(parse("[]").toJson(), "[]");
    EXPECT_EQ(parse("{}").toJson(), "{}");
    EXPECT_EQ(parse("{\"a\":[1,{\"b\":\"c\"}],\"z\":null}").toJson(),
              "{\"a\":[1,{\"b\":\"c\"}],\"z\":null}");
}

// ---- getVariantType ----

TEST(VariantTest, GetVariantType) {
    EXPECT_EQ(parse("null").getVariantType(), VariantType::Null);
    EXPECT_EQ(parse("true").getVariantType(), VariantType::Boolean);
    EXPECT_EQ(parse("\"s\"").getVariantType(), VariantType::String);
    EXPECT_EQ(parse("{\"x\":1}").getVariantType(), VariantType::Object);
    EXPECT_EQ(parse("[1]").getVariantType(), VariantType::Array);
    EXPECT_EQ(parse("1.5").getVariantType(), VariantType::Double);
    // Integer width selection.
    EXPECT_EQ(parse("1").getVariantType(), VariantType::Byte);
    EXPECT_EQ(parse("1000").getVariantType(), VariantType::Short);
    EXPECT_EQ(parse("100000").getVariantType(), VariantType::Int);
    EXPECT_EQ(parse("10000000000").getVariantType(), VariantType::Long);
}

// ---- scalar getters ----

TEST(VariantTest, ScalarGetters) {
    EXPECT_TRUE(parse("true").getBoolean());
    EXPECT_FALSE(parse("false").getBoolean());
    EXPECT_EQ(parse("127").getLong(), 127);
    EXPECT_EQ(parse("-128").getLong(), -128);
    EXPECT_EQ(parse("32000").getLong(), 32000);
    EXPECT_EQ(parse("10000000000").getLong(), 10000000000LL);
    EXPECT_DOUBLE_EQ(parse("3.25").getDouble(), 3.25);
    EXPECT_EQ(parse("\"abc\"").getString(), "abc");
}

TEST(VariantTest, WrongTypeGetterThrows) {
    EXPECT_THROW(parse("true").getLong(), VariantException);
    EXPECT_THROW(parse("1").getBoolean(), VariantException);
    EXPECT_THROW(parse("1").getString(), VariantException);
}

// ---- object / array navigation ----

TEST(VariantTest, ObjectNavigation) {
    Variant v = parse("{\"a\":1,\"b\":\"two\",\"c\":true}");
    EXPECT_EQ(v.numObjectElements(), 3);

    auto a = v.getFieldByKey("a");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->getLong(), 1);

    auto b = v.getFieldByKey("b");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->getString(), "two");

    // Miss -> nullopt.
    EXPECT_FALSE(v.getFieldByKey("missing").has_value());

    // getFieldAtIndex returns fields key-sorted.
    auto f0 = v.getFieldAtIndex(0);
    EXPECT_EQ(f0.first, "a");
    EXPECT_EQ(f0.second.getLong(), 1);
    auto f2 = v.getFieldAtIndex(2);
    EXPECT_EQ(f2.first, "c");
    EXPECT_TRUE(f2.second.getBoolean());
}

TEST(VariantTest, ArrayNavigation) {
    Variant v = parse("[10,20,30]");
    EXPECT_EQ(v.numArrayElements(), 3);
    ASSERT_TRUE(v.getElementAtIndex(0).has_value());
    EXPECT_EQ(v.getElementAtIndex(0)->getLong(), 10);
    EXPECT_EQ(v.getElementAtIndex(2)->getLong(), 30);
    // Out of bounds -> nullopt.
    EXPECT_FALSE(v.getElementAtIndex(3).has_value());
    EXPECT_FALSE(v.getElementAtIndex(-1).has_value());
}

// ---- decimal ----

TEST(VariantTest, DecimalString) {
    // A big integer literal wider than 64 bits becomes a scale-0 decimal.
    Variant big = parse("123456789012345678901234567890");
    EXPECT_EQ(big.getVariantType(), VariantType::Decimal16);
    EXPECT_EQ(big.getDecimalString(), "123456789012345678901234567890");
    EXPECT_EQ(big.toJson(), "123456789012345678901234567890");

    Variant negBig = parse("-123456789012345678901234567890");
    EXPECT_EQ(negBig.getDecimalString(), "-123456789012345678901234567890");

    // Decimal parts: unscaled big-endian bytes + scale (scale 0 here).
    std::vector<uint8_t> unscaled;
    int scale = -1;
    big.getDecimalParts(unscaled, scale);
    EXPECT_EQ(scale, 0);
    EXPECT_EQ(unscaled.size(), 16u);
}

// ---- standalone re-encoding of a navigated sub-variant ----

TEST(VariantTest, StandaloneReencode) {
    Variant v = parse("{\"a\":[1,2,3],\"b\":{\"c\":\"deep\"}}");

    // For a root variant, standaloneValueBytes() == valueBytes().
    EXPECT_EQ(v.standaloneValueBytes(), v.valueBytes());

    auto field = v.getFieldByKey("b");
    ASSERT_TRUE(field.has_value());
    // A sub-variant re-encodes as (metadataBytes(), standaloneValueBytes()).
    Variant reencoded(field->standaloneValueBytes(), field->metadataBytes());
    EXPECT_EQ(reencoded.toJson(), field->toJson());
    EXPECT_EQ(reencoded.toJson(), "{\"c\":\"deep\"}");

    auto arr = v.getFieldByKey("a");
    ASSERT_TRUE(arr.has_value());
    Variant arrReencoded(arr->standaloneValueBytes(), arr->metadataBytes());
    EXPECT_EQ(arrReencoded.toJson(), "[1,2,3]");
}

// ---- malformed JSON ----

TEST(VariantTest, MalformedJsonThrows) {
    EXPECT_THROW(parse("{not valid"), VariantException);
    EXPECT_THROW(parse("[1,2,"), VariantException);
    EXPECT_THROW(parse(""), VariantException);
}
