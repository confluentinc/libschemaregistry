/**
 * A CEL rule's computed decimal, timestamp or variant must be written back into the
 * Avro datum. Before this, `toAvroValue` had no arm for any of the three - a decimal and a
 * variant arrive as proto messages and a timestamp as a CEL timestamp, none of which matched
 * a branch - so all three reached the trailing `return original` and the computed value was
 * silently discarded. No error, wrong data.
 *
 * These drive the real field-transform walk, so they cover the same path a serializer takes.
 */

#include <gtest/gtest.h>


#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "schemaregistry/rest/ClientConfiguration.h"
#include "schemaregistry/rest/MockSchemaRegistryClient.h"
#include "schemaregistry/rest/model/Rule.h"
#include "schemaregistry/rest/model/RuleSet.h"
#include "schemaregistry/rest/model/Schema.h"
#include "schemaregistry/rules/cel/CelExecutor.h"
#include "schemaregistry/serdes/avro/AvroDeserializer.h"
#include "schemaregistry/rules/cel/CelFieldExecutor.h"
#include "schemaregistry/serdes/Serde.h"
#include "schemaregistry/serdes/avro/AvroSerializer.h"
#include "schemaregistry/serdes/avro/AvroUtils.h"
#include "schemaregistry/serdes/protobuf/ProtobufTypes.h"
#include "schemaregistry/serdes/protobuf/ProtobufUtils.h"
#include "schemaregistry/serdes/Variant.h"
#include "test/parity.pb.h"

using namespace schemaregistry::serdes;
using schemaregistry::rules::cel::CelFieldExecutor;
using schemaregistry::rest::ClientConfiguration;
using schemaregistry::rest::MockSchemaRegistryClient;
using schemaregistry::serdes::avro::AvroDeserializer;
using schemaregistry::serdes::avro::AvroSerializer;

namespace {

const char *kSchema = R"({
  "type": "record",
  "name": "R",
  "fields": [
    {"name": "amount",
     "type": {"type": "bytes", "logicalType": "decimal", "precision": 8, "scale": 2},
     "confluent:tags": ["AMOUNT"]},
    {"name": "ts",
     "type": {"type": "long", "logicalType": "timestamp-millis"},
     "confluent:tags": ["TS"]},
    {"name": "label", "type": "string", "confluent:tags": ["LABEL"]}
  ]
})";

::avro::GenericDatum makeRecord(const ::avro::ValidSchema &schema) {
    ::avro::GenericDatum datum(schema);
    auto &record = datum.value<::avro::GenericRecord>();
    // 0x04D2 = 1234 unscaled, i.e. 12.34 at scale 2.
    record.fieldAt(0).value<std::vector<uint8_t>>() = {0x04, 0xD2};
    record.fieldAt(1).value<int64_t>() = 1700000000123L;
    record.fieldAt(2).value<std::string>() = "hi";
    return datum;
}

/// Runs one tagged CEL_FIELD transform over the record and returns the result.
::avro::GenericDatum runTransform(const ::avro::ValidSchema &schema,
                                  const ::avro::GenericDatum &datum,
                                  const std::string &tag, const std::string &expr) {
    Rule rule;
    rule.setName("r");
    rule.setType("CEL_FIELD");
    rule.setKind(Kind::Transform);
    rule.setMode(Mode::Write);
    rule.setExpr(expr);
    rule.setTags(std::vector<std::string>{tag});

    SerializationContext ser_ctx{"t", SerdeType::Value, SerdeFormat::Avro, std::nullopt};
    std::vector<Rule> rules{rule};

    // transformFields resolves the executor from the context's registry
    // (AvroUtils.cpp:134); without it the walk finds nothing and silently returns the
    // record unchanged - which looks exactly like a skipped rule.
    auto registry = std::make_shared<RuleRegistry>();
    registry->registerExecutor(std::make_shared<CelFieldExecutor>());

    // Field tags reach the walk through the context's inline_tags map, keyed by the
    // field's full name (RuleContext::enterField, Serde.cpp:426) - the walk itself
    // passes an empty tag set. A rule tagged for a field that carries no tags here never
    // applies, and the record comes back unchanged with no error.
    std::unordered_map<std::string, std::unordered_set<std::string>> inline_tags{
        {"R.amount", {"AMOUNT"}},
        {"R.ts", {"TS"}},
        {"R.label", {"LABEL"}},
    };

    RuleContext ctx(std::nullopt, ser_ctx, std::nullopt, std::nullopt, "t-value",
                    Mode::Write, rule, 0, rules, inline_tags, nullptr, registry);

    return schemaregistry::serdes::avro::utils::transformFields(ctx, schema, datum);
}

}  // namespace

/// A computed decimal reaches the datum. 12.34 + 1.00 = 13.34, unscaled 1334 = 0x0536.
TEST(CelAvroWriteBack, ComputedDecimalIsWrittenBack) {
    ::avro::ValidSchema schema = AvroSerializer::compileJsonSchema(kSchema);
    ::avro::GenericDatum datum = makeRecord(schema);

    ::avro::GenericDatum result = runTransform(
        schema, datum, "AMOUNT", "decimals.add(decimal(value), decimal('1.00'))");

    ASSERT_EQ(result.type(), ::avro::AVRO_RECORD);
    const auto &amount = result.value<::avro::GenericRecord>().fieldAt(0);
    const auto bytes = amount.value<std::vector<uint8_t>>();

    EXPECT_EQ(bytes, (std::vector<uint8_t>{0x05, 0x36})) << "expected 13.34 at scale 2";
    // Stated separately: if the write-back arm is missing, toAvroValue returns the original
    // and the field still reads 12.34. A regression cannot pass by doing nothing.
    EXPECT_NE(bytes, (std::vector<uint8_t>{0x04, 0xD2}))
        << "the computed decimal was discarded and the original returned";
}

/// A computed timestamp reaches the datum, in the field's own unit.
TEST(CelAvroWriteBack, ComputedTimestampIsWrittenBack) {
    ::avro::ValidSchema schema = AvroSerializer::compileJsonSchema(kSchema);
    ::avro::GenericDatum datum = makeRecord(schema);

    ::avro::GenericDatum result =
        runTransform(schema, datum, "TS", "value + duration('60s')");

    ASSERT_EQ(result.type(), ::avro::AVRO_RECORD);
    const int64_t millis = result.value<::avro::GenericRecord>().fieldAt(1).value<int64_t>();

    EXPECT_EQ(millis, 1700000060123L);
    EXPECT_NE(millis, 1700000000123L)
        << "the computed timestamp was discarded and the original returned";
}

/// A computed timestamp reaches a `timestamp-nanos` field too. The write-back switch handled only
/// millis and micros, so nanos hit the default and the transform silently returned the field
/// unchanged - the failure mode the EXPECT_NE below is there to catch.
TEST(CelAvroWriteBack, ComputedTimestampNanosIsWrittenBack) {
    // Named `R` with the field named `ts` so it matches runTransform's inline_tags key
    // ("R.ts"): a full name the map does not carry means the tag never applies and the record
    // comes back unchanged, which is indistinguishable from the bug this test is for.
    const char *nanosSchema = R"({
      "type": "record",
      "name": "R",
      "fields": [
        {"name": "ts",
         "type": {"type": "long", "logicalType": "timestamp-nanos"},
         "confluent:tags": ["TS"]}
      ]
    })";
    ::avro::ValidSchema schema = AvroSerializer::compileJsonSchema(nanosSchema);
    ::avro::GenericDatum datum(schema);
    datum.value<::avro::GenericRecord>().fieldAt(0).value<int64_t>() =
        1700000000123456789LL;

    ::avro::GenericDatum result =
        runTransform(schema, datum, "TS", "value + duration('60s')");

    ASSERT_EQ(result.type(), ::avro::AVRO_RECORD);
    const int64_t nanos = result.value<::avro::GenericRecord>().fieldAt(0).value<int64_t>();

    EXPECT_EQ(nanos, 1700000060123456789LL);
    EXPECT_NE(nanos, 1700000000123456789LL)
        << "the computed timestamp was discarded and the original returned";
}

