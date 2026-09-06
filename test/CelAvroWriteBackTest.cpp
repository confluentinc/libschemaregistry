/**
 * D2: a CEL rule's computed decimal, timestamp or variant must be written back into the
 * Avro datum. Before this, `toAvroValue` had no arm for any of the three - a decimal and a
 * variant arrive as proto messages and a timestamp as a CEL timestamp, none of which matched
 * a branch - so all three reached the trailing `return original` and the computed value was
 * silently discarded. No error, wrong data.
 *
 * These drive the real field-transform walk, so they cover the same path a serializer takes.
 */

#include <gtest/gtest.h>


#include <memory>
#include <string>
#include <vector>

#include "schemaregistry/rules/cel/CelExecutor.h"
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

/// The control: a string transform always worked, which is what originally isolated D2 to
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

