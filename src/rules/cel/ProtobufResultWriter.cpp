/**
 * Rebuilds a protobuf message from the map a message-level `CEL` transform returned.
 * See ProtobufResultWriter.h for the contract.
 */

#include "schemaregistry/rules/cel/ProtobufResultWriter.h"

#include <string>

#include "absl/status/statusor.h"
#include "google/protobuf/descriptor.h"

namespace schemaregistry::rules::cel::utils {

using schemaregistry::serdes::protobuf::ProtobufVariant;

namespace {

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
        nested->CopyFrom(*value.MessageOrDie());
        return;
    }
    if (value.IsTimestamp()) {
        const absl::Time time = value.TimestampOrDie();
        const google::protobuf::Descriptor *nd = nested->GetDescriptor();
        const google::protobuf::Reflection *nr = nested->GetReflection();
        if (const auto *sec = nd->FindFieldByName("seconds")) {
            nr->SetInt64(nested, sec, absl::ToUnixSeconds(time));
        }
        if (const auto *nanos = nd->FindFieldByName("nanos")) {
            nr->SetInt32(nested, nanos,
                         static_cast<int32_t>(absl::ToInt64Nanoseconds(
                             time - absl::FromUnixSeconds(absl::ToUnixSeconds(time)))));
        }
        return;
    }
    if (value.IsMap()) {
        fillFromCelMap(nested, value);
    }
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

/// Writes one scalar, narrowing the CEL value to what the field's type accepts. CEL has
/// one integer type, so a narrower field needs converting back rather than rejecting.
void writeScalar(const ScalarSink &sink,
                 const google::protobuf::FieldDescriptor *fd,
                 const google::api::expr::runtime::CelValue &value) {
    switch (fd->cpp_type()) {
        case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
            if (value.IsBool()) sink.setBool(value.BoolOrDie());
            return;
        case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
            if (value.IsString()) {
                sink.setString(std::string(value.StringOrDie().value()));
            } else if (value.IsBytes()) {
                auto b = value.BytesOrDie().value();
                sink.setString(std::string(b.begin(), b.end()));
            }
            return;
        case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
            if (value.IsInt64()) {
                sink.setInt32(static_cast<int32_t>(value.Int64OrDie()));
            }
            return;
        case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
            if (value.IsInt64()) sink.setInt64(value.Int64OrDie());
            return;
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
            if (value.IsUint64()) {
                sink.setUInt32(static_cast<uint32_t>(value.Uint64OrDie()));
            } else if (value.IsInt64()) {
                sink.setUInt32(static_cast<uint32_t>(value.Int64OrDie()));
            }
            return;
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
            if (value.IsUint64()) {
                sink.setUInt64(value.Uint64OrDie());
            } else if (value.IsInt64()) {
                sink.setUInt64(static_cast<uint64_t>(value.Int64OrDie()));
            }
            return;
        case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
            if (value.IsDouble()) sink.setDouble(value.DoubleOrDie());
            return;
        case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
            if (value.IsDouble()) {
                sink.setFloat(static_cast<float>(value.DoubleOrDie()));
            }
            return;
        case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
            if (value.IsInt64()) {
                sink.setEnumValue(static_cast<int>(value.Int64OrDie()));
            }
            return;
        default:
            return;
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
