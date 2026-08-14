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
     * Follow a "$ref" to the schema it names. References are flattened into
     * local pointers before the walk, so a ref that cannot be followed means
     * the schema is broken or refers somewhere we cannot see — which throws
     * rather than being treated as a node carrying no rules, since silently
     * skipping the referenced subtree would let a message serialize without the
     * checks the reference was there to supply.
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
            throw JsonError("cannot resolve reference '" + ref +
                            "' while validating inline rules");
        }
        try {
            return &root.at(nlohmann::json::json_pointer(ref.substr(1)));
        } catch (const nlohmann::json::exception &e) {
            throw JsonError("cannot resolve reference '" + ref +
                            "' while validating inline rules: " + e.what());
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
     * Whether the instance satisfies the subschema, used to pick the live
     * branch of a
     * oneOf/anyOf. Branches routinely share a JSON type and differ by const,
     * required,
     * ranges and so on, so the instance is validated against the whole
     * subschema; a
     * cheaper type comparison would evaluate rules from branches the value does
     * not
     * follow and reject valid messages.
     *
     * The root's definitions travel with the subschema, since references are
     * flattened
     * into them and a branch may point at one.
     */
    bool subschemaMatches(const nlohmann::json &subschema,
                          const nlohmann::json &value) const {
        try {
            nlohmann::json standalone = subschema;
            for (const char *defs : {"definitions", "$defs"}) {
                auto it = root.find(defs);
                if (it != root.end() && !standalone.contains(defs)) {
                    standalone[defs] = *it;
                }
            }
            auto compiled = jsoncons::jsonschema::make_json_schema(
                utils::jsonToOJson(standalone));
            return compiled.is_valid(utils::jsonToOJson(value));
        } catch (const std::exception &) {
            // An uncompilable branch cannot be the one the value follows.
            return false;
        }
    }

    /**
     * Unused legacy helper kept out of the walk; see subschemaMatches.
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
        bool one = std::string(keyword) == "oneOf";
        for (const auto &branch : *branches_it) {
            if (all || subschemaMatches(*resolveRef(branch), value)) {
                walk(branch, value, path);
                if (done()) {
                    return;
                }
                if (one) {
                    // oneOf has a single live branch; stop at the one that
                    // matched.
                    break;
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