/// The control: a string transform always worked, which is what originally isolated the fault to
/// the logical-type arms rather than to the walk.
TEST(CelAvroWriteBack, StringTransformStillWorks) {
    ::avro::ValidSchema schema = AvroSerializer::compileJsonSchema(kSchema);
    ::avro::GenericDatum datum = makeRecord(schema);

    ::avro::GenericDatum result =
        runTransform(schema, datum, "LABEL", "value + '-suffix'");

    ASSERT_EQ(result.type(), ::avro::AVRO_RECORD);
    EXPECT_EQ(result.value<::avro::GenericRecord>().fieldAt(2).value<std::string>(),
              "hi-suffix");
}

/// An identity transform must leave the value byte-identical. This is the C6 pass-through
/// case, and the cheapest regression test for any write-back path.
TEST(CelAvroWriteBack, IdentityDecimalRoundTrips) {
    ::avro::ValidSchema schema = AvroSerializer::compileJsonSchema(kSchema);
    ::avro::GenericDatum datum = makeRecord(schema);

    ::avro::GenericDatum result = runTransform(schema, datum, "AMOUNT", "value");

    ASSERT_EQ(result.type(), ::avro::AVRO_RECORD);
    EXPECT_EQ(result.value<::avro::GenericRecord>().fieldAt(0).value<std::vector<uint8_t>>(),
              (std::vector<uint8_t>{0x04, 0xD2}));
}

// ---- Message-level CEL transforms over protobuf (C6/C7) --------------------------------
//
// The rule returns a map and the message is rebuilt from it. Before this the result stayed a
// bare ProtobufVariant::Map, which the serializer cannot write, so every message-level
// transform failed - including an identity one.
//
// The transform has replace semantics: the map is the new message, so a field the rule does
// not name is dropped and a null clears its field.

namespace {

using schemaregistry::rules::cel::CelExecutor;
using schemaregistry::serdes::protobuf::ProtobufVariant;
using schemaregistry::serdes::protobuf::asProtobuf;
using schemaregistry::serdes::protobuf::makeProtobufValue;

std::unique_ptr<parity::ParityPlain> parityMessage() {
    auto m = std::make_unique<parity::ParityPlain>();
    m->mutable_amount()->set_value(std::string("\x04\xD2", 2));
    m->mutable_amount()->set_precision(8);
    m->mutable_amount()->set_scale(2);
    m->mutable_ts()->set_seconds(1700000000);
    m->mutable_ts()->set_nanos(123000000);
    auto v = Variant::parseJson("{\"name\":\"alice\"}");
    const auto &mb = v.metadataBytes();
    const auto &vb = v.valueBytes();
    m->mutable_data()->set_metadata(std::string(mb.begin(), mb.end()));
    m->mutable_data()->set_value(std::string(vb.begin(), vb.end()));
    m->set_plain("hi");
    return m;
}

/// Runs one message-level CEL transform and returns the rebuilt message.
parity::ParityPlain runMessageTransform(const std::string &expr) {
    Rule rule;
    rule.setName("r");
    rule.setType("CEL");
    rule.setKind(Kind::Transform);
    rule.setMode(Mode::Write);
    rule.setExpr(expr);

    SerializationContext serCtx{"t", SerdeType::Value, SerdeFormat::Protobuf, std::nullopt};
    std::vector<Rule> rules{rule};
    RuleContext ctx(std::nullopt, serCtx, std::nullopt, std::nullopt, "t-value",
                    Mode::Write, rule, 0, rules,
                    std::unordered_map<std::string, std::unordered_set<std::string>>{});

    auto input = makeProtobufValue(ProtobufVariant(parityMessage()));
    CelExecutor exec;
    auto result = exec.transform(ctx, *input);
    auto &pv = asProtobuf(*result);
    EXPECT_EQ(pv.type, ProtobufVariant::ValueType::Message)
        << "expected the message to be rebuilt";
    const auto &msg = std::get<std::unique_ptr<google::protobuf::Message>>(pv.value);
    parity::ParityPlain out;
    out.CopyFrom(*msg);
    return out;
}

const char *kAll =
    "'amount': message.amount, 'ts': message.ts, "
    "'data': message.data, 'plain': message.plain";

}  // namespace

/// An identity transform is the cheapest regression test for a write-back path: it fails for
/// any breakage in the plumbing, without depending on the computation.
TEST(CelProtobufMessageTransform, PassThrough) {
    auto out = runMessageTransform(std::string("{") + kAll + "}");

    EXPECT_EQ(out.amount().value(), std::string("\x04\xD2", 2));
    EXPECT_EQ(out.amount().scale(), 2);
    EXPECT_EQ(out.ts().seconds(), 1700000000);
    EXPECT_EQ(out.ts().nanos(), 123000000);
    EXPECT_EQ(out.plain(), "hi");
}

TEST(CelProtobufMessageTransform, ComputedDecimal) {
    auto out = runMessageTransform(
        "{'amount': decimals.add(decimal(message.amount), decimal('1.00')), "
        "'ts': message.ts, 'data': message.data, 'plain': message.plain}");

    // 0x0536 = 1334, i.e. 13.34 at scale 2.
    EXPECT_EQ(out.amount().value(), std::string("\x05\x36", 2));
    EXPECT_EQ(out.amount().scale(), 2);
    EXPECT_NE(out.amount().value(), std::string("\x04\xD2", 2));
}

TEST(CelProtobufMessageTransform, ComputedTimestamp) {
    auto out = runMessageTransform(
        "{'amount': message.amount, 'ts': message.ts + duration('60s'), "
        "'data': message.data, 'plain': message.plain}");

    EXPECT_EQ(out.ts().seconds(), 1700000060);
    EXPECT_EQ(out.ts().nanos(), 123000000);
}

/// Asserted through the decoded JSON rather than the metadata bytes: metadata holds the field
/// names, so the two documents share it and comparing metadata would prove nothing.
TEST(CelProtobufMessageTransform, ComputedVariant) {
    auto out = runMessageTransform(
        "{'amount': message.amount, 'ts': message.ts, "
        "'data': variants.parseJson('{\"name\":\"bob\"}'), 'plain': message.plain}");

    const std::string &meta = out.data().metadata();
    const std::string &val = out.data().value();
    Variant v(std::vector<uint8_t>(val.begin(), val.end()),
              std::vector<uint8_t>(meta.begin(), meta.end()));
    EXPECT_EQ(v.toJson(), "{\"name\":\"bob\"}");
}

/// Replace semantics, and the consequence most likely to surprise: a rule naming only the
/// field it changes discards everything else. Intended, but silent on protobuf - proto3 has no
/// required fields, so nothing catches it.
TEST(CelProtobufMessageTransform, DropsUnnamedFields) {
    auto out = runMessageTransform("{'plain': 'changed'}");

    EXPECT_EQ(out.plain(), "changed");
    EXPECT_FALSE(out.has_amount());
    EXPECT_FALSE(out.has_ts());
    EXPECT_FALSE(out.has_data());
}

/// The idiom for preserving absence across a transform that echoes a field is
/// `has(x) ? x : null`; without a null arm there would be no way to express it.
TEST(CelProtobufMessageTransform, NullClearsAField) {
    auto out = runMessageTransform(
        "{'amount': null, 'ts': message.ts, 'data': message.data, "
        "'plain': message.plain}");

    EXPECT_FALSE(out.has_amount());
    EXPECT_TRUE(out.has_ts());
    EXPECT_EQ(out.plain(), "hi");
}

// ---- CEL_FIELD over protobuf value types (C4/C5) ---------------------------------------
//
// Avro carries decimal and timestamp as logical types on a primitive, so the field is a leaf
// and a field rule reaches it. Protobuf carries them as messages, so the walk used to descend
// *past* the field and transform value/scale or seconds/nanos one at a time - meaning a rule
// tagged for the field never fired at all, and the message came back unchanged with no error.
//
// Port of the JVM client's #4538. Variant is deliberately not a leaf.

