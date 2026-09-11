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

// Evaluate a boolean rule with `this` bound to a proto Variant message wrapping a single
// nanosecond-precision timestamp value (nanoseconds since the Unix epoch).
bool evalNanosTsTz(int64_t nanos, const std::string &expr) {
    VariantBuilder builder;
    builder.appendTimestampNanosTz(nanos);
    Variant parsed = builder.build();
    auto msg = std::make_unique<confluent::type::Variant>();
    msg->set_metadata(std::string(parsed.metadataBytes().begin(),
                                  parsed.metadataBytes().end()));
    msg->set_value(std::string(parsed.valueBytes().begin(), parsed.valueBytes().end()));
    CelValidator validator;
    auto value = protobuf::makeProtobufValue(protobuf::ProtobufVariant(std::move(msg)));
    auto result = validator.execute(rule(expr), *value);
    EXPECT_TRUE(std::holds_alternative<bool>(result)) << expr;
    return std::holds_alternative<bool>(result) && std::get<bool>(result);
}

}  // namespace

// A variant timestamp spans the whole int64 range while a CEL timestamp is 0001-9999, so an
// out-of-range value is reachable from data. It used to be built anyway, leaving an instant
// cel-cpp refuses to render but will still compare - `< now` answered a confident false for a
// value that is not a time. Refused now, and routed through the as/tryAs split so a rule can
// guard, matching the reference's variantGetTimestamp.
TEST(CelVariantTest, VariantAsTimestampIsRangeChecked) {
    constexpr int64_t kMaxMicros = 253402300799LL * 1000000 + 999999;
    constexpr int64_t kMinMicros = -62135596800LL * 1000000;

    auto bind = [](int64_t micros) {
        VariantBuilder builder;
        builder.appendTimestampTz(micros);
        return builder.build();
    };
    auto evalTs = [](const Variant &parsed, const std::string &expr) {
        auto msg = std::make_unique<confluent::type::Variant>();
        msg->set_metadata(std::string(parsed.metadataBytes().begin(),
                                      parsed.metadataBytes().end()));
        msg->set_value(std::string(parsed.valueBytes().begin(),
                                   parsed.valueBytes().end()));
        auto value = protobuf::makeProtobufValue(
            protobuf::ProtobufVariant(std::move(msg)));
        CelValidator validator;
        auto r = validator.execute(rule(expr), *value);
        return std::holds_alternative<bool>(r) && std::get<bool>(r);
    };

    // The boundaries are inside the range, and tryAs answers a timestamp there.
    for (int64_t micros : {int64_t(0), kMaxMicros, kMinMicros}) {
        Variant v = bind(micros);
        EXPECT_TRUE(evalTs(v, "variants.as(this, \"timestamp\") == "
                              "variants.as(this, \"timestamp\")"))
            << micros;
        EXPECT_FALSE(evalTs(v, "variants.tryAs(this, \"timestamp\") == null")) << micros;
    }

    // Out of range: as refuses and names the range, tryAs answers CEL null.
    for (int64_t micros : {int64_t(9223372036854775807LL),
                           int64_t(-9223372036854775807LL),
                           kMaxMicros + 1000000}) {
        Variant v = bind(micros);
        try {
            evalTs(v, "variants.as(this, \"timestamp\") != null");
            ADD_FAILURE() << "variants.as should have refused " << micros;
        } catch (const std::exception &e) {
            EXPECT_NE(std::string(e.what()).find("is outside 0001-01-01T00:00:00Z"),
                      std::string::npos)
                << "should name the range: " << e.what();
        }
        EXPECT_TRUE(evalTs(v, "variants.tryAs(this, \"timestamp\") == null")) << micros;
    }
}

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
    // variant(null) passes CEL null through as CEL null (no error), and navigation over it
    // is still CEL null - so a null argument (here a missing field) composes cleanly.
    EXPECT_TRUE(evalWith("variant(null) == null", kDoc));
    EXPECT_TRUE(evalWith(
        "variants.field(variant(variants.field(variants.parseJson(this), 'missing')), 'k') == "
        "null",
        kDoc));
}

