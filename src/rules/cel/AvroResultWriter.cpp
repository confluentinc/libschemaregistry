/**
 * Rebuilds an Avro record from the map a message-level `CEL` transform returned.
 * See AvroResultWriter.h for the contract, and for why it is not a branch inside toAvroValue.
 */

#include "schemaregistry/rules/cel/AvroResultWriter.h"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "schemaregistry/rules/cel/CelUtils.h"

namespace schemaregistry::rules::cel::utils {

namespace {

/// Whether a null is a legal value for a field of this schema - the field is itself null, or a
/// union with a null branch. Used to tell a declared `"default": null` from no default at all,
/// which avro-cpp stores identically (Compiler.cc stores a default-constructed GenericDatum for
/// a field with no "default" key).
bool avroFieldAcceptsNull(const ::avro::NodePtr &field_schema) {
    if (field_schema->type() == ::avro::AVRO_NULL) {
        return true;
    }
    if (field_schema->type() != ::avro::AVRO_UNION) {
        return false;
    }
    for (size_t i = 0; i < field_schema->leaves(); ++i) {
        if (field_schema->leafAt(i)->type() == ::avro::AVRO_NULL) {
            return true;
        }
    }
    return false;
}


/// Re-wraps a converted value as a member of a union field.
///
/// `GenericDatum::type()`, `logicalType()` and `value<T>()` all forward transparently through a
/// union to its selected branch, so `toAvroValue` cannot tell that the template it was handed was
/// a union and returns a bare datum. Encoding a bare datum into a union slot writes no branch
/// index at all, and the record that comes off the wire is unreadable - avro-cpp reads the next
/// field's bytes as the branch index and throws `std::out_of_range` from `selectBranch`. Every
/// message-level `CEL` transform over a schema with a nullable field produced such a record.
/// Whether `value` belongs to this union branch.
///
/// The base type alone is ambiguous: a union may legally hold several branches of the same
/// kind (`["null","RecA","RecB"]`, two enums, two fixed). Selecting the first one whose
/// `type()` matched wrote the value under the wrong branch index, so a reader either decoded
/// it as the other type or failed outright. Java's AvroResultWriter.branchAccepts matches a
/// record branch by full name, an enum by symbol validity and a fixed by size; comparing the
/// value's own schema name is the equivalent here, since a GenericRecord/Enum/Fixed carries
/// the schema it was built from.
bool branchAccepts(const ::avro::NodePtr &branch, const ::avro::GenericDatum &value) {
    if (branch->type() != value.type()) {
        return false;
    }
    switch (value.type()) {
        case ::avro::AVRO_RECORD:
            return branch->name().fullname() ==
                   value.value<::avro::GenericRecord>().schema()->name().fullname();
        case ::avro::AVRO_ENUM:
            return branch->name().fullname() ==
                   value.value<::avro::GenericEnum>().schema()->name().fullname();
        case ::avro::AVRO_FIXED:
            // Name *and* size: a same-name-different-size branch would be selected and then
            // encode the wrong number of bytes.
            return branch->name().fullname() ==
                       value.value<::avro::GenericFixed>().schema()->name().fullname() &&
                   branch->fixedSize() ==
                       value.value<::avro::GenericFixed>().schema()->fixedSize();
        default:
            return true;  // unnamed types are fully described by their base type
    }
}

/// The full name of the proto message a CEL value carries, or "" if it carries none.
std::string celMessageName(const google::api::expr::runtime::CelValue &value) {
    if (!value.IsMessage()) {
        return {};
    }
    const google::protobuf::Message *message = value.MessageOrDie();
    return message == nullptr
               ? std::string()
               : std::string(message->GetDescriptor()->full_name());
}

/// Whether the value a rule returned can be written into this union branch.
///
/// The counterpart of `branchAccepts` on the CEL side, and the reason both exist: a branch has
/// to be chosen *before* the conversion (to supply the template) and confirmed after it (to
/// write the branch index), and only the first of those has a CEL value to look at.
///
/// Acceptance is judged against what this client's reader actually produces: `fromAvroValue`
/// turns only DECIMAL and TIMESTAMP_* into semantic CEL types, so a date or time-millis branch
/// is matched as the plain int or long it is read as - the JVM's branchAccepts has temporal
/// arms there because its reader converts those too.
bool branchAcceptsCel(const ::avro::NodePtr &branch,
                      const google::api::expr::runtime::CelValue &value) {
    const bool has_logical_type =
        branch->logicalType().type() != ::avro::LogicalType::NONE;
    switch (branch->type()) {
        case ::avro::AVRO_NULL:
            return value.IsNull();
        case ::avro::AVRO_BOOL:
            return value.IsBool();
        case ::avro::AVRO_INT:
            // CEL has no 32-bit integer, so an int branch takes an int64 that fits in one.
            if (value.IsInt64()) {
                return value.Int64OrDie() >=
                           std::numeric_limits<int32_t>::min() &&
                       value.Int64OrDie() <=
                           std::numeric_limits<int32_t>::max();
            }
            return value.IsUint64() &&
                   value.Uint64OrDie() <=
                       static_cast<uint64_t>(
                           std::numeric_limits<int32_t>::max());
        case ::avro::AVRO_LONG:
            return value.IsInt64() || value.IsUint64() ||
                   (has_logical_type && value.IsTimestamp());
        case ::avro::AVRO_FLOAT:
        case ::avro::AVRO_DOUBLE:
            return value.IsDouble() || value.IsInt64() || value.IsUint64();
        case ::avro::AVRO_STRING:
            return value.IsString();
        case ::avro::AVRO_ENUM: {
            if (!value.IsString()) {
                return false;
            }
            size_t symbol = 0;
            return branch->nameIndex(std::string(value.StringOrDie().value()),
                                     symbol);
        }
        case ::avro::AVRO_BYTES:
            return value.IsBytes() ||
                   (has_logical_type &&
                    celMessageName(value) == "confluent.type.Decimal");
        case ::avro::AVRO_FIXED:
            // Raw bytes have to be exactly the declared width, since toAvroValue refuses a
            // mismatch rather than guessing an alignment; a decimal is padded up to it, so
            // its width is not checkable here.
            if (value.IsBytes()) {
                return value.BytesOrDie().value().size() ==
                       branch->fixedSize();
            }
            return has_logical_type &&
                   celMessageName(value) == "confluent.type.Decimal";
        case ::avro::AVRO_ARRAY:
            return value.IsList();
        case ::avro::AVRO_MAP:
            return value.IsMap();
        case ::avro::AVRO_RECORD: {
            const std::string message = celMessageName(value);
            if (!message.empty()) {
                // A confluent.type.Variant record is read as a Variant message and written
                // back from one; no other message has a record shape to go into.
                return message == "confluent.type.Variant" &&
                       branch->name().fullname() == "confluent.type.Variant";
            }
            // CEL returns a record as an untagged map, which cannot be told apart from a
            // map for a different record branch, so the first record branch in declaration
            // order wins - the same convention as the JVM writer.
            return value.IsMap();
        }
        default:
            // Avro disallows a union directly inside a union.
            return false;
    }
}

[[noreturn]] void refuseField(const std::string &field,
                              const ::avro::NodePtr &field_schema,
                              const google::api::expr::runtime::CelValue &value) {
    throw std::runtime_error(
        "cannot write " + std::string(utils::celTypeName(value)) + " to field '" + field +
        "', which is " + ::avro::toString(field_schema->type()));
}

/// The datum `toAvroValue` converts a returned value against, or a rule error if the field
/// cannot hold what the rule returned.
///
/// Two things were wrong here, and one function fixes both because both are the same question:
/// which schema does this value get written against?
///
/// `toAvroValue` reads the field's logical type, scale and schema off the datum it is handed,
/// and a union datum forwards all three to its *currently selected* branch. So the field's own
/// value is only a usable template when it already holds the branch the returned value belongs
/// in: a nullable decimal that is presently null offers no scale, which turned a computed
/// `decimal('1.23')` into an error against a phantom scale of 0 (and a computed timestamp into
/// a silently dropped field). Resolving the branch from the value first is what the JVM
/// writer's resolveUnion does before it recurses into the branch schema.
///
/// And `toAvroValue` dispatches on the *CEL value*, not on the schema, with a trailing
/// `return original` for anything it does not recognise - so a value the field could not hold
/// was never reported. It went one of two ways, neither an error: the value was discarded and
/// the field kept its input (a timestamp, list or map returned for a string field), or a datum
/// of the CEL value's own type was written into the slot, giving a record that no longer
/// matches its schema (an int returned for a string field, a string for a long). The JVM
/// dispatches on the schema instead and throws `typeMismatch` for every one of these, so the
/// value is checked against the field's schema here before any of it runs.
::avro::GenericDatum conversionTemplate(
    const std::string &field, const ::avro::NodePtr &field_schema,
    const ::avro::GenericDatum &original,
    const google::api::expr::runtime::CelValue &cel_value) {
    if (field_schema->type() != ::avro::AVRO_UNION) {
        if (!branchAcceptsCel(field_schema, cel_value)) {
            refuseField(field, field_schema, cel_value);
        }
        return original;
    }
    for (size_t branch = 0; branch < field_schema->leaves(); ++branch) {
        if (!branchAcceptsCel(field_schema->leafAt(branch), cel_value)) {
            continue;
        }
        if (original.isUnion() && original.unionBranch() == branch) {
            return original;  // already this branch: keep the field's own datum
        }
        return ::avro::GenericDatum(field_schema->leafAt(branch));
    }
    // No branch can hold it, which is an UnresolvedUnionException on the JVM.
    refuseField(field, field_schema, cel_value);
}

::avro::GenericDatum wrapForUnionField(const ::avro::NodePtr &field_schema,
                                       const ::avro::GenericDatum &value) {
    if (field_schema->type() != ::avro::AVRO_UNION || value.isUnion()) {
        return value;
    }
    for (size_t branch = 0; branch < field_schema->leaves(); ++branch) {
        if (!branchAccepts(field_schema->leafAt(branch), value)) {
            continue;
        }
        ::avro::GenericDatum out{field_schema};
        out.selectBranch(branch);
        // `value<T>()` writes into the selected branch, which is how avro-cpp's own
        // GenericReader fills a union (see impl/Generic.cc).
        switch (value.type()) {
            case ::avro::AVRO_NULL:
                break;  // selectBranch already left it null
            case ::avro::AVRO_BOOL:
                out.value<bool>() = value.value<bool>();
                break;
            case ::avro::AVRO_INT:
                out.value<int32_t>() = value.value<int32_t>();
                break;
            case ::avro::AVRO_LONG:
                out.value<int64_t>() = value.value<int64_t>();
                break;
            case ::avro::AVRO_FLOAT:
                out.value<float>() = value.value<float>();
                break;
            case ::avro::AVRO_DOUBLE:
                out.value<double>() = value.value<double>();
                break;
            case ::avro::AVRO_STRING:
                out.value<std::string>() = value.value<std::string>();
                break;
            case ::avro::AVRO_BYTES:
                out.value<std::vector<uint8_t>>() =
                    value.value<std::vector<uint8_t>>();
                break;
            case ::avro::AVRO_FIXED:
                out.value<::avro::GenericFixed>() =
                    value.value<::avro::GenericFixed>();
                break;
            case ::avro::AVRO_ENUM:
                out.value<::avro::GenericEnum>() =
                    value.value<::avro::GenericEnum>();
                break;
            case ::avro::AVRO_RECORD:
                out.value<::avro::GenericRecord>() =
                    value.value<::avro::GenericRecord>();
                break;
            case ::avro::AVRO_ARRAY:
                out.value<::avro::GenericArray>() =
                    value.value<::avro::GenericArray>();
                break;
            case ::avro::AVRO_MAP:
                out.value<::avro::GenericMap>() =
                    value.value<::avro::GenericMap>();
                break;
            default:
                // Nothing else can come out of toAvroValue; leave the value unwrapped rather
                // than silently writing a wrong branch.
                return value;
        }
        return out;
    }
    return value;
}

}  // namespace

::avro::GenericDatum recordFromCelMap(
    const ::avro::GenericDatum &original,
    const google::api::expr::runtime::CelMap &cel_map_ref) {
    const auto *cel_map = &cel_map_ref;
    auto orig_record_schema =
        original.value<::avro::GenericRecord>().schema();
    ::avro::GenericDatum result_datum{
        ::avro::ValidSchema(orig_record_schema)};
    auto &result_record = result_datum.value<::avro::GenericRecord>();

    // Replace, not merge: the map *is* the new record, so a field the rule does not
    // name is not carried over from the input. Seeding from `original` here would be
    // the natural thing for a converter whose job is "convert this value against a
    // template" - which is what this function is for a field value - but it is the
    // wrong semantics for a message-level transform, and it made C++ the only client
    // that merged. See the JVM AvroResultWriter.convertRecord, which builds a fresh
    // GenericRecordBuilder and sets only the keys the map carries.
    auto &orig_record = original.value<::avro::GenericRecord>();
    std::vector<bool> named(orig_record.fieldCount(), false);

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
                            // The template is a *shape* for the conversion (it
                            // carries the logical type, scale and unit); its value
                            // is not used.
                            auto field_template = conversionTemplate(
                                key, orig_record_schema->leafAt(field_idx),
                                orig_record.fieldAt(field_idx),
                                value_lookup.value());
                            result_record.setFieldAt(
                                field_idx,
                                wrapForUnionField(
                                    orig_record_schema->leafAt(field_idx),
                                    toAvroValue(field_template,
                                                value_lookup.value())));
                            named[field_idx] = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    // An unnamed field takes the schema's declared default, or is an error - which is
    // what GenericRecordBuilder.build() does on the JVM side. Avro has no notion of an
    // absent field, so there is no third option: leaving the zero value would be a
    // silent wrong answer for the case this whole branch exists to get right.
    for (size_t field_idx = 0; field_idx < orig_record.fieldCount();
         ++field_idx) {
        if (named[field_idx]) {
            continue;
        }
        const ::avro::GenericDatum &declared =
            orig_record_schema->defaultValueAt(field_idx);
        // avro-cpp stores a null-typed datum both for "default: null" and for "no
        // default at all", so the two are told apart by whether the field can hold
        // null. That is more forgiving than the JVM in one corner - a nullable field
        // with no declared default gets null here and throws there - and identical
        // everywhere else.
        if (declared.type() != ::avro::AVRO_NULL ||
            avroFieldAcceptsNull(orig_record_schema->leafAt(field_idx))) {
            // Wrapped for the same reason as a named field: avro-cpp stores a union field's
            // declared default as a bare datum, and a bare datum in a union slot encodes
            // without a branch index.
            result_record.setFieldAt(
                field_idx,
                wrapForUnionField(orig_record_schema->leafAt(field_idx), declared));
            continue;
        }
        throw std::runtime_error(
            "CEL transform result does not set field '" +
            orig_record_schema->nameAt(field_idx) +
            "', which has no default value");
    }

    return result_datum;
}

}  // namespace schemaregistry::rules::cel::utils
