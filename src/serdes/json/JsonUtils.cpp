#include "schemaregistry/serdes/json/JsonUtils.h"

#include <algorithm>
#include <exception>
#include <sstream>

#include "schemaregistry/serdes/RuleRegistry.h"  // For global_registry functions

namespace schemaregistry::serdes::json::utils {

// Schema resolution implementations
namespace schema_resolution {

std::unordered_map<std::string, nlohmann::json> resolveNamedSchema(
    const schemaregistry::rest::model::Schema &schema,
    std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
    std::unordered_set<std::string> &visited) {
    std::unordered_map<std::string, nlohmann::json> resolved_schemas;

    auto references = schema.getReferences();
    if (!references.has_value()) {
        return resolved_schemas;
    }

    for (const auto &ref : references.value()) {
        auto name_opt = ref.getName();
        auto subject_opt = ref.getSubject();
        auto version_opt = ref.getVersion();

        if (!name_opt.has_value() || !subject_opt.has_value()) {
            continue;
        }

        std::string name = name_opt.value();
        if (visited.find(name) != visited.end()) {
            continue;  // Avoid cycles
        }
        visited.insert(name);

        try {
            auto registered_schema = client->getVersion(
                subject_opt.value(), version_opt.value_or(-1), true,
                std::nullopt);

            auto ref_schema = registered_schema.toSchema();
            auto ref_schema_str = ref_schema.getSchema();

            if (ref_schema_str.has_value()) {
                nlohmann::json parsed_ref_schema =
                    nlohmann::json::parse(ref_schema_str.value());
                resolved_schemas[name] = parsed_ref_schema;

                // Recursively resolve dependencies
                auto nested_deps =
                    resolveNamedSchema(ref_schema, client, visited);
                resolved_schemas.insert(nested_deps.begin(), nested_deps.end());
            }
        } catch (const std::exception &e) {
            // Skip missing references
        }
    }

    return resolved_schemas;
}

}  // namespace schema_resolution

// Value transformation implementations
namespace value_transform {

nlohmann::json transformFields(
    RuleContext &ctx,
    std::shared_ptr<jsoncons::jsonschema::json_schema<jsoncons::ojson>> schema,
    const nlohmann::json &value) {
    // Convert nlohmann::json to jsoncons::ojson for processing
    auto jsoncons_value = jsonToOJson(value);

    // Create a mutable copy for transformation
    auto mutable_value = jsoncons_value;

    // Track visited locations to avoid duplicate transformations
    std::unordered_set<std::string> visited_locations;

    // A field whose transform fails must not be passed through unchanged: for a
    // field-encryption rule that would emit the plaintext and report success. The walk
    // callback cannot throw through jsoncons, so the first failure is captured, the walk is
    // aborted, and it is rethrown to the caller below.
    std::exception_ptr failure;

    try {
        schema->walk(
            mutable_value,
            [&ctx, &mutable_value, &visited_locations, &failure](
                const std::string &keyword, const jsoncons::ojson &schema_node,
                const jsoncons::uri &schema_location,
                const jsoncons::ojson &instance_node,
                const jsoncons::jsonpointer::json_pointer &instance_location)
                -> jsoncons::jsonschema::walk_result {
                try {
                    std::string schema_location_str = schema_location.string();
                    std::string instance_location_str =
                        instance_location.to_string();

                    if ((schema_navigation::isObjectSchema(schema_node) ||
                        schema_node.contains("properties")) &&
                        instance_node.is_object()) {
                        auto properties =
                            schema_navigation::getSchemaProperties(schema_node);

                        for (const auto &[key, field_value] :
                             instance_node.object_range()) {
                            if (properties.contains(key)) {
                                // Create unique location identifier for this
                                // field
                                auto field_location = instance_location;
                                field_location /= key;
                                std::string field_location_str =
                                    field_location.to_string();

                                // Skip if we've already processed this location
                                if (visited_locations.find(
                                        field_location_str) !=
                                    visited_locations.end()) {
                                    continue;
                                }

                                // Mark this location as visited
                                visited_locations.insert(field_location_str);

                                std::string input =
                                    field_value.is_string()
                                        ? field_value.as_string()
                                        : "";
                                std::string field_path =
                                    path_utils::appendToPath(
                                        instance_location.to_string(), key);
                                auto transformed_value =
                                    transformFieldWithContext(
                                        ctx, properties[key], field_path,
                                        field_value);

                                std::string output =
                                    transformed_value.is_string()
                                        ? transformed_value.as_string()
                                        : "";

                                // Update the mutable_value using the JSON
                                // pointer. A transformed value that cannot be
                                // written back is a failure, not something to
                                // skip - the field would keep its original
                                // value.
                                jsoncons::jsonpointer::replace(
                                    mutable_value, field_location,
                                    transformed_value);
                            }
                        }
                    }
                } catch (...) {
                    failure = std::current_exception();
                    return jsoncons::jsonschema::walk_result::abort;
                }

                return jsoncons::jsonschema::walk_result::advance;
            });
    } catch (...) {
        // The walk itself failed rather than a field within it.
        if (!failure) {
            failure = std::current_exception();
        }
    }

    if (failure) {
        std::rethrow_exception(failure);
    }
    return ojsonToJson(mutable_value);
}

jsoncons::ojson transformFieldWithContext(RuleContext &ctx,
                                          const jsoncons::ojson &schema,
                                          const std::string &path,
                                          const jsoncons::ojson &value) {
    // Get field type from schema
    FieldType field_type = schema_navigation::getFieldType(schema);

    // Get field name from path
    std::string field_name = path_utils::getFieldName(path);

    // Create message value from the JSON value
    auto message_value = makeJsonValue(value);

    // Get inline tags from schema
    std::unordered_set<std::string> inline_tags =
        schema_navigation::getConfluentTags(schema);

    // Enter field context
    ctx.enterField(*message_value, path, field_name, field_type, inline_tags);

    try {
        // Transform the field value (synchronous call)
        jsoncons::ojson new_value = transform(ctx, schema, path, value);

        // Check for condition rules
        auto rule_kind = ctx.getRule().getKind();
        if (rule_kind.has_value() && rule_kind.value() == Kind::Condition) {
            if (new_value.is_bool()) {
                bool condition_result = new_value.as<bool>();
                if (!condition_result) {
                    throw JsonError("Rule condition failed for field: " +
                                    field_name);
                }
            }
        }

        ctx.exitField();
        return new_value;

    } catch (const std::exception &e) {
        ctx.exitField();
        throw;
    }
}

jsoncons::ojson transform(RuleContext &ctx, const jsoncons::ojson &schema,
                          const std::string &path,
                          const jsoncons::ojson &value) {
    // Field-level transformation logic
    auto field_ctx = ctx.currentField();
    if (field_ctx.has_value()) {
        field_ctx->setFieldType(schema_navigation::getFieldType(schema));

        auto rule_tags = ctx.getRule().getTags();
        std::unordered_set<std::string> rule_tags_set;
        if (rule_tags.has_value()) {
            rule_tags_set = std::unordered_set<std::string>(rule_tags->begin(),
                                                            rule_tags->end());
        }

        // Check if rule tags overlap with field context tags (empty
        // rule_tags means apply to all)
        bool should_apply = !rule_tags.has_value() || rule_tags_set.empty();
        if (!should_apply) {
            const auto &field_tags = field_ctx->getTags();
            for (const auto &field_tag : field_tags) {
                if (rule_tags_set.find(field_tag) != rule_tags_set.end()) {
                    should_apply = true;
                    break;
                }
            }
        }

        if (should_apply) {
            auto message_value = makeJsonValue(value);

            // Get field executor type from the rule
            auto field_executor_type = ctx.getRule().getType().value_or("");

            // Try to get executor from context's rule registry first,
            // then global
            std::shared_ptr<RuleExecutor> executor;
            if (ctx.getRuleRegistry()) {
                executor =
                    ctx.getRuleRegistry()->getExecutor(field_executor_type);
            }
            if (!executor) {
                executor =
                    global_registry::getRuleExecutor(field_executor_type);
            }

            if (executor) {
                auto field_executor =
                    std::dynamic_pointer_cast<FieldRuleExecutor>(executor);
                if (!field_executor) {
                    throw JsonError("executor " + field_executor_type +
                                    " is not a field rule executor");
                }

                auto new_value =
                    field_executor->transformField(ctx, *message_value);
                if (new_value && new_value->getFormat() == SerdeFormat::Json) {
                    return asOJson(*new_value);
                }
            }
        }
    }

    return value;
}

}  // namespace value_transform

// Schema navigation implementations
namespace schema_navigation {

namespace {

/// Maps a single JSON Schema type keyword. Null for anything this walk cannot act on -
/// never String, which would hand an encryption rule a number or an object to encrypt as
/// text.
FieldType scalarFieldType(const std::string &type, bool has_properties) {
    if (type == "object") {
        // An object with no declared properties is an open map.
        return has_properties ? FieldType::Record : FieldType::Map;
    }
    if (type == "array") return FieldType::Array;
    if (type == "string") return FieldType::String;
    if (type == "integer") return FieldType::Int;
    if (type == "number") return FieldType::Double;
    if (type == "boolean") return FieldType::Boolean;
    return FieldType::Null;
}

}  // namespace

FieldType getFieldType(const jsoncons::ojson &schema) {
    const bool has_properties = schema.contains("properties") &&
                                schema["properties"].is_object() &&
                                !schema["properties"].empty();
    const bool has_combined = schema.contains("allOf") ||
                              schema.contains("anyOf") ||
                              schema.contains("oneOf");

    if (!schema.contains("type")) {
        if (has_properties) return FieldType::Record;
        // A node built only from allOf/anyOf/oneOf is not any single type. Its branches
        // are separate nodes and the walk types each of them on its own.
        if (has_combined) return FieldType::Combined;
        return FieldType::Null;
    }

    // A nullable field is written ["string", "null"], and the type that matters is the one
    // that is not null - the JVM and Go clients reach the same place by narrowing the union
    // to the branch the value matches. Only a union with several real types has no single
    // type to act on.
    if (schema["type"].is_array()) {
        std::string single;
        int count = 0;
        for (const auto &entry : schema["type"].array_range()) {
            if (!entry.is_string()) continue;
            std::string type = entry.as<std::string>();
            if (type == "null") continue;
            single = type;
            ++count;
        }
        if (count == 0) return FieldType::Null;
        if (count > 1) return FieldType::Combined;
        return scalarFieldType(single, has_properties);
    }

    if (schema.contains("const") || schema.contains("enum")) {
        return FieldType::Enum;
    }
    return scalarFieldType(schema["type"].as<std::string>(), has_properties);
}

bool isObjectSchema(const jsoncons::ojson &schema) {
    return schema.contains("type") && schema["type"] == "object";
}

bool isArraySchema(const jsoncons::ojson &schema) {
    return schema.contains("type") && schema["type"] == "array";
}

jsoncons::ojson getSchemaProperties(const jsoncons::ojson &schema) {
    if (schema.contains("properties") && schema["properties"].is_object()) {
        return schema["properties"];
    }
    return jsoncons::ojson::object();
}

jsoncons::ojson getArrayItemsSchema(const jsoncons::ojson &schema) {
    if (schema.contains("items")) {
        return schema["items"];
    }
    return jsoncons::ojson::object();
}

std::unordered_set<std::string> getConfluentTags(
    const jsoncons::ojson &schema) {
    std::unordered_set<std::string> tags;

    // schema as str
    auto schema_str = schema.to_string();
    if (schema.contains("confluent:tags") &&
        schema["confluent:tags"].is_array()) {
        for (const auto &tag : schema["confluent:tags"].array_range()) {
            if (tag.is_string()) {
                tags.insert(tag.as<std::string>());
            }
        }
    }

    return tags;
}

}  // namespace schema_navigation

// Validation utilities implementations
namespace validation_utils {

bool validateJson(
    std::shared_ptr<jsoncons::jsonschema::json_schema<jsoncons::ojson>> schema,
    const nlohmann::json &value) {
    try {
        auto jsoncons_value = jsonToOJson(value);
        schema->validate(jsoncons_value);

        return true;
    } catch (const std::exception &e) {
        return false;
    }
}

}  // namespace validation_utils

// Path utilities implementations
namespace path_utils {

std::string appendToPath(const std::string &base_path,
                         const std::string &component) {
    return base_path + "." + component;
}

std::string getFieldName(const std::string &path) {
    auto last_dot = path.find_last_of('.');
    if (last_dot == std::string::npos) {
        return path;
    }
    return path.substr(last_dot + 1);
}

}  // namespace path_utils

// General utility functions
jsoncons::ojson jsonToOJson(const nlohmann::json &nlohmann_json) {
    try {
        // Convert nlohmann::json to string and parse with jsoncons
        std::string json_str = nlohmann_json.dump();
        return jsoncons::ojson::parse(json_str);
    } catch (const std::exception &e) {
        throw JsonError("Failed to convert nlohmann to jsoncons: " +
                        std::string(e.what()));
    }
}

nlohmann::json ojsonToJson(const jsoncons::ojson &jsoncons_json) {
    try {
        // Convert jsoncons::ojson to string and parse with nlohmann
        std::string json_str = jsoncons_json.to_string();
        return nlohmann::json::parse(json_str);
    } catch (const std::exception &e) {
        throw JsonError("Failed to convert jsoncons to nlohmann: " +
                        std::string(e.what()));
    }
}

}  // namespace schemaregistry::serdes::json::utils