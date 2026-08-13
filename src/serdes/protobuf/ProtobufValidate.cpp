#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include <string>
#include <vector>

#include "confluent/meta.pb.h"
#include "schemaregistry/serdes/ValidationRule.h"
#include "schemaregistry/serdes/protobuf/ProtobufTypes.h"
#include "schemaregistry/serdes/protobuf/ProtobufUtils.h"

// Fix for Windows GetMessage macro conflict
// On Windows, GetMessage is defined as a macro in winuser.h which conflicts
// with the GetMessage method in google::protobuf::Reflection
#ifdef _WIN32
#ifdef GetMessage
#undef GetMessage
#endif
#endif

namespace schemaregistry::serdes::protobuf::utils {

namespace {

std::vector<ValidationRule> toValidationRules(const confluent::Meta &meta) {
    std::vector<ValidationRule> rules;
    rules.reserve(static_cast<size_t>(meta.rules_size()));
    for (const auto &rule : meta.rules()) {
        rules.push_back(
            ValidationRule{rule.name(), rule.doc(), rule.expr(), rule.sql()});
    }
    return rules;
}

/**
 * Render a map key the way the dotted path notation expects: string keys are
 * quoted, everything else is rendered bare.
 */
std::string mapKeyToString(const google::protobuf::Message &entry,
                           const google::protobuf::FieldDescriptor *key_field) {
    const auto *reflection = entry.GetReflection();
    switch (key_field->cpp_type()) {
        case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
            return "\"" + reflection->GetString(entry, key_field) + "\"";
        case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
            return reflection->GetBool(entry, key_field) ? "true" : "false";
        case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
            return std::to_string(reflection->GetInt32(entry, key_field));
        case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
            return std::to_string(reflection->GetInt64(entry, key_field));
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
            return std::to_string(reflection->GetUInt32(entry, key_field));
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
            return std::to_string(reflection->GetUInt64(entry, key_field));
        default:
            return "";
    }
}

struct Walker {
    ValidationRuleExecutor &executor;
    bool fail_fast;
    std::vector<ValidationRuleError> &violations;

    bool done() const { return fail_fast && !violations.empty(); }

    bool evaluateRules(const std::vector<ValidationRule> &rules,
                       const SerdeValue &value, const std::string &path) {
        for (const auto &rule : rules) {
            evaluateValidationRule(executor, rule, value, path, violations);
            if (done()) {
                return true;
            }
        }
        return false;
    }

    /**
     * Wrap a single (non-repeated) field value so it can be handed to the
     * executor as `this`.
     */
    static std::unique_ptr<SerdeValue> fieldValue(
        const google::protobuf::Message &message,
        const google::protobuf::FieldDescriptor *field, int index);

