#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
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

    /**
     * Walk `message` against `schema_descriptor`, the registered schema's view of
     * its type.
     *
     * The walk is driven by `message`: it decides which fields exist, which are
     * absent, and what the values are. Each field is paired to the schema by
     * number - which is how protobuf identifies a field - and the schema's field
     * supplies the rules and the name used in the reported path. Fields the
     * schema does not declare are skipped, so the walk visits the intersection.
     *
     * `schema_message` is the same message re-read through `schema_descriptor`,
     * or null when the two descriptors present it identically. Where it exists,
     * it is what rules are handed: a rule's environment is built from the schema,
     * so `this.renamed` cannot read a field the caller's type calls something
     * else, and bytes and string are interchangeable at the same number without
     * being interchangeable to a rule.
     */
    void walkMessage(const google::protobuf::Descriptor *schema_descriptor,
                     const google::protobuf::Message &message,
                     const google::protobuf::Message *schema_message,
                     const std::string &path);
};

/**
 * The map entry in `entries` whose key matches `key`, or null. Map values pair by
 * key rather than by position.
 */
const google::protobuf::Message *findMapEntry(
    const google::protobuf::Message &owner,
    const google::protobuf::FieldDescriptor *field, const std::string &key) {
    const auto *reflection = owner.GetReflection();
    const auto *key_field = field->message_type()->map_key();
    int count = reflection->FieldSize(owner, field);
    for (int i = 0; i < count; ++i) {
        const auto &entry = reflection->GetRepeatedMessage(owner, field, i);
        if (mapKeyToString(entry, key_field) == key) {
            return &entry;
        }
    }
    return nullptr;
}

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

