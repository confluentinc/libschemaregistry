#include "schemaregistry/serdes/json/JsonSerializer.h"

#include "schemaregistry/serdes/json/JsonUtils.h"
#include <cctype>
#include <cstdio>

namespace schemaregistry::serdes::json {

using namespace utils;

// Schema flattening utilities
namespace {

std::string uriToDefinitionKey(const std::string& uri) {
    // Convert URI to safe definition key
    // "https://jsoncons.com/ref" -> "ref"
    // "https://example.com/schemas/user" -> "user"
    
    // Extract path component
    auto pos = uri.find_last_of('/');
    if (pos != std::string::npos && pos < uri.length() - 1) {
        std::string key = uri.substr(pos + 1);
        // Remove any non-alphanumeric characters for safety
        std::string safe_key;
        for (char c : key) {
            if (std::isalnum(c) || c == '_') {
                safe_key += c;
            }
        }
        if (!safe_key.empty()) {
            return safe_key;
        }
    }
    
    // Fallback: use hash of full URI
    return "ref_" + std::to_string(std::hash<std::string>{}(uri));
}

void rewriteReferences(nlohmann::json& schema, 
                      const std::unordered_map<std::string, std::string>& uri_map) {
    if (schema.is_object()) {
        if (schema.contains("$ref") && schema["$ref"].is_string()) {
            std::string ref_uri = schema["$ref"];
            auto it = uri_map.find(ref_uri);
            if (it != uri_map.end()) {
                schema["$ref"] = it->second;  // Rewrite to internal reference
            }
        }
        
        // Recursively process all object properties
        for (auto& [key, value] : schema.items()) {
            rewriteReferences(value, uri_map);
        }
    } else if (schema.is_array()) {
        // Recursively process array elements
        for (auto& element : schema) {
            rewriteReferences(element, uri_map);
        }
    }
}

nlohmann::json flattenSchemaReferences(
    const nlohmann::json& main_schema, 
    const std::unordered_map<std::string, nlohmann::json>& resolved_refs) {
    
    auto flattened = main_schema;
    
    // 0. Add schema meta-information to make it self-describing
    flattened["$schema"] = "http://json-schema.org/draft-07/schema#";
    flattened["$id"] = "internal://flattened-schema";
    
    // 1. Create definitions section if it doesn't exist
    if (!flattened.contains("definitions")) {
        flattened["definitions"] = nlohmann::json::object();
    }
    
    // 2. Add all resolved references to definitions
    std::unordered_map<std::string, std::string> uri_to_def_map;
    for (const auto& [uri, ref_schema] : resolved_refs) {
        // Create safe definition key from URI
        std::string def_key = uriToDefinitionKey(uri);
        
        // Handle potential key conflicts by adding suffix
        std::string final_key = def_key;
        int suffix = 1;
        while (flattened["definitions"].contains(final_key)) {
            final_key = def_key + "_" + std::to_string(suffix++);
        }
        
        flattened["definitions"][final_key] = ref_schema;
        
        // Map both the original URI and common base URI variations
        std::string def_ref = "#/definitions/" + final_key;
        uri_to_def_map[uri] = def_ref;
        
        // Also map potential full URI variations that jsoncons might construct
        std::string full_uri = "https://jsoncons.com/" + uri;
        uri_to_def_map[full_uri] = def_ref;
        
        // Map the base URI itself to avoid meta-schema loading
        uri_to_def_map["https://jsoncons.com"] = def_ref;
    }
    
    // 3. Recursively rewrite all $ref URIs in the schema
    rewriteReferences(flattened, uri_to_def_map);
    
    return flattened;
}

}  // anonymous namespace

// JsonSerde implementation
JsonSerde::JsonSerde() {}

std::shared_ptr<jsoncons::jsonschema::json_schema<jsoncons::ojson>>
JsonSerde::getParsedSchema(
    const schemaregistry::rest::model::Schema &schema,
    std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client) {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    // Create cache key from schema content
    auto schema_str = schema.getSchema();
    std::string cache_key = schema_str.value_or("");

    auto it = parsed_schemas_cache_.find(cache_key);
    if (it != parsed_schemas_cache_.end()) {
        return it->second;
    }

    // Parse schema with references
    std::unordered_set<std::string> visited;
    auto resolved_refs =
        schema_resolution::resolveNamedSchema(schema, client, visited);

    // Parse main schema
    nlohmann::json parsed_schema;
    try {
        parsed_schema = nlohmann::json::parse(cache_key);
    } catch (const nlohmann::json::parse_error &e) {
        throw JsonError("Failed to parse JSON schema: " +
                        std::string(e.what()));
    }

    // FLATTEN SCHEMA - NO RESOLVER NEEDED
    auto flattened_schema = flattenSchemaReferences(parsed_schema, resolved_refs);

    // Compile flattened schema WITHOUT resolver
    auto jsoncons_schema = jsonToOJson(flattened_schema);
    auto compiled_schema =
        std::make_shared<jsoncons::jsonschema::json_schema<jsoncons::ojson>>(
            jsoncons::jsonschema::make_json_schema(jsoncons_schema));

    // Store in cache
    parsed_schemas_cache_[cache_key] = compiled_schema;

    return compiled_schema;
}

void JsonSerde::clear() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    parsed_schemas_cache_.clear();
}

