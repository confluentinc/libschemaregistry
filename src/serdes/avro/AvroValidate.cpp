#include <avro/Generic.hh>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "schemaregistry/serdes/ValidationRule.h"
#include "schemaregistry/serdes/avro/AvroTypes.h"
#include "schemaregistry/serdes/avro/AvroUtils.h"

namespace schemaregistry::serdes::avro::utils {

namespace {

/**
 * Named record schemas encountered so far, keyed by fully-qualified name, so
 * that a field declared as a bare type name (e.g. "com.example.Address") can be
 * resolved back to the schema object carrying its inline rules. Avro requires a
 * name to be defined before it is referenced, and a record's own name is
 * registered before its fields are walked, so recursive types resolve too.
 */
using NamedSchemas = std::unordered_map<std::string, const nlohmann::json *>;

struct Walker {
    ValidationRuleExecutor &executor;
    bool fail_fast;
    std::vector<ValidationRuleError> &violations;
    NamedSchemas named;

    bool done() const { return fail_fast && !violations.empty(); }

    /**
     * Evaluate the inline rules declared on schema against datum. Skips null
     * values, honoring the skip-on-null contract.
     */
    bool evaluateRules(const nlohmann::json &schema,
                       const ::avro::GenericDatum &datum,
                       const std::string &path) {
        if (datum.type() == ::avro::AVRO_NULL) {
            return false;
        }
        auto rules_it = schema.find(VALIDATION_RULES_PROP);
        if (rules_it == schema.end()) {
            return false;
        }
        auto rules = parseValidationRules(*rules_it);
        if (rules.empty()) {
            return false;
        }
        auto value = makeAvroValue(datum);
        for (const auto &rule : rules) {
            evaluateValidationRule(executor, rule, *value, path, violations);
            if (done()) {
                return true;
            }
        }
        return false;
    }

    /**
     * Resolve a schema node to the object that may carry inline rules: a bare
     * string is either a primitive type or a reference to a named schema.
     */
    const nlohmann::json *resolve(const nlohmann::json &schema,
                                  const std::string &ns) {
        if (!schema.is_string()) {
            return &schema;
        }
        std::string name = schema.get<std::string>();
        // Avro resolves an unqualified name against the enclosing namespace first,
        // and only a dotted name is already fully qualified. Try the namespace-
        // qualified name before the bare/global one so that, with both a global
        // Address and a foo.Address defined, a reference to "Address" inside
        // namespace foo picks foo.Address.
        if (!ns.empty() && name.find('.') == std::string::npos) {
            auto qualified = named.find(ns + "." + name);
            if (qualified != named.end()) {
                return qualified->second;
            }
        }
        auto it = named.find(name);
        if (it != named.end()) {
            return it->second;
        }
        return nullptr;
    }

    void walk(const nlohmann::json &schema, const std::string &ns,
              const ::avro::GenericDatum &datum, const std::string &path);

    /**
     * Registers every named definition reachable from schema, without
     * evaluating rules.
     * Used to seed the walk with the referenced schemas.
     */
    void collectNamed(const nlohmann::json &schema, const std::string &ns);