// BUG P: dotted identifiers in a variant path must accept non-ASCII (UTF-8) key
// characters, matching Java's Character.isLetter/isLetterOrDigit. The C++ parser
// approximates by accepting any byte >= 0x80 as an identifier character, so keys like
// "café", "über", "naïve", and CJK resolve. ASCII paths and quoted keys are unaffected.
TEST(CelVariantTest, VariantPathUnicodeIdentifiers) {
    constexpr const char *kUnicodeDoc =
        R"({"café":1,"über":2,"naïve":3,"中文":4,"ascii_1":5})";
    // Dotted non-ASCII identifiers now resolve instead of raising a CEL error.
    EXPECT_TRUE(evalWith(
        "variants.as(variants.path(variants.parseJson(this), '$.café'), 'int') == 1",
        kUnicodeDoc));
    EXPECT_TRUE(evalWith(
        "variants.as(variants.path(variants.parseJson(this), '$.über'), 'int') == 2",
        kUnicodeDoc));
    EXPECT_TRUE(evalWith(
        "variants.as(variants.path(variants.parseJson(this), '$.naïve'), 'int') == 3",
        kUnicodeDoc));
    EXPECT_TRUE(evalWith(
        "variants.as(variants.path(variants.parseJson(this), '$.中文'), 'int') == 4",
        kUnicodeDoc));
    // ASCII dotted identifiers still work (incl. digits/underscore in continuation).
    EXPECT_TRUE(evalWith(
        "variants.as(variants.path(variants.parseJson(this), '$.ascii_1'), 'int') == 5",
        kUnicodeDoc));
    // The quoted form still resolves a non-ASCII key too.
    EXPECT_TRUE(evalWith(
        "variants.as(variants.path(variants.parseJson(this), '$[\"café\"]'), 'int') == 1",
        kUnicodeDoc));
}

// Empty / whitespace-only input is a soft failure: variants.tryParseJson maps
// the typed parse error to CEL null (rather than surfacing a CEL error).
TEST(CelVariantTest, TryParseJsonEmptyIsNull) {
    EXPECT_TRUE(evalWith("variants.tryParseJson('') == null", kDoc));
    EXPECT_TRUE(evalWith("variants.tryParseJson('   ') == null", kDoc));
    // CEL escape sequences so the whitespace reaches parseJson as tab/newline.
    EXPECT_TRUE(evalWith("variants.tryParseJson('\\t\\n ') == null", kDoc));
    // A well-formed document still parses.
    EXPECT_TRUE(
        evalWith("variants.type(variants.tryParseJson('{\"x\":1}')) == 'object'", kDoc));
}

// The non-finite bareword contract, end to end through the CEL layer. nlohmann rejects both the
// barewords and a literal that overflows to a non-finite double, so parseJson rewrites them; Java
// (Jackson), Python, C#, Rust, Go and JavaScript all accept these, and every client's toJson
// writes them back out as barewords.
TEST(CelVariantTest, NonFiniteThroughCel) {
    EXPECT_TRUE(evalWith("variants.type(variants.parseJson('NaN')) == 'double'", kDoc));
    EXPECT_TRUE(evalWith("variants.type(variants.parseJson('Infinity')) == 'double'", kDoc));
    EXPECT_TRUE(evalWith("variants.type(variants.parseJson('-Infinity')) == 'double'", kDoc));
    EXPECT_TRUE(evalWith("variants.toJson(variants.parseJson('NaN')) == 'NaN'", kDoc));
    EXPECT_TRUE(evalWith("variants.toJson(variants.parseJson('Infinity')) == 'Infinity'", kDoc));
    EXPECT_TRUE(
        evalWith("variants.toJson(variants.parseJson('-Infinity')) == '-Infinity'", kDoc));
    EXPECT_TRUE(evalWith(
        R"(variants.toJson(variants.parseJson('{"a":NaN}')) == '{"a":NaN}')", kDoc));
    EXPECT_TRUE(evalWith(
        R"(variants.toJson(variants.parseJson('[NaN,Infinity,-Infinity]')) == )"
        R"('[NaN,Infinity,-Infinity]')",
        kDoc));
    EXPECT_TRUE(evalWith(
        R"(variants.type(variants.field(variants.parseJson('{"a":NaN}'), 'a')) == 'double')",
        kDoc));
    // Magnitude overflow reads as +/-Infinity rather than failing the parse.
    EXPECT_TRUE(evalWith("variants.toJson(variants.parseJson('1e400')) == 'Infinity'", kDoc));
    EXPECT_TRUE(evalWith("variants.toJson(variants.parseJson('-1e400')) == '-Infinity'", kDoc));
    // A bareword is a successful parse, not a soft failure.
    EXPECT_TRUE(evalWith("variants.tryParseJson('NaN') != null", kDoc));
    // Spelling and case are exact, matching Jackson, so these stay soft failures.
    EXPECT_TRUE(evalWith("variants.tryParseJson('nan') == null", kDoc));
    EXPECT_TRUE(evalWith("variants.tryParseJson('INFINITY') == null", kDoc));
}