void JsonSerde::resolveNamedSchema(
    const schemaregistry::rest::model::Schema &schema,
    std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
    std::unordered_map<std::string, std::string> references) {
    if (schema.getReferences().has_value()) {
        for (const auto &ref : schema.getReferences().value()) {
            if (!ref.getName().has_value() || !ref.getSubject().has_value()) {
                continue;  // Skip invalid references
            }
            if (references.find(ref.getName().value()) == references.end()) {
                // Resolve reference recursively
                auto ref_schema =
                    client->getVersion(ref.getSubject().value(),
                                       ref.getVersion().value_or(-1), true);
                resolveNamedSchema(ref_schema.toSchema(), client, references);
            }
        }
    }
}

class JsonSerializer::Impl {
  public:
    Impl(std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
         std::optional<schemaregistry::rest::model::Schema> schema,
         std::shared_ptr<RuleRegistry> rule_registry,
         const SerializerConfig &config)
        : schema_(std::move(schema)),
          base_(std::make_shared<BaseSerializer>(
              Serde(std::move(client), rule_registry), config)),
          serde_(std::make_unique<JsonSerde>()) {
        std::vector<std::shared_ptr<RuleExecutor>> executors;
        if (rule_registry) {
            executors = rule_registry->getExecutors();
        } else {
            executors = global_registry::getRuleExecutors();
        }
        for (const auto &executor : executors) {
            try {
                auto cfg = base_->getSerde().getClient()->getConfiguration();
                executor->configure(cfg, config.rule_config);
            } catch (const std::exception &e) {
                throw JsonError("Failed to configure rule executor: " +
                                std::string(e.what()));
            }
        }
    }