namespace {

/// Runs one tagged CEL_FIELD rule over the parity fixture through the client's own walker.
parity::ParityPlain runFieldRule(const std::string &expr, Kind kind, const std::string &tag) {
    Rule rule;
    rule.setName("r");
    rule.setType("CEL_FIELD");
    rule.setKind(kind);
    rule.setMode(Mode::Write);
    rule.setExpr(expr);
    rule.setTags(std::vector<std::string>{tag});

    SerializationContext serCtx{"t", SerdeType::Value, SerdeFormat::Protobuf, std::nullopt};
    std::vector<Rule> rules{rule};

    auto registry = std::make_shared<RuleRegistry>();
    registry->registerExecutor(std::make_shared<CelFieldExecutor>());

    RuleContext ctx(std::nullopt, serCtx, std::nullopt, std::nullopt, "t-value",
                    Mode::Write, rule, 0, rules,
                    std::unordered_map<std::string, std::unordered_set<std::string>>{},
                    nullptr, registry);

    auto msg = parityMessage();
    auto input = makeProtobufValue(ProtobufVariant(parityMessage()));
    auto result = ::schemaregistry::serdes::protobuf::utils::transformFields(
        ctx, parity::ParityPlain::descriptor(), *input);
    auto &pv = asProtobuf(*result);
    EXPECT_EQ(pv.type, ProtobufVariant::ValueType::Message);
    const auto &out = std::get<std::unique_ptr<google::protobuf::Message>>(pv.value);
    parity::ParityPlain copy;
    copy.CopyFrom(*out);
    return copy;
}

}  // namespace

/// The declared type is what makes CEL_FIELD apply at all: a Record is skipped outright.
TEST(CelProtobufFieldValueTypes, FieldTypesMatchAvro) {
    const auto *desc = parity::ParityPlain::descriptor();

    EXPECT_TRUE(::schemaregistry::serdes::protobuf::utils::isCelLeafMessage(
        desc->FindFieldByName("amount")->message_type()));
    EXPECT_TRUE(::schemaregistry::serdes::protobuf::utils::isCelLeafMessage(
        desc->FindFieldByName("ts")->message_type()));
    // Variant stays a record, as in Avro - not a leaf.
    EXPECT_FALSE(::schemaregistry::serdes::protobuf::utils::isCelLeafMessage(
        desc->FindFieldByName("data")->message_type()));
}

/// A CEL_FIELD rule on a value-type field has to hand back that value type, and nothing else.
/// Both mismatches were silent: a message of the wrong type fell to `return original` and the
/// field kept its input, and the timestamp arm accepted *any* message - a Decimal has no
/// seconds or nanos field, so the field was replaced with an **empty Decimal**, which reads
/// back as zero. A scalar was worse still: the arms below would hand back a scalar variant for
/// a CPPTYPE_MESSAGE field, and protobuf's reflection setter CHECK-fails on that, aborting the
/// process. The JVM reports all three from ProtobufSchema.rebuildValueType, as "Rule returned
/// <type> for field '<name>', which is a <message>; expected a decimal" (or a timestamp).
TEST(CelProtobufFieldValueTypes, AWrongValueTypeForAFieldIsRejected) {
    // A timestamp for the decimal field, and a decimal for the timestamp field.
    EXPECT_THROW(runFieldRule("timestamp(0)", Kind::Transform, "AMOUNT"), std::exception);
    EXPECT_THROW(runFieldRule("decimal('1.23')", Kind::Transform, "TS"), std::exception);
    // A scalar for either.
    EXPECT_THROW(runFieldRule("1", Kind::Transform, "AMOUNT"), std::exception);
    EXPECT_THROW(runFieldRule("'x'", Kind::Transform, "TS"), std::exception);

    // The must-fail twins: the right value type still reaches the field.
    parity::ParityPlain dec = runFieldRule(
        "decimals.add(decimal(value), decimal('1.00'))", Kind::Transform, "AMOUNT");
    EXPECT_NE(dec.amount().value(), std::string("\x04\xD2", 2));
    parity::ParityPlain ts = runFieldRule("value + duration('60s')", Kind::Transform, "TS");
    EXPECT_EQ(ts.ts().seconds(), 1700000060);
}

/// C4. Before the port this reported nothing because the rule never ran.
TEST(CelProtobufFieldValueTypes, DecimalConditionFires) {
    EXPECT_NO_THROW(runFieldRule(
        "decimals.gt(decimal(value), decimal('10.00'))", Kind::Condition, "AMOUNT"));
}

/// The must-fail twin. Without it the test above would also pass if no rule ran at all -
/// which is exactly how the defect hid.
TEST(CelProtobufFieldValueTypes, DecimalConditionFailsWhenItShould) {
    EXPECT_ANY_THROW(runFieldRule(
        "decimals.gt(decimal(value), decimal('1000.00'))", Kind::Condition, "AMOUNT"));
}

TEST(CelProtobufFieldValueTypes, TimestampConditionFires) {
    EXPECT_NO_THROW(runFieldRule(
        "value > timestamp('2000-01-01T00:00:00Z')", Kind::Condition, "TS"));
}

TEST(CelProtobufFieldValueTypes, TimestampConditionFailsWhenItShould) {
    EXPECT_ANY_THROW(runFieldRule(
        "value > timestamp('2050-01-01T00:00:00Z')", Kind::Condition, "TS"));
}

/// C5.
TEST(CelProtobufFieldValueTypes, DecimalTransformIsWrittenBack) {
    auto out = runFieldRule(
        "decimals.add(decimal(value), decimal('1.00'))", Kind::Transform, "AMOUNT");

    // 0x0536 = 1334, i.e. 13.34 at scale 2.
    EXPECT_EQ(out.amount().value(), std::string("\x05\x36", 2));
    EXPECT_EQ(out.amount().scale(), 2);
}

TEST(CelProtobufFieldValueTypes, TimestampTransformIsWrittenBack) {
    auto out = runFieldRule("value + duration('60s')", Kind::Transform, "TS");

    EXPECT_EQ(out.ts().seconds(), 1700000060);
    EXPECT_EQ(out.ts().nanos(), 123000000);
}

/// The pass-through: the encode must invert the decode exactly.
TEST(CelProtobufFieldValueTypes, IdentityTransformRoundTrips) {
    auto out = runFieldRule("value", Kind::Transform, "AMOUNT");

    EXPECT_EQ(out.amount().value(), std::string("\x04\xD2", 2));
    EXPECT_EQ(out.amount().scale(), 2);
}

/// Variant is a record in both formats, so a field rule must not reach it. The rule below
/// would raise if it ran, so a clean return means it was skipped.
TEST(CelProtobufFieldValueTypes, VariantIsStillSkipped) {
    auto out = runFieldRule(
        "variants.type(value) == 'not-a-type'", Kind::Condition, "DATA");

    EXPECT_FALSE(out.data().metadata().empty());
}

// ---- Message-level CEL transforms over Avro: replace, not merge -----------------------
//
// The rule's map *is* the new record. C++ was the only client that merged: `toAvroValue` seeded
// the result with every field of the input before applying the map, so a field the rule did not
// name kept its old value. That is the natural shape for a converter whose job is "convert this
// value against a template" - which is what the function does for a field value - and the wrong
// one for a message-level transform.
//
// It went unseen because every C6/C7 case names all of the record's fields, which makes merge and
// replace indistinguishable. These are the cases that tell them apart.

namespace {

const char *kDefaultsSchema = R"({
  "type": "record",
  "name": "D",
  "fields": [
    {"name": "kept", "type": "string"},
    {"name": "withDefault", "type": "string", "default": "fallback"},
    {"name": "nullable", "type": ["null", "string"]}
  ]
})";

::avro::GenericDatum makeDefaultsRecord(const ::avro::ValidSchema &schema) {
    ::avro::GenericDatum datum(schema);
    auto &record = datum.value<::avro::GenericRecord>();
    record.fieldAt(0).value<std::string>() = "original-kept";
    record.fieldAt(1).value<std::string>() = "original-withDefault";
    return datum;
}