void Walker::walkMessage(const google::protobuf::Descriptor *schema_descriptor,
                         const google::protobuf::Message &message,
                         const google::protobuf::Message *schema_message,
                         const std::string &path) {
    if (done()) {
        return;
    }
    const auto *descriptor = message.GetDescriptor();
    const auto *reflection = message.GetReflection();
    if (descriptor == nullptr || reflection == nullptr ||
        schema_descriptor == nullptr) {
        return;
    }

    // Message-level rules: `this` is the message itself, read as the schema
    // names it.
    auto message_meta = getMessageMeta(schema_descriptor);
    if (message_meta.has_value()) {
        auto rules = toValidationRules(*message_meta);
        if (!rules.empty()) {
            const auto &bound =
                schema_message != nullptr ? *schema_message : message;
            auto copy = std::unique_ptr<google::protobuf::Message>(bound.New());
            copy->CopyFrom(bound);
            auto value = makeProtobufValue(ProtobufVariant(std::move(copy)));
            if (evaluateRules(rules, *value, path)) {
                return;
            }
        }
    }

    for (int i = 0; i < descriptor->field_count(); ++i) {
        const auto *field = descriptor->field(i);
        const auto *schema_field =
            schema_descriptor->FindFieldByNumber(field->number());
        if (schema_field == nullptr) {
            // The registered schema does not declare this field, so it carries no
            // rules and nothing below it can either.
            continue;
        }
        std::vector<ValidationRule> rules;
        auto field_meta = getFieldMeta(schema_field);
        if (field_meta.has_value()) {
            rules = toValidationRules(*field_meta);
        }
        // The path names the field as the registered schema does, which is what a
        // rule refers to; the value is still read through the caller's field.
        std::string field_path = appendValidationPath(path, schema_field->name());
        // Where a schema view exists, values come from it, read through the
        // schema's own field.
        const google::protobuf::Message *value_owner =
            schema_message != nullptr ? schema_message : &message;
        const google::protobuf::FieldDescriptor *value_field =
            schema_message != nullptr ? schema_field : field;

        if (field->is_map()) {
            const auto *entry = field->message_type();
            const auto *key_field = entry->map_key();
            const auto *entry_value_field = entry->map_value();
            // Field-level rules see the whole map, matching the JVM client: `this`
            // is the map itself, so a rule over its contents is written as a
            // comprehension - this.all(k, this[k] >= 0) - rather than being
            // invoked once per entry.
            if (!rules.empty()) {
                auto map_value = mapFieldValue(*value_owner, value_field);
                if (map_value && evaluateRules(rules, *map_value, field_path)) {
                    return;
                }
            }
            int count = reflection->FieldSize(message, field);
            for (int index = 0; index < count; ++index) {
                const auto &map_entry =
                    reflection->GetRepeatedMessage(message, field, index);
                std::string key = mapKeyToString(map_entry, key_field);
                std::string entry_path = field_path + "[" + key + "]";
                if (entry_value_field->cpp_type() !=
                    google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
                    continue;
                }
                const auto *schema_entry_value = static_cast<
                    const google::protobuf::Message *>(nullptr);
                if (schema_message != nullptr) {
                    const auto *schema_entry =
                        findMapEntry(*schema_message, schema_field, key);
                    if (schema_entry != nullptr) {
                        schema_entry_value = &schema_entry->GetReflection()
                                                  ->GetMessage(
                                                      *schema_entry,
                                                      schema_field->message_type()
                                                          ->map_value());
                    }
                }
                walkMessage(schema_field->message_type()->map_value()->message_type(),
                            map_entry.GetReflection()->GetMessage(
                                map_entry, entry_value_field),
                            schema_entry_value, entry_path);
                if (done()) {
                    return;
                }
            }
            continue;
        }

        if (field->is_repeated()) {
            // As for maps: the whole list is bound to `this`.
            if (!rules.empty()) {
                auto list_value = repeatedFieldValue(*value_owner, value_field);
                if (list_value && evaluateRules(rules, *list_value, field_path)) {
                    return;
                }
            }
            int count = reflection->FieldSize(message, field);
            for (int index = 0; index < count; ++index) {
                std::string element_path =
                    field_path + "[" + std::to_string(index) + "]";
                if (field->cpp_type() !=
                        google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE ||
                    schema_field->cpp_type() !=
                        google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
                    continue;
                }
                // Both lists came from the same bytes, so they line up; the guard
                // is for safety.
                const auto *schema_element = static_cast<
                    const google::protobuf::Message *>(nullptr);
                if (schema_message != nullptr &&
                    index < schema_message->GetReflection()->FieldSize(
                                *schema_message, schema_field)) {
                    schema_element =
                        &schema_message->GetReflection()->GetRepeatedMessage(
                            *schema_message, schema_field, index);
                }
                walkMessage(
                    schema_field->message_type(),
                    reflection->GetRepeatedMessage(message, field, index),
                    schema_element, element_path);
                if (done()) {
                    return;
                }
            }
            continue;
        }

        // Skip-on-null: an unset optional or oneof member does not have its
        // rules invoked. Proto3 scalars without presence always report as set
        // here, matching how the other clients treat a defaulted scalar.
        //
        // Both halves are read from the caller's message: whether an unset field
        // counts as absent is decided by the type that wrote it, not by the
        // registered schema, and the two can disagree - moving a field into or
        // out of a oneof is a compatible change.
        if (field->has_presence() && !reflection->HasField(message, field)) {
            continue;
        }

        if (!rules.empty()) {
            auto value = fieldValue(*value_owner, value_field, 0);
            if (value && evaluateRules(rules, *value, field_path)) {
                return;
            }
        }
        if (field->cpp_type() ==
                google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE &&
            schema_field->cpp_type() ==
                google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
            const auto *schema_nested = static_cast<
                const google::protobuf::Message *>(nullptr);
            if (schema_message != nullptr) {
                schema_nested = &schema_message->GetReflection()->GetMessage(
                    *schema_message, schema_field);
            }
            walkMessage(schema_field->message_type(),
                        reflection->GetMessage(message, field), schema_nested,
                        field_path);
            if (done()) {
                return;
            }
        }
    }
}

/**
 * Whether the two descriptors present every field they share - paired by number,
 * which is how protobuf identifies a field - under the same name, type and label,
 * recursively through message-valued fields.
 *
 * A field the registered schema declares and the caller's type does not counts as
 * a difference: adding a field is a compatible change, and a message-level rule
 * may reference the added field expecting the schema's default for it, which only
 * a message read through the schema can supply. Fields only the caller declares
 * are ignored - no rule can name them, and the walk skips them.
 *
 * `visited` holds the descriptor pairs already compared, so a self-referential
 * message type terminates.
 */