    std::vector<uint8_t> serialize(const SerializationContext &ctx,
                                   const nlohmann::json &value) {
        auto mutable_value = value;  // Copy for potential transformation

        // Get subject using strategy
        auto strategy = base_->getConfig().subject_name_strategy;
        auto subject_opt = strategy(ctx.topic, ctx.serde_type, schema_);
        if (!subject_opt.has_value()) {
            throw JsonError("Subject name strategy returned no subject");
        }
        std::string subject = subject_opt.value();

        // Get or register schema
        SchemaId schema_id(SerdeFormat::Json);
        std::optional<schemaregistry::rest::model::RegisteredSchema>
            latest_schema;

        try {
            latest_schema = base_->getSerde().getReaderSchema(
                subject, std::nullopt, base_->getConfig().use_schema);
        } catch (const std::exception &e) {
            // Schema not found - will use provided schema
        }

        schemaregistry::rest::model::Schema target_schema;
        std::shared_ptr<jsoncons::jsonschema::json_schema<jsoncons::ojson>>
            parsed_schema;

        if (latest_schema.has_value()) {
            target_schema = latest_schema->toSchema();
            auto id_opt = latest_schema->getId();
            if (id_opt.has_value()) {
                schema_id.setId(id_opt.value());
            }
            auto guid_opt = latest_schema->getGuid();
            if (guid_opt.has_value()) {
                schema_id.setGuid(guid_opt.value());
            }

            // Get parsed schema
            parsed_schema = getParsedSchema(target_schema);

            // Create field transformer lambda
            auto field_transformer =
                [this, &parsed_schema](
                    RuleContext &ctx, const std::string &rule_type,
                    const SerdeValue &msg) -> std::unique_ptr<SerdeValue> {
                if (msg.getFormat() == SerdeFormat::Json) {
                    auto json = asJson(msg);
                    auto transformed = utils::value_transform::transformFields(
                        ctx, parsed_schema, json);
                    return makeJsonValue(transformed);
                }
                return msg.clone();
            };

            auto json_value = makeJsonValue(mutable_value);

            // Execute rules on the serde value
            auto transformed_value = base_->getSerde().executeRules(
                ctx, subject, Mode::Write, std::nullopt, target_schema,
                *json_value, {},
                std::make_shared<FieldTransformer>(field_transformer));

            // Extract Json value from result
            if (transformed_value->getFormat() == SerdeFormat::Json) {
                mutable_value = asJson(*transformed_value);
            } else {
                throw JsonError(
                    "Unexpected serde value type returned from rule execution");
            }
        } else {
            // Use provided schema
            if (!schema_.has_value()) {
                throw JsonError("Schema needs to be set for auto-registration");
            }
            target_schema = schema_.value();

            // Register or get schema
            if (base_->getConfig().auto_register_schemas) {
                auto registered_schema =
                    base_->getSerde().getClient()->registerSchema(
                        subject, target_schema,
                        base_->getConfig().normalize_schemas);
                auto id_opt = registered_schema.getId();
                if (id_opt.has_value()) {
                    schema_id.setId(id_opt.value());
                }
                auto guid_opt = registered_schema.getGuid();
                if (guid_opt.has_value()) {
                    schema_id.setGuid(guid_opt.value());
                }
            } else {
                auto registered_schema =
                    base_->getSerde().getClient()->getBySchema(
                        subject, target_schema,
                        base_->getConfig().normalize_schemas, false);
                auto id_opt = registered_schema.getId();
                if (id_opt.has_value()) {
                    schema_id.setId(id_opt.value());
                }
                auto guid_opt = registered_schema.getGuid();
                if (guid_opt.has_value()) {
                    schema_id.setGuid(guid_opt.value());
                }
            }

            parsed_schema = getParsedSchema(target_schema);
        }

        // Validate JSON against schema if validation is enabled
        if (base_->getConfig().validate) {
            try {
                validation_utils::validateJson(parsed_schema, mutable_value);
            } catch (const std::exception &e) {
                throw JsonValidationError("JSON validation failed: " +
                                          std::string(e.what()));
            }
        }

        // Serialize JSON to bytes
        std::string json_string = mutable_value.dump();
        std::vector<uint8_t> encoded_bytes(json_string.begin(),
                                           json_string.end());

        // Apply encoding rules if present
        if (target_schema.getRuleSet().has_value()) {
            auto rule_set = target_schema.getRuleSet().value();
            if (rule_set.getEncodingRules().has_value()) {
                auto bytes_value =
                    SerdeValue::newBytes(SerdeFormat::Json, encoded_bytes);
                auto result = base_->getSerde().executeRulesWithPhase(
                    ctx, subject, Phase::Encoding, Mode::Write, std::nullopt,
                    std::make_optional(target_schema), *bytes_value, {});
                encoded_bytes = result->asBytes();
            }
        }

        // Serialize schema ID with message
        auto id_serializer = base_->getConfig().schema_id_serializer;
        return id_serializer(encoded_bytes, ctx, schema_id);
    }

    void close() { serde_->clear(); }

    std::shared_ptr<jsoncons::jsonschema::json_schema<jsoncons::ojson>>
    getParsedSchema(const schemaregistry::rest::model::Schema &schema) {
        return serde_->getParsedSchema(schema, base_->getSerde().getClient());
    }

  private:
    std::optional<schemaregistry::rest::model::Schema> schema_;
    std::shared_ptr<BaseSerializer> base_;
    std::unique_ptr<JsonSerde> serde_;
};

JsonSerializer::JsonSerializer(
    std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
    std::optional<schemaregistry::rest::model::Schema> schema,
    std::shared_ptr<RuleRegistry> rule_registry, const SerializerConfig &config)
    : impl_(std::make_unique<Impl>(std::move(client), std::move(schema),
                                   std::move(rule_registry), config)) {}

JsonSerializer::~JsonSerializer() = default;

std::vector<uint8_t> JsonSerializer::serialize(const SerializationContext &ctx,
                                               const nlohmann::json &value) {
    return impl_->serialize(ctx, value);
}

void JsonSerializer::close() { impl_->close(); }

}  // namespace schemaregistry::serdes::json