/// Runs one message-level CEL transform over the record and returns the result.
::avro::GenericDatum runMessageTransform(const ::avro::ValidSchema &schema,
                                         const ::avro::GenericDatum &datum,
                                         const std::string &expr) {
    Rule rule;
    rule.setName("r");
    rule.setType("CEL");
    rule.setKind(Kind::Transform);
    rule.setMode(Mode::Write);
    rule.setExpr(expr);

    SerializationContext ser_ctx{"t", SerdeType::Value, SerdeFormat::Avro, std::nullopt};
    std::vector<Rule> rules{rule};
    auto registry = std::make_shared<RuleRegistry>();
    registry->registerExecutor(std::make_shared<CelExecutor>());
    RuleContext ctx(std::nullopt, ser_ctx, std::nullopt, std::nullopt, "t-value",
                    Mode::Write, rule, 0, rules, {}, nullptr, registry);

    CelExecutor exec;
    auto input = schemaregistry::serdes::avro::makeAvroValue(datum);
    auto result = exec.transform(ctx, *input);
    return schemaregistry::serdes::avro::asAvro(*result);
}

}  // namespace

/// The case the C6/C7 fixture could not see. Before this, `amount` and `ts` came back with their
/// original values from a rule that named neither.
TEST(CelAvroMessageTransform, AFieldTheRuleDoesNotNameIsDropped) {
    ::avro::ValidSchema schema = AvroSerializer::compileJsonSchema(kSchema);
    ::avro::GenericDatum datum = makeRecord(schema);

    // `label` is the only field named; `amount` and `ts` have no default, so replace semantics
    // make this an error rather than a silent partial record.
    EXPECT_THROW(runMessageTransform(schema, datum, "{'label': message.label}"),
                 std::exception);
}

/// The must-fail twin of the above: naming every field still round-trips. Without this, "throws"
/// could just as well mean the transform stopped working altogether.
TEST(CelAvroMessageTransform, NamingEveryFieldRoundTrips) {
    ::avro::ValidSchema schema = AvroSerializer::compileJsonSchema(kSchema);
    ::avro::GenericDatum datum = makeRecord(schema);

    ::avro::GenericDatum result = runMessageTransform(
        schema, datum,
        "{'amount': message.amount, 'ts': message.ts, 'label': message.label}");

    ASSERT_EQ(result.type(), ::avro::AVRO_RECORD);
    const auto &record = result.value<::avro::GenericRecord>();
    EXPECT_EQ(record.fieldAt(0).value<std::vector<uint8_t>>(),
              (std::vector<uint8_t>{0x04, 0xD2}));
    EXPECT_EQ(record.fieldAt(1).value<int64_t>(), 1700000000123L);
    EXPECT_EQ(record.fieldAt(2).value<std::string>(), "hi");
}

/// An unnamed field takes the schema's declared default, matching GenericRecordBuilder.build().
/// The value it does *not* take is the one it had on the way in - which is what merge would give.
TEST(CelAvroMessageTransform, AnUnnamedFieldTakesItsDeclaredDefault) {
    ::avro::ValidSchema schema = AvroSerializer::compileJsonSchema(kDefaultsSchema);
    ::avro::GenericDatum datum = makeDefaultsRecord(schema);

    ::avro::GenericDatum result =
        runMessageTransform(schema, datum, "{'kept': message.kept}");

    ASSERT_EQ(result.type(), ::avro::AVRO_RECORD);
    const auto &record = result.value<::avro::GenericRecord>();
    EXPECT_EQ(record.fieldAt(0).value<std::string>(), "original-kept");
    EXPECT_EQ(record.fieldAt(1).value<std::string>(), "fallback");
    EXPECT_NE(record.fieldAt(1).value<std::string>(), "original-withDefault")
        << "the unnamed field kept its input value - this is the merge behaviour";
    EXPECT_EQ(record.fieldAt(2).type(), ::avro::AVRO_NULL);
}


// ---- Message-level CEL transforms over a nullable field: the bytes have to be readable -------
//
// Every test above asserts on the datum the executor returned, never on the bytes it encodes to.
// That is what let a union field's write-back go wrong unseen: `GenericDatum::type()`,
// `logicalType()` and `value<T>()` all forward through a union to its selected branch, so
// `toAvroValue` could not tell its template was a union and returned a bare datum. A bare datum
// in a union slot encodes with *no branch index*, and reading it back threw
// `std::out_of_range` from `selectBranch` - avro-cpp had taken the next field's bytes as the
// branch. So this one goes through the real serializer and back.

namespace {

const char *kNullableSchema = R"({
  "type": "record",
  "name": "N",
  "fields": [
    {"name": "amount",
     "type": ["null", {"type": "bytes", "logicalType": "decimal", "precision": 8, "scale": 2}]},
    {"name": "label", "type": "string"}
  ]
})";

/// Serializes with one message-level CEL transform and reads the record back off the bytes.
::avro::GenericDatum roundTripMessageTransform(
    const std::string &schema_text, const std::string &expr,
    const std::string &subject,
    const std::function<void(::avro::GenericRecord &)> &seed = nullptr) {
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = std::make_shared<MockSchemaRegistryClient>(client_config);

    Rule rule;
    rule.setName(std::make_optional<std::string>("r"));
    rule.setKind(std::make_optional<Kind>(Kind::Transform));
    rule.setMode(std::make_optional<Mode>(Mode::Write));
    rule.setType(std::make_optional<std::string>("CEL"));
    rule.setExpr(std::make_optional<std::string>(expr));

    RuleSet rule_set;
    rule_set.setDomainRules(
        std::make_optional<std::vector<Rule>>(std::vector<Rule>{rule}));
    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("AVRO"));
    schema.setSchema(std::make_optional<std::string>(schema_text));
    schema.setRuleSet(std::make_optional<RuleSet>(rule_set));
    client->registerSchema(subject + "-value", schema, false);

    ::avro::ValidSchema avro_schema = AvroSerializer::compileJsonSchema(schema_text);
    ::avro::GenericDatum datum(avro_schema);
    auto &record = datum.value<::avro::GenericRecord>();
    if (seed) {
        // A seed owns the whole record: the defaults below name this file's nullable fixture,
        // which not every schema passed here declares.
        seed(record);
    } else {
        record.field("amount").selectBranch(0);
        record.field("label").value<std::string>() = "hi";
    }

    auto registry = std::make_shared<RuleRegistry>();
    registry->registerExecutor(std::make_shared<CelExecutor>());
    auto ser_config = SerializerConfig(
        false, std::make_optional(SchemaSelector::useLatestVersion()), true, false,
        std::unordered_map<std::string, std::string>{});
    AvroSerializer serializer(client, std::nullopt, registry, ser_config);
    AvroDeserializer deserializer(client, registry, DeserializerConfig::createDefault());

    SerializationContext ser_ctx;
    ser_ctx.topic = subject;
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Avro;

    return deserializer.deserialize(ser_ctx, serializer.serialize(ser_ctx, datum)).value;
}

}  // namespace

/// Pass-through over a null union branch: the field the rule echoed has to come back null, and
/// the record has to be readable at all.
TEST(CelAvroMessageTransform, NullableFieldRoundTripsThroughTheWire) {
    ::avro::GenericDatum result = roundTripMessageTransform(
        kNullableSchema, "{'amount': message.amount, 'label': message.label}", "nullable-pass");

    ASSERT_EQ(result.type(), ::avro::AVRO_RECORD);
    const auto &record = result.value<::avro::GenericRecord>();
    EXPECT_EQ(record.field("amount").type(), ::avro::AVRO_NULL);
    EXPECT_EQ(record.field("label").value<std::string>(), "hi");
}

/// The same for a field the rule does not name: it takes null (the branch its union allows), and
/// that null still has to be encoded as a union member.
TEST(CelAvroMessageTransform, UnnamedNullableFieldRoundTripsThroughTheWire) {
    ::avro::GenericDatum result = roundTripMessageTransform(
        kNullableSchema, "{'label': message.label}", "nullable-omit");

    ASSERT_EQ(result.type(), ::avro::AVRO_RECORD);
    const auto &record = result.value<::avro::GenericRecord>();
    EXPECT_EQ(record.field("amount").type(), ::avro::AVRO_NULL);
    EXPECT_EQ(record.field("label").value<std::string>(), "hi");
}

