#include <nlohmann/json.hpp>
#include <map>
#include <memory>
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
    /// Compiled branch matchers, keyed by the branch's serialized form.
    mutable std::map<
        std::string,
        std::shared_ptr<jsoncons::jsonschema::json_schema<jsoncons::ojson>>>
        matcher_cache;

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
     * The key the root document travels under inside a standalone branch document.
     * Chosen to be one no schema would use.
     */
    static constexpr const char *kRootKey = "__confluent_root__";

    /**
     * Repoints every same-document reference at the copy of the root carried under
     * kRootKey.
     *
     * A "#/..." reference is resolved against the document root, so a branch lifted out
     * of its schema cannot follow one - "#/properties/shared" would be looked up in the
     * branch. Rewriting them, in the branch and in the carried root alike, keeps every
     * such reference pointing at the same node it named in the original document.
     *
     * References that name another document, or use $anchor or $dynamicRef, are left
     * alone; they resolve the same way in either document.
     */
    static void repointFragmentRefs(nlohmann::json &node) {
        if (node.is_object()) {
            for (auto &[key, child] : node.items()) {
                if (key == "$ref" && child.is_string()) {
                    const std::string ref = child.get<std::string>();
                    if (ref == "#") {
                        child = std::string("#/") + kRootKey;
                    } else if (ref.rfind("#/", 0) == 0) {
                        child = std::string("#/") + kRootKey + "/" + ref.substr(2);
                    }
                    continue;
                }
                repointFragmentRefs(child);
            }
        } else if (node.is_array()) {
            for (auto &element : node) {
                repointFragmentRefs(element);
            }
        }
    }

    /**
     * A compiled schema that matches the subschema alone, with the root document still
     * reachable so that references resolve, or null when it cannot be compiled.
     *
     * Compiling is far more expensive than validating, and the same branch is matched
     * once per value the walk reaches, so the result is kept for the walk's lifetime.
     */
    std::shared_ptr<jsoncons::jsonschema::json_schema<jsoncons::ojson>> matcherFor(
        const nlohmann::json &subschema) const {
        const std::string key = subschema.dump();
        auto cached = matcher_cache.find(key);
        if (cached != matcher_cache.end()) {
            return cached->second;
        }

        std::shared_ptr<jsoncons::jsonschema::json_schema<jsoncons::ojson>>
            compiled;
        try {
            nlohmann::json standalone = subschema;
            repointFragmentRefs(standalone);
            standalone[kRootKey] = root;
            repointFragmentRefs(standalone[kRootKey]);
            // The dialect belongs to the document, not to the branch, and decides how
            // the branch's own keywords are read.
            auto schema_it = root.find("$schema");
            if (schema_it != root.end() && !standalone.contains("$schema")) {
                standalone["$schema"] = *schema_it;
            }
            compiled = std::make_shared<
                jsoncons::jsonschema::json_schema<jsoncons::ojson>>(
                jsoncons::jsonschema::make_json_schema(
                    utils::jsonToOJson(standalone)));
        } catch (const std::exception &) {
            compiled = nullptr;
        }
        matcher_cache.emplace(key, compiled);
        return compiled;
    }

    /**
     * Whether the instance satisfies the subschema, used to pick the live branch of a
     * oneOf/anyOf. Branches routinely share a JSON type and differ by const, required,
     * ranges and so on, so the instance is validated against the whole subschema; a
     * cheaper type comparison would evaluate rules from branches the value does not
     * follow and reject valid messages.
     *
     * A branch that cannot be compiled falls back to comparing types rather than
     * reporting no match: reporting no match would skip every rule in the branch the
     * value actually follows, and skip it silently.
     */
    bool subschemaMatches(const nlohmann::json &subschema,
                          const nlohmann::json &value) const {
        auto compiled = matcherFor(subschema);
        if (compiled == nullptr) {
            return typeMatches(subschema, value);
        }
        try {
            return compiled->is_valid(utils::jsonToOJson(value));
        } catch (const std::exception &) {
            return typeMatches(subschema, value);
        }
    }

    /**
     * A structural comparison of a schema's declared type against the instance. Used
     * only when a branch cannot be compiled; see subschemaMatches.
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
