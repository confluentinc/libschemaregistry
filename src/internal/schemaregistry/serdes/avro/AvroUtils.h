#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Avro C++ includes
#include <avro/Compiler.hh>
#include <avro/Decoder.hh>
#include <avro/Encoder.hh>
#include <avro/Generic.hh>
#include <avro/Specific.hh>
#include <avro/ValidSchema.hh>

// Project includes
#include <nlohmann/json.hpp>

#include "schemaregistry/rest/model/Schema.h"
#include "schemaregistry/serdes/SerdeError.h"
#include "schemaregistry/serdes/SerdeTypes.h"
#include "schemaregistry/serdes/ValidationRule.h"
#include "schemaregistry/serdes/avro/AvroTypes.h"

namespace schemaregistry::serdes::avro {

/**
 * Utility functions for Avro schema and data manipulation
 */
namespace utils {

/**
 * Apply field transformation rules
 * @param ctx Rule context
 * @param schema Schema for the datum
 * @param datum Avro datum to transform
 * @return Transformed datum
 */
::avro::GenericDatum transformFields(RuleContext &ctx,
                                     const ::avro::ValidSchema &schema,
                                     const ::avro::GenericDatum &datum);

/**
 * Transform individual field with context handling
 * @param ctx Rule context
 * @param record_schema Schema of the parent record
 * @param record_datum The containing record, which the rule sees as `message`
 * @param field_name Name of the field
 * @param field_datum Field datum to transform
 * @param field_schema Schema of the field
 * @return Transformed field datum
 */
::avro::GenericDatum transformFieldWithContext(
    RuleContext &ctx, const ::avro::ValidSchema &record_schema,
    const ::avro::GenericDatum &record_datum, const std::string &field_name,
    const ::avro::GenericDatum &field_datum,
    const ::avro::ValidSchema &field_schema);

/**
 * Convert Avro schema type to FieldType enum
 * @param schema Avro schema to convert
 * @return Corresponding FieldType
 */
FieldType avroSchemaToFieldType(const ::avro::ValidSchema &schema);

/**
 * Convert Avro GenericDatum to JSON
 * @param datum Avro datum to convert
 * @return JSON representation
 */
nlohmann::json avroToJson(const ::avro::GenericDatum &datum);

/**
 * Convert JSON to Avro GenericDatum
 * @param json_value JSON value to convert
 * @param schema Avro schema to guide conversion
 * @return Converted Avro datum
 */
::avro::GenericDatum jsonToAvro(const nlohmann::json &json_value,
                                const ::avro::ValidSchema &schema);

/**
 * Follow a symbolic link to the node it stands for; any other node is returned as-is.
 * avro-cpp represents a reused named type this way, and a ValidSchema cannot be built from one.
 * @param node Schema node, possibly symbolic
 * @return The node the link points at, or the node itself
 */
::avro::NodePtr resolveNode(const ::avro::NodePtr &node);

/**
 * Resolve union schema branch for a given datum
 * @param union_schema Union schema
 * @param datum Datum to match against union branches
 * @return Pair of branch index and corresponding schema
 */
std::pair<size_t, ::avro::ValidSchema> resolveUnion(
    const ::avro::ValidSchema &union_schema, const ::avro::GenericDatum &datum);

/**
 * Extract schema name from Avro ValidSchema
 * @param schema Avro schema
 * @return Optional schema name
 */
std::optional<std::string> getSchemaName(const ::avro::ValidSchema &schema);

/**
 * Serialize Avro datum to byte array
 * @param datum Avro datum to serialize
 * @param writer_schema Schema to use for writing
 * @param named_schemas Additional named schemas for resolution
 * @return Serialized bytes
 */
std::vector<uint8_t> serializeAvroData(
    const ::avro::GenericDatum &datum, const ::avro::ValidSchema &writer_schema,
    const std::vector<::avro::ValidSchema> &named_schemas = {});

/**
 * Deserialize byte array to Avro datum
 * @param data Serialized bytes
 * @param writer_schema Schema used for writing
 * @param reader_schema Optional reader schema for schema evolution
 * @param named_schemas Additional named schemas for resolution
 * @return Deserialized Avro datum
 */
::avro::GenericDatum deserializeAvroData(
    const std::vector<uint8_t> &data, const ::avro::ValidSchema &writer_schema,
    const ::avro::ValidSchema *reader_schema = nullptr,
    const std::vector<::avro::ValidSchema> &named_schemas = {});

/**
 * Parse Avro schema string with named schema support
 * @param schema_str Main schema string
 * @param named_schemas Vector of named schema strings
 * @return Tuple of parsed main schema and named schemas
 */
std::pair<::avro::ValidSchema, std::vector<::avro::ValidSchema>>
parseSchemaWithNamed(const std::string &schema_str,
                     const std::vector<std::string> &named_schemas = {});

/**
 * Validate schema compatibility between writer and reader
 * @param writer_schema Writer schema
 * @param reader_schema Reader schema
 * @return True if schemas are compatible
 */
bool isSchemaCompatible(const ::avro::ValidSchema &writer_schema,
                        const ::avro::ValidSchema &reader_schema);

/**
 * Extract the implied namespace from a qualified name
 * @param name Fully qualified name (e.g., "com.example.MyRecord")
 * @return Implied namespace (e.g., "com.example") or empty string if no
 * namespace
 */
std::string impliedNamespace(const std::string &name);

/**
 * Get inline tags from an Avro schema
 * @param schema Avro schema as JSON object
 * @return Map of field paths to their tag sets
 */
std::unordered_map<std::string, std::unordered_set<std::string>> getInlineTags(
    const nlohmann::json &schema);

/**
 * Recursively extract inline tags from Avro schema
 * @param ns Current namespace
 * @param name Current name/path
 * @param schema Schema object to process
 * @param tags Output map of field paths to tag sets
 */
void getInlineTagsRecursively(
    const std::string &ns, const std::string &name,
    const nlohmann::json &schema,
    std::unordered_map<std::string, std::unordered_set<std::string>> &tags);

/**
 * Walk datum against schema, evaluating every inline "confluent:rules" CHECK
 * constraint encountered and collecting all failures. Read-only — the datum is
 * not modified.
 *
 * Two kinds of rules are evaluated:
 *   - Record-level ("confluent:rules" on a record schema) — `this` is the
 *     record.
 *   - Field-level ("confluent:rules" on a record's field) — `this` is the field
 *     value. Honors the skip-on-null contract: a null field value does not have
 *     its rules invoked.
 *
 * Failures carry their dotted-path location (e.g. addr.zip, tags[3],
 * scores["foo"]). The walk continues after each failure so callers see the full
 * set rather than only the first, unless fail_fast is set.
 *
 * @param executor Executor used to evaluate each rule
 * @param schema Raw schema JSON — the Avro parser drops custom attributes, so
 * the rules must be read from the original schema text
 * @param named_schemas Raw JSON of the referenced schemas, so that a field
 * whose type names a record defined in another subject resolves to its
 * definition instead of being skipped
 * @param datum Avro datum to validate
 * @param fail_fast Stop at the first violation
 * @return Every violation found, in walk order
 */
std::vector<ValidationRuleError> validateMessage(
    ValidationRuleExecutor &executor, const nlohmann::json &schema,
    const std::vector<nlohmann::json> &named_schemas,
    const ::avro::GenericDatum &datum, bool fail_fast);

/**
 * Compile a JSON schema string to an Avro ValidSchema, removing confluent:tags
 * properties
 * @param schema_str JSON schema string to compile
 * @return Compiled Avro ValidSchema
 */
::avro::ValidSchema compileJsonSchema(const std::string &schema_str);

}  // namespace utils

}  // namespace schemaregistry::serdes::avro