// ---- the union branch a computed value belongs in -------------------------------------------
//
// `toAvroValue` reads the logical type, scale and unit off the datum it is handed, and a union
// datum forwards all three to its *currently selected* branch. The template was the field's own
// datum, so for a nullable field that is presently null there was nothing to convert against:
// a decimal was rescaled to a phantom scale of 0 and a timestamp had no unit at all. The branch
// is now resolved from the returned value first, the way the JVM writer's resolveUnion does.

namespace {

const char *kNullableTimestampSchema = R"({
  "type": "record",
  "name": "N",
  "fields": [
    {"name": "amount", "type": ["null", {"type": "long", "logicalType": "timestamp-millis"}]},
    {"name": "label", "type": "string"}
  ]
})";

}  // namespace

/// A decimal computed for a null nullable field is written at the *field's* scale. 5 rescaled to
/// scale 2 is unscaled 500 = 0x01F4; the value the bug produced is unscaled 5, which reads back
/// as 0.05 - a silent wrong answer, not an error, which is why this asserts on both.
TEST(CelAvroMessageTransform, NullBranchDecimalTakesTheBranchsScale) {
    ::avro::GenericDatum result = roundTripMessageTransform(
        kNullableSchema, "{'amount': decimal('5'), 'label': message.label}",
        "nullable-decimal-scale");

    ASSERT_EQ(result.type(), ::avro::AVRO_RECORD);
    const auto &amount = result.value<::avro::GenericRecord>().field("amount");
    ASSERT_EQ(amount.type(), ::avro::AVRO_BYTES);
    EXPECT_EQ(amount.value<std::vector<uint8_t>>(),
              (std::vector<uint8_t>{0x01, 0xF4})) << "expected 5.00 at scale 2";
    EXPECT_NE(amount.value<std::vector<uint8_t>>(), (std::vector<uint8_t>{0x05}))
        << "written at the phantom scale of the null branch, so it reads back as 0.05";
}

/// The scale that cannot round-trip at all: 1.23 rescaled to 0 is inexact, so before the branch
/// was resolved this whole transform failed rather than writing 1.23.
TEST(CelAvroMessageTransform, NullBranchDecimalKeepsItsFractionalDigits) {
    ::avro::GenericDatum result = roundTripMessageTransform(
        kNullableSchema, "{'amount': decimal('1.23'), 'label': message.label}",
        "nullable-decimal-fraction");

    ASSERT_EQ(result.type(), ::avro::AVRO_RECORD);
    const auto &amount = result.value<::avro::GenericRecord>().field("amount");
    ASSERT_EQ(amount.type(), ::avro::AVRO_BYTES);
    EXPECT_EQ(amount.value<std::vector<uint8_t>>(), (std::vector<uint8_t>{0x7B}));
}

/// A timestamp computed for a null nullable field is written in the branch's unit. With no unit
/// on the template it hit the write-back switch's default and the field came back null.
TEST(CelAvroMessageTransform, NullBranchTimestampTakesTheBranchsUnit) {
    ::avro::GenericDatum result = roundTripMessageTransform(
        kNullableTimestampSchema,
        "{'amount': timestamp('2023-11-14T22:13:20.123Z'), 'label': message.label}",
        "nullable-timestamp-unit");

    ASSERT_EQ(result.type(), ::avro::AVRO_RECORD);
    const auto &amount = result.value<::avro::GenericRecord>().field("amount");
    ASSERT_EQ(amount.type(), ::avro::AVRO_LONG) << "the computed timestamp was dropped";
    EXPECT_EQ(amount.value<int64_t>(), 1700000000123L);
}

/// The control for the other half of the branch choice: a field that *already* holds the branch
/// keeps its own datum as the template, so an arithmetic transform over it still works.
/// 12.34 + 1.00 = 13.34, unscaled 1334 = 0x0536.
TEST(CelAvroMessageTransform, NonNullBranchStillConvertsAgainstItself) {
    ::avro::GenericDatum result = roundTripMessageTransform(
        kNullableSchema,
        "{'amount': decimals.add(decimal(message.amount), decimal('1.00')), "
        "'label': message.label}",
        "nullable-decimal-present", [](::avro::GenericRecord &record) {
            record.field("amount").selectBranch(1);
            record.field("amount").value<std::vector<uint8_t>>() = {0x04, 0xD2};
            record.field("label").value<std::string>() = "hi";
        });

    ASSERT_EQ(result.type(), ::avro::AVRO_RECORD);
    const auto &amount = result.value<::avro::GenericRecord>().field("amount");
    ASSERT_EQ(amount.type(), ::avro::AVRO_BYTES);
    EXPECT_EQ(amount.value<std::vector<uint8_t>>(),
              (std::vector<uint8_t>{0x05, 0x36}));
}

// ---- a value the field's schema cannot hold has to be reported --------------------------------
//
// `toAvroValue` dispatches on the *CEL value*, not on the schema, with a trailing
// `return original` for anything it does not recognise - so a value the field could not hold
// was never reported. It went one of two ways, neither an error:
//
//   * the value was discarded and the field kept its **input** - a timestamp, list or map
//     returned for a string field, or a timestamp for a decimal field;
//   * a datum of the CEL value's own type was written into the slot, giving a record that no
//     longer matches its schema - an int returned for a string field, a string for a long, a
//     string for an array. Reading such a record back crashed the reader outright.
//
// The JVM dispatches on the schema and throws typeMismatch for all of them, so the value is now
// checked against the field's schema before any conversion runs.

namespace {

const char *kShapesSchema = R"({
  "type": "record",
  "name": "S",
  "fields": [
    {"name": "amount",
     "type": {"type": "bytes", "logicalType": "decimal", "precision": 8, "scale": 2}},
    {"name": "ts", "type": {"type": "long", "logicalType": "timestamp-millis"}},
    {"name": "label", "type": "string"},
    {"name": "codes", "type": {"type": "array", "items": "string"}},
    {"name": "tags", "type": {"type": "map", "values": "string"}}
  ]
})";

/// The identity map over kShapesSchema with exactly one field replaced. Every field has to be
/// named because the transform replaces rather than merges, and none of them has a default.
std::string shapesExpr(const std::string &field, const std::string &value) {
    const char *names[][2] = {{"amount", "message.amount"}, {"ts", "message.ts"},
                              {"label", "message.label"}, {"codes", "message.codes"},
                              {"tags", "message.tags"}};
    std::string expr = "{";
    for (const auto &n : names) {
        if (expr.size() > 1) {
            expr += ", ";
        }
        expr += std::string("'") + n[0] + "': " + (field == n[0] ? value : n[1]);
    }
    return expr + "}";
}

::avro::GenericDatum shapesRecord(const ::avro::ValidSchema &schema) {
    ::avro::GenericDatum datum(schema);
    auto &record = datum.value<::avro::GenericRecord>();
    record.fieldAt(0).value<std::vector<uint8_t>>() = {0x04, 0xD2};
    record.fieldAt(1).value<int64_t>() = 1700000000123L;
    record.fieldAt(2).value<std::string>() = "hi";
    record.fieldAt(3).value<::avro::GenericArray>().value().push_back(
        ::avro::GenericDatum(std::string("a")));
    record.fieldAt(4).value<::avro::GenericMap>().value().emplace_back(
        "k", ::avro::GenericDatum(std::string("v")));
    return datum;
}

::avro::GenericDatum runShapes(const std::string &field, const std::string &value) {
    ::avro::ValidSchema schema = AvroSerializer::compileJsonSchema(kShapesSchema);
    return runMessageTransform(schema, shapesRecord(schema), shapesExpr(field, value));
}

}  // namespace

