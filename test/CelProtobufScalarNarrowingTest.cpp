/**
 * Narrowing a CEL value to a scalar field's type, on the message-level protobuf write-back.
 *
 * Every arm of `writeScalar` was `if (it matches) write it;` with no else and no error path
 * anywhere above it, so a wrong-typed value was **silently dropped** and the field came back
 * as its proto3 default. Under replace semantics that is a wrong answer, not a no-op - and two
 * of the cases were ordinary rules rather than authoring mistakes: `{'dbl': 3}` wrote 0.0,
 * because a CEL integer is not `IsDouble()`, and `{'i32': 2.0}` wrote 0. Where a value *was*
 * written, `static_cast` did the narrowing, so 2147483648 into an int32 became -2147483648 and
 * 1e40 into a float became +Inf.
 *
 * The contract is protobuf's own JSON parser, which is what the JVM's write-back parses the
 * result map with. Measured against protobuf-java 4.35.1:
 *
 *     int32  <- 1.9        REJECT "Not an int32 value: 1.9"
 *     int32  <- 2.0        2
 *     int32  <- 2147483648 REJECT "Not an int32 value"
 *     int32  <- true       REJECT "Not an int32 value: true"
 *     bool   <- 0          REJECT "Invalid bool value: 0"
 *     bytes  <- 5          REJECT
 *     float  <- 1.0e40     REJECT "Out of range float value: 1.0e40"
 *     double <- 3          3.0
 *
 * That parser is also lenient the other way - it stringifies a number into a string field,
 * reads "true"/"false" as a bool and base64-decodes a string into a bytes field - and none of
 * that is followed. Those coercions exist only because its input crossed a JSON transport,
 * which this writer does not cross, and each turns a rule-authoring mistake into wrong data.
 */
#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <memory>
#include <string>

#include "schemaregistry/rules/cel/CelExecutor.h"
#include "schemaregistry/serdes/Serde.h"
#include "schemaregistry/serdes/protobuf/ProtobufTypes.h"
#include "test/parity.pb.h"

using namespace schemaregistry::serdes;
using namespace schemaregistry::serdes::protobuf;
using schemaregistry::rules::cel::CelExecutor;

namespace {

/// Runs one message-level CEL transform over an all-defaults ScalarKinds and returns the
/// rebuilt message. The rule names one field, so replace semantics leave the rest at their
/// proto3 defaults - which is exactly what a silently dropped write looked like.
parity::ScalarKinds transform(const std::string &expr) {
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
    auto input = makeProtobufValue(
        ProtobufVariant(std::make_unique<parity::ScalarKinds>()));
    auto result = exec.transform(ctx, *input);
    auto &pv = asProtobuf(*result);
    EXPECT_EQ(pv.type, ProtobufVariant::ValueType::Message);

    // Round-tripped through the wire rather than inspected in place: the defect was in what
    // got written, and reflection on the rebuilt message reports the same either way.
    parity::ScalarKinds out;
    out.ParseFromString(
        std::get<std::unique_ptr<google::protobuf::Message>>(pv.value)->SerializeAsString());
    return out;
}

}  // namespace

/// An exact conversion is still a conversion, and still happens.
TEST(CelProtobufScalarNarrowing, ExactConversionsAreKept) {
    EXPECT_EQ(transform("{'i32': 2}").i32(), 2);
    EXPECT_EQ(transform("{'i32': 2.0}").i32(), 2);
    // An integer for a floating field: the case that silently wrote 0.0.
    EXPECT_EQ(transform("{'dbl': 3}").dbl(), 3.0);
    EXPECT_EQ(transform("{'dbl': 3u}").dbl(), 3.0);
    EXPECT_EQ(transform("{'flt': 1.5}").flt(), 1.5f);
    EXPECT_TRUE(transform("{'flag': true}").flag());
    EXPECT_EQ(transform("{'text': 'ok'}").text(), "ok");
    EXPECT_EQ(transform("{'blob': b'ab'}").blob(), "ab");
    // The whole uint64 domain round-trips, which is why unsigned narrowing does not route
    // through int64.
    EXPECT_EQ(transform("{'u64': 18446744073709551615u}").u64(),
              std::numeric_limits<uint64_t>::max());
}

/// An inexact or out-of-range number: dropped or wrapped before, an error now.
TEST(CelProtobufScalarNarrowing, InexactOrOutOfRangeNumbersAreRefused) {
    EXPECT_THROW(transform("{'i32': 1.9}"), std::exception);
    EXPECT_THROW(transform("{'i32': 2147483648}"), std::exception);
    EXPECT_THROW(transform("{'i32': -2147483649}"), std::exception);
    EXPECT_THROW(transform("{'u32': -1}"), std::exception);
    EXPECT_THROW(transform("{'flt': 1.0e40}"), std::exception);
    EXPECT_THROW(transform("{'flt': -1.0e40}"), std::exception);
}

/// A value of the wrong kind entirely. Each of these left the field at its default.
TEST(CelProtobufScalarNarrowing, AValueOfTheWrongKindIsRefused) {
    EXPECT_THROW(transform("{'text': 1}"), std::exception);
    EXPECT_THROW(transform("{'text': true}"), std::exception);
    EXPECT_THROW(transform("{'flag': 'false'}"), std::exception);
    EXPECT_THROW(transform("{'flag': 1}"), std::exception);
    EXPECT_THROW(transform("{'blob': 5}"), std::exception);
    EXPECT_THROW(transform("{'i32': true}"), std::exception);
    EXPECT_THROW(transform("{'dbl': true}"), std::exception);
}

/// A `string` and a `bytes` field share one C++ type, so only `type()` tells them apart. Both
/// took either kind before, which stored a base64 literal's *text* in a bytes field.
TEST(CelProtobufScalarNarrowing, StringAndBytesAreNotInterchangeable) {
    EXPECT_EQ(transform("{'blob': b'ab'}").blob(), "ab");
    EXPECT_THROW(transform("{'blob': 'YWI='}"), std::exception);
    EXPECT_THROW(transform("{'text': b'ab'}"), std::exception);
}

/// NaN and the infinities are not range errors: `JsonFormat.parseFloat` accepts those
/// explicitly, and only a *finite* value outside the float range is refused.
TEST(CelProtobufScalarNarrowing, NonFiniteFloatsPassThrough) {
    EXPECT_TRUE(std::isnan(transform("{'flt': 0.0/0.0}").flt()));
    EXPECT_TRUE(std::isinf(transform("{'flt': 1.0/0.0}").flt()));
}
