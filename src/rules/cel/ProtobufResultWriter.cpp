/**
 * Rebuilds a protobuf message from the map a message-level `CEL` transform returned.
 * See ProtobufResultWriter.h for the contract.
 */

#include "schemaregistry/rules/cel/ProtobufResultWriter.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#include "absl/status/statusor.h"
#include "google/protobuf/descriptor.h"

namespace schemaregistry::rules::cel::utils {

using schemaregistry::serdes::protobuf::ProtobufVariant;

namespace {

constexpr const char *kTimestampTypeName = "google.protobuf.Timestamp";

/// Resolves a result key to a field by declared name, then by JSON name: a rule may
/// legitimately return either, so matching only the declared name would silently skip a field
/// like `total_amount`.
const google::protobuf::FieldDescriptor *findResultField(
    const google::protobuf::Descriptor *desc, const std::string &name) {
    if (const auto *fd = desc->FindFieldByName(name)) {
        return fd;
    }
    for (int i = 0; i < desc->field_count(); ++i) {
        const auto *candidate = desc->field(i);
        if (candidate->json_name() == name) {
            return candidate;
        }
    }
    return nullptr;
}

/// Fills `out` from a CEL map, one entry per declared field. Defined below; declared here
/// because a constructed nested message sends the walk back through it.
void fillFromCelMap(google::protobuf::Message *out,
                    const google::api::expr::runtime::CelValue &cel_value);

/// Fills a message from whatever shape the runtime handed back for it.
///
/// Three shapes reach here and only the first two used to: a message the rule **echoed** (this
/// client carries a decimal and a variant as proto messages), a CEL **timestamp**, and a
/// **map** - which is what a rule that *constructs* a nested message returns. Without the third,
/// a computed `{"inner": decimal("8.88")}` wrote nothing at all and the field came back at its
/// default, silently.
void fillMessageFromCel(google::protobuf::Message *nested,
                        const google::api::expr::runtime::CelValue &value) {
    if (value.IsMessage() && value.MessageOrDie() != nullptr) {
        const google::protobuf::Message &source = *value.MessageOrDie();
        // CopyFrom requires identical descriptors and ABSL_CHECKs otherwise, which aborts the
        // process - verified with a death test. A CEL map can legitimately hand an unrelated
        // message (a Decimal, say) to a differently-typed message field, so the mismatch is a
        // rule-authoring error to report, which is what the JVM does: its message-level
        // write-back goes through a protobuf JSON parse that rejects the type.
        if (source.GetDescriptor() != nested->GetDescriptor()) {
            throw std::runtime_error(
                "cannot write " + std::string(source.GetDescriptor()->full_name()) +
                " to a field of type " +
                std::string(nested->GetDescriptor()->full_name()));
        }
        nested->CopyFrom(source);
        return;
    }
    if (value.IsTimestamp()) {
        const google::protobuf::Descriptor *nd = nested->GetDescriptor();
        // Only google.protobuf.Timestamp. Any other message was accepted before: one with a
        // differently-typed `seconds` field made SetInt64 fail protobuf's reflection CHECK and
        // abort the process, and one without those fields was written empty and silently
        // wrong. The JVM rejects the mismatch, its write-back going through a protobuf JSON
        // parse.
        if (nd->full_name() != kTimestampTypeName) {
            throw std::runtime_error("cannot write a timestamp to a field of type " +
                                     std::string(nd->full_name()));
        }
        const absl::Time time = value.TimestampOrDie();
        const google::protobuf::Reflection *nr = nested->GetReflection();
        nr->SetInt64(nested, nd->FindFieldByName("seconds"), absl::ToUnixSeconds(time));
        nr->SetInt32(nested, nd->FindFieldByName("nanos"),
                     static_cast<int32_t>(absl::ToInt64Nanoseconds(
                         time - absl::FromUnixSeconds(absl::ToUnixSeconds(time)))));
        return;
    }
    if (value.IsMap()) {
        fillFromCelMap(nested, value);
        return;
    }
    // Anything else falls here *after* the caller materialized the field, so returning
    // quietly left an empty nested message: `{"nested": 1}` looked like it had been applied.
    throw std::runtime_error("cannot write this CEL value to a field of type " +
                             std::string(nested->GetDescriptor()->full_name()));
}

/// Writes one message-valued field from a CEL value.
void setMessageField(google::protobuf::Message *out,
                     const google::protobuf::FieldDescriptor *fd,
                     const google::api::expr::runtime::CelValue &value) {
    fillMessageFromCel(out->GetReflection()->MutableMessage(out, fd), value);
}

/// Where a scalar goes: the field itself, or a new element appended to a repeated one.
///
/// One switch serves both. A second, parallel switch is exactly how the two Avro converters in
/// this client drifted apart - one grew a `variant` arm and the other did not - and
/// a scalar switch has thirteen arms to keep in step rather than one.
struct ScalarSink {
    google::protobuf::Message *msg;
    const google::protobuf::FieldDescriptor *fd;
    bool append;

