#include "schemaregistry/serdes/avro/AvroUtils.h"

#include <avro/Exception.hh>
#include <avro/Stream.hh>
#include <iostream>
#include <sstream>

#include "schemaregistry/serdes/RuleRegistry.h"
#include "schemaregistry/serdes/Serde.h"
#include "schemaregistry/serdes/avro/AvroTypes.h"

namespace schemaregistry::serdes::avro {

namespace utils {

::avro::GenericDatum transformFields(RuleContext &ctx,
                                     const ::avro::ValidSchema &schema,
                                     const ::avro::GenericDatum &datum) {
    switch (schema.root()->type()) {
        case ::avro::AVRO_RECORD: {
            auto record = datum.value<::avro::GenericRecord>();
            ::avro::GenericRecord result(schema.root());

            for (size_t i = 0; i < record.fieldCount(); ++i) {
                auto field_datum = record.fieldAt(i);
                auto field_schema_node = schema.root()->leafAt(i);
                ::avro::ValidSchema field_schema(field_schema_node);
                const std::string &field_name = schema.root()->nameAt(i);

                auto transformed_field = transformFieldWithContext(
                    ctx, schema, field_name, field_datum, field_schema);
                result.setFieldAt(i, transformed_field);
            }

            // Create a new GenericDatum with the schema and set the record
            // value
            ::avro::GenericDatum result_datum(schema);
            result_datum.value<::avro::GenericRecord>() = result;
            return result_datum;
        }

        case ::avro::AVRO_ARRAY: {
            auto array = datum.value<::avro::GenericArray>();
            ::avro::GenericDatum result_datum(schema);
            auto &result = result_datum.value<::avro::GenericArray>();
            auto item_schema_node = schema.root()->leafAt(0);
            ::avro::ValidSchema item_schema(item_schema_node);

            for (size_t i = 0; i < array.value().size(); ++i) {
                auto transformed =
                    transformFields(ctx, item_schema, array.value()[i]);
                result.value().push_back(transformed);
            }

            return result_datum;
        }

        case ::avro::AVRO_MAP: {
            auto map = datum.value<::avro::GenericMap>();
            ::avro::GenericDatum result_datum(schema);
            auto &result = result_datum.value<::avro::GenericMap>();
            for (const auto &[key, value] : map.value()) {
                auto value_schema_node = schema.root()->leafAt(0);
                ::avro::ValidSchema value_schema(value_schema_node);
                auto transformed = transformFields(ctx, value_schema, value);
                result.value().emplace_back(key, transformed);
            }
            return result_datum;
        }

        case ::avro::AVRO_UNION: {
            auto union_val = datum.value<::avro::GenericUnion>();
            auto [branch_idx, branch_schema] = resolveUnion(schema, datum);
            auto transformed =
                transformFields(ctx, branch_schema, union_val.datum());

            ::avro::GenericDatum result_datum(schema);
            auto &result = result_datum.value<::avro::GenericUnion>();
            result.selectBranch(branch_idx);
            result.datum() = transformed;

            return result_datum;
        }

        default: {
            // Field-level transformation logic
            auto field_ctx = ctx.currentField();
            if (field_ctx.has_value()) {
                field_ctx->setFieldType(avroSchemaToFieldType(schema));

                auto rule_tags = ctx.getRule().getTags();
                std::unordered_set<std::string> rule_tags_set;
                if (rule_tags.has_value()) {
                    rule_tags_set = std::unordered_set<std::string>(
                        rule_tags->begin(), rule_tags->end());
                }

                // Check if rule tags overlap with field context tags (empty
                // rule_tags means apply to all)
                bool should_apply =
                    !rule_tags.has_value() || rule_tags_set.empty();
                if (!should_apply) {
                    const auto &field_tags = field_ctx->getTags();
                    for (const auto &field_tag : field_tags) {
                        if (rule_tags_set.find(field_tag) !=
                            rule_tags_set.end()) {
                            should_apply = true;
                            break;
                        }
                    }
                }

                if (should_apply) {
                    auto message_value = makeAvroValue(datum);

                    // Get field executor type from the rule
                    auto field_executor_type =
                        ctx.getRule().getType().value_or("");

                    // Try to get executor from context's rule registry first,
                    // then global
                    std::shared_ptr<RuleExecutor> executor;
                    if (ctx.getRuleRegistry()) {
                        executor = ctx.getRuleRegistry()->getExecutor(
                            field_executor_type);
                    }
                    if (!executor) {
                        executor = global_registry::getRuleExecutor(
                            field_executor_type);
                    }

                    if (executor) {
                        auto field_executor =
                            std::dynamic_pointer_cast<FieldRuleExecutor>(
                                executor);
                        if (!field_executor) {
                            throw AvroError("executor " + field_executor_type +
                                            " is not a field rule executor");
                        }

                        auto new_value =
                            field_executor->transformField(ctx, *message_value);
                        if (new_value &&
                            new_value->getFormat() == SerdeFormat::Avro) {
                            return asAvro(*new_value);
                        }
                    }
                }
            }

            return datum;
        }
    }
}

// Transform individual field with context handling
::avro::GenericDatum transformFieldWithContext(
    RuleContext &ctx, const ::avro::ValidSchema &record_schema,
    const std::string &field_name, const ::avro::GenericDatum &field_datum,
    const ::avro::ValidSchema &field_schema) {
    // Get field type from schema
    FieldType field_type = avroSchemaToFieldType(field_schema);

    // Create full field name
    std::string schema_name = getSchemaName(record_schema).value_or("unknown");
    std::string full_name = schema_name + "." + field_name;

    // Create message value from current field datum
    auto message_value = makeAvroValue(field_datum);

    // Enter field context
    ctx.enterField(*message_value, full_name, field_name, field_type, {});

    try {
        // Transform the field value (synchronous call)
        ::avro::GenericDatum new_value =
            transformFields(ctx, field_schema, field_datum);

        // Check for condition rules
        auto rule_kind = ctx.getRule().getKind();
        if (rule_kind.has_value() && rule_kind.value() == Kind::Condition) {
            if (new_value.type() == ::avro::AVRO_BOOL) {
                bool condition_result = new_value.value<bool>();
                if (!condition_result) {
                    throw AvroError("Rule condition failed for field: " +
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

FieldType avroSchemaToFieldType(const ::avro::ValidSchema &schema) {
    switch (schema.root()->type()) {
        case ::avro::AVRO_NULL:
            return FieldType::Null;
        case ::avro::AVRO_BOOL:
            return FieldType::Boolean;
        case ::avro::AVRO_INT:
            return FieldType::Int;
        case ::avro::AVRO_LONG:
            return FieldType::Long;
        case ::avro::AVRO_FLOAT:
            return FieldType::Float;
        case ::avro::AVRO_DOUBLE:
            return FieldType::Double;
        case ::avro::AVRO_BYTES:
            return FieldType::Bytes;
        case ::avro::AVRO_STRING:
            return FieldType::String;
        case ::avro::AVRO_RECORD:
            return FieldType::Record;
        case ::avro::AVRO_ENUM:
            return FieldType::Enum;
        case ::avro::AVRO_ARRAY:
            return FieldType::Array;
        case ::avro::AVRO_MAP:
            return FieldType::Map;
        case ::avro::AVRO_UNION:
            return FieldType::Combined;
        case ::avro::AVRO_FIXED:
            return FieldType::Fixed;
        case ::avro::AVRO_SYMBOLIC:
            return FieldType::Record;  // Assume symbolic references are records
        default:
            return FieldType::String;  // Default fallback
    }
}

nlohmann::json avroToJson(const ::avro::GenericDatum &datum) {
    switch (datum.type()) {
        case ::avro::AVRO_NULL:
            return nlohmann::json(nullptr);

        case ::avro::AVRO_BOOL:
            return nlohmann::json(datum.value<bool>());

        case ::avro::AVRO_INT:
            return nlohmann::json(datum.value<int32_t>());

        case ::avro::AVRO_LONG:
            return nlohmann::json(datum.value<int64_t>());

        case ::avro::AVRO_FLOAT:
            return nlohmann::json(datum.value<float>());

        case ::avro::AVRO_DOUBLE:
            return nlohmann::json(datum.value<double>());

        case ::avro::AVRO_STRING:
            return nlohmann::json(datum.value<std::string>());

        case ::avro::AVRO_BYTES: {
            const auto &bytes = datum.value<std::vector<uint8_t>>();
            nlohmann::json array = nlohmann::json::array();
            for (uint8_t byte : bytes) {
                array.push_back(static_cast<unsigned int>(byte));
            }
            return array;
        }

        case ::avro::AVRO_FIXED: {
            const auto &fixed = datum.value<::avro::GenericFixed>();
            nlohmann::json array = nlohmann::json::array();
            for (size_t i = 0; i < fixed.value().size(); ++i) {
                array.push_back(static_cast<unsigned int>(fixed.value()[i]));
            }
            return array;
        }

        case ::avro::AVRO_ENUM: {
            const auto &enum_val = datum.value<::avro::GenericEnum>();
            return nlohmann::json(enum_val.symbol());
        }

        case ::avro::AVRO_ARRAY: {
            const auto &array = datum.value<::avro::GenericArray>();
            nlohmann::json json_array = nlohmann::json::array();
            for (size_t i = 0; i < array.value().size(); ++i) {
                json_array.push_back(avroToJson(array.value()[i]));
            }
            return json_array;
        }

        case ::avro::AVRO_MAP: {
            const auto &map = datum.value<::avro::GenericMap>();
            nlohmann::json json_obj = nlohmann::json::object();
            for (const auto &pair : map.value()) {
                json_obj[pair.first] = avroToJson(pair.second);
            }
            return json_obj;
        }

        case ::avro::AVRO_RECORD: {
            const auto &record = datum.value<::avro::GenericRecord>();
            nlohmann::json json_obj = nlohmann::json::object();
            for (size_t i = 0; i < record.fieldCount(); ++i) {
                json_obj[record.schema()->nameAt(i)] =
                    avroToJson(record.fieldAt(i));
            }
            return json_obj;
        }

        case ::avro::AVRO_UNION: {
            const auto &union_val = datum.value<::avro::GenericUnion>();
            return avroToJson(union_val.datum());
        }

        default:
            throw AvroError("Unsupported Avro type for JSON conversion");
    }
}

::avro::GenericDatum jsonToAvro(const nlohmann::json &json_value,
                                const ::avro::ValidSchema &schema) {
    switch (schema.root()->type()) {
        case ::avro::AVRO_NULL:
            return ::avro::GenericDatum();

        case ::avro::AVRO_BOOL:
            if (!json_value.is_boolean()) {
                throw AvroError("Expected boolean value");
            }
            return ::avro::GenericDatum(json_value.get<bool>());

        case ::avro::AVRO_INT:
            if (!json_value.is_number_integer()) {
                throw AvroError("Expected integer value");
            }
            return ::avro::GenericDatum(json_value.get<int32_t>());

        case ::avro::AVRO_LONG:
            if (!json_value.is_number()) {
                throw AvroError("Expected number value for long");
            }
            return ::avro::GenericDatum(json_value.get<int64_t>());

        case ::avro::AVRO_FLOAT:
            if (!json_value.is_number()) {
                throw AvroError("Expected number value for float");
            }
            return ::avro::GenericDatum(json_value.get<float>());

        case ::avro::AVRO_DOUBLE:
            if (!json_value.is_number()) {
                throw AvroError("Expected number value for double");
            }
            return ::avro::GenericDatum(json_value.get<double>());

        case ::avro::AVRO_STRING:
            if (!json_value.is_string()) {
                throw AvroError("Expected string value");
            }
            return ::avro::GenericDatum(json_value.get<std::string>());

        case ::avro::AVRO_BYTES: {
            if (!json_value.is_array()) {
                throw AvroError("Expected array for bytes");
            }
            std::vector<uint8_t> bytes;
            for (const auto &elem : json_value) {
                if (!elem.is_number_unsigned()) {
                    throw AvroError(
                        "Expected unsigned integers in bytes array");
                }
                bytes.push_back(elem.get<uint8_t>());
            }
            return ::avro::GenericDatum(bytes);
        }

        case ::avro::AVRO_ENUM: {
            if (!json_value.is_string()) {
                throw AvroError("Expected string value for enum");
            }
            ::avro::GenericDatum datum(schema);
            auto &enum_val = datum.value<::avro::GenericEnum>();
            enum_val.set(json_value.get<std::string>());
            return datum;
        }

        case ::avro::AVRO_ARRAY: {
            if (!json_value.is_array()) {
                throw AvroError("Expected array value");
            }
            ::avro::GenericDatum datum(schema);
            auto &array = datum.value<::avro::GenericArray>();
            auto item_schema = schema.root()->leafAt(0);
            ::avro::ValidSchema item_valid_schema(item_schema);

            for (const auto &item : json_value) {
                array.value().push_back(jsonToAvro(item, item_valid_schema));
            }
            return datum;
        }

        case ::avro::AVRO_MAP: {
            if (!json_value.is_object()) {
                throw AvroError("Expected object value for map");
            }
            ::avro::GenericDatum datum(schema);
            auto &map = datum.value<::avro::GenericMap>();
            auto value_schema = schema.root()->leafAt(0);
            ::avro::ValidSchema value_valid_schema(value_schema);

            for (const auto &[key, value] : json_value.items()) {
                map.value().push_back(
                    {key, jsonToAvro(value, value_valid_schema)});
            }
            return datum;
        }

        case ::avro::AVRO_RECORD: {
            if (!json_value.is_object()) {
                throw AvroError("Expected object value for record");
            }
            ::avro::GenericDatum datum(schema);
            auto &record = datum.value<::avro::GenericRecord>();

            for (size_t i = 0; i < schema.root()->leaves(); ++i) {
                const std::string &field_name = schema.root()->nameAt(i);
                auto field_it = json_value.find(field_name);
                if (field_it != json_value.end()) {
                    auto field_schema = schema.root()->leafAt(i);
                    ::avro::ValidSchema field_valid_schema(field_schema);
                    record.setFieldAt(
                        i, jsonToAvro(*field_it, field_valid_schema));
                }
            }
            return datum;
        }

        default:
            throw AvroError(
                "Unsupported schema type for JSON to Avro conversion");
    }
}

std::pair<size_t, ::avro::ValidSchema> resolveUnion(
    const ::avro::ValidSchema &union_schema,
    const ::avro::GenericDatum &datum) {
    if (union_schema.root()->type() != ::avro::AVRO_UNION) {
        throw AvroError("Schema is not a union type");
    }

    // Try to find matching branch based on datum type
    for (size_t i = 0; i < union_schema.root()->leaves(); ++i) {
        auto branch_schema = union_schema.root()->leafAt(i);
        if (branch_schema->type() == datum.type()) {
            return {i, ::avro::ValidSchema(branch_schema)};
        }
    }

    throw AvroError("No matching union branch found for datum type");
}

std::optional<std::string> getSchemaName(const ::avro::ValidSchema &schema) {
    if (schema.root()->type() == ::avro::AVRO_RECORD) {
        return schema.root()->name().fullname();
    }
    return std::nullopt;
}

std::vector<uint8_t> serializeAvroData(
    const ::avro::GenericDatum &datum, const ::avro::ValidSchema &writer_schema,
    const std::vector<::avro::ValidSchema> &named_schemas) {
    try {
        auto output_stream = ::avro::memoryOutputStream();
        auto encoder = ::avro::binaryEncoder();
        encoder->init(*output_stream);

        ::avro::encode(*encoder, datum);
        encoder->flush();

        // Get the data from the output stream
        auto input_stream = ::avro::memoryInputStream(*output_stream);
        const uint8_t *data = nullptr;
        size_t size = 0;
        if (input_stream->next(&data, &size)) {
            return std::vector<uint8_t>(data, data + size);
        }
        return {};
    } catch (const ::avro::Exception &e) {
        throw AvroError(e);
    }
}

::avro::GenericDatum deserializeAvroData(
    const std::vector<uint8_t> &data, const ::avro::ValidSchema &writer_schema,
    const ::avro::ValidSchema *reader_schema,
    const std::vector<::avro::ValidSchema> &named_schemas) {
    try {
        auto input_stream = ::avro::memoryInputStream(data.data(), data.size());
        auto decoder = ::avro::binaryDecoder();
        decoder->init(*input_stream);

        ::avro::GenericDatum datum(reader_schema ? *reader_schema
                                                 : writer_schema);
        ::avro::decode(*decoder, datum);

        return datum;
    } catch (const ::avro::Exception &e) {
        throw AvroError(e);
    }
}

std::pair<::avro::ValidSchema, std::vector<::avro::ValidSchema>>
parseSchemaWithNamed(const std::string &schema_str,
                     const std::vector<std::string> &named_schemas) {
    // Currently Avro does not support references to named schema
    // The code below is a placeholder for future implementation
    try {
        // Parse named schemas first
        std::vector<::avro::ValidSchema> parsed_named;
        for (const auto &named_schema_str : named_schemas) {
            ::avro::ValidSchema named_schema =
                compileJsonSchema(named_schema_str);
            parsed_named.push_back(named_schema);
        }

        // Parse main schema
        ::avro::ValidSchema main_schema = compileJsonSchema(schema_str);

        return {main_schema, parsed_named};
    } catch (const ::avro::Exception &e) {
        throw AvroError(e);
    }
}

std::string impliedNamespace(const std::string &name) {
    size_t last_dot = name.find_last_of('.');
    if (last_dot != std::string::npos && last_dot > 0) {
        return name.substr(0, last_dot);
    }
    return "";
}

std::unordered_map<std::string, std::unordered_set<std::string>> getInlineTags(
    const nlohmann::json &schema) {
    std::unordered_map<std::string, std::unordered_set<std::string>>
        inline_tags;
    getInlineTagsRecursively("", "", schema, inline_tags);
    return inline_tags;
}

void getInlineTagsRecursively(
    const std::string &ns, const std::string &name,
    const nlohmann::json &schema,
    std::unordered_map<std::string, std::unordered_set<std::string>> &tags) {
    if (schema.is_null()) {
        return;
    }

    if (schema.is_array()) {
        for (const auto &subschema : schema) {
            getInlineTagsRecursively(ns, name, subschema, tags);
        }
    } else if (!schema.is_object()) {
        // string schemas; this could be either a named schema or a primitive
        // type
        return;
    } else {
        auto schema_type_it = schema.find("type");
        if (schema_type_it == schema.end()) {
            return;
        }

        std::string schema_type = schema_type_it->is_string()
                                      ? schema_type_it->get<std::string>()
                                      : "";

        if (schema_type == "array") {
            auto items_it = schema.find("items");
            if (items_it != schema.end()) {
                getInlineTagsRecursively(ns, name, *items_it, tags);
            }
        } else if (schema_type == "map") {
            auto values_it = schema.find("values");
            if (values_it != schema.end()) {
                getInlineTagsRecursively(ns, name, *values_it, tags);
            }
        } else if (schema_type == "record") {
            std::string record_ns;
            std::string record_name;

            auto ns_it = schema.find("namespace");
            if (ns_it != schema.end() && ns_it->is_string()) {
                record_ns = ns_it->get<std::string>();
            } else {
                record_ns = impliedNamespace(name);
                if (record_ns.empty()) {
                    record_ns = ns;
                }
            }

            auto name_it = schema.find("name");
            if (name_it != schema.end() && name_it->is_string()) {
                record_name = name_it->get<std::string>();

                // Add namespace prefix if needed
                if (!record_ns.empty() && record_name.find(record_ns) != 0) {
                    record_name = record_ns + "." + record_name;
                }
            }

            auto fields_it = schema.find("fields");
            if (fields_it != schema.end() && fields_it->is_array()) {
                for (const auto &field : *fields_it) {
                    if (!field.is_object()) continue;

                    auto field_tags_it = field.find("confluent:tags");
                    auto field_name_it = field.find("name");
                    auto field_type_it = field.find("type");

                    if (field_tags_it != field.end() &&
                        field_name_it != field.end() &&
                        field_tags_it->is_array() &&
                        field_name_it->is_string()) {
                        std::string field_name =
                            field_name_it->get<std::string>();
                        std::string full_field_name =
                            record_name + "." + field_name;

                        for (const auto &tag : *field_tags_it) {
                            if (tag.is_string()) {
                                tags[full_field_name].insert(
                                    tag.get<std::string>());
                            }
                        }
                    }

                    if (field_type_it != field.end()) {
                        getInlineTagsRecursively(record_ns, record_name,
                                                 *field_type_it, tags);
                    }
                }
            }
        }
    }
}

/**
 * Recursively remove "confluent:tags" properties from a JSON schema
 * @param schema JSON schema object to clean
 */
void removeConfluentTags(nlohmann::json &schema) {
    if (schema.is_object()) {
        // Remove confluent:tags if it exists
        auto tags_it = schema.find("confluent:tags");
        if (tags_it != schema.end()) {
            schema.erase(tags_it);
        }

        // Recursively process all values in the object
        for (auto &[key, value] : schema.items()) {
            removeConfluentTags(value);
        }
    } else if (schema.is_array()) {
        // Recursively process all elements in the array
        for (auto &element : schema) {
            removeConfluentTags(element);
        }
    }
}

::avro::ValidSchema compileJsonSchema(const std::string &schema_str) {
    try {
        // Parse the schema string as JSON
        nlohmann::json schema_json = nlohmann::json::parse(schema_str);

        // Remove confluent:tags properties recursively
        removeConfluentTags(schema_json);

        // Convert back to string
        std::string cleaned_schema_str = schema_json.dump();

        // Create istringstream and compile the schema
        std::istringstream schema_stream(cleaned_schema_str);
        ::avro::ValidSchema avro_schema;
        ::avro::compileJsonSchema(schema_stream, avro_schema);

        return avro_schema;
    } catch (const nlohmann::json::parse_error &e) {
        throw AvroError("Failed to parse schema JSON: " +
                        std::string(e.what()));
    } catch (const ::avro::Exception &e) {
        throw AvroError("Failed to compile Avro schema: " +
                        std::string(e.what()));
    }
}

}  // namespace utils

}  // namespace schemaregistry::serdes::avro