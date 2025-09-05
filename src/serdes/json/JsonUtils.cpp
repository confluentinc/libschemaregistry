#include "schemaregistry/serdes/json/JsonUtils.h"

#include <algorithm>
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

    // Use the schema's walk method to traverse and transform the JSON structure
    try {
        schema->walk(
            mutable_value,
            [&ctx, &mutable_value, &visited_locations](
                const std::string &keyword, const jsoncons::ojson &schema_node,
                const jsoncons::uri &schema_location,
                const jsoncons::ojson &instance_node,
                const jsoncons::jsonpointer::json_pointer &instance_location)
                -> jsoncons::jsonschema::walk_result {
                try {
                    std::string schema_location_str = schema_location.string();
                    std::string instance_location_str =
                        instance_location.to_string();

                    if (schema_navigation::isObjectSchema(schema_node) &&
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
                                // pointer
                                try {
                                    jsoncons::jsonpointer::replace(
                                        mutable_value, field_location,
                                        transformed_value);
                                } catch (const std::exception &e) {
                                    // If pointer access fails, continue with
                                    // next field
                                }
                            }
                        }
                    }
                } catch (const std::exception &e) {
                    // Continue processing even if a single field transformation
                    // fails This maintains consistency with the recursive
                    // approach
                }

                return jsoncons::jsonschema::walk_result::advance;
            });

        // Convert back to nlohmann::json and return
        return ojsonToJson(mutable_value);

    } catch (const std::exception &e) {
        // If walk fails, fall back to the original value
        return value;
    }
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

FieldType getFieldType(const jsoncons::ojson &schema) {
    if (!schema.contains("type")) {
        return FieldType::String;  // Default fallback
    }

    std::string type = schema["type"].as<std::string>();

    if (type == "object") {
        return FieldType::Record;
    } else if (type == "array") {
        return FieldType::Array;
    } else if (type == "string") {
        return FieldType::String;
    } else if (type == "integer") {
        return FieldType::Int;
    } else if (type == "number") {
        return FieldType::Double;
    } else if (type == "boolean") {
        return FieldType::Boolean;
    } else if (type == "null") {
        return FieldType::Null;
    }

    return FieldType::String;  // Default fallback
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