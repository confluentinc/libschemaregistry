/**
 * VariantTest
 *
 * Tests the self-contained Spark/Parquet Variant binary codec: parseJson /
 * toJson round-trips, getType per type, scalar getters, object/array
 * navigation (including misses), element counts, decimal string formatting, the
 * standalone re-encoding of navigated sub-variants, and malformed-JSON errors.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "schemaregistry/serdes/Variant.h"

using schemaregistry::serdes::Variant;
using schemaregistry::serdes::VariantBuilder;
using schemaregistry::serdes::VariantException;
using schemaregistry::serdes::VariantType;

namespace {

Variant parse(const std::string &json) { return Variant::parseJson(json); }

// Primitive type codes (see Variant.cpp).
constexpr int kTInt1 = 3, kTInt2 = 4, kTInt4 = 5, kTInt8 = 6, kTDouble = 7,
              kTFloat = 14;

// Minimal empty metadata: version 1, offset_size 1, dictionary_size 0.
const std::vector<uint8_t> kEmptyMeta = {1, 0, 0};

// Build a raw primitive Variant with the given type code and payload bytes.
Variant prim(int code, const std::vector<uint8_t> &payload) {
    std::vector<uint8_t> value;
    value.push_back(static_cast<uint8_t>(code << 2));
    value.insert(value.end(), payload.begin(), payload.end());
    return Variant(std::move(value), kEmptyMeta);
}

// Little-endian bytes for a signed integer of the given width.
std::vector<uint8_t> le(int64_t v, int width) {
    std::vector<uint8_t> b(width);
    for (int i = 0; i < width; i++) {
        b[i] = static_cast<uint8_t>(v & 0xFF);
        v >>= 8;
    }
    return b;
}

std::vector<uint8_t> f32(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return le(static_cast<int64_t>(bits), 4);
}

std::vector<uint8_t> f64(double d) {
    uint64_t bits;
    std::memcpy(&bits, &d, sizeof(bits));
    std::vector<uint8_t> b(8);
    for (int i = 0; i < 8; i++) {
        b[i] = static_cast<uint8_t>(bits & 0xFF);
        bits >>= 8;
    }
    return b;
}

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

// ---- getType ----

TEST(VariantTest, GetVariantType) {
    EXPECT_EQ(parse("null").getType(), VariantType::Null);
    EXPECT_EQ(parse("true").getType(), VariantType::Boolean);
    EXPECT_EQ(parse("\"s\"").getType(), VariantType::String);
    EXPECT_EQ(parse("{\"x\":1}").getType(), VariantType::Object);
    EXPECT_EQ(parse("[1]").getType(), VariantType::Array);
    EXPECT_EQ(parse("1.5").getType(), VariantType::Double);
    // Integer width selection.
    EXPECT_EQ(parse("1").getType(), VariantType::Byte);
    EXPECT_EQ(parse("1000").getType(), VariantType::Short);
    EXPECT_EQ(parse("100000").getType(), VariantType::Int);
    EXPECT_EQ(parse("10000000000").getType(), VariantType::Long);
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

TEST(VariantTest, NarrowedIntGetters) {
    // getByte reads INT8 only.
    EXPECT_EQ(prim(kTInt1, le(-5, 1)).getByte(), -5);
    EXPECT_THROW(prim(kTInt2, le(-300, 2)).getByte(), VariantException);
    // getShort widens INT8 -> INT16.
    EXPECT_EQ(prim(kTInt1, le(-5, 1)).getShort(), -5);
    EXPECT_EQ(prim(kTInt2, le(-300, 2)).getShort(), -300);
    EXPECT_THROW(prim(kTInt4, le(100000, 4)).getShort(), VariantException);
    // getInt widens INT8/INT16 -> INT32.
    EXPECT_EQ(prim(kTInt1, le(-5, 1)).getInt(), -5);
    EXPECT_EQ(prim(kTInt2, le(-300, 2)).getInt(), -300);
    EXPECT_EQ(prim(kTInt4, le(100000, 4)).getInt(), 100000);
    EXPECT_THROW(prim(kTInt8, le(10000000000LL, 8)).getInt(), VariantException);
}

TEST(VariantTest, FloatAndDoubleAreExact) {
    // getFloat reads FLOAT only; getDouble reads DOUBLE only (no widening).
    EXPECT_FLOAT_EQ(prim(kTFloat, f32(2.5f)).getFloat(), 2.5f);
    EXPECT_DOUBLE_EQ(prim(kTDouble, f64(2.5)).getDouble(), 2.5);
    EXPECT_THROW(prim(kTFloat, f32(2.5f)).getDouble(), VariantException);
    EXPECT_THROW(prim(kTDouble, f64(2.5)).getFloat(), VariantException);
}

// ---- object / array navigation ----

TEST(VariantTest, ObjectNavigation) {
    Variant v = parse("{\"a\":1,\"b\":\"two\",\"c\":true}");
    EXPECT_EQ(v.numObjectFields(), 3);

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

TEST(VariantTest, DuplicateKeysLastWins) {
    // The SAX JSON parser passes duplicate object keys through; the codec must
    // dedup them last-wins. The differently-sized duplicate value for "a"
    // exercises the leftward repacking of the value buffer.
    Variant v = parse("{\"b\":1,\"a\":\"x\",\"a\":\"second-longer-value\",\"c\":3}");
    EXPECT_EQ(v.numObjectFields(), 3);

    auto a = v.getFieldByKey("a");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->getString(), "second-longer-value");

    auto b = v.getFieldByKey("b");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->getLong(), 1);

    auto c = v.getFieldByKey("c");
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->getLong(), 3);
}

TEST(VariantTest, DuplicateKeysSimple) {
    Variant v = parse("{\"a\":1,\"a\":2}");
    EXPECT_EQ(v.numObjectFields(), 1);
    auto a = v.getFieldByKey("a");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->getLong(), 2);
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
    EXPECT_EQ(big.getType(), VariantType::Decimal16);
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

// ---- VariantBuilder (flat streaming writer) ----

TEST(VariantTest, BuilderNestedDocMatchesParseJson) {
    // A document using only JSON-representable types, so parseJson produces
    // identical bytes. amount=150 does not fit INT8, so the JSON path selects
    // INT16 - appendShort matches it.
    const std::string json =
        "{\"id\":42,\"amount\":150,"
        "\"big\":123456789012345678901234567890,\"tags\":[\"x\",\"y\"],"
        "\"nested\":{\"flag\":true,\"pi\":3.5},\"note\":null}";

    // 123456789012345678901234567890 as two's-complement big-endian bytes.
    std::vector<uint8_t> bigBe = {0x01, 0x8E, 0xE9, 0x0F, 0xF6, 0xC3,
                                  0x73, 0xE0, 0xEE, 0x4E, 0x3F, 0x0A,
                                  0xD2};

    VariantBuilder b;
    b.startObject();
    b.appendKey("id");
    b.appendByte(42);
    b.appendKey("amount");
    b.appendShort(150);
    b.appendKey("big");
    b.appendDecimal(bigBe, 0);
    b.appendKey("tags");
    b.startArray();
    b.appendString("x");
    b.appendString("y");
    b.endArray();
    b.appendKey("nested");
    b.startObject();
    b.appendKey("flag");
    b.appendBoolean(true);
    b.appendKey("pi");
    b.appendDouble(3.5);
    b.endObject();
    b.appendKey("note");
    b.appendNull();
    b.endObject();
    Variant built = b.build();

    Variant parsed = Variant::parseJson(json);
    EXPECT_EQ(built.toJson(), parsed.toJson());
    EXPECT_EQ(built.valueBytes(), parsed.valueBytes());        // byte-identical
    EXPECT_EQ(built.metadataBytes(), parsed.metadataBytes());  // byte-identical
}

TEST(VariantTest, BuilderRootScalarMatchesParseJson) {
    VariantBuilder b;
    b.appendString("hello");
    Variant built = b.build();

    Variant parsed = Variant::parseJson("\"hello\"");
    EXPECT_EQ(built.toJson(), "\"hello\"");
    EXPECT_EQ(built.getType(), VariantType::String);
    EXPECT_EQ(built.valueBytes(), parsed.valueBytes());
    EXPECT_EQ(built.metadataBytes(), parsed.metadataBytes());
}

TEST(VariantTest, BuilderTypedScalarsRoundTrip) {
    VariantBuilder b;
    b.startArray();
    b.appendLong(9876543210LL);
    b.appendFloat(2.5f);
    b.appendUuid({0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
                  0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff});
    b.appendDate(18262);
    b.endArray();
    Variant v = b.build();

    EXPECT_EQ(v.getType(), VariantType::Array);
    EXPECT_EQ(v.getElementAtIndex(0)->getLong(), 9876543210LL);
    EXPECT_FLOAT_EQ(v.getElementAtIndex(1)->getFloat(), 2.5f);
    EXPECT_EQ(v.getElementAtIndex(2)->getUuid(),
              "00112233-4455-6677-8899-aabbccddeeff");
    EXPECT_EQ(v.getElementAtIndex(3)->getType(), VariantType::Date);
}

TEST(VariantTest, FloatToJsonUsesShortestFloat32) {
    // Regression: FLOAT must render the shortest decimal that round-trips to the
    // same float32 (matching Java Float.toString / Apache Arrow), not the
    // double-widened shortest string (e.g. "0.10000000149011612").
    VariantBuilder b1;
    b1.appendFloat(0.1f);
    EXPECT_EQ(b1.build().toJson(), "0.1");

    VariantBuilder b2;
    b2.appendFloat(0.3f);
    EXPECT_EQ(b2.build().toJson(), "0.3");

    VariantBuilder b3;
    b3.appendFloat(2.0f);
    EXPECT_EQ(b3.build().toJson(), "2.0");  // integer ".0" preserved
}

TEST(VariantTest, LargeDataRegionUses4ByteOffsets) {
    // Regression: a container whose data region exceeds 0xFFFFFF (16 MiB)
    // requires 4-byte offsets. A single string element of length 16777216
    // pushes the array's data region past the 3-byte cap, exercising the
    // 4-byte size path. (~16 MiB alloc; takes a couple seconds.)
    constexpr size_t len = 16777216;  // 0x1000000
    VariantBuilder b;
    b.startArray();
    b.appendString(std::string(len, 'a'));
    b.endArray();
    Variant v = b.build();

    EXPECT_EQ(v.getType(), VariantType::Array);
    EXPECT_EQ(v.numArrayElements(), 1);
    ASSERT_TRUE(v.getElementAtIndex(0).has_value());
    EXPECT_EQ(v.getElementAtIndex(0)->getString().size(), len);
}

TEST(VariantTest, BuilderMisuseThrows) {
    // build() with an open container.
    {
        VariantBuilder b;
        b.startObject();
        EXPECT_THROW(b.build(), VariantException);
    }
    // appendKey outside an object.
    {
        VariantBuilder b;
        EXPECT_THROW(b.appendKey("k"), VariantException);
    }
    // Value in an object without a preceding appendKey.
    {
        VariantBuilder b;
        b.startObject();
        EXPECT_THROW(b.appendLong(1), VariantException);
    }
    // Mismatched end.
    {
        VariantBuilder b;
        b.startArray();
        EXPECT_THROW(b.endObject(), VariantException);
    }
}

// ---- malformed JSON ----

TEST(VariantTest, MalformedJsonThrows) {
    EXPECT_THROW(parse("{not valid"), VariantException);
    EXPECT_THROW(parse("[1,2,"), VariantException);
    EXPECT_THROW(parse(""), VariantException);
}