/// The must-fail twin first: the identity transform over this fixture has to keep working, so
/// "throws" below cannot just mean the walk stopped working.
TEST(CelAvroMessageTransform, TheShapesFixtureRoundTrips) {
    ::avro::GenericDatum out = runShapes("", "");

    ASSERT_EQ(out.type(), ::avro::AVRO_RECORD);
    const auto &record = out.value<::avro::GenericRecord>();
    EXPECT_EQ(record.field("amount").value<std::vector<uint8_t>>(),
              (std::vector<uint8_t>{0x04, 0xD2}));
    EXPECT_EQ(record.field("ts").value<int64_t>(), 1700000000123L);
    EXPECT_EQ(record.field("label").value<std::string>(), "hi");
    EXPECT_EQ(record.field("codes").value<::avro::GenericArray>().value().size(), 1u);
    EXPECT_EQ(record.field("tags").value<::avro::GenericMap>().value().size(), 1u);
}

/// The cases that kept the field's input value. Nothing about the result distinguished these
/// from a rule that had legitimately echoed the field, which is why they went unnoticed.
TEST(CelAvroMessageTransform, AValueThatWasSilentlyDiscardedIsRefused) {
    EXPECT_THROW(runShapes("label", "timestamp(0)"), std::exception);
    EXPECT_THROW(runShapes("label", "['x']"), std::exception);
    EXPECT_THROW(runShapes("label", "{'a': 'b'}"), std::exception);
    EXPECT_THROW(runShapes("amount", "timestamp(0)"), std::exception);
    EXPECT_THROW(runShapes("codes", "{'a': 'b'}"), std::exception);
    EXPECT_THROW(runShapes("tags", "['x']"), std::exception);
}

/// The cases that wrote a datum of the wrong type into the slot. These produced a record that
/// does not match its own schema - reading one back crashes the reader.
TEST(CelAvroMessageTransform, AWrongTypedDatumIsRefused) {
    EXPECT_THROW(runShapes("label", "1"), std::exception);
    EXPECT_THROW(runShapes("label", "message.amount"), std::exception);
    EXPECT_THROW(runShapes("ts", "'x'"), std::exception);
    EXPECT_THROW(runShapes("ts", "message.amount"), std::exception);
    EXPECT_THROW(runShapes("amount", "'x'"), std::exception);
    EXPECT_THROW(runShapes("amount", "7"), std::exception);
    EXPECT_THROW(runShapes("codes", "'x'"), std::exception);
    EXPECT_THROW(runShapes("tags", "'x'"), std::exception);
}

/// A plain `long` field is not a timestamp field: its unit is not declared, so there is nothing
/// to write a computed timestamp in. The JVM's branchAccepts requires the logical type too.
TEST(CelAvroMessageTransform, ATimestampNeedsATimestampLogicalType) {
    const char *plain = R"({
      "type": "record", "name": "S",
      "fields": [{"name": "n", "type": "long"}]
    })";
    ::avro::ValidSchema schema = AvroSerializer::compileJsonSchema(plain);
    ::avro::GenericDatum datum(schema);
    datum.value<::avro::GenericRecord>().fieldAt(0).value<int64_t>() = 7;

    EXPECT_THROW(runMessageTransform(schema, datum, "{'n': timestamp(0)}"), std::exception);
    EXPECT_EQ(runMessageTransform(schema, datum, "{'n': 8}")
                  .value<::avro::GenericRecord>().fieldAt(0).value<int64_t>(), 8);
}

/// A CEL timestamp spans years 1 to 9999; an epoch-nanosecond int64 spans only 1677 to 2262,
/// and absl saturates rather than reporting the loss, so an out-of-range instant was written as
/// a different one. The JVM reaches the same rejection through Avro's TimestampNanosConversion,
/// whose Math.multiplyExact raises "long overflow" - measured against avro 1.12.2:
///   2023-11-14T22:13:20.123456789Z -> 1700000000123456789
///   2262-04-11T23:47:16.854775807Z -> 9223372036854775807  (the last representable instant)
///   2263-01-01T00:00:00Z           -> THROW ArithmeticException: long overflow
///   9999-12-31T23:59:59.999999999Z -> THROW
TEST(CelAvroMessageTransform, TimestampNanosRangeIsChecked) {
    const char *nanos = R"({
      "type": "record", "name": "S",
      "fields": [{"name": "ts", "type": {"type": "long", "logicalType": "timestamp-nanos"}}]
    })";
    ::avro::ValidSchema schema = AvroSerializer::compileJsonSchema(nanos);
    ::avro::GenericDatum datum(schema);
    datum.value<::avro::GenericRecord>().fieldAt(0).value<int64_t>() = 1700000000123456789LL;

    // In range, including the last instant an int64 of nanoseconds can hold.
    EXPECT_EQ(runMessageTransform(schema, datum, "{'ts': timestamp('2023-11-14T22:13:20.123456789Z')}")
                  .value<::avro::GenericRecord>().fieldAt(0).value<int64_t>(),
              1700000000123456789LL);
    EXPECT_EQ(runMessageTransform(schema, datum, "{'ts': timestamp('2262-04-11T23:47:16.854775807Z')}")
                  .value<::avro::GenericRecord>().fieldAt(0).value<int64_t>(),
              9223372036854775807LL);
    // Out of range, and a CEL rule can name either of these directly.
    EXPECT_THROW(runMessageTransform(schema, datum, "{'ts': timestamp('2263-01-01T00:00:00Z')}"),
                 std::exception);
    EXPECT_THROW(runMessageTransform(schema, datum,
                                     "{'ts': timestamp('9999-12-31T23:59:59.999999999Z')}"),
                 std::exception);
    // A millis field takes the whole CEL range, so the check belongs to nanos alone.
    const char *millis = R"({
      "type": "record", "name": "S",
      "fields": [{"name": "ts", "type": {"type": "long", "logicalType": "timestamp-millis"}}]
    })";
    ::avro::ValidSchema ms = AvroSerializer::compileJsonSchema(millis);
    ::avro::GenericDatum msDatum(ms);
    msDatum.value<::avro::GenericRecord>().fieldAt(0).value<int64_t>() = 0;
    EXPECT_NO_THROW(runMessageTransform(ms, msDatum,
                                        "{'ts': timestamp('9999-12-31T23:59:59.999Z')}"));
}

// ---- a numeric field has to get a datum of its own type ---------------------------------------
//
// `toAvroValue` built a datum of the CEL value's type, not the field's. `fromAvroValue` widens
// an Avro int to CEL's only integer type and an Avro float to its only floating one, so an
// **identity** transform handed an `int` field an AVRO_LONG datum and a `float` field an
// AVRO_DOUBLE one. The record no longer matched its own schema, and nothing reported it - the
// damage appeared on the wire, because the encoder wrote eight bytes for the double where the
// reader expected four for the float and every field after it decoded from misaligned bytes:
//
//   declared:   int long float double
//   before:     long long double double     -> read back f=0, d=5.3024e-315
//   after:      int  long float  double     -> read back f=1.5, d=2.5
//
// The JVM converts against the schema instead: AvroResultWriter's INT/LONG/FLOAT/DOUBLE cases
// call narrowToInt/narrowToLong/narrowToFloat/narrowToDouble.

namespace {

const char *kNumericSchema = R"({
  "type": "record",
  "name": "N2",
  "fields": [
    {"name": "i", "type": "int"},
    {"name": "l", "type": "long"},
    {"name": "f", "type": "float"},
    {"name": "d", "type": "double"}
  ]
})";

::avro::GenericDatum numericRecord(const ::avro::ValidSchema &schema) {
    ::avro::GenericDatum datum(schema);
    auto &record = datum.value<::avro::GenericRecord>();
    record.fieldAt(0).value<int32_t>() = 7;
    record.fieldAt(1).value<int64_t>() = 8;
    record.fieldAt(2).value<float>() = 1.5f;
    record.fieldAt(3).value<double>() = 2.5;
    return datum;
}

const char *kNumericIdentity = "{'i': message.i, 'l': message.l, 'f': message.f, "
                               "'d': message.d}";

}  // namespace