    const google::protobuf::Reflection *refl() const { return msg->GetReflection(); }

    void setBool(bool v) const {
        if (append) {
            refl()->AddBool(msg, fd, v);
        } else {
            refl()->SetBool(msg, fd, v);
        }
    }
    void setString(const std::string &v) const {
        if (append) {
            refl()->AddString(msg, fd, v);
        } else {
            refl()->SetString(msg, fd, v);
        }
    }
    void setInt32(int32_t v) const {
        if (append) {
            refl()->AddInt32(msg, fd, v);
        } else {
            refl()->SetInt32(msg, fd, v);
        }
    }
    void setInt64(int64_t v) const {
        if (append) {
            refl()->AddInt64(msg, fd, v);
        } else {
            refl()->SetInt64(msg, fd, v);
        }
    }
    void setUInt32(uint32_t v) const {
        if (append) {
            refl()->AddUInt32(msg, fd, v);
        } else {
            refl()->SetUInt32(msg, fd, v);
        }
    }
    void setUInt64(uint64_t v) const {
        if (append) {
            refl()->AddUInt64(msg, fd, v);
        } else {
            refl()->SetUInt64(msg, fd, v);
        }
    }
    void setDouble(double v) const {
        if (append) {
            refl()->AddDouble(msg, fd, v);
        } else {
            refl()->SetDouble(msg, fd, v);
        }
    }
    void setFloat(float v) const {
        if (append) {
            refl()->AddFloat(msg, fd, v);
        } else {
            refl()->SetFloat(msg, fd, v);
        }
    }
    void setEnumValue(int v) const {
        if (append) {
            refl()->AddEnumValue(msg, fd, v);
        } else {
            refl()->SetEnumValue(msg, fd, v);
        }
    }
};

/// The CEL type a value carries, for an error message.
const char *celTypeName(const google::api::expr::runtime::CelValue &value) {
    if (value.IsBool()) return "a bool";
    if (value.IsInt64()) return "an int";
    if (value.IsUint64()) return "a uint";
    if (value.IsDouble()) return "a double";
    if (value.IsString()) return "a string";
    if (value.IsBytes()) return "bytes";
    if (value.IsList()) return "a list";
    if (value.IsMap()) return "a map";
    if (value.IsMessage()) return "a message";
    if (value.IsTimestamp()) return "a timestamp";
    if (value.IsDuration()) return "a duration";
    if (value.IsNull()) return "null";
    return "this value";
}

[[noreturn]] void refuseScalar(const google::protobuf::FieldDescriptor *fd,
                               const google::api::expr::runtime::CelValue &value,
                               const char *kind) {
    throw std::runtime_error("cannot write " + std::string(celTypeName(value)) + " to " +
                             kind + " field " + std::string(fd->full_name()));
}

[[noreturn]] void refuseRange(const google::protobuf::FieldDescriptor *fd,
                              const std::string &shown) {
    throw std::runtime_error("value " + shown + " is out of range for field " +
                             std::string(fd->full_name()));
}

/// A double as an integer, only when it is exactly integral and inside the int64 range. A
/// fractional value is a rule-authoring mistake rather than something to truncate.
///
/// The upper bound is exclusive of 2^63: `int64 max` as a double rounds *up* to 2^63, so
/// comparing against it would admit 2^63 itself, which the cast then makes undefined.
/// `-(int64 min as double)` is exactly 2^63. NaN fails the integral test and an infinity fails
/// the range test.
int64_t exactlyIntegral(const google::protobuf::FieldDescriptor *fd, double d) {
    const double truncated = std::trunc(d);
    if (truncated != d) {
        throw std::runtime_error("cannot write non-integral " + std::to_string(d) +
                                 " to integer field " + std::string(fd->full_name()));
    }
    constexpr double kMin = static_cast<double>(std::numeric_limits<int64_t>::min());
    if (!(truncated >= kMin && truncated < -kMin)) {
        refuseRange(fd, std::to_string(d));
    }
    return static_cast<int64_t>(truncated);
}

/// A CEL value as a signed integer.
int64_t celAsInt(const google::protobuf::FieldDescriptor *fd,
                 const google::api::expr::runtime::CelValue &value) {
    // A bool is checked first because protobuf JSON refuses `true` for an integer field, and
    // it would otherwise be a perfectly good 1.
    if (value.IsBool()) refuseScalar(fd, value, "integer");
    if (value.IsInt64()) return value.Int64OrDie();
    if (value.IsUint64()) {
        const uint64_t u = value.Uint64OrDie();
        if (u > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            refuseRange(fd, std::to_string(u));
        }
        return static_cast<int64_t>(u);
    }
    if (value.IsDouble()) return exactlyIntegral(fd, value.DoubleOrDie());
    refuseScalar(fd, value, "integer");
}

/// A CEL value as an unsigned integer. Kept separate from `celAsInt` because routing an
/// unsigned value through int64 would reject everything above int64 max - half the protobuf
/// uint64 domain, which an identity transform has to round-trip.
uint64_t celAsUint(const google::protobuf::FieldDescriptor *fd,
                   const google::api::expr::runtime::CelValue &value) {
    if (value.IsUint64()) return value.Uint64OrDie();
    const int64_t i = celAsInt(fd, value);
    if (i < 0) {
        refuseRange(fd, std::to_string(i));
    }
    return static_cast<uint64_t>(i);
}

/// A CEL value as a double. A bool is refused rather than written as 1, matching protobuf
/// JSON ("Not a double value: true") and the integer path above.
double celAsDouble(const google::protobuf::FieldDescriptor *fd,
                   const google::api::expr::runtime::CelValue &value) {
    if (value.IsBool()) refuseScalar(fd, value, "float");
    if (value.IsDouble()) return value.DoubleOrDie();
    // An integer for a floating field is a widening, not a coercion, and every other client
    // takes it. Missing this, `{"price": 3}` for a double field silently wrote 0.0.
    if (value.IsInt64()) return static_cast<double>(value.Int64OrDie());
    if (value.IsUint64()) return static_cast<double>(value.Uint64OrDie());
    refuseScalar(fd, value, "float");
}

/// Narrows a double the way `JsonFormat.parseFloat` does: a finite value outside the float
/// range is an error rather than an infinity, with the same 1e-6 slack that method allows.
/// NaN and the infinities pass through - it accepts those explicitly.
float narrowToFloat(const google::protobuf::FieldDescriptor *fd, double d) {
    constexpr double kEpsilon = 1e-6;
    const double limit = static_cast<double>(std::numeric_limits<float>::max()) * (1 + kEpsilon);
    if (std::isfinite(d) && (d > limit || d < -limit)) {
        throw std::runtime_error("out of range float value for field " +
                                 std::string(fd->full_name()) + ": " + std::to_string(d));
    }
    return static_cast<float>(d);
}

int64_t boundedInt(const google::protobuf::FieldDescriptor *fd, int64_t v, int64_t min,
                   int64_t max) {
    if (v < min || v > max) {
        refuseRange(fd, std::to_string(v));
    }
    return v;
}

uint64_t boundedUint(const google::protobuf::FieldDescriptor *fd, uint64_t v, uint64_t max) {
    if (v > max) {
        refuseRange(fd, std::to_string(v));
    }
    return v;
}

/// Writes one scalar, narrowing the CEL value to what the field's type accepts. CEL has one
/// integer type and one floating type, so a narrower field needs converting back - but only
/// where the conversion is exact, and only from a value of the field's own kind.
///
/// Every arm used to be `if (it matches) write it;` with no else and no error path anywhere
/// above, so a wrong-typed value was **silently dropped** and the field came back as its
/// proto3 default. Under replace semantics that is a wrong answer, not a no-op, and two of the
/// cases were ordinary rules rather than mistakes: `{"price": 3}` for a `double` field wrote
/// 0.0 (an int is not `IsDouble()`), and `2.0` for an `int32` wrote 0. Where a value was
/// written, `static_cast` did the narrowing, so 2147483648 became -2147483648.
///
/// The contract is protobuf's own JSON parser, which is what the JVM's write-back parses the
/// result map with. Measured against protobuf-java 4.35.1:
///
///     int32 <- 1.9        REJECT "Not an int32 value: 1.9"
///     int32 <- 2.0        2
///     int32 <- 2147483648 REJECT "Not an int32 value"
///     bool  <- 0          REJECT "Invalid bool value: 0"
///     bytes <- 5          REJECT
///     float <- 1.0e40     REJECT "Out of range float value"
///     double <- 3         3.0
///
/// That parser is also lenient the other way - it stringifies a number into a string field,
/// reads "true"/"false" as a bool and base64-decodes a string into a bytes field - and none of
/// that is followed here. Those coercions exist only because its input crossed a JSON
/// transport, which this writer does not cross, and each one turns a rule-authoring mistake
/// into silently wrong data.
void writeScalar(const ScalarSink &sink,
                 const google::protobuf::FieldDescriptor *fd,
                 const google::api::expr::runtime::CelValue &value) {
    switch (fd->cpp_type()) {
        case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
            if (!value.IsBool()) refuseScalar(fd, value, "bool");
            sink.setBool(value.BoolOrDie());
            return;
        case google::protobuf::FieldDescriptor::CPPTYPE_STRING: {
            // protobuf gives `string` and `bytes` the same C++ type, so the two have to be
            // told apart by `type()`. A CEL string is text and CEL bytes are bytes; neither
            // substitutes for the other. Accepting bytes for a string field produced a string
            // that need not be valid UTF-8, and a string for a bytes field stored the text of
            // a base64 literal rather than the bytes it encodes.
            if (fd->type() == google::protobuf::FieldDescriptor::TYPE_BYTES) {
                if (!value.IsBytes()) refuseScalar(fd, value, "bytes");
                const auto bytes = value.BytesOrDie().value();
                sink.setString(std::string(bytes.begin(), bytes.end()));
                return;
            }
            if (!value.IsString()) refuseScalar(fd, value, "string");
            sink.setString(std::string(value.StringOrDie().value()));
            return;
        }
        case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
            sink.setInt32(static_cast<int32_t>(
                boundedInt(fd, celAsInt(fd, value), std::numeric_limits<int32_t>::min(),
                           std::numeric_limits<int32_t>::max())));
            return;
        case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
            sink.setInt64(celAsInt(fd, value));
            return;
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
            sink.setUInt32(static_cast<uint32_t>(boundedUint(
                fd, celAsUint(fd, value), std::numeric_limits<uint32_t>::max())));
            return;
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
            sink.setUInt64(celAsUint(fd, value));
            return;
        case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
            sink.setDouble(celAsDouble(fd, value));
            return;
        case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
            sink.setFloat(narrowToFloat(fd, celAsDouble(fd, value)));
            return;
        case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
            // This client presents a protobuf enum as its number, not its symbol, so that is
            // what a rule hands back.
            sink.setEnumValue(static_cast<int>(
                boundedInt(fd, celAsInt(fd, value), std::numeric_limits<int32_t>::min(),
                           std::numeric_limits<int32_t>::max())));
            return;
        default:
            refuseScalar(fd, value, "this");
    }
}

/// Writes one scalar field.
void setScalarField(google::protobuf::Message *out,
                    const google::protobuf::FieldDescriptor *fd,
                    const google::api::expr::runtime::CelValue &value) {
    writeScalar(ScalarSink{out, fd, /*append=*/false}, fd, value);
}

/// Writes a repeated field from a CEL list, one appended element per item.
void setRepeatedField(google::protobuf::Message *out,
                      const google::protobuf::FieldDescriptor *fd,
                      const google::api::expr::runtime::CelValue &value) {
    if (!value.IsList()) {
        return;
    }
    const auto *list = value.ListOrDie();
    for (int i = 0; i < list->size(); ++i) {
        auto element = list->Get(nullptr, i);
        if (element.IsError() || element.IsNull()) {
            continue;
        }
        if (fd->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
            fillMessageFromCel(out->GetReflection()->AddMessage(out, fd), element);
        } else {
            writeScalar(ScalarSink{out, fd, /*append=*/true}, fd, element);
        }
    }
}

/// Writes a map field from a CEL map.
///
/// protobuf models a map as a repeated message of a synthesised entry type, so each entry is an
/// added message with its `key` and `value` fields written - which is also why `is_map()` has to
/// be tested *before* `is_repeated()`: a map field answers true to both.
void setMapField(google::protobuf::Message *out,
                 const google::protobuf::FieldDescriptor *fd,
                 const google::api::expr::runtime::CelValue &value) {
    if (!value.IsMap()) {
        return;
    }
    const auto *cel_map = value.MapOrDie();
    auto map_keys = cel_map->ListKeys(nullptr);
    if (!map_keys.ok()) {
        return;
    }
    const google::protobuf::Descriptor *entry = fd->message_type();
    const auto *key_fd = entry->map_key();
    const auto *value_fd = entry->map_value();
    if (key_fd == nullptr || value_fd == nullptr) {
        return;
    }
    const auto *keys_list = map_keys.value();
    for (int i = 0; i < keys_list->size(); ++i) {
        auto key_val = keys_list->Get(nullptr, i);
        if (key_val.IsError()) {
            continue;
        }
        auto lookup = cel_map->Get(nullptr, key_val);
        if (!lookup.has_value() || lookup.value().IsNull()) {
            continue;
        }
        google::protobuf::Message *pair = out->GetReflection()->AddMessage(out, fd);
        writeScalar(ScalarSink{pair, key_fd, /*append=*/false}, key_fd, key_val);
        if (value_fd->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
            fillMessageFromCel(pair->GetReflection()->MutableMessage(pair, value_fd),
                               lookup.value());
        } else {
            writeScalar(ScalarSink{pair, value_fd, /*append=*/false}, value_fd, lookup.value());
        }
    }
}

// Split from messageFromCelMap so that a *constructed* nested message can come back through
// it: fillMessageFromCel calls this when a rule returns a map for a message-valued field.
void fillFromCelMap(google::protobuf::Message *out,
                    const google::api::expr::runtime::CelValue &cel_value) {
    const auto *cel_map = cel_value.MapOrDie();
    const google::protobuf::Descriptor *desc = out->GetDescriptor();

    auto map_keys = cel_map->ListKeys(nullptr);
    if (!map_keys.ok()) {
        return;
    }
    const auto *keys_list = map_keys.value();
    for (int i = 0; i < keys_list->size(); ++i) {
        auto key_val = keys_list->Get(nullptr, i);
        if (key_val.IsError() || !key_val.IsString()) {
            continue;
        }
        const auto *fd = findResultField(desc, std::string(key_val.StringOrDie().value()));
        if (fd == nullptr) {
            // A key the schema does not declare has nowhere to go. Dropping it matches the
            // JVM client, whose JSON parse ignores unknown fields.
            continue;
        }
        auto lookup = cel_map->Get(nullptr, key_val);
        if (!lookup.has_value()) {
            continue;
        }
        const auto &value = lookup.value();
        if (value.IsNull()) {
            // An explicit null clears the field, which is how a rule preserves an absent
            // value across a transform that echoes it.
            out->GetReflection()->ClearField(out, fd);
            continue;
        }
        // A map answers true to is_repeated() as well, so it has to be tested first.
        if (fd->is_map()) {
            setMapField(out, fd, value);
        } else if (fd->is_repeated()) {
            setRepeatedField(out, fd, value);
        } else if (fd->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
            setMessageField(out, fd, value);
        } else {
            setScalarField(out, fd, value);
        }
    }
}

// Contract and mechanism notes live on the declaration in ProtobufResultWriter.h.
}  // namespace

ProtobufVariant messageFromCelMap(
    const google::protobuf::Message &original,
    const google::api::expr::runtime::CelValue &cel_value) {
    std::unique_ptr<google::protobuf::Message> out(original.New());
    fillFromCelMap(out.get(), cel_value);
    return ProtobufVariant(std::move(out));
}

}  // namespace schemaregistry::rules::cel::utils