    void walkRecord(const nlohmann::json &schema, const std::string &ns,
                    const ::avro::GenericDatum &datum, const std::string &path);
};

void Walker::walkRecord(const nlohmann::json &schema, const std::string &ns,
                        const ::avro::GenericDatum &datum,
                        const std::string &path) {
    std::string record_ns = ns;
    auto ns_it = schema.find("namespace");
    if (ns_it != schema.end() && ns_it->is_string()) {
        record_ns = ns_it->get<std::string>();
    }

    auto name_it = schema.find("name");
    if (name_it != schema.end() && name_it->is_string()) {
        std::string name = name_it->get<std::string>();
        // A name containing a dot is already fully qualified and also carries
        // its own namespace for nested definitions.
        auto dot = name.rfind('.');
        if (dot != std::string::npos) {
            record_ns = name.substr(0, dot);
            named[name] = &schema;
        } else {
            named[record_ns.empty() ? name : record_ns + "." + name] = &schema;
            if (record_ns.empty()) {
                named[name] = &schema;
            }
        }
    }

    // Record-level rules: `this` is the record itself.
    if (evaluateRules(schema, datum, path)) {
        return;
    }

    auto fields_it = schema.find("fields");
    if (fields_it == schema.end() || !fields_it->is_array() ||
        datum.type() != ::avro::AVRO_RECORD) {
        return;
    }
    const auto &record = datum.value<::avro::GenericRecord>();

    for (const auto &field : *fields_it) {
        if (!field.is_object()) {
            continue;
        }
        auto field_name_it = field.find("name");
        auto field_type_it = field.find("type");
        if (field_name_it == field.end() || !field_name_it->is_string() ||
            field_type_it == field.end()) {
            continue;
        }
        std::string field_name = field_name_it->get<std::string>();
        if (!record.hasField(field_name)) {
            continue;
        }
        const auto &field_datum = record.field(field_name);
        std::string field_path = appendValidationPath(path, field_name);

        // Field-level rules: `this` is the field value.
        if (evaluateRules(field, field_datum, field_path)) {
            return;
        }
        walk(*field_type_it, record_ns, field_datum, field_path);
        if (done()) {
            return;
        }
    }
}

void Walker::collectNamed(const nlohmann::json &schema, const std::string &ns) {
    if (schema.is_array()) {
        for (const auto &variant : schema) {
            collectNamed(variant, ns);
        }
        return;
    }
    if (!schema.is_object()) {
        return;
    }
    auto type_it = schema.find("type");
    if (type_it == schema.end()) {
        return;
    }
    if (!type_it->is_string()) {
        collectNamed(*type_it, ns);
        return;
    }
    std::string type = type_it->get<std::string>();
    if (type == "array") {
        auto items_it = schema.find("items");
        if (items_it != schema.end()) {
            collectNamed(*items_it, ns);
        }
        return;
    }
    if (type == "map") {
        auto values_it = schema.find("values");
        if (values_it != schema.end()) {
            collectNamed(*values_it, ns);
        }
        return;
    }
    if (type != "record") {
        return;
    }

    std::string record_ns = ns;
    std::string record_name;
    auto name_it = schema.find("name");
    if (name_it != schema.end() && name_it->is_string()) {
        record_name = name_it->get<std::string>();
    }
    if (record_name.find('.') != std::string::npos) {
        record_ns = impliedNamespace(record_name);
        named[record_name] = &schema;
    } else {
        auto ns_it = schema.find("namespace");
        if (ns_it != schema.end() && ns_it->is_string()) {
            record_ns = ns_it->get<std::string>();
        }
        if (!record_name.empty()) {
            named[record_ns.empty() ? record_name
                                    : record_ns + "." + record_name] = &schema;
            if (record_ns.empty()) {
                named[record_name] = &schema;
            }
        }
    }

    auto fields_it = schema.find("fields");
    if (fields_it == schema.end() || !fields_it->is_array()) {
        return;
    }
    for (const auto &field : *fields_it) {
        if (!field.is_object()) {
            continue;
        }
        auto field_type_it = field.find("type");
        if (field_type_it != field.end()) {
            collectNamed(*field_type_it, record_ns);
        }
    }
}

void Walker::walk(const nlohmann::json &schema, const std::string &ns,
                  const ::avro::GenericDatum &datum, const std::string &path) {
    if (done()) {
        return;
    }

    if (schema.is_array()) {
        // A union: descend into the branch the datum actually holds.
        size_t branch = datum.isUnion() ? datum.unionBranch() : 0;
        if (branch < schema.size()) {
            walk(schema[branch], ns, datum, path);
        }
        return;
    }

    const nlohmann::json *resolved = resolve(schema, ns);
    if (resolved == nullptr || !resolved->is_object()) {
        return;
    }
    const nlohmann::json &node = *resolved;

    auto type_it = node.find("type");
    if (type_it == node.end()) {
        return;
    }
    // A field's "type" may itself be a union or a named reference rather than a
    // type keyword.
    if (!type_it->is_string()) {
        walk(*type_it, ns, datum, path);
        return;
    }
    std::string type = type_it->get<std::string>();

    if (type == "record") {
        walkRecord(node, ns, datum, path);
    } else if (type == "array") {
        auto items_it = node.find("items");
        if (items_it == node.end() || datum.type() != ::avro::AVRO_ARRAY) {
            return;
        }
        const auto &array = datum.value<::avro::GenericArray>().value();
        for (size_t i = 0; i < array.size(); ++i) {
            walk(*items_it, ns, array[i], path + "[" + std::to_string(i) + "]");
            if (done()) {
                return;
            }
        }
    } else if (type == "map") {
        auto values_it = node.find("values");
        if (values_it == node.end() || datum.type() != ::avro::AVRO_MAP) {
            return;
        }
        const auto &map = datum.value<::avro::GenericMap>().value();
        for (const auto &entry : map) {
            walk(*values_it, ns, entry.second,
                 path + "[\"" + entry.first + "\"]");
            if (done()) {
                return;
            }
        }
    }
    // Anything else is a primitive, enum or fixed: no children to descend into,
    // and its rules were evaluated by the declaring record.
}

}  // namespace

std::vector<ValidationRuleError> validateMessage(
    ValidationRuleExecutor &executor, const nlohmann::json &schema,
    const std::vector<nlohmann::json> &named_schemas,
    const ::avro::GenericDatum &datum, bool fail_fast) {
    std::vector<ValidationRuleError> violations;
    Walker walker{executor, fail_fast, violations, {}};
    // Index the referenced schemas first: a field may name a record defined in
    // another subject, and without their definitions those rules would be
    // silently skipped.
    for (const auto &named : named_schemas) {
        walker.collectNamed(named, "");
    }
    walker.walk(schema, "", datum, "");
    return violations;
}

}  // namespace schemaregistry::serdes::avro::utils
