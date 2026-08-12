#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "schemaregistry/serdes/ValidationRule.h"
#include "schemaregistry/serdes/json/JsonUtils.h"
#include "schemaregistry/serdes/json/JsonValue.h"

namespace schemaregistry::serdes::json::utils {

namespace {

struct Walker {
    ValidationRuleExecutor &executor;
    bool fail_fast;
    std::vector<ValidationRuleError> &violations;
    const nlohmann::json &root;

    bool done() const { return fail_fast && !violations.empty(); }

    /**
     * Follow a local "$ref" ("#/definitions/Foo") to the schema it names.
     * Non-local refs are left unresolved — the schema node is returned as-is.
     */
    const nlohmann::json *resolveRef(const nlohmann::json &schema) const {
        if (!schema.is_object()) {
            return &schema;
        }
        auto ref_it = schema.find("$ref");
        if (ref_it == schema.end() || !ref_it->is_string()) {
            return &schema;
        }
        std::string ref = ref_it->get<std::string>();
        if (ref.empty() || ref[0] != '#') {
            return &schema;
        }
        try {
            return &root.at(nlohmann::json::json_pointer(ref.substr(1)));
        } catch (const nlohmann::json::exception &) {
            return &schema;
        }
    }

    /**
     * Evaluate the inline rules declared on schema against value. Skips null
     * values, honoring the skip-on-null contract.
     */
    bool evaluateRules(const nlohmann::json &schema,
                       const nlohmann::json &value, const std::string &path) {
        if (value.is_null() || !schema.is_object()) {
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
        auto serde_value = makeJsonValue(value);
        for (const auto &rule : rules) {
            evaluateValidationRule(executor, rule, *serde_value, path,
                                   violations);
            if (done()) {
                return true;
            }
        }
        return false;
    }

    /**
     * Whether the instance could plausibly be described by the subschema, used
     * to pick the live branch of a oneOf/anyOf. Only the declared "type" is
     * considered: a full validation would need a compiled validator per branch,
     * and getting this wrong would attribute violations to a branch the value
     * does not follow.
     */
    static bool typeMatches(const nlohmann::json &schema,
                            const nlohmann::json &value) {
        if (!schema.is_object()) {
            return false;
        }
        auto type_it = schema.find("type");
        if (type_it == schema.end()) {
            // No declared type; a $ref or bare property set still describes an
            // object.
            return schema.contains("$ref") || schema.contains("properties")
                       ? value.is_object()
                       : false;
        }
        auto matches = [&value](const std::string &type) {
            if (type == "object") return value.is_object();
            if (type == "array") return value.is_array();
            if (type == "string") return value.is_string();
            if (type == "boolean") return value.is_boolean();
            if (type == "null") return value.is_null();
            if (type == "integer") return value.is_number_integer();
            if (type == "number") return value.is_number();
            return false;
        };
        if (type_it->is_string()) {
            return matches(type_it->get<std::string>());
        }
        if (type_it->is_array()) {
            for (const auto &type : *type_it) {
                if (type.is_string() && matches(type.get<std::string>())) {
                    return true;
                }
            }
        }
        return false;
    }

    void walk(const nlohmann::json &schema, const nlohmann::json &value,
              const std::string &path);
};

void Walker::walk(const nlohmann::json &schema, const nlohmann::json &value,
                  const std::string &path) {
    if (done()) {
        return;
    }
    const nlohmann::json &node = *resolveRef(schema);
    if (!node.is_object()) {
        return;
    }

    // Rules declared at this level: `this` is the value at this location.
    if (evaluateRules(node, value, path)) {
        return;
    }

    // allOf branches all apply; for oneOf/anyOf only the branches whose type
    // matches the instance do.
    for (const char *keyword : {"allOf", "oneOf", "anyOf"}) {
        auto branches_it = node.find(keyword);
        if (branches_it == node.end() || !branches_it->is_array()) {
            continue;
        }
        bool all = std::string(keyword) == "allOf";
        for (const auto &branch : *branches_it) {
            if (all || typeMatches(*resolveRef(branch), value)) {
                walk(branch, value, path);
                if (done()) {
                    return;
                }
            }
        }
    }

    auto properties_it = node.find("properties");
    if (properties_it != node.end() && properties_it->is_object() &&
        value.is_object()) {
        for (const auto &[name, property_schema] : properties_it->items()) {
            auto value_it = value.find(name);
            if (value_it == value.end()) {
                continue;
            }
            walk(property_schema, *value_it, appendValidationPath(path, name));
            if (done()) {
                return;
            }
        }
    }

    auto items_it = node.find("items");
    if (items_it != node.end() && items_it->is_object() && value.is_array()) {
        for (size_t i = 0; i < value.size(); ++i) {
            walk(*items_it, value[i], path + "[" + std::to_string(i) + "]");
            if (done()) {
                return;
            }
        }
    }
}

}  // namespace

std::vector<ValidationRuleError> validateMessage(
    ValidationRuleExecutor &executor, const nlohmann::json &schema,
    const nlohmann::json &value, bool fail_fast) {
    std::vector<ValidationRuleError> violations;
    Walker walker{executor, fail_fast, violations, schema};
    walker.walk(schema, value, "$");
    return violations;
}

}  // namespace schemaregistry::serdes::json::utils
