/**
 * Rebuilds an Avro record from the map a message-level `CEL` transform returned.
 * See AvroResultWriter.h for the contract, and for why it is not a branch inside toAvroValue.
 */

#include "schemaregistry/rules/cel/AvroResultWriter.h"

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
                            // The original field is a *shape* template for the
                            // conversion (it carries the logical type, scale and
                            // unit); its value is not used.
                            auto field_template =
                                orig_record.fieldAt(field_idx);
                            result_record.setFieldAt(
                                field_idx,
                                toAvroValue(field_template,
                                            value_lookup.value()));
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
            result_record.setFieldAt(field_idx, declared);
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