// ITEM #27: variants.as('timestamp') on a NANOS variant must preserve full nanosecond
// resolution and floor-divide for negatives (matching Java's fromEpochNanos, which uses
// Math.floorDiv into seconds+nanos). A raw/1000 truncation would drop sub-microsecond
// nanos and mis-round negative timestamps toward zero. The RFC-3339 literals are an
// independent oracle (parsed by absl, not through the nanos->Time path under test).
TEST(CelVariantTest, VariantAsTimestampPreservesNanos) {
    // Positive: 2020-01-01T00:00:00Z (1577836800 s) + 123456789 ns.
    EXPECT_TRUE(evalNanosTsTz(
        1577836800123456789LL,
        "variants.as(variant(this), 'timestamp') == "
        "timestamp('2020-01-01T00:00:00.123456789Z')"));
    // Negative: -1500 ns = 1969-12-31T23:59:59.999998500Z (floor-divides to seconds=-1,
    // nanos=999998500). The buggy raw/1000 path would yield -1000 ns instead.
    EXPECT_TRUE(evalNanosTsTz(
        -1500LL,
        "variants.as(variant(this), 'timestamp') == "
        "timestamp('1969-12-31T23:59:59.9999985Z')"));
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

// Cross-client parity, Protobuf half: a confluent.type.Variant message is usable with the
// variants.* accessors with **no variant(...) call**. CelProtoWrapper carries it as a message
// and variants.* are declared over {kAny}, so they take it directly.
TEST(CelVariantTest, ProtoVariantNeedsNoConstructor) {
    auto evalProtoVariant = [](const std::string &expr) {
        auto parsed = Variant::parseJson(kDoc);
        auto msg = std::make_unique<confluent::type::Variant>();
        msg->set_metadata(
            std::string(parsed.metadataBytes().begin(), parsed.metadataBytes().end()));
        msg->set_value(std::string(parsed.valueBytes().begin(), parsed.valueBytes().end()));
        CelValidator validator;
        auto value = protobuf::makeProtobufValue(protobuf::ProtobufVariant(std::move(msg)));
        auto result = validator.execute(rule(expr), *value);
        EXPECT_TRUE(std::holds_alternative<bool>(result)) << expr;
        return std::holds_alternative<bool>(result) && std::get<bool>(result);
    };

    // Bare: no constructor call.
    EXPECT_TRUE(evalProtoVariant("variants.type(this) == 'object'"));
    EXPECT_TRUE(evalProtoVariant("variants.as(variants.field(this, 'age'), 'int') == 30"));
    EXPECT_TRUE(evalProtoVariant("variants.as(variants.path(this, '$.age'), 'int') == 30"));
    // The wrapped form must keep working (variant(...) re-entry).
    EXPECT_TRUE(
        evalProtoVariant("variants.as(variants.field(variant(this), 'age'), 'int') == 30"));
    // A missing key is CEL null, not an error.
    EXPECT_TRUE(evalProtoVariant("variants.field(this, 'nope') == null"));
    // Negative control.
    EXPECT_FALSE(evalProtoVariant("variants.as(variants.field(this, 'age'), 'int') == 31"));
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

// Cross-client parity: an Avro variant field is usable with the variants.* accessors with **no
// variant(...) call**, and the wrapped form keeps working alongside it. fromAvroValue surfaces
// the record as a confluent.type.Variant message, and variants.* are declared over {kAny}, so
// they take it directly.
// `variants.isNull` must coerce its receiver like every other accessor. It is declared over dyn,
// so a bare variant field reaches it. A bare *object* cannot catch a missing coercion - isNull on
// an object is false either way - so only a variant that is itself null discriminates.
TEST(CelVariantTest, VariantIsNullCoercesBareReceiver) {
    auto evalIsNull = [](const std::string &expr, const std::string &json) {
        auto parsed = Variant::parseJson(json);
        auto msg = std::make_unique<confluent::type::Variant>();
        msg->set_metadata(
            std::string(parsed.metadataBytes().begin(), parsed.metadataBytes().end()));
        msg->set_value(std::string(parsed.valueBytes().begin(), parsed.valueBytes().end()));
        CelValidator validator;
        auto value = protobuf::makeProtobufValue(protobuf::ProtobufVariant(std::move(msg)));
        auto result = validator.execute(rule(expr), *value);
        EXPECT_TRUE(std::holds_alternative<bool>(result)) << expr;
        return std::holds_alternative<bool>(result) && std::get<bool>(result);
    };

    EXPECT_TRUE(evalIsNull("variants.isNull(this)", "null"));
    // The wrapped form has always worked and must keep working.
    EXPECT_TRUE(evalIsNull("variants.isNull(variant(this))", "null"));
    // A variant holding 5 is not variant-null.
    EXPECT_FALSE(evalIsNull("variants.isNull(this)", "5"));
}

TEST(CelVariantTest, AvroVariantNeedsNoConstructor) {
    auto evalVariant = [](const std::string &expr) {
        std::string schema = R"json({
            "type": "record", "name": "Holder",
            "confluent:rules": [
                {"name": "r", "expr": ")json" + expr + R"json("}
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
        auto &dataRec =
            datum.value<::avro::GenericRecord>().fieldAt(0).value<::avro::GenericRecord>();
        auto parsed = Variant::parseJson(R"({"name":"alice","age":30})");
        dataRec.field("metadata").value<std::vector<uint8_t>>() = parsed.metadataBytes();
        dataRec.field("value").value<std::vector<uint8_t>>() = parsed.valueBytes();
        CelValidator validator;
        return schemaregistry::serdes::avro::utils::validateMessage(
                   validator, nlohmann::json::parse(schema), {}, datum, false)
            .empty();
    };

    // Bare: no constructor call on the field.
    EXPECT_TRUE(evalVariant(R"(variants.type(this.data) == 'object')"));
    EXPECT_TRUE(
        evalVariant(R"(variants.as(variants.field(this.data, 'name'), 'string') == 'alice')"));
    EXPECT_TRUE(evalVariant(R"(variants.as(variants.path(this.data, '$.age'), 'int') == 30)"));
    // The wrapped form must keep working (variant(...) re-entry).
    EXPECT_TRUE(evalVariant(
        R"(variants.as(variants.field(variant(this.data), 'name'), 'string') == 'alice')"));
    // A missing key is CEL null, not an error — the null model still holds on a bare field.
    EXPECT_TRUE(evalVariant(R"(variants.field(this.data, 'nope') == null)"));
    // Negative control: a false comparison must fail.
    EXPECT_FALSE(
        evalVariant(R"(variants.as(variants.field(this.data, 'name'), 'string') == 'bob')"));
}
#endif

// An *absent* variant - a Protobuf field left unset, or an Avro variant record whose byte
// fields are empty - carries no metadata, so there is nothing to read. It reads as CEL null and
// every accessor propagates that, rather than the SrVariant constructor throwing on a metadata
// version byte that isn't there.
TEST(CelVariantTest, AbsentVariantReadsAsNull) {
    auto evalAbsent = [](const std::string &expr) {
        auto msg = std::make_unique<confluent::type::Variant>();
        msg->set_metadata("");
        msg->set_value("");
        CelValidator validator;
        auto value = protobuf::makeProtobufValue(protobuf::ProtobufVariant(std::move(msg)));
        auto result = validator.execute(rule(expr), *value);
        EXPECT_TRUE(std::holds_alternative<bool>(result)) << expr;
        return std::holds_alternative<bool>(result) && std::get<bool>(result);
    };

    EXPECT_TRUE(evalAbsent(R"(variants.type(this) == null)"));
    // isNull is false, not an error: an absent variant is not a JSON null.
    EXPECT_TRUE(evalAbsent(R"(!variants.isNull(this))"));
    EXPECT_TRUE(evalAbsent(R"(variants.field(this, 'name') == null)"));
    EXPECT_TRUE(evalAbsent(R"(variants.path(this, '$.name') == null)"));
    EXPECT_TRUE(evalAbsent(R"(variants.toJson(this) == null)"));
    // The explicit constructor reports it as CEL null too, like variant(null).
    EXPECT_TRUE(evalAbsent(R"(variant(this) == null)"));
}

// Absent must stay distinguishable from a variant that genuinely holds JSON null: the former is
// CEL null, the latter a present variant whose type is NULL.
TEST(CelVariantTest, ExplicitNullVariantIsNotAbsent) {
    auto evalNullVariant = [](const std::string &expr) {
        auto parsed = Variant::parseJson("null");
        auto msg = std::make_unique<confluent::type::Variant>();
        msg->set_metadata(
            std::string(parsed.metadataBytes().begin(), parsed.metadataBytes().end()));
        msg->set_value(std::string(parsed.valueBytes().begin(), parsed.valueBytes().end()));
        CelValidator validator;
        auto value = protobuf::makeProtobufValue(protobuf::ProtobufVariant(std::move(msg)));
        auto result = validator.execute(rule(expr), *value);
        EXPECT_TRUE(std::holds_alternative<bool>(result)) << expr;
        return std::holds_alternative<bool>(result) && std::get<bool>(result);
    };

    EXPECT_TRUE(evalNullVariant(R"(variants.isNull(this))"));
    EXPECT_TRUE(evalNullVariant(R"(variants.type(this) != null)"));
}
