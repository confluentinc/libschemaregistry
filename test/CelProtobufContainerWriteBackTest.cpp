/**
 * A message-level `CEL` transform that returns a **container**.
 *
 * The write-back skipped every repeated field outright - `if (fd->is_repeated()) continue;`, with
 * a comment saying that was safer than writing a partially-understood shape. It was not safer, it
 * was silent: the **identity** transform, the first rule anyone writes, returned a message whose
 * repeated field was empty and whose map had no keys, with no error, and the result still
 * validated. Three shapes were unwritable:
 *
 *   1. a repeated field (a map answers `is_repeated()` too, so it has to be tested first);
 *   2. a map field, which protobuf models as a repeated message of a synthesised entry type;
 *   3. a **constructed** nested message - a rule returning `{"inner": ...}` rather than echoing
 *      one. This third was hidden inside the same cell: `C7` also reported the nested value lost.
 *
 * Every positive here is paired with something that distinguishes "the container was written"
 * from "the container was left alone": either a computed value that differs from the input, or an
 * omitting rule that must leave the field empty.
 */
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "schemaregistry/rules/cel/CelExecutor.h"
#include "schemaregistry/serdes/Serde.h"
#include "schemaregistry/serdes/protobuf/ProtobufTypes.h"
#include "test/parity.pb.h"

using namespace schemaregistry::serdes;
using namespace schemaregistry::serdes::protobuf;
using schemaregistry::rules::cel::CelExecutor;

namespace {

/// Unscaled two's-complement big-endian bytes: encoding the magnitude instead turns 2.22 into
/// -0.34, and an unsigned reader shows it as 2.22 either way.
void setDec(confluent::type::Decimal *d, long long unscaled) {
    std::vector<unsigned char> bytes;
    for (long long n = unscaled; n > 0; n >>= 8) {
        bytes.insert(bytes.begin(), static_cast<unsigned char>(n & 0xff));
    }
    if (bytes.empty()) bytes.push_back(0);
    if (bytes.front() & 0x80) bytes.insert(bytes.begin(), 0);
    d->set_value(std::string(bytes.begin(), bytes.end()));
    d->set_precision(8);
    d->set_scale(2);
}

long long unscaledOf(const confluent::type::Decimal &d) {
    long long n = 0;
    for (unsigned char c : d.value()) n = (n << 8) | c;
    if (!d.value().empty() &&
        (static_cast<unsigned char>(d.value()[0]) & 0x80)) {
        n -= 1LL << (8 * d.value().size());
    }
    return n;
}

std::unique_ptr<parity::C9Containers> containerMsg() {
    auto m = std::make_unique<parity::C9Containers>();
    m->set_label("hi");
    setDec(m->add_amounts(), 111);
    setDec(m->add_amounts(), 222);
    setDec(&(*m->mutable_amount_map())["a"], 333);
    setDec(m->mutable_nested()->mutable_inner(), 444);
    return m;
}

/// Runs one message-level CEL transform and returns the rebuilt message.
parity::C9Containers transform(const std::string &expr) {
    Rule rule;
    rule.setName("r");
    rule.setType("CEL");
    rule.setKind(Kind::Transform);
    rule.setMode(Mode::Write);
    rule.setExpr(expr);

    SerializationContext serCtx{"t", SerdeType::Value, SerdeFormat::Protobuf, std::nullopt};
    std::vector<Rule> rules{rule};
    auto registry = std::make_shared<RuleRegistry>();
    registry->registerExecutor(std::make_shared<CelExecutor>());
    RuleContext ctx(std::nullopt, serCtx, std::nullopt, std::nullopt, "t-value", Mode::Write,
                    rule, 0, rules,
                    std::unordered_map<std::string, std::unordered_set<std::string>>{},
                    nullptr, registry);

    CelExecutor exec;
    auto input = makeProtobufValue(ProtobufVariant(containerMsg()));
    auto result = exec.transform(ctx, *input);
    auto &pv = asProtobuf(*result);
    EXPECT_EQ(pv.type, ProtobufVariant::ValueType::Message);

    // Round-tripped through the wire rather than inspected in place: the defect was in what got
    // written, and reflection on the rebuilt message would report the same either way.
    parity::C9Containers out;
    out.ParseFromString(
        std::get<std::unique_ptr<google::protobuf::Message>>(pv.value)->SerializeAsString());
    return out;
}

const char *kIdentity =
    "{'amounts': message.amounts, 'amount_map': message.amount_map, "
    "'nested': message.nested, 'label': message.label}";

}  // namespace

TEST(CelProtobufContainerWriteBack, IdentityKeepsEveryContainer) {
    parity::C9Containers out = transform(kIdentity);

    ASSERT_EQ(out.amounts_size(), 2);
    EXPECT_EQ(unscaledOf(out.amounts(0)), 111);
    EXPECT_EQ(unscaledOf(out.amounts(1)), 222);

    auto it = out.amount_map().find("a");
    ASSERT_NE(it, out.amount_map().end());
    EXPECT_EQ(unscaledOf(it->second), 333);

    EXPECT_EQ(unscaledOf(out.nested().inner()), 444);
    EXPECT_EQ(out.label(), "hi");
}

// The discriminator for the test above: a *computed* element proves the repeated field was
// written, so "the containers survived" cannot mean "nothing was written at all".
TEST(CelProtobufContainerWriteBack, WritesAComputedRepeatedField) {
    parity::C9Containers out = transform(
        "{'amounts': [decimal('9.99')], 'amount_map': message.amount_map, "
        "'nested': message.nested, 'label': message.label}");

    ASSERT_EQ(out.amounts_size(), 1);
    EXPECT_EQ(unscaledOf(out.amounts(0)), 999);
}

TEST(CelProtobufContainerWriteBack, WritesAComputedMap) {
    parity::C9Containers out = transform(
        "{'amounts': message.amounts, 'amount_map': {'b': decimal('7.77')}, "
        "'nested': message.nested, 'label': message.label}");

    EXPECT_EQ(out.amount_map().find("a"), out.amount_map().end());
    auto it = out.amount_map().find("b");
    ASSERT_NE(it, out.amount_map().end());
    EXPECT_EQ(unscaledOf(it->second), 777);
}

// The third shape, hidden inside the same cell: a nested message the rule *constructs* rather
// than echoes arrives as a CEL map, and used to write nothing.
TEST(CelProtobufContainerWriteBack, WritesAConstructedNestedMessage) {
    parity::C9Containers out = transform(
        "{'amounts': message.amounts, 'amount_map': message.amount_map, "
        "'nested': {'inner': decimal('8.88')}, 'label': message.label}");

    EXPECT_EQ(unscaledOf(out.nested().inner()), 888);
    // And the echoed containers are still intact alongside it.
    ASSERT_EQ(out.amounts_size(), 2);
    EXPECT_NE(out.amount_map().find("a"), out.amount_map().end());
}

// Replace semantics still hold for a container: a field the rule does not name is dropped.
// Without this, every assertion above would also pass on a walk that merged into the input.
TEST(CelProtobufContainerWriteBack, OmittingAContainerLeavesItEmpty) {
    parity::C9Containers out = transform(
        "{'nested': message.nested, 'label': message.label}");

    EXPECT_EQ(out.amounts_size(), 0);
    EXPECT_TRUE(out.amount_map().empty());
    EXPECT_EQ(unscaledOf(out.nested().inner()), 444);
}
