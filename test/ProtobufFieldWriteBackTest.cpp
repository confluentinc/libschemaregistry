/**
 * Writing a field executor's result back into a protobuf message.
 *
 * `setMessageField` dispatched on the value's own kind and called the matching reflection
 * setter, with nothing checking that the *field* was of that kind - and protobuf's reflection
 * setters CHECK the field's type and **abort the process** on a mismatch. So a rule that
 * answered with a shape the field could not hold killed the process rather than reporting
 * anything.
 *
 * A `confluent.type.Decimal` field is what makes this reachable. The walk hands such a field
 * to the rule *whole* rather than descending into value/scale - it has to, or a rule tagged
 * for the field never fires - so any field executor that answers with a scalar hands back a
 * bytes value for a message-typed field. That is not a CEL-only concern: the JVM's
 * `setTransformedField` is ungated for exactly this reason, its own comment naming "a broad
 * untagged masking or redaction rule reaching one of these fields", and it turns the mismatch
 * into a named rule error instead of the bare ClassCastException `setField` would raise.
 * Gating the shortcut to CEL_FIELD would have diverged from that; reporting the mismatch
 * matches it.
 */
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "schemaregistry/serdes/Serde.h"
#include "schemaregistry/serdes/protobuf/ProtobufTypes.h"
#include "schemaregistry/serdes/protobuf/ProtobufUtils.h"
#include "test/parity.pb.h"

using namespace schemaregistry::serdes;
using namespace schemaregistry::serdes::protobuf;

namespace {

/// Stands in for any non-CEL field executor. A mask, a redaction and ENCRYPT all answer with
/// a bytes value whatever the field's declared shape, which is the case that aborted.
class BytesReturningExecutor : public FieldRuleExecutor {
  public:
    std::string getType() const override { return "MASK"; }
    std::unique_ptr<SerdeValue> transformField(
        RuleContext &ctx, const SerdeValue &field_value) override {
        return makeProtobufValue(ProtobufVariant(std::vector<uint8_t>{0xDE, 0xAD}));
    }
};

/// Echoes the field back unchanged, so the same walk can be shown to work.
class EchoExecutor : public FieldRuleExecutor {
  public:
    std::string getType() const override { return "MASK"; }
    std::unique_ptr<SerdeValue> transformField(
        RuleContext &ctx, const SerdeValue &field_value) override {
        return field_value.clone();
    }
};

parity::ParityPlain runFieldRule(const std::shared_ptr<FieldRuleExecutor> &executor,
                                 const std::string &tag) {
    Rule rule;
    rule.setName("r");
    rule.setType("MASK");
    rule.setKind(Kind::Transform);
    rule.setMode(Mode::Write);
    rule.setTags(std::vector<std::string>{tag});

    SerializationContext ser_ctx{"t", SerdeType::Value, SerdeFormat::Protobuf, std::nullopt};
    std::vector<Rule> rules{rule};
    auto registry = std::make_shared<RuleRegistry>();
    registry->registerExecutor(executor);
    RuleContext ctx(std::nullopt, ser_ctx, std::nullopt, std::nullopt, "t-value",
                    Mode::Write, rule, 0, rules,
                    std::unordered_map<std::string, std::unordered_set<std::string>>{},
                    nullptr, registry);

    auto msg = std::make_unique<parity::ParityPlain>();
    msg->mutable_amount()->set_value(std::string("\x04\xD2", 2));
    msg->mutable_amount()->set_scale(2);
    // `ts` has to be set: the walk has nothing to hand a rule for an unset message field, so
    // an unset one would make the TS case pass by never firing.
    msg->mutable_ts()->set_seconds(1700000000);
    msg->set_plain("hi");
    auto input = makeProtobufValue(ProtobufVariant(std::move(msg)));
    auto result = ::schemaregistry::serdes::protobuf::utils::transformFields(
        ctx, parity::ParityPlain::descriptor(), *input);
    auto &pv = asProtobuf(*result);
    parity::ParityPlain out;
    out.CopyFrom(*std::get<std::unique_ptr<google::protobuf::Message>>(pv.value));
    return out;
}

}  // namespace

/// The one that aborted: bytes for a confluent.type.Decimal field.
TEST(ProtobufFieldWriteBack, AScalarForAValueTypeFieldIsReported) {
    EXPECT_THROW(runFieldRule(std::make_shared<BytesReturningExecutor>(), "AMOUNT"),
                 std::exception);
    // The timestamp field is the same shape of mistake.
    EXPECT_THROW(runFieldRule(std::make_shared<BytesReturningExecutor>(), "TS"),
                 std::exception);
}

/// Bytes for a *string* field still go through: string and bytes share CPPTYPE_STRING and
/// both reach SetString, so narrowing that would break an encryption rule on a string field.
TEST(ProtobufFieldWriteBack, BytesForAStringFieldStillWork) {
    parity::ParityPlain out =
        runFieldRule(std::make_shared<BytesReturningExecutor>(), "PLAIN");

    EXPECT_EQ(out.plain(), std::string("\xDE\xAD", 2));
    // The value-type fields were not named by the rule, so they come through untouched.
    EXPECT_EQ(out.amount().scale(), 2);
}

/// The must-fail twin: an executor that echoes the field leaves the message intact, so
/// "throws" above cannot mean the walk stopped reaching value-type fields at all.
TEST(ProtobufFieldWriteBack, AnEchoingRuleLeavesAValueTypeFieldIntact) {
    parity::ParityPlain out = runFieldRule(std::make_shared<EchoExecutor>(), "AMOUNT");

    EXPECT_EQ(out.amount().value(), std::string("\x04\xD2", 2));
    EXPECT_EQ(out.amount().scale(), 2);
    EXPECT_EQ(out.plain(), "hi");
}
