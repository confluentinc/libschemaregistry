#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include <map>
#include <optional>
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

/**
 * A map key as the variant a map value is keyed by, or nullopt for a key type
 * protobuf cannot produce.
 */
std::optional<MapKey> mapKey(const google::protobuf::Message &entry,
                             const google::protobuf::FieldDescriptor *key_field) {
    const auto *reflection = entry.GetReflection();
    switch (key_field->cpp_type()) {
        case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
            return MapKey(reflection->GetString(entry, key_field));
        case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
            return MapKey(reflection->GetBool(entry, key_field));
        case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
            return MapKey(reflection->GetInt32(entry, key_field));
        case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
            return MapKey(reflection->GetInt64(entry, key_field));
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
            return MapKey(reflection->GetUInt32(entry, key_field));
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
            return MapKey(reflection->GetUInt64(entry, key_field));
        default:
            return std::nullopt;
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

    /**
     * A repeated field's elements as a single list value, so that a field-level
     * rule sees the whole collection - `this` is the list, as in the JVM client.
     */
    static std::unique_ptr<SerdeValue> repeatedFieldValue(
        const google::protobuf::Message &message,
        const google::protobuf::FieldDescriptor *field);

    /**
     * A map field's entries as a single map value, for the same reason.
     */
    static std::unique_ptr<SerdeValue> mapFieldValue(
        const google::protobuf::Message &message,
        const google::protobuf::FieldDescriptor *field);

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

std::unique_ptr<SerdeValue> Walker::repeatedFieldValue(
    const google::protobuf::Message &message,
    const google::protobuf::FieldDescriptor *field) {
    int count = message.GetReflection()->FieldSize(message, field);
    std::vector<ProtobufVariant> elements;
    elements.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        auto element = fieldValue(message, field, index);
        if (!element) {
            return nullptr;
        }
        elements.push_back(asProtobuf(*element));
    }
    return makeProtobufValue(ProtobufVariant(std::move(elements)));
}

std::unique_ptr<SerdeValue> Walker::mapFieldValue(
    const google::protobuf::Message &message,
    const google::protobuf::FieldDescriptor *field) {
    const auto *reflection = message.GetReflection();
    const auto *entry_type = field->message_type();
    const auto *key_field = entry_type->map_key();
    const auto *value_field = entry_type->map_value();
    int count = reflection->FieldSize(message, field);
    std::map<MapKey, ProtobufVariant> entries;
    for (int index = 0; index < count; ++index) {
        const auto &entry = reflection->GetRepeatedMessage(message, field, index);
        auto key = mapKey(entry, key_field);
        if (!key.has_value()) {
            return nullptr;
        }
        auto value = fieldValue(entry, value_field, 0);
        if (!value) {
            return nullptr;
        }
        entries.emplace(*key, asProtobuf(*value));
    }
    return makeProtobufValue(ProtobufVariant(std::move(entries)));
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
            // Field-level rules see the whole map, matching the JVM client: `this`
            // is the map itself, so a rule over its contents is written as a
            // comprehension - this.all(k, this[k] >= 0) - rather than being
            // invoked once per entry.
            if (!rules.empty()) {
                auto map_value = mapFieldValue(message, field);
                if (map_value && evaluateRules(rules, *map_value, field_path)) {
                    return;
                }
            }
            int count = reflection->FieldSize(message, field);
            for (int index = 0; index < count; ++index) {
                const auto &map_entry =
                    reflection->GetRepeatedMessage(message, field, index);
                std::string entry_path = field_path + "[" +
                                         mapKeyToString(map_entry, key_field) +
                                         "]";
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
            // As for maps: the whole list is bound to `this`.
            if (!rules.empty()) {
                auto list_value = repeatedFieldValue(message, field);
                if (list_value && evaluateRules(rules, *list_value, field_path)) {
                    return;
                }
            }
            int count = reflection->FieldSize(message, field);
            for (int index = 0; index < count; ++index) {
                std::string element_path =
                    field_path + "[" + std::to_string(index) + "]";
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
