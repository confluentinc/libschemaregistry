#include "schemaregistry/rules/cel/CelUtils.h"

#include <utility>

#include "eval/public/containers/container_backed_list_impl.h"
#include "eval/public/containers/container_backed_map_impl.h"

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

google::api::expr::runtime::CelValue fromAvroValue(
    const ::avro::GenericDatum &avro, google::protobuf::Arena *arena) {
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
            auto *map_impl = google::protobuf::Arena::Create<
                google::api::expr::runtime::CelMapBuilder>(arena);
            const auto &record = avro.value<::avro::GenericRecord>();
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

::avro::GenericDatum toAvroValue(
    const ::avro::GenericDatum &original,
    const google::api::expr::runtime::CelValue &cel_value) {
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
        return ::avro::GenericDatum(
            std::string(cel_value.StringOrDie().value()));
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
            auto orig_record_schema =
                original.value<::avro::GenericRecord>().schema();
            ::avro::GenericDatum result_datum{
                ::avro::ValidSchema(orig_record_schema)};
            auto &result_record = result_datum.value<::avro::GenericRecord>();

            auto &orig_record = original.value<::avro::GenericRecord>();
            for (size_t i = 0; i < orig_record.fieldCount(); ++i) {
                result_record.setFieldAt(i, orig_record.fieldAt(i));
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
                            for (size_t field_idx = 0;
                                 field_idx < orig_record_schema->leaves();
                                 ++field_idx) {
                                if (orig_record_schema->nameAt(field_idx) ==
                                    key) {
                                    auto field_template =
                                        orig_record.fieldAt(field_idx);
                                    result_record.setFieldAt(
                                        field_idx,
                                        toAvroValue(field_template,
                                                    value_lookup.value()));
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            return result_datum;
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

schemaregistry::serdes::protobuf::ProtobufVariant toProtobufValue(
    const schemaregistry::serdes::protobuf::ProtobufVariant &original,
    const google::api::expr::runtime::CelValue &cel_value) {
    using namespace schemaregistry::serdes::protobuf;

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

        case ProtobufVariant::ValueType::U32:
            return google::api::expr::runtime::CelValue::CreateInt64(
                static_cast<int64_t>(variant.get<uint32_t>()));

        case ProtobufVariant::ValueType::U64:
            return google::api::expr::runtime::CelValue::CreateInt64(
                static_cast<int64_t>(variant.get<uint64_t>()));

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

            const auto *descriptor = msg->GetDescriptor();
            if (!descriptor)
                return google::api::expr::runtime::CelValue::CreateNull();
            const auto *reflection = msg->GetReflection();
            if (!reflection)
                return google::api::expr::runtime::CelValue::CreateNull();

            auto *map_impl = google::protobuf::Arena::Create<
                google::api::expr::runtime::CelMapBuilder>(arena);

            std::vector<const google::protobuf::FieldDescriptor *> fields;
            reflection->ListFields(*msg, &fields);

            for (const auto *field : fields) {
                auto *arena_field_name =
                    google::protobuf::Arena::Create<std::string>(arena,
                                                                 field->name());
                auto cel_key =
                    google::api::expr::runtime::CelValue::CreateString(
                        arena_field_name);

                google::api::expr::runtime::CelValue cel_value =
                    google::api::expr::runtime::CelValue::CreateNull();

                if (field->is_repeated()) {
                    std::vector<google::api::expr::runtime::CelValue> vec;
                    int field_size = reflection->FieldSize(*msg, field);

                    for (int i = 0; i < field_size; ++i) {
                        cel_value = convertProtobufFieldToCel(
                            *msg, field, reflection, arena, i);
                        if (!cel_value.IsError()) {
                            vec.push_back(cel_value);
                        }
                    }

                    auto *list_impl = google::protobuf::Arena::Create<
                        google::api::expr::runtime::ContainerBackedListImpl>(
                        arena, vec);
                    cel_value =
                        google::api::expr::runtime::CelValue::CreateList(
                            list_impl);
                } else {
                    cel_value = convertProtobufFieldToCel(
                        *msg, field, reflection, arena, -1);
                }

                if (!cel_value.IsError()) {
                    auto status = map_impl->Add(cel_key, cel_value);
                    if (!status.ok()) {
                    }
                }
            }

            return google::api::expr::runtime::CelValue::CreateMap(map_impl);
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

        case google::protobuf::FieldDescriptor::CPPTYPE_UINT32: {
            uint32_t value =
                (index >= 0)
                    ? reflection->GetRepeatedUInt32(message, field, index)
                    : reflection->GetUInt32(message, field);
            return google::api::expr::runtime::CelValue::CreateInt64(
                static_cast<int64_t>(value));
        }

        case google::protobuf::FieldDescriptor::CPPTYPE_UINT64: {
            uint64_t value =
                (index >= 0)
                    ? reflection->GetRepeatedUInt64(message, field, index)
                    : reflection->GetUInt64(message, field);
            return google::api::expr::runtime::CelValue::CreateInt64(
                static_cast<int64_t>(value));
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
                return convertProtobufMapToCel(nested_message, field, arena);
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
    const google::protobuf::Message &map_entry,
    const google::protobuf::FieldDescriptor * /*map_field*/,
    google::protobuf::Arena *arena) {
    auto *map_impl = google::protobuf::Arena::Create<
        google::api::expr::runtime::CelMapBuilder>(arena);

    const auto *descriptor = map_entry.GetDescriptor();
    const auto *reflection = map_entry.GetReflection();

    if (!descriptor || !reflection) {
        return google::api::expr::runtime::CelValue::CreateMap(map_impl);
    }

    const auto *key_field = descriptor->field(0);
    const auto *value_field = descriptor->field(1);

    if (!key_field || !value_field) {
        return google::api::expr::runtime::CelValue::CreateMap(map_impl);
    }

    auto cel_key =
        convertProtobufFieldToCel(map_entry, key_field, reflection, arena, -1);
    auto cel_value = convertProtobufFieldToCel(map_entry, value_field,
                                               reflection, arena, -1);

    if (!cel_key.IsError() && !cel_value.IsError()) {
        auto status = map_impl->Add(cel_key, cel_value);
        if (!status.ok()) {
        }
    }

    return google::api::expr::runtime::CelValue::CreateMap(map_impl);
}

}  // namespace schemaregistry::rules::cel::utils