bool presentsSameValues(
    const google::protobuf::Descriptor *schema_descriptor,
    const google::protobuf::Descriptor *runtime_descriptor,
    std::set<std::pair<const google::protobuf::Descriptor *,
                       const google::protobuf::Descriptor *>> &visited) {
    if (!visited.emplace(schema_descriptor, runtime_descriptor).second) {
        // Already compared on another path, or cycling back to it. Either way
        // this pair contributes no new disagreement.
        return true;
    }
    for (int i = 0; i < schema_descriptor->field_count(); ++i) {
        const auto *schema_field = schema_descriptor->field(i);
        if (runtime_descriptor->FindFieldByNumber(schema_field->number()) ==
            nullptr) {
            return false;
        }
    }
    for (int i = 0; i < runtime_descriptor->field_count(); ++i) {
        const auto *runtime_field = runtime_descriptor->field(i);
        const auto *schema_field =
            schema_descriptor->FindFieldByNumber(runtime_field->number());
        if (schema_field == nullptr) {
            continue;
        }
        if (schema_field->name() != runtime_field->name() ||
            schema_field->type() != runtime_field->type() ||
            schema_field->is_repeated() != runtime_field->is_repeated()) {
            return false;
        }
        if (schema_field->cpp_type() ==
                google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE &&
            runtime_field->cpp_type() ==
                google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE &&
            !presentsSameValues(schema_field->message_type(),
                                runtime_field->message_type(), visited)) {
            return false;
        }
    }
    return true;
}

/**
 * Whether a message whose descriptor is `runtime_descriptor` has to be re-read
 * through `schema_descriptor` before rules can bind `this` to it.
 *
 * Presence deliberately does not count. Whether an unset field is absent is
 * decided by the producer's field on the producer's message, which the walk reads
 * directly, so a schema that only moved a field into or out of a oneof needs no
 * re-read.
 *
 * A field the schema declares and the caller's type does not does count, which
 * means a type running behind the registered schema - the use.latest.version case
 * - re-reads every record. Only an exact match skips the re-read. Narrowing that
 * to the rules that could actually observe the added field is possible but not
 * simple: a rule binding `this` at any ancestor can traverse into the field, and a
 * field-level rule on a message-valued field binds `this` to a type that need not
 * declare rules of its own, so a per-descriptor test for message-level rules would
 * be wrong in both directions.
 *
 * Deliberately not memoized, unlike the other clients. A cache would have to be
 * keyed by descriptor pointers, which carry no ownership: the pools that own them
 * are held elsewhere and can be released, and a later pool allocated at the same
 * address would then read the previous one's answer. The comparison itself is a
 * bounded walk over the schema's own shape - no allocation, and skipped outright
 * when the two descriptors are the same object, which is the usual case - against
 * the serialize-and-parse it decides about.
 */
bool needsSchemaView(const google::protobuf::Descriptor *schema_descriptor,
                     const google::protobuf::Descriptor *runtime_descriptor) {
    if (schema_descriptor == runtime_descriptor) {
        return false;
    }
    std::set<std::pair<const google::protobuf::Descriptor *,
                       const google::protobuf::Descriptor *>>
        visited;
    bool needed = !presentsSameValues(schema_descriptor, runtime_descriptor, visited);
    return needed;
}

}  // namespace

std::vector<ValidationRuleError> validateMessage(
    ValidationRuleExecutor &executor, const google::protobuf::Message &message,
    const google::protobuf::Descriptor *schema_descriptor, bool fail_fast) {
    std::vector<ValidationRuleError> violations;
    const auto *runtime_descriptor = message.GetDescriptor();
    if (schema_descriptor == nullptr) {
        schema_descriptor = runtime_descriptor;
    }
    Walker walker{executor, fail_fast, violations};
    if (!needsSchemaView(schema_descriptor, runtime_descriptor)) {
        walker.walkMessage(schema_descriptor, message, nullptr, "");
        return violations;
    }

    // A rule that binds `this` to a message needs it in the schema's terms: a
    // rule's environment is built from the registered schema, so `this.renamed`
    // cannot read a field the caller's type calls something else. The two
    // descriptors live in different pools, so round-trip through the wire format
    // rather than CopyFrom - protobuf pairs fields by number there, which is what
    // carries the values across a rename.
    google::protobuf::DynamicMessageFactory factory;
    auto schema_message = std::unique_ptr<google::protobuf::Message>(
        factory.GetPrototype(schema_descriptor)->New());
    std::string bytes;
    if (!message.SerializeToString(&bytes) ||
        !schema_message->ParseFromString(bytes)) {
        // The message cannot be read through the registered schema. Walk it
        // anyway, without the schema's view: every rule still runs, and one that
        // depends on the schema's names fails as a rule error rather than
        // silently not running at all.
        walker.walkMessage(schema_descriptor, message, nullptr, "");
        return violations;
    }
    walker.walkMessage(schema_descriptor, message, schema_message.get(), "");
    return violations;
}

}  // namespace schemaregistry::serdes::protobuf::utils
