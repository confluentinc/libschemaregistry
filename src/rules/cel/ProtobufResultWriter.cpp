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

/// Writes one message-valued field from a CEL value, inverting how it was read: this client
/// carries a decimal and a variant as proto messages and a timestamp as a CEL timestamp.
void setMessageField(google::protobuf::Message *out,
                     const google::protobuf::FieldDescriptor *fd,
                     const google::api::expr::runtime::CelValue &value) {
    const google::protobuf::Reflection *refl = out->GetReflection();
    if (value.IsMessage() && value.MessageOrDie() != nullptr) {
        refl->MutableMessage(out, fd)->CopyFrom(*value.MessageOrDie());
        return;
    }
    if (value.IsTimestamp()) {
        const absl::Time time = value.TimestampOrDie();
        google::protobuf::Message *nested = refl->MutableMessage(out, fd);
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
}

/// Writes one scalar field, narrowing the CEL value to what the field's type accepts. CEL has
/// one integer type, so a narrower field needs converting back rather than rejecting.
void setScalarField(google::protobuf::Message *out,
                    const google::protobuf::FieldDescriptor *fd,
                    const google::api::expr::runtime::CelValue &value) {
    const google::protobuf::Reflection *refl = out->GetReflection();
    switch (fd->cpp_type()) {
        case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
            if (value.IsBool()) refl->SetBool(out, fd, value.BoolOrDie());
            return;
        case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
            if (value.IsString()) {
                refl->SetString(out, fd, std::string(value.StringOrDie().value()));
            } else if (value.IsBytes()) {
                auto b = value.BytesOrDie().value();
                refl->SetString(out, fd, std::string(b.begin(), b.end()));
            }
            return;
        case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
            if (value.IsInt64()) {
                refl->SetInt32(out, fd, static_cast<int32_t>(value.Int64OrDie()));
            }
            return;
        case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
            if (value.IsInt64()) refl->SetInt64(out, fd, value.Int64OrDie());
            return;
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
            if (value.IsUint64()) {
                refl->SetUInt32(out, fd, static_cast<uint32_t>(value.Uint64OrDie()));
            } else if (value.IsInt64()) {
                refl->SetUInt32(out, fd, static_cast<uint32_t>(value.Int64OrDie()));
            }
            return;
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
            if (value.IsUint64()) {
                refl->SetUInt64(out, fd, value.Uint64OrDie());
            } else if (value.IsInt64()) {
                refl->SetUInt64(out, fd, static_cast<uint64_t>(value.Int64OrDie()));
            }
            return;
        case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
            if (value.IsDouble()) refl->SetDouble(out, fd, value.DoubleOrDie());
            return;
        case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
            if (value.IsDouble()) {
                refl->SetFloat(out, fd, static_cast<float>(value.DoubleOrDie()));
            }
            return;
        case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
            if (value.IsInt64()) {
                refl->SetEnumValue(out, fd, static_cast<int>(value.Int64OrDie()));
            }
            return;
        default:
            return;
    }
}

// Contract and mechanism notes live on the declaration in ProtobufResultWriter.h.
}  // namespace

ProtobufVariant messageFromCelMap(
    const google::protobuf::Message &original,
    const google::api::expr::runtime::CelValue &cel_value) {
    const auto *cel_map = cel_value.MapOrDie();
    std::unique_ptr<google::protobuf::Message> out(original.New());
    const google::protobuf::Descriptor *desc = out->GetDescriptor();

    auto map_keys = cel_map->ListKeys(nullptr);
    if (!map_keys.ok()) {
        return ProtobufVariant(std::move(out));
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
            out->GetReflection()->ClearField(out.get(), fd);
            continue;
        }
        if (fd->is_repeated()) {
            // Repeated and map fields are not reached by the value-type work; leaving them
            // alone is safer than writing a partially-understood shape.
            continue;
        }
        if (fd->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
            setMessageField(out.get(), fd, value);
        } else {
            setScalarField(out.get(), fd, value);
        }
    }
    return ProtobufVariant(std::move(out));
}

}  // namespace schemaregistry::rules::cel::utils