/// The datum types themselves, which is where the fault was.
TEST(CelAvroMessageTransform, ANumericFieldKeepsItsOwnDatumType) {
    ::avro::ValidSchema schema = AvroSerializer::compileJsonSchema(kNumericSchema);
    ::avro::GenericDatum out =
        runMessageTransform(schema, numericRecord(schema), kNumericIdentity);

    ASSERT_EQ(out.type(), ::avro::AVRO_RECORD);
    const auto &record = out.value<::avro::GenericRecord>();
    EXPECT_EQ(record.field("i").type(), ::avro::AVRO_INT);
    EXPECT_EQ(record.field("l").type(), ::avro::AVRO_LONG);
    EXPECT_EQ(record.field("f").type(), ::avro::AVRO_FLOAT);
    EXPECT_EQ(record.field("d").type(), ::avro::AVRO_DOUBLE);
    EXPECT_EQ(record.field("i").value<int32_t>(), 7);
    EXPECT_EQ(record.field("f").value<float>(), 1.5f);
}

/// And the consequence, through the real serializer and back: asserting on the datum alone is
/// what let this hide, since a wrong-typed datum reports its own type quite happily.
TEST(CelAvroMessageTransform, ANumericRecordSurvivesTheWire) {
    ::avro::GenericDatum out = roundTripMessageTransform(
        kNumericSchema, kNumericIdentity, "numeric-wire",
        [](::avro::GenericRecord &record) {
            record.field("i").value<int32_t>() = 7;
            record.field("l").value<int64_t>() = 8;
            record.field("f").value<float>() = 1.5f;
            record.field("d").value<double>() = 2.5;
        });

    ASSERT_EQ(out.type(), ::avro::AVRO_RECORD);
    const auto &record = out.value<::avro::GenericRecord>();
    EXPECT_EQ(record.field("i").value<int32_t>(), 7);
    EXPECT_EQ(record.field("l").value<int64_t>(), 8);
    // The two that came back as 0 and 5.3024e-315 before, from the misalignment.
    EXPECT_EQ(record.field("f").value<float>(), 1.5f);
    EXPECT_EQ(record.field("d").value<double>(), 2.5);
}

/// An int field takes an integer in int32 range and refuses one outside it, which is
/// narrowToInt's "Value X out of range for INT field". A float field takes any number and
/// accepts the precision loss, as narrowToFloat's floatValue() does.
TEST(CelAvroMessageTransform, NumericNarrowingIsRangeChecked) {
    ::avro::ValidSchema schema = AvroSerializer::compileJsonSchema(kNumericSchema);
    ::avro::GenericDatum in = numericRecord(schema);

    auto with = [&](const std::string &field, const std::string &value) {
        std::string expr = "{";
        for (const char *name : {"i", "l", "f", "d"}) {
            if (expr.size() > 1) {
                expr += ", ";
            }
            expr += std::string("'") + name + "': " +
                    (field == name ? value : std::string("message.") + name);
        }
        return runMessageTransform(schema, in, expr + "}");
    };

    EXPECT_EQ(with("i", "2147483647").value<::avro::GenericRecord>()
                  .field("i").value<int32_t>(), 2147483647);
    EXPECT_EQ(with("i", "-2147483648").value<::avro::GenericRecord>()
                  .field("i").value<int32_t>(), -2147483648);
    EXPECT_THROW(with("i", "2147483648"), std::exception);
    EXPECT_THROW(with("i", "-2147483649"), std::exception);
    // An integer for a floating field is a widening both clients allow.
    EXPECT_EQ(with("f", "3").value<::avro::GenericRecord>().field("f").value<float>(), 3.0f);
    EXPECT_EQ(with("d", "3").value<::avro::GenericRecord>().field("d").value<double>(), 3.0);
    // A double for an int field is a type mismatch on the JVM, not a truncation.
    EXPECT_THROW(with("i", "2.0"), std::exception);
    EXPECT_THROW(with("l", "2.0"), std::exception);
}

/// An array's element template now comes from its schema rather than from element [0], so a
/// rule that adds elements to an **empty** array converts them against the declared item type
/// instead of against a default-constructed null datum.
TEST(CelAvroMessageTransform, ElementsAddedToAnEmptyArrayGetTheDeclaredType) {
    const char *arraySchema = R"({
      "type": "record", "name": "N2",
      "fields": [{"name": "ns", "type": {"type": "array", "items": "int"}}]
    })";
    ::avro::ValidSchema schema = AvroSerializer::compileJsonSchema(arraySchema);
    ::avro::GenericDatum datum(schema);  // ns is empty

    ::avro::GenericDatum out = runMessageTransform(schema, datum, "{'ns': [1, 2]}");
    const auto &array = out.value<::avro::GenericRecord>()
                            .field("ns").value<::avro::GenericArray>().value();
    ASSERT_EQ(array.size(), 2u);
    EXPECT_EQ(array[0].type(), ::avro::AVRO_INT);
    EXPECT_EQ(array[0].value<int32_t>(), 1);
    // Out of int32 range is refused per element, the same as for a scalar field.
    EXPECT_THROW(runMessageTransform(schema, datum, "{'ns': [2147483648]}"), std::exception);
}

// ---- decimal-on-fixed: the padding has to sign-extend -----------------------------------------
//
// A `fixed` field is exactly fixedSize() bytes wide, but a decimal's two's-complement encoding is
// minimal, so it has to be padded up. The pad byte is 0xFF for a negative coefficient and 0x00
// for a non-negative one - zero-padding a negative decimal turns it into a large positive one,
// which is why the negative case is the one that matters. Raw bytes, by contrast, are not padded
// at all: a width mismatch there is an error rather than a guessed alignment.

namespace {

/// `amount` is nullable so the shared fixture can seed it as null; the branch is resolved from
/// the returned decimal.
const char *kNullableFixedDecimalSchema = R"({
  "type": "record",
  "name": "N",
  "fields": [
    {"name": "amount",
     "type": ["null", {"type": "fixed", "name": "Dec", "size": 8,
                       "logicalType": "decimal", "precision": 18, "scale": 2}]},
    {"name": "label", "type": "string"}
  ]
})";

std::vector<uint8_t> fixedBytesOf(const ::avro::GenericDatum &datum) {
    return datum.value<::avro::GenericFixed>().value();
}

}  // namespace

/// A non-negative coefficient pads with 0x00. 1.23 is unscaled 123 = 0x7B in one byte.
TEST(CelAvroMessageTransform, FixedDecimalPadsToTheDeclaredWidth) {
    ::avro::GenericDatum result = roundTripMessageTransform(
        kNullableFixedDecimalSchema, "{'amount': decimal('1.23'), 'label': message.label}",
        "fixed-decimal-positive");

    ASSERT_EQ(result.type(), ::avro::AVRO_RECORD);
    const auto &amount = result.value<::avro::GenericRecord>().field("amount");
    ASSERT_EQ(amount.type(), ::avro::AVRO_FIXED);
    EXPECT_EQ(fixedBytesOf(amount),
              (std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 0x7B}));
}

/// The case zero-padding gets wrong: -1.23 is unscaled -123 = 0x85 minimally, so the seven pad
/// bytes have to be 0xFF. Padded with 0x00 the same bytes read back as 133, not -1.23.
TEST(CelAvroMessageTransform, FixedDecimalPadSignExtends) {
    ::avro::GenericDatum result = roundTripMessageTransform(
        kNullableFixedDecimalSchema, "{'amount': decimal('-1.23'), 'label': message.label}",
        "fixed-decimal-negative");

    ASSERT_EQ(result.type(), ::avro::AVRO_RECORD);
    const auto &amount = result.value<::avro::GenericRecord>().field("amount");
    ASSERT_EQ(amount.type(), ::avro::AVRO_FIXED);
    EXPECT_EQ(fixedBytesOf(amount),
              (std::vector<uint8_t>{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x85}));
    EXPECT_NE(fixedBytesOf(amount),
              (std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 0x85}))
        << "zero-padded, so the field reads back as 1.33 instead of -1.23";
}

