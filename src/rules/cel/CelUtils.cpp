#include "schemaregistry/rules/cel/CelUtils.h"
#include "schemaregistry/rules/cel/AvroResultWriter.h"
#include "schemaregistry/rules/cel/ProtobufResultWriter.h"

#include <utility>

#include "absl/time/time.h"
#include "confluent/type/decimal.pb.h"
#include "schemaregistry/rules/cel/DecimalUtil.h"
#include "confluent/type/variant.pb.h"
#include "eval/public/containers/container_backed_list_impl.h"
#include "eval/public/containers/container_backed_map_impl.h"
#include "eval/public/structs/cel_proto_wrapper.h"

// Fix for Windows GetMessage macro conflict
// On Windows, GetMessage is defined as a macro in winuser.h which conflicts
// with the GetMessage method in google::protobuf::Reflection
#ifdef _WIN32
#ifdef GetMessage
#undef GetMessage
#endif
#endif

namespace schemaregistry::rules::cel::utils {

google::api::expr::runtime::CelValue fromJsonValue(
    const nlohmann::json &json, google::protobuf::Arena *arena) {
    if (json.is_null())
        return google::api::expr::runtime::CelValue::CreateNull();
    if (json.is_boolean())
        return google::api::expr::runtime::CelValue::CreateBool(
            json.get<bool>());
    if (json.is_number_integer())
        return google::api::expr::runtime::CelValue::CreateInt64(
            json.get<int64_t>());
    if (json.is_number_unsigned())
        return google::api::expr::runtime::CelValue::CreateUint64(
            json.get<uint64_t>());
    if (json.is_number_float())
        return google::api::expr::runtime::CelValue::CreateDouble(
            json.get<double>());
    if (json.is_string()) {
        auto str_value = json.get<std::string>();
        auto *arena_str =
            google::protobuf::Arena::Create<std::string>(arena, str_value);
        return google::api::expr::runtime::CelValue::CreateString(arena_str);
    }
    if (json.is_array()) {
        std::vector<google::api::expr::runtime::CelValue> vec;
        for (const auto &item : json) {
            vec.push_back(fromJsonValue(item, arena));
        }
        auto *list_impl = google::protobuf::Arena::Create<
            google::api::expr::runtime::ContainerBackedListImpl>(arena, vec);
        return google::api::expr::runtime::CelValue::CreateList(list_impl);
    }
    if (json.is_object()) {
        auto *map_impl = google::protobuf::Arena::Create<
            google::api::expr::runtime::CelMapBuilder>(arena);
        for (const auto &[key, value] : json.items()) {
            auto status = map_impl->Add(fromJsonValue(key, arena),
                                        fromJsonValue(value, arena));
            if (!status.ok()) {
            }
        }
        return google::api::expr::runtime::CelValue::CreateMap(map_impl);
    }
    return google::api::expr::runtime::CelValue::CreateNull();
}

nlohmann::json toJsonValue(
    const nlohmann::json &original,
    const google::api::expr::runtime::CelValue &cel_value) {
    if (cel_value.IsBool()) {
        return nlohmann::json(cel_value.BoolOrDie());
    } else if (cel_value.IsInt64()) {
        return nlohmann::json(cel_value.Int64OrDie());
    } else if (cel_value.IsUint64()) {
        return nlohmann::json(cel_value.Uint64OrDie());
    } else if (cel_value.IsDouble()) {
        return nlohmann::json(cel_value.DoubleOrDie());
    } else if (cel_value.IsString()) {
        return nlohmann::json(std::string(cel_value.StringOrDie().value()));
    } else if (cel_value.IsNull()) {
        return nlohmann::json(nullptr);
    } else if (cel_value.IsList()) {
        nlohmann::json json_array = nlohmann::json::array();
        const auto *cel_list = cel_value.ListOrDie();
        for (int i = 0; i < cel_list->size(); ++i) {
            auto item = cel_list->Get(nullptr, i);
            json_array.push_back(toJsonValue(nlohmann::json(), item));
        }
        return json_array;
    } else if (cel_value.IsMap()) {
        nlohmann::json json_object = nlohmann::json::object();
        const auto *cel_map = cel_value.MapOrDie();
        auto map_keys = cel_map->ListKeys(nullptr);
        if (map_keys.ok()) {
            const auto *keys_list = map_keys.value();
            for (int i = 0; i < keys_list->size(); ++i) {
                auto key_val = keys_list->Get(nullptr, i);
                if (key_val.IsString()) {
                    std::string key =
                        std::string(key_val.StringOrDie().value());
                    auto value_lookup = cel_map->Get(nullptr, key_val);
                    if (value_lookup.has_value()) {
                        json_object[key] =
                            toJsonValue(nlohmann::json(), value_lookup.value());
                    }
                }
            }
        }
        return json_object;
    }
    return original;
}

#ifdef SCHEMAREGISTRY_USE_AVRO

namespace {


}  // namespace

google::api::expr::runtime::CelValue fromAvroValue(
    const ::avro::GenericDatum &avro, google::protobuf::Arena *arena) {
    // Logical types are converted to their CEL semantic type so that portable
    // expressions (decimal(this.amount), timestamp(this.ts)) work the same as
    // in the other clients. Decimal -> confluent.type.Decimal message; timestamp
    // -> CEL timestamp. Everything else falls through to the base-type switch.
    switch (avro.logicalType().type()) {
        case ::avro::LogicalType::DECIMAL: {
            std::vector<uint8_t> bytes =
                avro.type() == ::avro::AVRO_FIXED
                    ? avro.value<::avro::GenericFixed>().value()
                    : avro.value<std::vector<uint8_t>>();
            auto *msg =
                google::protobuf::Arena::Create<confluent::type::Decimal>(arena);
            msg->set_value(std::string(bytes.begin(), bytes.end()));
            msg->set_scale(avro.logicalType().scale());
            return google::api::expr::runtime::CelProtoWrapper::CreateMessage(
                msg, arena);
        }
        case ::avro::LogicalType::TIMESTAMP_MILLIS:
            return google::api::expr::runtime::CelValue::CreateTimestamp(
                absl::FromUnixMillis(avro.value<int64_t>()));
        case ::avro::LogicalType::TIMESTAMP_MICROS:
            return google::api::expr::runtime::CelValue::CreateTimestamp(
                absl::FromUnixMicros(avro.value<int64_t>()));
        default:
            break;
    }
    switch (avro.type()) {
        case ::avro::AVRO_BOOL:
            return google::api::expr::runtime::CelValue::CreateBool(
                avro.value<bool>());
        case ::avro::AVRO_INT:
            return google::api::expr::runtime::CelValue::CreateInt64(
                avro.value<int32_t>());
        case ::avro::AVRO_LONG:
            return google::api::expr::runtime::CelValue::CreateInt64(
                avro.value<int64_t>());
        case ::avro::AVRO_FLOAT:
            return google::api::expr::runtime::CelValue::CreateDouble(
                avro.value<float>());
        case ::avro::AVRO_DOUBLE:
            return google::api::expr::runtime::CelValue::CreateDouble(
                avro.value<double>());
        case ::avro::AVRO_STRING: {
            auto *arena_str = google::protobuf::Arena::Create<std::string>(
                arena, avro.value<std::string>());
            return google::api::expr::runtime::CelValue::CreateString(
                arena_str);
        }
        case ::avro::AVRO_BYTES: {
            auto bytes_vec = avro.value<std::vector<uint8_t>>();
            auto *arena_bytes =
                google::protobuf::Arena::Create<std::string>(arena);
            arena_bytes->assign(bytes_vec.begin(), bytes_vec.end());
            return google::api::expr::runtime::CelValue::CreateBytes(
                arena_bytes);
        }
        // A `fixed` is a fixed-width byte string, so it is presented as CEL bytes - exactly like
        // AVRO_BYTES above, and what the Java reference does (GenericFixed -> CelByteString) and
        // Rust does (Fixed -> Value::Bytes). Missing this arm, a bare fixed fell through to the
        // default and compared *false* against a bytes literal rather than erroring, so a rule on
        // a checksum or fixed-width id silently failed its record. Note a fixed carrying the
        // `decimal` logical type was already handled by the logical-type switch above, which is
        // why only the bare case was affected.
        case ::avro::AVRO_FIXED: {
            const auto &fixed_vec = avro.value<::avro::GenericFixed>().value();
            auto *arena_fixed =
                google::protobuf::Arena::Create<std::string>(arena);
            arena_fixed->assign(fixed_vec.begin(), fixed_vec.end());
            return google::api::expr::runtime::CelValue::CreateBytes(
                arena_fixed);
        }
        // An `enum` is presented as its symbol name, matching the Java reference
        // (GenericEnumSymbol -> String) and Rust (Enum -> Value::String). This deliberately
        // differs from a *protobuf* enum, which every client presents as an int: an Avro enum
        // symbol has no ordinal in the data model, only a name. Missing this arm, `this.status ==
        // 'ACTIVE'` was silently false.
        case ::avro::AVRO_ENUM: {
            auto *arena_sym = google::protobuf::Arena::Create<std::string>(
                arena, avro.value<::avro::GenericEnum>().symbol());
            return google::api::expr::runtime::CelValue::CreateString(
                arena_sym);
        }
        case ::avro::AVRO_ARRAY: {
            const auto &arr = avro.value<::avro::GenericArray>().value();
            std::vector<google::api::expr::runtime::CelValue> vec;
            for (const auto &item : arr) {
                vec.push_back(fromAvroValue(item, arena));
            }
            auto *list_impl = google::protobuf::Arena::Create<
                google::api::expr::runtime::ContainerBackedListImpl>(arena,
                                                                     vec);
            return google::api::expr::runtime::CelValue::CreateList(list_impl);
        }
        case ::avro::AVRO_MAP: {
            auto *map_impl = google::protobuf::Arena::Create<
                google::api::expr::runtime::CelMapBuilder>(arena);
            const auto &map = avro.value<::avro::GenericMap>().value();
            for (const auto &pair : map) {
                auto *arena_key = google::protobuf::Arena::Create<std::string>(
                    arena, pair.first);
                auto status = map_impl->Add(
                    google::api::expr::runtime::CelValue::CreateString(
                        arena_key),
                    fromAvroValue(pair.second, arena));
                if (!status.ok()) {
                }
            }
            return google::api::expr::runtime::CelValue::CreateMap(map_impl);
        }
        case ::avro::AVRO_RECORD: {
            const auto &record = avro.value<::avro::GenericRecord>();
            // A confluent.type.Variant record (two bytes fields metadata/value) is
            // surfaced as a confluent.type.Variant proto message, so the variants.*
            // functions see it as a first-class Variant - the counterpart of the
            // decimal logical-type branch above. Recognized by record name, since
            // avro-cpp does not apply a logical type to a record.
            if (record.schema()->name().fullname() == "confluent.type.Variant") {
                std::vector<uint8_t> metadata;
                std::vector<uint8_t> value;
                for (size_t i = 0; i < record.schema()->names(); ++i) {
                    const std::string &fieldName = record.schema()->nameAt(i);
                    if (fieldName == "metadata") {
                        metadata = record.fieldAt(i).value<std::vector<uint8_t>>();
                    } else if (fieldName == "value") {
                        value = record.fieldAt(i).value<std::vector<uint8_t>>();
                    }
                }
                auto *msg = google::protobuf::Arena::Create<confluent::type::Variant>(
                    arena);
                msg->set_metadata(std::string(metadata.begin(), metadata.end()));
                msg->set_value(std::string(value.begin(), value.end()));
                return google::api::expr::runtime::CelProtoWrapper::CreateMessage(
                    msg, arena);
            }
            auto *map_impl = google::protobuf::Arena::Create<
                google::api::expr::runtime::CelMapBuilder>(arena);
            for (size_t i = 0; i < record.schema()->names(); ++i) {
                auto *arena_name = google::protobuf::Arena::Create<std::string>(
                    arena, record.schema()->nameAt(i));
                auto status = map_impl->Add(
                    google::api::expr::runtime::CelValue::CreateString(
                        arena_name),
                    fromAvroValue(record.fieldAt(i), arena));
                if (!status.ok()) {
                }
            }
            return google::api::expr::runtime::CelValue::CreateMap(map_impl);
        }
        case ::avro::AVRO_NULL:
            return google::api::expr::runtime::CelValue::CreateNull();
        default:
            return google::api::expr::runtime::CelValue::CreateNull();
    }
}

namespace {

/// Writes a CEL decimal back into the shape the field's schema declares.
///
/// `fromAvroValue` reads a DECIMAL logical type into a confluent.type.Decimal message, so a
/// rule that computes one hands back a message rather than any primitive CEL type. Without
/// this the value fell through `toAvroValue`'s trailing `return original` and the computed
/// result was silently discarded.
///
/// The computed decimal carries its own scale, which arithmetic may have changed; the Avro
/// schema's scale is fixed. Rescaling to the schema's scale is what the JVM client gets from
/// Avro's own DecimalConversion, which rejects a mismatch rather than truncating - so an
/// inexact rescale is an error here too, not a silent narrowing.
::avro::GenericDatum decimalToAvro(const ::avro::GenericDatum &original,
                                   const google::protobuf::Message &message) {
    const auto *decimal_msg =
        google::protobuf::DynamicCastToGenerated<confluent::type::Decimal>(&message);
    confluent::type::Decimal owned;
    if (decimal_msg == nullptr) {
        // A DynamicMessage of the same type: round-trip through the wire format.
        if (!owned.ParseFromString(message.SerializeAsString())) {
            throw std::runtime_error(
                "cannot read confluent.type.Decimal returned by a CEL rule");
        }
        decimal_msg = &owned;
    }

    decimal::Decimal value = DecimalUtil::fromProto(*decimal_msg);
    const int32_t target_scale = original.logicalType().scale();
    decimal::Context ctx = DecimalUtil::exactContext();
    decimal::Decimal rescaled = value.rescale(-target_scale, ctx);
    if (ctx.status() & (MPD_Inexact | MPD_Invalid_operation)) {
        throw std::runtime_error(
            "decimal result does not fit the field's scale of " +
            std::to_string(target_scale));
    }

    std::string unscaled = DecimalUtil::toProto(rescaled).value();
    std::vector<uint8_t> bytes(unscaled.begin(), unscaled.end());
    if (original.type() == ::avro::AVRO_FIXED) {
        ::avro::GenericDatum result{
            ::avro::ValidSchema(original.value<::avro::GenericFixed>().schema())};
        result.value<::avro::GenericFixed>().value() = bytes;
        return result;
    }
    return ::avro::GenericDatum(bytes);
}

/// Writes a CEL variant back into the confluent.type.Variant record it was read from.
/// The record's own schema is the only place the field layout is available, so it is taken
/// from the original datum - the same reason the enum, fixed and container arms thread it
/// through.
::avro::GenericDatum variantToAvro(const ::avro::GenericDatum &original,
                                   const google::protobuf::Message &message) {
    const auto *variant_msg =
        google::protobuf::DynamicCastToGenerated<confluent::type::Variant>(&message);
    confluent::type::Variant owned;
    if (variant_msg == nullptr) {
        if (!owned.ParseFromString(message.SerializeAsString())) {
            throw std::runtime_error(
                "cannot read confluent.type.Variant returned by a CEL rule");
        }
        variant_msg = &owned;
    }
    if (original.type() != ::avro::AVRO_RECORD) {
        return original;
    }

    ::avro::GenericDatum result{
        ::avro::ValidSchema(original.value<::avro::GenericRecord>().schema())};
    auto &record = result.value<::avro::GenericRecord>();
    const std::string &metadata = variant_msg->metadata();
    const std::string &value = variant_msg->value();
    for (size_t i = 0; i < record.schema()->names(); ++i) {
        const std::string &field_name = record.schema()->nameAt(i);
        if (field_name == "metadata") {
            record.fieldAt(i).value<std::vector<uint8_t>>() =
                std::vector<uint8_t>(metadata.begin(), metadata.end());
        } else if (field_name == "value") {
            record.fieldAt(i).value<std::vector<uint8_t>>() =
                std::vector<uint8_t>(value.begin(), value.end());
        }
    }
    return result;
}

}  // namespace

::avro::GenericDatum toAvroValue(
    const ::avro::GenericDatum &original,
    const google::api::expr::runtime::CelValue &cel_value) {
    // Logical types first, mirroring fromAvroValue: a decimal and a variant are carried as
    // proto messages and a timestamp as a CEL timestamp, none of which any arm below matches.
    // Before this they all reached the trailing `return original` and were discarded.
    if (cel_value.IsMessage()) {
        const google::protobuf::Message *message = cel_value.MessageOrDie();
        if (message != nullptr) {
            const auto name = message->GetDescriptor()->full_name();
            if (name == "confluent.type.Decimal") {
                return decimalToAvro(original, *message);
            }
            if (name == "confluent.type.Variant") {
                return variantToAvro(original, *message);
            }
        }
        return original;
    } else if (cel_value.IsTimestamp()) {
        const absl::Time time = cel_value.TimestampOrDie();
        switch (original.logicalType().type()) {
            case ::avro::LogicalType::TIMESTAMP_MILLIS:
                return ::avro::GenericDatum(absl::ToUnixMillis(time));
            case ::avro::LogicalType::TIMESTAMP_MICROS:
                return ::avro::GenericDatum(absl::ToUnixMicros(time));
            default:
                // A timestamp computed for a field that is not a timestamp logical type has
                // no unit to be written in; leaving the field alone matches the fallback.
                return original;
        }
    }
    if (cel_value.IsBool()) {
        return ::avro::GenericDatum(cel_value.BoolOrDie());
    } else if (cel_value.IsInt64()) {
        return ::avro::GenericDatum(
            static_cast<int64_t>(cel_value.Int64OrDie()));
    } else if (cel_value.IsUint64()) {
        return ::avro::GenericDatum(
            static_cast<int64_t>(cel_value.Uint64OrDie()));
    } else if (cel_value.IsDouble()) {
        return ::avro::GenericDatum(cel_value.DoubleOrDie());
    } else if (cel_value.IsString()) {
        std::string text(cel_value.StringOrDie().value());
        // An enum field is read as its symbol name (see AVRO_ENUM in fromAvroValue), so a rule
        // that returns a string for one has to be written back as a GenericEnum carrying that
        // symbol - a plain string datum does not satisfy an enum schema. The original datum is the
        // only place the enum's schema is available, which is why it is threaded through here, the
        // same way the array/record/map arms below use it.
        if (original.type() == ::avro::AVRO_ENUM) {
            ::avro::GenericDatum result{
                ::avro::ValidSchema(original.value<::avro::GenericEnum>().schema())};
            result.value<::avro::GenericEnum>().set(text);
            return result;
        }
        return ::avro::GenericDatum(text);
    } else if (cel_value.IsBytes()) {
        auto bytes_view = cel_value.BytesOrDie().value();
        std::vector<uint8_t> bytes(bytes_view.begin(), bytes_view.end());
        // A fixed field needs a GenericFixed of its own schema; plain `bytes` takes the vector
        // directly. There was no bytes arm here at all before, so a rule returning bytes for
        // either shape fell through to the fallback and was silently discarded.
        if (original.type() == ::avro::AVRO_FIXED) {
            ::avro::GenericDatum result{
                ::avro::ValidSchema(original.value<::avro::GenericFixed>().schema())};
            result.value<::avro::GenericFixed>().value() = bytes;
            return result;
        }
        return ::avro::GenericDatum(bytes);
    } else if (cel_value.IsNull()) {
        return ::avro::GenericDatum();
    } else if (cel_value.IsList()) {
        const auto *cel_list = cel_value.ListOrDie();

        if (original.type() == ::avro::AVRO_ARRAY) {
            auto orig_array_schema =
                original.value<::avro::GenericArray>().schema();
            ::avro::GenericDatum result_datum{
                ::avro::ValidSchema(orig_array_schema)};
            auto &result_array = result_datum.value<::avro::GenericArray>();

            ::avro::GenericDatum element_template;
            auto &orig_array = original.value<::avro::GenericArray>().value();
            if (!orig_array.empty()) {
                element_template = orig_array[0];
            }

            for (int i = 0; i < cel_list->size(); ++i) {
                auto item = cel_list->Get(nullptr, i);
                if (!item.IsError()) {
                    result_array.value().push_back(
                        toAvroValue(element_template, item));
                }
            }

            return result_datum;
        } else {
            return original;
        }
    } else if (cel_value.IsMap()) {
        const auto *cel_map = cel_value.MapOrDie();

        if (original.type() == ::avro::AVRO_RECORD) {
            return recordFromCelMap(original, *cel_map);
        } else if (original.type() == ::avro::AVRO_MAP) {
            auto orig_map_schema =
                original.value<::avro::GenericMap>().schema();
            ::avro::GenericDatum result_datum{
                ::avro::ValidSchema(orig_map_schema)};
            auto &result_map = result_datum.value<::avro::GenericMap>();

            ::avro::GenericDatum value_template;
            auto &orig_map = original.value<::avro::GenericMap>().value();
            if (!orig_map.empty()) {
                value_template = orig_map.begin()->second;
            }

            auto map_keys = cel_map->ListKeys(nullptr);
            if (map_keys.ok()) {
                const auto *keys_list = map_keys.value();
                for (int i = 0; i < keys_list->size(); ++i) {
                    auto key_val = keys_list->Get(nullptr, i);
                    if (!key_val.IsError() && key_val.IsString()) {
                        std::string key =
                            std::string(key_val.StringOrDie().value());
                        auto value_lookup = cel_map->Get(nullptr, key_val);
                        if (value_lookup.has_value()) {
                            result_map.value().emplace_back(
                                key, toAvroValue(value_template,
                                                 value_lookup.value()));
                        }
                    }
                }
            }

            return result_datum;
        } else {
            return original;
        }
    }

    return original;
}

#endif

namespace {

using schemaregistry::serdes::protobuf::ProtobufVariant;

}  // namespace

schemaregistry::serdes::protobuf::ProtobufVariant toProtobufValue(
    const schemaregistry::serdes::protobuf::ProtobufVariant &original,
    const google::api::expr::runtime::CelValue &cel_value) {
    using namespace schemaregistry::serdes::protobuf;

    // A CEL_FIELD rule over a decimal or timestamp field is handed the whole message and
    // hands back a message or a CEL timestamp - neither of which any arm below matches, so
    // both used to reach the fallback and be written as bytes, which aborts reflection with
    // "Expected CPPTYPE_STRING, field type CPPTYPE_MESSAGE". The counterpart of the arms
    // toAvroValue needs for the same reason.
    if (original.type == ProtobufVariant::ValueType::Message) {
        const auto &orig =
            std::get<std::unique_ptr<google::protobuf::Message>>(original.value);
        if (orig != nullptr && cel_value.IsMessage() &&
            cel_value.MessageOrDie() != nullptr) {
            auto out = std::unique_ptr<google::protobuf::Message>(orig->New());
            const google::protobuf::Message *src = cel_value.MessageOrDie();
            if (src->GetDescriptor()->full_name() ==
                out->GetDescriptor()->full_name()) {
                out->CopyFrom(*src);
                return ProtobufVariant(std::move(out));
            }
            return original;
        }
        if (orig != nullptr && cel_value.IsTimestamp()) {
            const absl::Time time = cel_value.TimestampOrDie();
            auto out = std::unique_ptr<google::protobuf::Message>(orig->New());
            const google::protobuf::Descriptor *desc = out->GetDescriptor();
            const google::protobuf::Reflection *refl = out->GetReflection();
            const int64_t seconds = absl::ToUnixSeconds(time);
            if (const auto *sec = desc->FindFieldByName("seconds")) {
                refl->SetInt64(out.get(), sec, seconds);
            }
            if (const auto *nanos = desc->FindFieldByName("nanos")) {
                refl->SetInt32(out.get(), nanos,
                               static_cast<int32_t>(absl::ToInt64Nanoseconds(
                                   time - absl::FromUnixSeconds(seconds))));
            }
            return ProtobufVariant(std::move(out));
        }
    }

    if (cel_value.IsBool()) {
        return ProtobufVariant(cel_value.BoolOrDie());
    } else if (cel_value.IsInt64()) {
        int64_t value = cel_value.Int64OrDie();
        switch (original.type) {
            case ProtobufVariant::ValueType::I32:
                return ProtobufVariant(static_cast<int32_t>(value),
                                       ProtobufVariant::ValueType::I32);
            case ProtobufVariant::ValueType::I64:
                return ProtobufVariant(value);
            case ProtobufVariant::ValueType::U32:
                return ProtobufVariant(static_cast<uint32_t>(value));
            case ProtobufVariant::ValueType::U64:
                return ProtobufVariant(static_cast<uint64_t>(value));
            case ProtobufVariant::ValueType::EnumNumber:
                return ProtobufVariant::createEnum(static_cast<int32_t>(value));
            default:
                return ProtobufVariant(value);
        }
    } else if (cel_value.IsUint64()) {
        uint64_t value = cel_value.Uint64OrDie();
        switch (original.type) {
            case ProtobufVariant::ValueType::I32:
                return ProtobufVariant(static_cast<int32_t>(value),
                                       ProtobufVariant::ValueType::I32);
            case ProtobufVariant::ValueType::I64:
                return ProtobufVariant(static_cast<int64_t>(value));
            case ProtobufVariant::ValueType::U32:
                return ProtobufVariant(static_cast<uint32_t>(value));
            case ProtobufVariant::ValueType::U64:
                return ProtobufVariant(value);
            case ProtobufVariant::ValueType::EnumNumber:
                return ProtobufVariant::createEnum(static_cast<int32_t>(value));
            default:
                return ProtobufVariant(value);
        }
    } else if (cel_value.IsDouble()) {
        double value = cel_value.DoubleOrDie();
        if (original.type == ProtobufVariant::ValueType::F32) {
            return ProtobufVariant(static_cast<float>(value));
        } else {
            return ProtobufVariant(value);
        }
    } else if (cel_value.IsString()) {
        std::string value = std::string(cel_value.StringOrDie().value());
        return ProtobufVariant(value);
    } else if (cel_value.IsBytes()) {
        auto bytes_view = cel_value.BytesOrDie();
        std::vector<uint8_t> bytes(bytes_view.value().begin(),
                                   bytes_view.value().end());
        return ProtobufVariant(bytes);
    } else if (cel_value.IsList()) {
        const auto *cel_list = cel_value.ListOrDie();
        std::vector<ProtobufVariant> result_list;

        ProtobufVariant element_template(false);
        if (original.type == ProtobufVariant::ValueType::List) {
            const auto &orig_list =
                original.get<std::vector<ProtobufVariant>>();
            if (!orig_list.empty()) {
                element_template = orig_list[0];
            }
        }

        for (int i = 0; i < cel_list->size(); ++i) {
            auto item = cel_list->Get(nullptr, i);
            if (!item.IsError()) {
                result_list.push_back(toProtobufValue(element_template, item));
            }
        }

        return ProtobufVariant(result_list);
    } else if (cel_value.IsMap()) {
        // A message-level transform returns a map that is the whole new message, so rebuild
        // it rather than producing a bare protobuf map - the serializer cannot write one.
        if (original.type == ProtobufVariant::ValueType::Message) {
            const auto &orig_msg =
                original.get<std::unique_ptr<google::protobuf::Message>>();
            if (orig_msg != nullptr) {
                return messageFromCelMap(*orig_msg, cel_value);
            }
        }
        const auto *cel_map = cel_value.MapOrDie();
        std::map<MapKey, ProtobufVariant> result_map;

        ProtobufVariant value_template(false);
        if (original.type == ProtobufVariant::ValueType::Map) {
            const auto &orig_map =
                original.get<std::map<MapKey, ProtobufVariant>>();
            if (!orig_map.empty()) {
                value_template = orig_map.begin()->second;
            }
        }

        auto map_keys = cel_map->ListKeys(nullptr);
        if (map_keys.ok()) {
            const auto *keys_list = map_keys.value();
            for (int i = 0; i < keys_list->size(); ++i) {
                auto key_val = keys_list->Get(nullptr, i);
                if (!key_val.IsError()) {
                    MapKey map_key;
                    if (key_val.IsBool()) {
                        map_key = key_val.BoolOrDie();
                    } else if (key_val.IsInt64()) {
                        map_key = key_val.Int64OrDie();
                    } else if (key_val.IsUint64()) {
                        map_key = key_val.Uint64OrDie();
                    } else if (key_val.IsString()) {
                        map_key = std::string(key_val.StringOrDie().value());
                    } else {
                        map_key = std::string("unknown");
                    }

                    auto value_lookup = cel_map->Get(nullptr, key_val);
                    if (value_lookup.has_value()) {
                        result_map.emplace(
                            map_key, toProtobufValue(value_template,
                                                     value_lookup.value()));
                    }
                }
            }
        }

        return ProtobufVariant(result_map);
    } else if (cel_value.IsNull()) {
        return ProtobufVariant(std::vector<uint8_t>());
    } else {
        return ProtobufVariant(std::vector<uint8_t>());
    }
}

google::api::expr::runtime::CelValue fromProtobufValue(
    const schemaregistry::serdes::protobuf::ProtobufVariant &variant,
    google::protobuf::Arena *arena) {
    using namespace schemaregistry::serdes::protobuf;

    switch (variant.type) {
        case ProtobufVariant::ValueType::Bool:
            return google::api::expr::runtime::CelValue::CreateBool(
                variant.get<bool>());

        case ProtobufVariant::ValueType::I32:
            return google::api::expr::runtime::CelValue::CreateInt64(
                static_cast<int64_t>(variant.get<int32_t>()));

        case ProtobufVariant::ValueType::I64:
            return google::api::expr::runtime::CelValue::CreateInt64(
                variant.get<int64_t>());

        // CEL has a distinct unsigned type; narrowing these to Int would wrap
        // any u64 above int64 max to a negative number, so `this > 0` would
        // reject valid values.
        case ProtobufVariant::ValueType::U32:
            return google::api::expr::runtime::CelValue::CreateUint64(
                static_cast<uint64_t>(variant.get<uint32_t>()));

        case ProtobufVariant::ValueType::U64:
            return google::api::expr::runtime::CelValue::CreateUint64(
                variant.get<uint64_t>());

        case ProtobufVariant::ValueType::F32:
            return google::api::expr::runtime::CelValue::CreateDouble(
                static_cast<double>(variant.get<float>()));

        case ProtobufVariant::ValueType::F64:
            return google::api::expr::runtime::CelValue::CreateDouble(
                variant.get<double>());

        case ProtobufVariant::ValueType::String: {
            const auto &str = variant.get<std::string>();
            auto *arena_str =
                google::protobuf::Arena::Create<std::string>(arena, str);
            return google::api::expr::runtime::CelValue::CreateString(
                arena_str);
        }

        case ProtobufVariant::ValueType::Bytes: {
            const auto &bytes = variant.get<std::vector<uint8_t>>();
            auto *arena_bytes =
                google::protobuf::Arena::Create<std::string>(arena);
            arena_bytes->assign(bytes.begin(), bytes.end());
            return google::api::expr::runtime::CelValue::CreateBytes(
                arena_bytes);
        }

        case ProtobufVariant::ValueType::EnumNumber:
            return google::api::expr::runtime::CelValue::CreateInt64(
                static_cast<int64_t>(variant.get<int32_t>()));

        case ProtobufVariant::ValueType::Message: {
            const auto &msg =
                variant.get<std::unique_ptr<google::protobuf::Message>>();
            if (!msg) {
                return google::api::expr::runtime::CelValue::CreateNull();
            }

            // Hand cel-cpp the message itself rather than a map of its fields.
            // The engine then answers from the descriptor, which is what the other
            // clients' engines do and what protovalidate-cc does:
            //
            //   - has() follows protobuf presence, so an unset field reads as unset
            //     rather than as its default. A map cannot express that: the key has
            //     to be there for `this.count == 0` to resolve, and its being there
            //     is what has() reports.
            //   - a well-known type becomes the value it wraps - a Timestamp a CEL
            //     timestamp, a StringValue a string - which CreateMessage does by
            //     downcasting. Built as a map it stayed a map of seconds and nanos.
            //
            // The copy is arena-allocated because the CelValue holds a bare pointer
            // and has to stay valid for the whole evaluation, outliving this variant.
            auto *owned = msg->New(arena);
            owned->CopyFrom(*msg);
            return google::api::expr::runtime::CelProtoWrapper::CreateMessage(
                owned, arena);
        }

        case ProtobufVariant::ValueType::List: {
            const auto &list = variant.get<std::vector<ProtobufVariant>>();
            std::vector<google::api::expr::runtime::CelValue> vec;

            for (const auto &item : list) {
                vec.push_back(fromProtobufValue(item, arena));
            }

            auto *list_impl = google::protobuf::Arena::Create<
                google::api::expr::runtime::ContainerBackedListImpl>(arena,
                                                                     vec);
            return google::api::expr::runtime::CelValue::CreateList(list_impl);
        }

        case ProtobufVariant::ValueType::Map: {
            const auto &map = variant.get<std::map<MapKey, ProtobufVariant>>();
            auto *map_impl = google::protobuf::Arena::Create<
                google::api::expr::runtime::CelMapBuilder>(arena);

            for (const auto &[key, value] : map) {
                google::api::expr::runtime::CelValue cel_key;
                std::visit(
                    [&](const auto &k) {
                        using T = std::decay_t<decltype(k)>;
                        if constexpr (std::is_same_v<T, std::string>) {
                            auto *arena_str =
                                google::protobuf::Arena::Create<std::string>(
                                    arena, k);
                            cel_key = google::api::expr::runtime::CelValue::
                                CreateString(arena_str);
                        } else if constexpr (std::is_same_v<T, bool>) {
                            cel_key = google::api::expr::runtime::CelValue::
                                CreateBool(k);
                        } else if constexpr (std::is_unsigned_v<T>) {
                            // CEL has a distinct unsigned type, and a map key is
                            // the one value whose type comes from the key field
                            // rather than from the value itself. Narrowing an
                            // unsigned key to int64 wraps anything above int64
                            // max to a negative number, so a rule could neither
                            // index by such a key nor compare it.
                            cel_key = google::api::expr::runtime::CelValue::
                                CreateUint64(static_cast<uint64_t>(k));
                        } else {
                            cel_key = google::api::expr::runtime::CelValue::
                                CreateInt64(static_cast<int64_t>(k));
                        }
                    },
                    key);

                auto cel_value = fromProtobufValue(value, arena);
                auto status = map_impl->Add(cel_key, cel_value);
                if (!status.ok()) {
                }
            }

            return google::api::expr::runtime::CelValue::CreateMap(map_impl);
        }

        default:
            return google::api::expr::runtime::CelValue::CreateNull();
    }
}

google::api::expr::runtime::CelValue convertProtobufFieldToCel(
    const google::protobuf::Message &message,
    const google::protobuf::FieldDescriptor *field,
    const google::protobuf::Reflection *reflection,
    google::protobuf::Arena *arena, int index) {
    switch (field->cpp_type()) {
        case google::protobuf::FieldDescriptor::CPPTYPE_BOOL: {
            bool value = (index >= 0) ? reflection->GetRepeatedBool(
                                            message, field, index)
                                      : reflection->GetBool(message, field);
            return google::api::expr::runtime::CelValue::CreateBool(value);
        }

        case google::protobuf::FieldDescriptor::CPPTYPE_INT32: {
            int32_t value = (index >= 0) ? reflection->GetRepeatedInt32(
                                               message, field, index)
                                         : reflection->GetInt32(message, field);
            return google::api::expr::runtime::CelValue::CreateInt64(
                static_cast<int64_t>(value));
        }

        case google::protobuf::FieldDescriptor::CPPTYPE_INT64: {
            int64_t value = (index >= 0) ? reflection->GetRepeatedInt64(
                                               message, field, index)
                                         : reflection->GetInt64(message, field);
            return google::api::expr::runtime::CelValue::CreateInt64(value);
        }

        // As in fromProtobufValue: unsigned values keep CEL's unsigned type, so
        // that a uint64 above int64 max is not seen as negative.
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT32: {
            uint32_t value =
                (index >= 0)
                    ? reflection->GetRepeatedUInt32(message, field, index)
                    : reflection->GetUInt32(message, field);
            return google::api::expr::runtime::CelValue::CreateUint64(
                static_cast<uint64_t>(value));
        }

        case google::protobuf::FieldDescriptor::CPPTYPE_UINT64: {
            uint64_t value =
                (index >= 0)
                    ? reflection->GetRepeatedUInt64(message, field, index)
                    : reflection->GetUInt64(message, field);
            return google::api::expr::runtime::CelValue::CreateUint64(value);
        }

        case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT: {
            float value = (index >= 0) ? reflection->GetRepeatedFloat(
                                             message, field, index)
                                       : reflection->GetFloat(message, field);
            return google::api::expr::runtime::CelValue::CreateDouble(
                static_cast<double>(value));
        }

        case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE: {
            double value = (index >= 0) ? reflection->GetRepeatedDouble(
                                              message, field, index)
                                        : reflection->GetDouble(message, field);
            return google::api::expr::runtime::CelValue::CreateDouble(value);
        }

        case google::protobuf::FieldDescriptor::CPPTYPE_STRING: {
            std::string value =
                (index >= 0)
                    ? reflection->GetRepeatedString(message, field, index)
                    : reflection->GetString(message, field);
            auto *arena_str =
                google::protobuf::Arena::Create<std::string>(arena, value);

            if (field->type() ==
                google::protobuf::FieldDescriptor::TYPE_BYTES) {
                return google::api::expr::runtime::CelValue::CreateBytes(
                    arena_str);
            } else {
                return google::api::expr::runtime::CelValue::CreateString(
                    arena_str);
            }
        }

        case google::protobuf::FieldDescriptor::CPPTYPE_ENUM: {
            int value = (index >= 0) ? reflection->GetRepeatedEnumValue(
                                           message, field, index)
                                     : reflection->GetEnumValue(message, field);
            return google::api::expr::runtime::CelValue::CreateInt64(
                static_cast<int64_t>(value));
        }

        case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE: {
            const google::protobuf::Message &nested_message =
                (index >= 0)
                    ? reflection->GetRepeatedMessage(message, field, index)
                    : reflection->GetMessage(message, field);

            if (field->is_map()) {
                // Reached only when a caller asks for a single entry; the whole
                // map is built by the message binding below, which passes the
                // containing message.
                return convertProtobufMapToCel(message, field, arena);
            } else {
                auto message_copy = std::unique_ptr<google::protobuf::Message>(
                    nested_message.New());
                message_copy->CopyFrom(nested_message);
                schemaregistry::serdes::protobuf::ProtobufVariant variant(
                    std::move(message_copy));
                return fromProtobufValue(variant, arena);
            }
        }

        default:
            return google::api::expr::runtime::CelValue::CreateNull();
    }
}

google::api::expr::runtime::CelValue convertProtobufMapToCel(
    const google::protobuf::Message &message,
    const google::protobuf::FieldDescriptor *map_field,
    google::protobuf::Arena *arena) {
    auto *map_impl = google::protobuf::Arena::Create<
        google::api::expr::runtime::CelMapBuilder>(arena);

    const auto *reflection = message.GetReflection();
    if (map_field == nullptr || reflection == nullptr ||
        !map_field->is_map()) {
        return google::api::expr::runtime::CelValue::CreateMap(map_impl);
    }

    // A protobuf map is a repeated field of two-field entry messages. Aggregate
    // every entry into a single CEL map, so that `this.scores["foo"]` indexes a
    // map rather than a list of one-entry maps.
    const auto *entry_descriptor = map_field->message_type();
    const auto *key_field = entry_descriptor->map_key();
    const auto *value_field = entry_descriptor->map_value();
    if (key_field == nullptr || value_field == nullptr) {
        return google::api::expr::runtime::CelValue::CreateMap(map_impl);
    }

    int count = reflection->FieldSize(message, map_field);
    for (int i = 0; i < count; ++i) {
        const auto &entry =
            reflection->GetRepeatedMessage(message, map_field, i);
        const auto *entry_reflection = entry.GetReflection();
        auto cel_key = convertProtobufFieldToCel(entry, key_field,
                                                 entry_reflection, arena, -1);
        auto cel_value = convertProtobufFieldToCel(entry, value_field,
                                                   entry_reflection, arena, -1);
        if (!cel_key.IsError() && !cel_value.IsError()) {
            auto status = map_impl->Add(cel_key, cel_value);
            if (!status.ok()) {
            }
        }
    }

    return google::api::expr::runtime::CelValue::CreateMap(map_impl);
}

}  // namespace schemaregistry::rules::cel::utils