    void walkMessage(const google::protobuf::Message &message,
                     const std::string &path);
};

std::unique_ptr<SerdeValue> Walker::fieldValue(
    const google::protobuf::Message &message,
    const google::protobuf::FieldDescriptor *field, int index) {
    const auto *reflection = message.GetReflection();
    bool repeated = field->is_repeated();
    switch (field->cpp_type()) {
        case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
            return makeProtobufValue(ProtobufVariant(
                repeated ? reflection->GetRepeatedBool(message, field, index)
                         : reflection->GetBool(message, field)));
        case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
            return makeProtobufValue(ProtobufVariant(
                repeated ? reflection->GetRepeatedInt32(message, field, index)
                         : reflection->GetInt32(message, field)));
        case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
            return makeProtobufValue(ProtobufVariant(
                repeated ? reflection->GetRepeatedInt64(message, field, index)
                         : reflection->GetInt64(message, field)));
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
            return makeProtobufValue(ProtobufVariant(
                repeated ? reflection->GetRepeatedUInt32(message, field, index)
                         : reflection->GetUInt32(message, field)));
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
            return makeProtobufValue(ProtobufVariant(
                repeated ? reflection->GetRepeatedUInt64(message, field, index)
                         : reflection->GetUInt64(message, field)));
        case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
            return makeProtobufValue(ProtobufVariant(
                repeated ? reflection->GetRepeatedFloat(message, field, index)
                         : reflection->GetFloat(message, field)));
        case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
            return makeProtobufValue(ProtobufVariant(
                repeated ? reflection->GetRepeatedDouble(message, field, index)
                         : reflection->GetDouble(message, field)));
        case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
            return makeProtobufValue(ProtobufVariant::createEnum(
                repeated ? reflection->GetRepeatedEnum(message, field, index)
                               ->number()
                         : reflection->GetEnum(message, field)->number()));
        case google::protobuf::FieldDescriptor::CPPTYPE_STRING: {
            std::string scratch;
            const std::string &value =
                repeated
                    ? reflection->GetRepeatedStringReference(message, field,
                                                             index, &scratch)
                    : reflection->GetStringReference(message, field, &scratch);
            if (field->type() ==
                google::protobuf::FieldDescriptor::TYPE_BYTES) {
                return makeProtobufValue(ProtobufVariant(
                    std::vector<uint8_t>(value.begin(), value.end())));
            }
            return makeProtobufValue(ProtobufVariant(value));
        }
        case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE: {
            const auto &nested =
                repeated ? reflection->GetRepeatedMessage(message, field, index)
                         : reflection->GetMessage(message, field);
            auto copy =
                std::unique_ptr<google::protobuf::Message>(nested.New());
            copy->CopyFrom(nested);
            return makeProtobufValue(ProtobufVariant(std::move(copy)));
        }
        default:
            return nullptr;
    }
}

void Walker::walkMessage(const google::protobuf::Message &message,
                         const std::string &path) {
    if (done()) {
        return;
    }
    const auto *descriptor = message.GetDescriptor();
    const auto *reflection = message.GetReflection();
    if (descriptor == nullptr || reflection == nullptr) {
        return;
    }

    // Message-level rules: `this` is the message itself.
    auto message_meta = getMessageMeta(descriptor);
    if (message_meta.has_value()) {
        auto rules = toValidationRules(*message_meta);
        if (!rules.empty()) {
            auto copy =
                std::unique_ptr<google::protobuf::Message>(message.New());
            copy->CopyFrom(message);
            auto value = makeProtobufValue(ProtobufVariant(std::move(copy)));
            if (evaluateRules(rules, *value, path)) {
                return;
            }
        }
    }

    for (int i = 0; i < descriptor->field_count(); ++i) {
        const auto *field = descriptor->field(i);
        std::vector<ValidationRule> rules;
        auto field_meta = getFieldMeta(field);
        if (field_meta.has_value()) {
            rules = toValidationRules(*field_meta);
        }
        std::string field_path = appendValidationPath(path, field->name());

        if (field->is_map()) {
            const auto *entry = field->message_type();
            const auto *key_field = entry->map_key();
            const auto *value_field = entry->map_value();
            int count = reflection->FieldSize(message, field);
            for (int index = 0; index < count; ++index) {
                const auto &map_entry =
                    reflection->GetRepeatedMessage(message, field, index);
                std::string entry_path = field_path + "[" +
                                         mapKeyToString(map_entry, key_field) +
                                         "]";
                auto value = fieldValue(map_entry, value_field, 0);
                if (value && evaluateRules(rules, *value, entry_path)) {
                    return;
                }
                if (value_field->cpp_type() ==
                    google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
                    walkMessage(map_entry.GetReflection()->GetMessage(
                                    map_entry, value_field),
                                entry_path);
                    if (done()) {
                        return;
                    }
                }
            }
            continue;
        }

        if (field->is_repeated()) {
            int count = reflection->FieldSize(message, field);
            for (int index = 0; index < count; ++index) {
                std::string element_path =
                    field_path + "[" + std::to_string(index) + "]";
                auto value = fieldValue(message, field, index);
                if (value && evaluateRules(rules, *value, element_path)) {
                    return;
                }
                if (field->cpp_type() ==
                    google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
                    walkMessage(
                        reflection->GetRepeatedMessage(message, field, index),
                        element_path);
                    if (done()) {
                        return;
                    }
                }
            }
            continue;
        }

        // Skip-on-null: an unset optional or oneof member does not have its
        // rules invoked. Proto3 scalars without presence always report as set
        // here, matching how the other clients treat a defaulted scalar.
        if (field->has_presence() && !reflection->HasField(message, field)) {
            continue;
        }

        if (!rules.empty()) {
            auto value = fieldValue(message, field, 0);
            if (value && evaluateRules(rules, *value, field_path)) {
                return;
            }
        }
        if (field->cpp_type() ==
            google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
            walkMessage(reflection->GetMessage(message, field), field_path);
            if (done()) {
                return;
            }
        }
    }
}

}  // namespace

std::vector<ValidationRuleError> validateMessage(
    ValidationRuleExecutor &executor, const google::protobuf::Message &message,
    bool fail_fast) {
    std::vector<ValidationRuleError> violations;
    Walker walker{executor, fail_fast, violations};
    walker.walkMessage(message, "");
    return violations;
}

}  // namespace schemaregistry::serdes::protobuf::utils
