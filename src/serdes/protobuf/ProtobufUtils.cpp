#include "schemaregistry/serdes/protobuf/ProtobufUtils.h"

#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/util/json_util.h>

#include <memory>  // For std::dynamic_pointer_cast

#include "absl/strings/escaping.h"
#include "confluent/meta.pb.h"
#include "schemaregistry/serdes/RuleRegistry.h"  // For global_registry functions

// Fix for Windows GetMessage macro conflict
// On Windows, GetMessage is defined as a macro in winuser.h which conflicts
// with the GetMessage method in google::protobuf::Reflection
#ifdef _WIN32
#ifdef GetMessage
#undef GetMessage
#endif
#endif

namespace schemaregistry::serdes::protobuf::utils {

// Base64 encoding/decoding utilities using absl
namespace {
std::string base64_encode(const std::vector<uint8_t>& bytes) {
    std::string input(reinterpret_cast<const char*>(bytes.data()),
                      bytes.size());
    return absl::Base64Escape(input);
}

std::vector<uint8_t> base64_decode(const std::string& encoded_string) {
    std::string decoded;
    if (!absl::Base64Unescape(encoded_string, &decoded)) {
        // Return empty vector on decode failure
        return std::vector<uint8_t>();
    }
    return std::vector<uint8_t>(decoded.begin(), decoded.end());
}
}  // namespace

std::unique_ptr<SerdeValue> transformFields(
    RuleContext& ctx, const google::protobuf::Descriptor* descriptor,
    const SerdeValue& value) {
    // Check if we have a protobuf schema and value
    if (value.getFormat() == SerdeFormat::Protobuf) {
        auto& proto_variant = asProtobuf(value);
        if (proto_variant.type != schemaregistry::serdes::protobuf::
                                      ProtobufVariant::ValueType::Message) {
            throw ProtobufError(
                "Expected message variant but got different type");
        }
        auto& message =
            *proto_variant
                 .template get<std::unique_ptr<google::protobuf::Message>>();
        auto message_ptr = const_cast<google::protobuf::Message*>(&message);
        if (message_ptr) {
            if (!descriptor) {
                throw ProtobufError("Message descriptor not found for " +
                                    message_ptr->GetTypeName());
            }

            // Transform the message using the synchronous method
            auto message_ptr_copy =
                std::unique_ptr<google::protobuf::Message>(message_ptr->New());
            message_ptr_copy->CopyFrom(*message_ptr);
            ProtobufVariant message_variant(std::move(message_ptr_copy));
            auto transformed_message =
                transformRecursive(ctx, descriptor, message_variant);

            // Extract the transformed message and create SerdeValue
            if (transformed_message.type ==
                    ProtobufVariant::ValueType::Message &&
                transformed_message
                    .is<std::unique_ptr<google::protobuf::Message>>()) {
                return protobuf::makeProtobufValue(transformed_message);
            }

            // Fallback: return original message
            auto fallback_msg =
                std::unique_ptr<google::protobuf::Message>(message_ptr->New());
            fallback_msg->CopyFrom(*message_ptr);
            return protobuf::makeProtobufValue(
                ProtobufVariant(std::move(fallback_msg)));
        }
    }
    return value.clone();
}

ProtobufVariant transformRecursive(
    RuleContext& ctx, const google::protobuf::Descriptor* descriptor,
    const ProtobufVariant& message) {
    switch (message.type) {
        case ProtobufVariant::ValueType::List: {
            // Handle List
            const auto& list =
                std::get<std::vector<ProtobufVariant>>(message.value);
            std::vector<ProtobufVariant> result;
            result.reserve(list.size());
            for (const auto& item : list) {
                result.push_back(transformRecursive(ctx, descriptor, item));
            }
            return ProtobufVariant(result);
        }
        case ProtobufVariant::ValueType::Map: {
            // Handle Map
            const auto& map =
                std::get<std::map<MapKey, ProtobufVariant>>(message.value);
            std::map<MapKey, ProtobufVariant> result;
            for (const auto& [key, value] : map) {
                result.emplace(key, transformRecursive(ctx, descriptor, value));
            }
            return ProtobufVariant(result);
        }
        case ProtobufVariant::ValueType::Message: {
            // Handle Message
            const auto& msg_ptr =
                std::get<std::unique_ptr<google::protobuf::Message>>(
                    message.value);
            if (!msg_ptr) {
                return message;  // Return original if null
            }

            auto result =
                std::unique_ptr<google::protobuf::Message>(msg_ptr->New());
            result->CopyFrom(*msg_ptr);

            for (int i = 0; i < descriptor->field_count(); ++i) {
                const google::protobuf::FieldDescriptor* fd =
                    descriptor->field(i);
                auto field = transformFieldWithContext(ctx, fd, descriptor,
                                                       result.get());
                if (field.has_value()) {
                    // Set the field in the message based on the transformed
                    // value
                    setMessageField(result.get(), fd, field.value());
                }
            }
            return ProtobufVariant(std::move(result));
        }
        default: {
            // Handle primitive types (Bool, I32, I64, U32, U64, F32, F64,
            // String, Bytes, EnumNumber) Field-level transformation logic
            auto field_ctx = ctx.currentField();
            if (field_ctx.has_value()) {
                auto rule_tags = ctx.getRule().getTags();
                std::unordered_set<std::string> rule_tags_set;
                if (rule_tags.has_value()) {
                    rule_tags_set = std::unordered_set<std::string>(
                        rule_tags->begin(), rule_tags->end());
                }

                // Check if rule tags overlap with field context tags (empty
                // rule_tags means apply to all)
                bool should_apply =
                    !rule_tags.has_value() || rule_tags_set.empty();
                if (!should_apply) {
                    const auto& field_tags = field_ctx->getTags();
                    for (const auto& field_tag : field_tags) {
                        if (rule_tags_set.find(field_tag) !=
                            rule_tags_set.end()) {
                            should_apply = true;
                            break;
                        }
                    }
                }

                if (should_apply) {
                    // Create a SerdeValue from the current ProtobufVariant
                    auto message_value = makeProtobufValue(message);

                    // Get field executor type from the rule
                    auto field_executor_type =
                        ctx.getRule().getType().value_or("");

                    // Try to get executor from context's rule registry first,
                    // then global
                    std::shared_ptr<RuleExecutor> executor;
                    if (ctx.getRuleRegistry()) {
                        executor = ctx.getRuleRegistry()->getExecutor(
                            field_executor_type);
                    }
                    if (!executor) {
                        executor = global_registry::getRuleExecutor(
                            field_executor_type);
                    }

                    if (executor) {
                        auto field_executor =
                            std::dynamic_pointer_cast<FieldRuleExecutor>(
                                executor);
                        if (!field_executor) {
                            throw ProtobufError(
                                "executor " + field_executor_type +
                                " is not a field rule executor");
                        }

                        auto new_value =
                            field_executor->transformField(ctx, *message_value);
                        if (new_value &&
                            new_value->getFormat() == SerdeFormat::Protobuf) {
                            return asProtobuf(*new_value);
                        }
                    }
                }
            }
            return message;
        }
    }
}

std::optional<ProtobufVariant> transformFieldWithContext(
    RuleContext& ctx, const google::protobuf::FieldDescriptor* fd,
    const google::protobuf::Descriptor* desc,
    const google::protobuf::Message* message) {
    auto temp_message =
        std::unique_ptr<google::protobuf::Message>(message->New());
    temp_message->CopyFrom(*message);
    auto temp_serde_value =
        protobuf::makeProtobufValue(ProtobufVariant(std::move(temp_message)));

    ctx.enterField(*temp_serde_value, fd->full_name(), fd->name(),
                   getFieldType(fd), getInlineTags(fd));

    if (fd->containing_oneof() &&
        !message->GetReflection()->HasField(*message, fd)) {
        // Skip oneof fields that are not set
        ctx.exitField();
        return std::nullopt;
    }

    try {
        ProtobufVariant value = getMessageFieldValue(message, fd);
        ProtobufVariant new_value = transformRecursive(ctx, desc, value);

        // Check for condition rules
        auto rule_kind = ctx.getRule().getKind();
        if (rule_kind.has_value() && rule_kind.value() == Kind::Condition) {
            if (new_value.type == ProtobufVariant::ValueType::Bool) {
                bool condition_result = new_value.get<bool>();
                if (!condition_result) {
                    throw ProtobufError("Rule condition failed for field: " +
                                        fd->name());
                }
            }
        }

        ctx.exitField();
        return new_value;
    } catch (const std::exception& e) {
        ctx.exitField();
        throw;
    }
}

// Helper function to extract field value from protobuf message
ProtobufVariant getMessageFieldValue(
    const google::protobuf::Message* message,
    const google::protobuf::FieldDescriptor* fd) {
    const google::protobuf::Reflection* reflection = message->GetReflection();

    if (fd->is_repeated()) {
        std::vector<ProtobufVariant> list_values;
        int field_size = reflection->FieldSize(*message, fd);

        for (int i = 0; i < field_size; ++i) {
            switch (fd->type()) {
                case google::protobuf::FieldDescriptor::TYPE_BOOL:
                    list_values.emplace_back(
                        reflection->GetRepeatedBool(*message, fd, i));
                    break;
                case google::protobuf::FieldDescriptor::TYPE_INT32:
                case google::protobuf::FieldDescriptor::TYPE_SINT32:
                case google::protobuf::FieldDescriptor::TYPE_SFIXED32:
                    list_values.emplace_back(
                        reflection->GetRepeatedInt32(*message, fd, i));
                    break;
                case google::protobuf::FieldDescriptor::TYPE_INT64:
                case google::protobuf::FieldDescriptor::TYPE_SINT64:
                case google::protobuf::FieldDescriptor::TYPE_SFIXED64:
                    list_values.emplace_back(
                        reflection->GetRepeatedInt64(*message, fd, i));
                    break;
                case google::protobuf::FieldDescriptor::TYPE_UINT32:
                case google::protobuf::FieldDescriptor::TYPE_FIXED32:
                    list_values.emplace_back(
                        reflection->GetRepeatedUInt32(*message, fd, i));
                    break;
                case google::protobuf::FieldDescriptor::TYPE_UINT64:
                case google::protobuf::FieldDescriptor::TYPE_FIXED64:
                    list_values.emplace_back(
                        reflection->GetRepeatedUInt64(*message, fd, i));
                    break;
                case google::protobuf::FieldDescriptor::TYPE_FLOAT:
                    list_values.emplace_back(
                        reflection->GetRepeatedFloat(*message, fd, i));
                    break;
                case google::protobuf::FieldDescriptor::TYPE_DOUBLE:
                    list_values.emplace_back(
                        reflection->GetRepeatedDouble(*message, fd, i));
                    break;
                case google::protobuf::FieldDescriptor::TYPE_STRING:
                    list_values.emplace_back(
                        reflection->GetRepeatedString(*message, fd, i));
                    break;
                case google::protobuf::FieldDescriptor::TYPE_BYTES: {
                    std::string bytes_str =
                        reflection->GetRepeatedString(*message, fd, i);
                    std::vector<uint8_t> bytes(bytes_str.begin(),
                                               bytes_str.end());
                    list_values.emplace_back(bytes);
                    break;
                }
                case google::protobuf::FieldDescriptor::TYPE_ENUM:
                    list_values.emplace_back(ProtobufVariant::createEnum(
                        reflection->GetRepeatedEnumValue(*message, fd, i)));
                    break;
                case google::protobuf::FieldDescriptor::TYPE_MESSAGE: {
                    const google::protobuf::Message& sub_message =
                        reflection->GetRepeatedMessage(*message, fd, i);
                    auto msg_copy = std::unique_ptr<google::protobuf::Message>(
                        sub_message.New());
                    msg_copy->CopyFrom(sub_message);
                    list_values.emplace_back(std::move(msg_copy));
                    break;
                }
                case google::protobuf::FieldDescriptor::TYPE_GROUP: {
                    // Handle deprecated GROUP type as message
                    const google::protobuf::Message& sub_message =
                        reflection->GetRepeatedMessage(*message, fd, i);
                    auto msg_copy = std::unique_ptr<google::protobuf::Message>(
                        sub_message.New());
                    msg_copy->CopyFrom(sub_message);
                    list_values.emplace_back(std::move(msg_copy));
                    break;
                }
            }
        }
        return ProtobufVariant(list_values);
    } else {
        // Handle singular fields
        switch (fd->type()) {
            case google::protobuf::FieldDescriptor::TYPE_BOOL:
                return ProtobufVariant(reflection->GetBool(*message, fd));
            case google::protobuf::FieldDescriptor::TYPE_INT32:
            case google::protobuf::FieldDescriptor::TYPE_SINT32:
            case google::protobuf::FieldDescriptor::TYPE_SFIXED32:
                return ProtobufVariant(reflection->GetInt32(*message, fd));
            case google::protobuf::FieldDescriptor::TYPE_INT64:
            case google::protobuf::FieldDescriptor::TYPE_SINT64:
            case google::protobuf::FieldDescriptor::TYPE_SFIXED64:
                return ProtobufVariant(reflection->GetInt64(*message, fd));
            case google::protobuf::FieldDescriptor::TYPE_UINT32:
            case google::protobuf::FieldDescriptor::TYPE_FIXED32:
                return ProtobufVariant(reflection->GetUInt32(*message, fd));
            case google::protobuf::FieldDescriptor::TYPE_UINT64:
            case google::protobuf::FieldDescriptor::TYPE_FIXED64:
                return ProtobufVariant(reflection->GetUInt64(*message, fd));
            case google::protobuf::FieldDescriptor::TYPE_FLOAT:
                return ProtobufVariant(reflection->GetFloat(*message, fd));
            case google::protobuf::FieldDescriptor::TYPE_DOUBLE:
                return ProtobufVariant(reflection->GetDouble(*message, fd));
            case google::protobuf::FieldDescriptor::TYPE_STRING:
                return ProtobufVariant(reflection->GetString(*message, fd));
            case google::protobuf::FieldDescriptor::TYPE_BYTES: {
                std::string bytes_str = reflection->GetString(*message, fd);
                std::vector<uint8_t> bytes(bytes_str.begin(), bytes_str.end());
                return ProtobufVariant(bytes);
            }
            case google::protobuf::FieldDescriptor::TYPE_ENUM:
                return ProtobufVariant::createEnum(
                    reflection->GetEnumValue(*message, fd));
            case google::protobuf::FieldDescriptor::TYPE_MESSAGE: {
                const google::protobuf::Message& sub_message =
                    reflection->GetMessage(*message, fd);
                auto msg_copy = std::unique_ptr<google::protobuf::Message>(
                    sub_message.New());
                msg_copy->CopyFrom(sub_message);
                return ProtobufVariant(std::move(msg_copy));
            }
            case google::protobuf::FieldDescriptor::TYPE_GROUP:
                // Handle deprecated GROUP type as message
                {
                    const google::protobuf::Message& sub_message =
                        reflection->GetMessage(*message, fd);
                    auto msg_copy = std::unique_ptr<google::protobuf::Message>(
                        sub_message.New());
                    msg_copy->CopyFrom(sub_message);
                    return ProtobufVariant(std::move(msg_copy));
                }
            default:
                return ProtobufVariant(std::string(""));  // Default fallback
        }
    }
}

// Helper function to set field value in protobuf message
void setMessageField(google::protobuf::Message* message,
                     const google::protobuf::FieldDescriptor* fd,
                     const ProtobufVariant& value) {
    const google::protobuf::Reflection* reflection = message->GetReflection();

    switch (value.type) {
        case ProtobufVariant::ValueType::Bool: {
            const auto& v = std::get<bool>(value.value);
            reflection->SetBool(message, fd, v);
            break;
        }
        case ProtobufVariant::ValueType::I32: {
            const auto& v = std::get<int32_t>(value.value);
            reflection->SetInt32(message, fd, v);
            break;
        }
        case ProtobufVariant::ValueType::EnumNumber: {
            const auto& v = std::get<int32_t>(value.value);
            reflection->SetEnumValue(message, fd, v);
            break;
        }
        case ProtobufVariant::ValueType::I64: {
            const auto& v = std::get<int64_t>(value.value);
            reflection->SetInt64(message, fd, v);
            break;
        }
        case ProtobufVariant::ValueType::U32: {
            const auto& v = std::get<uint32_t>(value.value);
            reflection->SetUInt32(message, fd, v);
            break;
        }
        case ProtobufVariant::ValueType::U64: {
            const auto& v = std::get<uint64_t>(value.value);
            reflection->SetUInt64(message, fd, v);
            break;
        }
        case ProtobufVariant::ValueType::F32: {
            const auto& v = std::get<float>(value.value);
            reflection->SetFloat(message, fd, v);
            break;
        }
        case ProtobufVariant::ValueType::F64: {
            const auto& v = std::get<double>(value.value);
            reflection->SetDouble(message, fd, v);
            break;
        }
        case ProtobufVariant::ValueType::String: {
            const auto& v = std::get<std::string>(value.value);
            reflection->SetString(message, fd, v);
            break;
        }
        case ProtobufVariant::ValueType::Bytes: {
            const auto& v = std::get<std::vector<uint8_t>>(value.value);
            std::string bytes_str(v.begin(), v.end());
            reflection->SetString(message, fd, bytes_str);
            break;
        }
        case ProtobufVariant::ValueType::Message: {
            const auto& v =
                std::get<std::unique_ptr<google::protobuf::Message>>(
                    value.value);
            if (v) {
                google::protobuf::Message* mutable_msg =
                    reflection->MutableMessage(message, fd);
                mutable_msg->CopyFrom(*v);
            }
            break;
        }
        case ProtobufVariant::ValueType::List: {
            const auto& list =
                std::get<std::vector<ProtobufVariant>>(value.value);

            // Clear the repeated field first
            reflection->ClearField(message, fd);

            // Add each element to the repeated field
            for (const auto& item : list) {
                switch (fd->type()) {
                    case google::protobuf::FieldDescriptor::TYPE_BOOL:
                        if (item.type == ProtobufVariant::ValueType::Bool) {
                            reflection->AddBool(message, fd,
                                                std::get<bool>(item.value));
                        }
                        break;
                    case google::protobuf::FieldDescriptor::TYPE_INT32:
                    case google::protobuf::FieldDescriptor::TYPE_SINT32:
                    case google::protobuf::FieldDescriptor::TYPE_SFIXED32:
                        if (item.type == ProtobufVariant::ValueType::I32) {
                            reflection->AddInt32(message, fd,
                                                 std::get<int32_t>(item.value));
                        }
                        break;
                    case google::protobuf::FieldDescriptor::TYPE_INT64:
                    case google::protobuf::FieldDescriptor::TYPE_SINT64:
                    case google::protobuf::FieldDescriptor::TYPE_SFIXED64:
                        if (item.type == ProtobufVariant::ValueType::I64) {
                            reflection->AddInt64(message, fd,
                                                 std::get<int64_t>(item.value));
                        }
                        break;
                    case google::protobuf::FieldDescriptor::TYPE_UINT32:
                    case google::protobuf::FieldDescriptor::TYPE_FIXED32:
                        if (item.type == ProtobufVariant::ValueType::U32) {
                            reflection->AddUInt32(
                                message, fd, std::get<uint32_t>(item.value));
                        }
                        break;
                    case google::protobuf::FieldDescriptor::TYPE_UINT64:
                    case google::protobuf::FieldDescriptor::TYPE_FIXED64:
                        if (item.type == ProtobufVariant::ValueType::U64) {
                            reflection->AddUInt64(
                                message, fd, std::get<uint64_t>(item.value));
                        }
                        break;
                    case google::protobuf::FieldDescriptor::TYPE_FLOAT:
                        if (item.type == ProtobufVariant::ValueType::F32) {
                            reflection->AddFloat(message, fd,
                                                 std::get<float>(item.value));
                        }
                        break;
                    case google::protobuf::FieldDescriptor::TYPE_DOUBLE:
                        if (item.type == ProtobufVariant::ValueType::F64) {
                            reflection->AddDouble(message, fd,
                                                  std::get<double>(item.value));
                        }
                        break;
                    case google::protobuf::FieldDescriptor::TYPE_STRING:
                        if (item.type == ProtobufVariant::ValueType::String) {
                            reflection->AddString(
                                message, fd, std::get<std::string>(item.value));
                        }
                        break;
                    case google::protobuf::FieldDescriptor::TYPE_BYTES:
                        if (item.type == ProtobufVariant::ValueType::Bytes) {
                            const auto& bytes =
                                std::get<std::vector<uint8_t>>(item.value);
                            std::string bytes_str(bytes.begin(), bytes.end());
                            reflection->AddString(message, fd, bytes_str);
                        }
                        break;
                    case google::protobuf::FieldDescriptor::TYPE_ENUM:
                        if (item.type ==
                            ProtobufVariant::ValueType::EnumNumber) {
                            reflection->AddEnumValue(
                                message, fd, std::get<int32_t>(item.value));
                        }
                        break;
                    case google::protobuf::FieldDescriptor::TYPE_MESSAGE:
                    case google::protobuf::FieldDescriptor::TYPE_GROUP:
                        if (item.type == ProtobufVariant::ValueType::Message) {
                            const auto& msg_ptr = std::get<
                                std::unique_ptr<google::protobuf::Message>>(
                                item.value);
                            if (msg_ptr) {
                                google::protobuf::Message* added_msg =
                                    reflection->AddMessage(message, fd);
                                added_msg->CopyFrom(*msg_ptr);
                            }
                        }
                        break;
                }
            }
            break;
        }
        case ProtobufVariant::ValueType::Map: {
            const auto& map =
                std::get<std::map<MapKey, ProtobufVariant>>(value.value);

            // Clear the map field first
            reflection->ClearField(message, fd);

            // Map fields in protobuf are implemented as repeated fields with a
            // synthetic message type that has 'key' and 'value' fields
            const google::protobuf::Descriptor* map_entry_descriptor =
                fd->message_type();
            const google::protobuf::FieldDescriptor* key_field =
                map_entry_descriptor->FindFieldByName("key");
            const google::protobuf::FieldDescriptor* value_field =
                map_entry_descriptor->FindFieldByName("value");

            if (!key_field || !value_field) {
                throw ProtobufError(
                    "Map field does not have proper key/value structure");
            }

            for (const auto& [map_key, map_value] : map) {
                // Add a new map entry message
                google::protobuf::Message* entry_msg =
                    reflection->AddMessage(message, fd);
                const google::protobuf::Reflection* entry_reflection =
                    entry_msg->GetReflection();

                // Set the key field
                std::visit(
                    [&](const auto& key_val) {
                        using KeyType = std::decay_t<decltype(key_val)>;
                        if constexpr (std::is_same_v<KeyType, std::string>) {
                            entry_reflection->SetString(entry_msg, key_field,
                                                        key_val);
                        } else if constexpr (std::is_same_v<KeyType, int32_t>) {
                            entry_reflection->SetInt32(entry_msg, key_field,
                                                       key_val);
                        } else if constexpr (std::is_same_v<KeyType, int64_t>) {
                            entry_reflection->SetInt64(entry_msg, key_field,
                                                       key_val);
                        } else if constexpr (std::is_same_v<KeyType,
                                                            uint32_t>) {
                            entry_reflection->SetUInt32(entry_msg, key_field,
                                                        key_val);
                        } else if constexpr (std::is_same_v<KeyType,
                                                            uint64_t>) {
                            entry_reflection->SetUInt64(entry_msg, key_field,
                                                        key_val);
                        } else if constexpr (std::is_same_v<KeyType, bool>) {
                            entry_reflection->SetBool(entry_msg, key_field,
                                                      key_val);
                        }
                    },
                    map_key);

                // Set the value field based on the ProtobufVariant type
                switch (map_value.type) {
                    case ProtobufVariant::ValueType::Bool:
                        entry_reflection->SetBool(
                            entry_msg, value_field,
                            std::get<bool>(map_value.value));
                        break;
                    case ProtobufVariant::ValueType::I32:
                        entry_reflection->SetInt32(
                            entry_msg, value_field,
                            std::get<int32_t>(map_value.value));
                        break;
                    case ProtobufVariant::ValueType::EnumNumber:
                        entry_reflection->SetEnumValue(
                            entry_msg, value_field,
                            std::get<int32_t>(map_value.value));
                        break;
                    case ProtobufVariant::ValueType::I64:
                        entry_reflection->SetInt64(
                            entry_msg, value_field,
                            std::get<int64_t>(map_value.value));
                        break;
                    case ProtobufVariant::ValueType::U32:
                        entry_reflection->SetUInt32(
                            entry_msg, value_field,
                            std::get<uint32_t>(map_value.value));
                        break;
                    case ProtobufVariant::ValueType::U64:
                        entry_reflection->SetUInt64(
                            entry_msg, value_field,
                            std::get<uint64_t>(map_value.value));
                        break;
                    case ProtobufVariant::ValueType::F32:
                        entry_reflection->SetFloat(
                            entry_msg, value_field,
                            std::get<float>(map_value.value));
                        break;
                    case ProtobufVariant::ValueType::F64:
                        entry_reflection->SetDouble(
                            entry_msg, value_field,
                            std::get<double>(map_value.value));
                        break;
                    case ProtobufVariant::ValueType::String:
                        entry_reflection->SetString(
                            entry_msg, value_field,
                            std::get<std::string>(map_value.value));
                        break;
                    case ProtobufVariant::ValueType::Bytes: {
                        const auto& bytes =
                            std::get<std::vector<uint8_t>>(map_value.value);
                        std::string bytes_str(bytes.begin(), bytes.end());
                        entry_reflection->SetString(entry_msg, value_field,
                                                    bytes_str);
                        break;
                    }
                    case ProtobufVariant::ValueType::Message: {
                        const auto& msg_ptr = std::get<
                            std::unique_ptr<google::protobuf::Message>>(
                            map_value.value);
                        if (msg_ptr) {
                            google::protobuf::Message* mutable_value_msg =
                                entry_reflection->MutableMessage(entry_msg,
                                                                 value_field);
                            mutable_value_msg->CopyFrom(*msg_ptr);
                        }
                        break;
                    }
                    case ProtobufVariant::ValueType::List:
                    case ProtobufVariant::ValueType::Map:
                        // Nested lists and maps in map values would require
                        // recursive handling For now, this is not supported
                        throw ProtobufError(
                            "Nested lists and maps in map values are not "
                            "supported");
                        break;
                }
            }
            break;
        }
    }
}

std::unordered_set<std::string> getInlineTags(
    const google::protobuf::FieldDescriptor* field_desc) {
    std::unordered_set<std::string> tag_set;

    if (!field_desc) {
        return tag_set;
    }

    // Try to get the confluent.field_meta extension from the field options
    const google::protobuf::FieldOptions& options = field_desc->options();

    if (options.HasExtension(confluent::field_meta)) {
        auto ext = options.GetExtension(confluent::field_meta);
        const auto& tags =
            ext.tags();  // Call tags() method to get RepeatedPtrField
        for (const auto& tag : tags) {
            tag_set.insert(tag);
        }
    }

    return tag_set;
}

std::string schemaToString(const google::protobuf::FileDescriptor* file_desc) {
    std::string serialized;
    google::protobuf::FileDescriptorProto proto;
    file_desc->CopyTo(&proto);
    if (!proto.SerializeToString(&serialized)) {
        throw ProtobufError("Failed to serialize FileDescriptor to string");
    }

    // Base64 encode the serialized data
    std::vector<uint8_t> bytes(serialized.begin(), serialized.end());
    return base64_encode(bytes);
}

const google::protobuf::FileDescriptor* stringToSchema(
    google::protobuf::DescriptorPool* pool, const std::string& name,
    const std::string& schema_string) {
    // Base64 decode
    std::vector<uint8_t> bytes = base64_decode(schema_string);

    decodeFileDescriptorProtoWithName(pool, name, bytes);

    const google::protobuf::FileDescriptor* file_desc =
        pool->FindFileByName(name);
    if (!file_desc) {
        throw ProtobufError("File descriptor not found after decoding: " +
                            name);
    }

    return file_desc;
}

void decodeFileDescriptorProtoWithName(google::protobuf::DescriptorPool* pool,
                                       const std::string& name,
                                       const std::vector<uint8_t>& data) {
    google::protobuf::FileDescriptorProto proto;
    if (!proto.ParseFromArray(data.data(), data.size())) {
        throw ProtobufError("Failed to parse FileDescriptorProto from data");
    }

    proto.set_name(name);

    const google::protobuf::FileDescriptor* file_desc = pool->BuildFile(proto);
    if (!file_desc) {
        throw ProtobufError("Failed to build FileDescriptor from proto");
    }
}

bool isBuiltin(const std::string& name) {
    return name.find("confluent/") == 0 || name.find("google/protobuf/") == 0 ||
           name.find("google/type/") == 0;
}

nlohmann::json messageToJson(const google::protobuf::Message& message) {
    std::string json_string;
    auto status =
        google::protobuf::util::MessageToJsonString(message, &json_string);
    if (!status.ok()) {
        throw ProtobufError("Failed to convert message to JSON: " +
                            status.ToString());
    }
    return nlohmann::json::parse(json_string);
}

void jsonToMessage(const nlohmann::json& json_value,
                   google::protobuf::Message* message) {
    std::string json_string = json_value.dump();
    google::protobuf::util::JsonParseOptions options;
    auto status = google::protobuf::util::JsonStringToMessage(json_string,
                                                              message, options);
    if (!status.ok()) {
        throw ProtobufError("Failed to parse JSON to message: " +
                            status.ToString());
    }
}

FieldType getFieldType(const google::protobuf::FieldDescriptor* field_desc) {
    // Check for map fields first (like the Rust version did)
    if (field_desc->is_map()) {
        return FieldType::Map;
    }

    switch (field_desc->type()) {
        case google::protobuf::FieldDescriptor::TYPE_STRING:
            return FieldType::String;
        case google::protobuf::FieldDescriptor::TYPE_BYTES:
            return FieldType::Bytes;
        case google::protobuf::FieldDescriptor::TYPE_INT32:
        case google::protobuf::FieldDescriptor::TYPE_SINT32:
        case google::protobuf::FieldDescriptor::TYPE_UINT32:
        case google::protobuf::FieldDescriptor::TYPE_FIXED32:
        case google::protobuf::FieldDescriptor::TYPE_SFIXED32:
            return FieldType::Int;
        case google::protobuf::FieldDescriptor::TYPE_INT64:
        case google::protobuf::FieldDescriptor::TYPE_SINT64:
        case google::protobuf::FieldDescriptor::TYPE_UINT64:
        case google::protobuf::FieldDescriptor::TYPE_FIXED64:
        case google::protobuf::FieldDescriptor::TYPE_SFIXED64:
            return FieldType::Long;
        case google::protobuf::FieldDescriptor::TYPE_FLOAT:
            return FieldType::Float;
        case google::protobuf::FieldDescriptor::TYPE_DOUBLE:
            return FieldType::Double;
        case google::protobuf::FieldDescriptor::TYPE_BOOL:
            return FieldType::Boolean;
        case google::protobuf::FieldDescriptor::TYPE_ENUM:
            return FieldType::Enum;
        case google::protobuf::FieldDescriptor::TYPE_MESSAGE:
            return FieldType::Record;
        default:
            return FieldType::String;  // Default fallback
    }
}

const google::protobuf::Descriptor* getMessageDescriptorByIndex(
    const google::protobuf::DescriptorPool* pool,
    const google::protobuf::FileDescriptor* file_desc,
    const std::vector<int32_t>& msg_index) {
    if (msg_index.empty() || msg_index[0] >= file_desc->message_type_count()) {
        return nullptr;
    }

    const google::protobuf::Descriptor* descriptor =
        file_desc->message_type(msg_index[0]);

    // Navigate nested types if there are more indexes
    for (size_t i = 1; i < msg_index.size() && descriptor; ++i) {
        if (msg_index[i] < descriptor->nested_type_count()) {
            descriptor = descriptor->nested_type(msg_index[i]);
        } else {
            return nullptr;
        }
    }

    return descriptor;
}

std::vector<int32_t> createMessageIndexArray(
    const google::protobuf::Descriptor* descriptor) {
    std::vector<int32_t> indexes;

    // Build index path from file descriptor to this message type
    const google::protobuf::FileDescriptor* file = descriptor->file();

    // Find the message type index within the file
    for (int i = 0; i < file->message_type_count(); ++i) {
        if (file->message_type(i) == descriptor) {
            indexes.push_back(i);
            return indexes;
        }
    }

    return indexes;
}

// extractFieldValue implementation moved to value_transform namespace above

}  // namespace schemaregistry::serdes::protobuf::utils