/// A coefficient wider than the field is refused at encode rather than truncated or widened:
/// avro-cpp would not read back bytes wider than the declared size. 99999999 needs four bytes,
/// the field holds two.
TEST(CelAvroMessageTransform, FixedDecimalTooWideIsRefused) {
    const char *narrow = R"({
      "type": "record",
      "name": "N",
      "fields": [
        {"name": "amount",
         "type": ["null", {"type": "fixed", "name": "Dec", "size": 2,
                           "logicalType": "decimal", "precision": 4, "scale": 0}]},
        {"name": "label", "type": "string"}
      ]
    })";
    EXPECT_THROW(roundTripMessageTransform(
                     narrow, "{'amount': decimal('99999999'), 'label': message.label}",
                     "fixed-decimal-too-wide"),
                 std::exception);

    // The must-fail twin: a coefficient that does fit still round-trips, so "throws" cannot
    // just mean the fixed path stopped working. 1234 = 0x04D2.
    ::avro::GenericDatum ok = roundTripMessageTransform(
        narrow, "{'amount': decimal('1234'), 'label': message.label}",
        "fixed-decimal-fits");
    EXPECT_EQ(fixedBytesOf(ok.value<::avro::GenericRecord>().field("amount")),
              (std::vector<uint8_t>{0x04, 0xD2}));
}

/// Raw bytes are not padded: a rule returning a byte string of the wrong width for a `fixed`
/// field is an error, matching the JVM's toFixed ("expects N bytes, got M").
TEST(CelAvroMessageTransform, FixedRawBytesMustMatchTheWidthExactly) {
    const char *rawFixed = R"({
      "type": "record",
      "name": "N",
      "fields": [
        {"name": "amount", "type": ["null", {"type": "fixed", "name": "Id", "size": 4}]},
        {"name": "label", "type": "string"}
      ]
    })";
    EXPECT_THROW(roundTripMessageTransform(
                     rawFixed, "{'amount': b'ab', 'label': message.label}",
                     "fixed-raw-short"),
                 std::exception);

    ::avro::GenericDatum ok = roundTripMessageTransform(
        rawFixed, "{'amount': b'abcd', 'label': message.label}", "fixed-raw-exact");
    EXPECT_EQ(fixedBytesOf(ok.value<::avro::GenericRecord>().field("amount")),
              (std::vector<uint8_t>{'a', 'b', 'c', 'd'}));
}

// ---- a rule that cannot handle a null must fail loudly ---------------------------------------
//
// cel-cpp reports a runtime failure as an error Value carrying an OK status - a failed
// conversion, an unresolved overload - so the executor's status check never saw it. The error
// value then reached `toAvroValue`, which has no arm for it and hands its input back, so a
// message-level condition over a null neither passed, failed nor errored: the record went out
// exactly as it came in. That was the only silent wrong answer in the whole C8/C9 sweep, and the
// worst of the three possible outcomes - a raise names the rule, a false is at least a verdict,
// a no-op is neither.

namespace {

const char *kNullableDecimalSchema = R"({
  "type": "record",
  "name": "N",
  "fields": [
    {"name": "amount",
     "type": ["null", {"type": "bytes", "logicalType": "decimal", "precision": 8, "scale": 2}],
     "confluent:tags": ["AMOUNT"]},
    {"name": "label", "type": "string"}
  ]
})";

/// Runs one message-level CEL *condition* over a record whose decimal field is null.
::avro::GenericDatum runNullCondition(const std::string &expr) {
    ::avro::ValidSchema schema = AvroSerializer::compileJsonSchema(kNullableDecimalSchema);
    ::avro::GenericDatum datum(schema);
    auto &record = datum.value<::avro::GenericRecord>();
    record.field("amount").selectBranch(0);
    record.field("label").value<std::string>() = "hi";

    Rule rule;
    rule.setName("r");
    rule.setType("CEL");
    rule.setKind(Kind::Condition);
    rule.setMode(Mode::Write);
    rule.setExpr(expr);

    SerializationContext ser_ctx{"t", SerdeType::Value, SerdeFormat::Avro, std::nullopt};
    std::vector<Rule> rules{rule};
    auto registry = std::make_shared<RuleRegistry>();
    registry->registerExecutor(std::make_shared<CelExecutor>());
    RuleContext ctx(std::nullopt, ser_ctx, std::nullopt, std::nullopt, "t-value",
                    Mode::Write, rule, 0, rules, {}, nullptr, registry);

    CelExecutor exec;
    auto input = schemaregistry::serdes::avro::makeAvroValue(datum);
    return schemaregistry::serdes::avro::asAvro(*exec.transform(ctx, *input));
}

}  // namespace

/// The load-bearing case: an unguarded rule over a null must throw, not return the record.
TEST(CelAvroNullCondition, UnguardedRuleOverANullThrows) {
    EXPECT_THROW(runNullCondition("decimals.gt(message.amount, decimal(\"10.00\"))"),
                 std::exception);
}

/// Its twin. `== null` is the guard the reference recommends for exactly this case, and it has
/// to keep working - otherwise "it throws" could just mean nulls broke altogether.
TEST(CelAvroNullCondition, NullGuardStillAnswersTrue) {
    ::avro::GenericDatum result = runNullCondition("message.amount == null");

    ASSERT_EQ(result.type(), ::avro::AVRO_BOOL);
    EXPECT_TRUE(result.value<bool>());
}

/// And a false verdict is still a verdict, not an error - so the new throw has not swallowed
/// the ordinary condition path.
TEST(CelAvroNullCondition, AFalseConditionIsStillABool) {
    ::avro::GenericDatum result = runNullCondition("message.label == 'nope'");

    ASSERT_EQ(result.type(), ::avro::AVRO_BOOL);
    EXPECT_FALSE(result.value<bool>());
}

/// A decimal written back into a `bytes` decimal field carries the *minimal* two's-complement
/// encoding, byte-for-byte what Avro Java's Conversions.DecimalConversion.toBytes produces.
/// Measured against avro 1.12 at scale 2: 12.34 -> 04d2, -12.34 -> fb2e, 0.00 -> 00.
///
/// The sibling `fixed` shape is padded to the field's declared width instead (toFixed gives
/// 00000000000004d2 for the same value); see toAvroValue in CelUtils.cpp.
TEST(CelAvroWriteBack, ComputedDecimalUsesMinimalBytes) {
    const char *bytesSchema = R"({
      "type": "record",
      "name": "R",
      "fields": [
        {"name": "amount",
         "type": {"type": "bytes", "logicalType": "decimal", "precision": 10, "scale": 2},
         "confluent:tags": ["AMOUNT"]}
      ]
    })";
    ::avro::ValidSchema schema = AvroSerializer::compileJsonSchema(bytesSchema);

    struct Case {
        const char *expr;
        const char *expected;  // hex, from avro java's DecimalConversion.toBytes
    };
    for (const Case &c : {Case{"decimals.add(value, decimal('0.00'))", "04d2"},
                          Case{"decimals.sub(value, decimal('24.68'))", "fb2e"},
                          Case{"decimals.sub(value, decimal('12.34'))", "00"}}) {
        ::avro::GenericDatum datum(schema);
        // 0x04D2 = 1234 unscaled, i.e. 12.34 at scale 2.
        datum.value<::avro::GenericRecord>().fieldAt(0).value<std::vector<uint8_t>>() =
            std::vector<uint8_t>{0x04, 0xD2};

        ::avro::GenericDatum result = runTransform(schema, datum, "AMOUNT", c.expr);
        ASSERT_EQ(result.type(), ::avro::AVRO_RECORD) << c.expr;
        const std::vector<uint8_t> &bytes =
            result.value<::avro::GenericRecord>().fieldAt(0).value<std::vector<uint8_t>>();
        std::string hex;
        for (uint8_t b : bytes) {
            static const char *digits = "0123456789abcdef";
            hex.push_back(digits[b >> 4]);
            hex.push_back(digits[b & 0x0F]);
        }
        EXPECT_EQ(hex, c.expected) << c.expr;
        // Two of the three change the value, so a regression cannot pass by doing nothing.
    }
}

