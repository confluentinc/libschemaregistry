/**
 * AvroTest
 * Tests for the Avro serialization functionality
 */

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <avro/Compiler.hh>
#include <avro/ValidSchema.hh>
#include <avro/Generic.hh>

// Project includes
#include "schemaregistry/rest/MockSchemaRegistryClient.h"
#include "schemaregistry/rest/ClientConfiguration.h"
#include "schemaregistry/serdes/avro/AvroSerializer.h"
#include "schemaregistry/serdes/avro/AvroDeserializer.h"
#include "schemaregistry/serdes/avro/AvroUtils.h"
#include "schemaregistry/serdes/SerdeConfig.h"
#include "schemaregistry/serdes/SerdeTypes.h"
#include "schemaregistry/serdes/RuleRegistry.h"
#include "schemaregistry/rest/model/Metadata.h"
#include "schemaregistry/rest/model/Schema.h"
#include "schemaregistry/rest/model/ServerConfig.h"
#include "schemaregistry/rest/model/Rule.h"
#include "schemaregistry/rest/model/RuleSet.h"
#include "schemaregistry/serdes/SerdeError.h"
#include "schemaregistry/serdes/Serde.h"

#ifdef SCHEMAREGISTRY_USE_RULES
#include "schemaregistry/rules/cel/CelFieldExecutor.h"
#include "schemaregistry/rules/encryption/FieldEncryptionExecutor.h"
#include "schemaregistry/rules/encryption/EncryptionExecutor.h"
#include "schemaregistry/rules/encryption/localkms/LocalKmsDriver.h"
#include "schemaregistry/rest/MockDekRegistryClient.h"
#include "schemaregistry/rules/jsonata/JsonataExecutor.h"
#endif

using namespace schemaregistry::serdes;
using namespace schemaregistry::serdes::avro;
using namespace schemaregistry::rest;
using namespace schemaregistry::rest::model;

#ifdef SCHEMAREGISTRY_USE_RULES
using namespace schemaregistry::rules::cel;
using namespace schemaregistry::rules::encryption;
using namespace schemaregistry::rules::encryption::localkms;
using namespace schemaregistry::rules::jsonata;
#endif

TEST(AvroTest, BasicSerialization) {
    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);

    // Create serializer and deserializer configurations
    auto ser_config = SerializerConfig::createDefault();
    auto deser_config = DeserializerConfig::createDefault();
    
    // Define the Avro schema
    const std::string schema_str = R"({
        "type": "record",
        "name": "test",
        "fields": [
            {"name": "intField", "type": "int"},
            {"name": "doubleField", "type": "double"},
            {"name": "stringField", "type": "string"},
            {"name": "booleanField", "type": "boolean"},
            {"name": "bytesField", "type": "bytes"}
        ]
    })";
    
    // Create schema object
    schemaregistry::rest::model::Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("AVRO"));
    schema.setSchema(std::make_optional<std::string>(schema_str));
    
    // Parse the Avro schema
    ::avro::ValidSchema avro_schema = AvroSerializer::compileJsonSchema(schema_str);
    
    // Create the Avro record
    ::avro::GenericDatum datum(avro_schema);
    auto& record = datum.value<::avro::GenericRecord>();
    
    // Set field values
    record.setFieldAt(0, ::avro::GenericDatum(static_cast<int32_t>(123)));           // intField
    record.setFieldAt(1, ::avro::GenericDatum(45.67));                              // doubleField  
    record.setFieldAt(2, ::avro::GenericDatum(std::string("hi")));                  // stringField
    record.setFieldAt(3, ::avro::GenericDatum(true));                               // booleanField
    record.setFieldAt(4, ::avro::GenericDatum(std::vector<uint8_t>{1, 2, 3}));      // bytesField
    
    // Create rule registry
    auto rule_registry = std::make_shared<RuleRegistry>();
    
    // Create serializer and deserializer
    AvroSerializer serializer(client, std::make_optional(schema), rule_registry, ser_config);
    AvroDeserializer deserializer(client, rule_registry, deser_config);
    
    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Avro;
    
    // Serialize the record
    auto serialized_bytes = serializer.serialize(ser_ctx, datum);
    
    // Deserialize the record
    auto deserialized_value = deserializer.deserialize(ser_ctx, serialized_bytes);
    
    // Verify the deserialized record
    ASSERT_TRUE(deserialized_value.value.type() == ::avro::AVRO_RECORD);
    
    auto& deserialized_record = deserialized_value.value.value<::avro::GenericRecord>();
    ASSERT_EQ(deserialized_record.fieldCount(), 5);
    
    // Verify field values
    EXPECT_EQ(deserialized_record.fieldAt(0).value<int32_t>(), 123);
    EXPECT_DOUBLE_EQ(deserialized_record.fieldAt(1).value<double>(), 45.67);
    EXPECT_EQ(deserialized_record.fieldAt(2).value<std::string>(), "hi");
    EXPECT_EQ(deserialized_record.fieldAt(3).value<bool>(), true);
    
    auto bytes_field = deserialized_record.fieldAt(4).value<std::vector<uint8_t>>();
    ASSERT_EQ(bytes_field.size(), 3);
    EXPECT_EQ(bytes_field[0], 1);
    EXPECT_EQ(bytes_field[1], 2);
    EXPECT_EQ(bytes_field[2], 3);
}

TEST(AvroTest, GuidInHeader) {
    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);
    
    // Create serializer configuration with header schema ID serializer
    auto ser_config = SerializerConfig::createDefault();
    ser_config.schema_id_serializer = headerSchemaIdSerializer;
    auto deser_config = DeserializerConfig::createDefault();
    
    // Define the Avro schema
    const std::string schema_str = R"({
        "type": "record",
        "name": "test",
        "fields": [
            {"name": "intField", "type": "int"},
            {"name": "doubleField", "type": "double"},
            {"name": "stringField", "type": "string"},
            {"name": "booleanField", "type": "boolean"},
            {"name": "bytesField", "type": "bytes"}
        ]
    })";
    
    // Create schema object
    schemaregistry::rest::model::Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("AVRO"));
    schema.setSchema(std::make_optional<std::string>(schema_str));
    
    // Parse the Avro schema
    ::avro::ValidSchema avro_schema = AvroSerializer::compileJsonSchema(schema_str);
    
    // Create the Avro record with test data
    ::avro::GenericDatum datum(avro_schema);
    auto& record = datum.value<::avro::GenericRecord>();
    
    // Set field values
    record.setFieldAt(0, ::avro::GenericDatum(static_cast<int32_t>(123)));           // intField
    record.setFieldAt(1, ::avro::GenericDatum(45.67));                              // doubleField  
    record.setFieldAt(2, ::avro::GenericDatum(std::string("hi")));                  // stringField
    record.setFieldAt(3, ::avro::GenericDatum(true));                               // booleanField
    record.setFieldAt(4, ::avro::GenericDatum(std::vector<uint8_t>{1, 2, 3}));      // bytesField
    
    // Create rule registry
    auto rule_registry = std::make_shared<RuleRegistry>();
    
    // Create serializer and deserializer
    AvroSerializer serializer(client, std::make_optional(schema), rule_registry, ser_config);
    AvroDeserializer deserializer(client, rule_registry, deser_config);
    
    // Create serialization context with headers
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Avro;
    ser_ctx.headers = std::make_optional<SerdeHeaders>(SerdeHeaders());
    
    // Serialize the record (schema ID should be stored in header)
    auto serialized_bytes = serializer.serialize(ser_ctx, datum);
    
    // Deserialize the record
    auto deserialized_value = deserializer.deserialize(ser_ctx, serialized_bytes);
    
    // Verify the deserialized record
    ASSERT_TRUE(deserialized_value.value.type() == ::avro::AVRO_RECORD);
    
    auto& deserialized_record = deserialized_value.value.value<::avro::GenericRecord>();
    ASSERT_EQ(deserialized_record.fieldCount(), 5);
    
    // Verify field values
    EXPECT_EQ(deserialized_record.fieldAt(0).value<int32_t>(), 123);
    EXPECT_DOUBLE_EQ(deserialized_record.fieldAt(1).value<double>(), 45.67);
    EXPECT_EQ(deserialized_record.fieldAt(2).value<std::string>(), "hi");
    EXPECT_EQ(deserialized_record.fieldAt(3).value<bool>(), true);
    
    auto bytes_field = deserialized_record.fieldAt(4).value<std::vector<uint8_t>>();
    ASSERT_EQ(bytes_field.size(), 3);
    EXPECT_EQ(bytes_field[0], 1);
    EXPECT_EQ(bytes_field[1], 2);
    EXPECT_EQ(bytes_field[2], 3);
}

#ifdef SCHEMAREGISTRY_USE_RULES

TEST(AvroTest, CelCondition) {
    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);
    
    // Create serializer configuration
    auto ser_config = SerializerConfig(
        false,  // auto_register_schemas
        std::make_optional(SchemaSelector::useLatestVersion()),  // use_schema
        true,   // normalize_schemas
        false,  // validate
        std::unordered_map<std::string, std::string>{}  // rule_config
    );
    auto deser_config = DeserializerConfig::createDefault();
    
    // Define the Avro schema
    const std::string schema_str = R"({
        "type": "record",
        "name": "test",
        "fields": [
            {"name": "intField", "type": "int"},
            {"name": "doubleField", "type": "double"},
            {"name": "stringField", "type": "string"},
            {"name": "booleanField", "type": "boolean"},
            {"name": "bytesField", "type": "bytes"}
        ]
    })";
    
    // Create CEL rule for condition checking
    Rule cel_rule;
    cel_rule.setName(std::make_optional<std::string>("test-cel"));
    cel_rule.setKind(std::make_optional<Kind>(Kind::Condition));
    cel_rule.setMode(std::make_optional<Mode>(Mode::Write));
    cel_rule.setType(std::make_optional<std::string>("CEL"));
    cel_rule.setExpr(std::make_optional<std::string>("message.stringField == 'hi'"));
    
    // Create rule set with domain rules
    RuleSet rule_set;
    std::vector<Rule> domain_rules = {cel_rule};
    rule_set.setDomainRules(std::make_optional<std::vector<Rule>>(domain_rules));
    
    // Create schema object with rule set
    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("AVRO"));
    schema.setSchema(std::make_optional<std::string>(schema_str));
    schema.setRuleSet(std::make_optional<RuleSet>(rule_set));
    
    // Register the schema
    auto registered_schema = client->registerSchema("test-value", schema, false);
    
    // Parse the Avro schema
    ::avro::ValidSchema avro_schema = AvroSerializer::compileJsonSchema(schema_str);
    
    // Create the Avro record with test data
    ::avro::GenericDatum datum(avro_schema);
    auto& record = datum.value<::avro::GenericRecord>();
    
    // Set field values
    record.setFieldAt(0, ::avro::GenericDatum(static_cast<int32_t>(123)));           // intField
    record.setFieldAt(1, ::avro::GenericDatum(45.67));                              // doubleField  
    record.setFieldAt(2, ::avro::GenericDatum(std::string("hi")));                  // stringField
    record.setFieldAt(3, ::avro::GenericDatum(true));                               // booleanField
    record.setFieldAt(4, ::avro::GenericDatum(std::vector<uint8_t>{1, 2, 3}));      // bytesField
    
    // Create rule registry and register CEL executor
    auto rule_registry = std::make_shared<RuleRegistry>();
    auto cel_executor = std::make_shared<CelExecutor>();
    rule_registry->registerExecutor(cel_executor);
    
    // Create serializer and deserializer
    AvroSerializer serializer(client, std::nullopt, rule_registry, ser_config);
    AvroDeserializer deserializer(client, rule_registry, deser_config);
    
    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Avro;
    
    // Serialize the record (CEL condition should be evaluated)
    auto serialized_bytes = serializer.serialize(ser_ctx, datum);
    
    // Deserialize the record
    auto deserialized_value = deserializer.deserialize(ser_ctx, serialized_bytes);
    
    // Verify the deserialized record
    ASSERT_TRUE(deserialized_value.value.type() == ::avro::AVRO_RECORD);
    
    auto& deserialized_record = deserialized_value.value.value<::avro::GenericRecord>();
    ASSERT_EQ(deserialized_record.fieldCount(), 5);
    
    // Verify field values - should match original data since CEL condition was satisfied
    EXPECT_EQ(deserialized_record.fieldAt(0).value<int32_t>(), 123);
    EXPECT_DOUBLE_EQ(deserialized_record.fieldAt(1).value<double>(), 45.67);
    EXPECT_EQ(deserialized_record.fieldAt(2).value<std::string>(), "hi");
    EXPECT_EQ(deserialized_record.fieldAt(3).value<bool>(), true);
    
    auto bytes_field = deserialized_record.fieldAt(4).value<std::vector<uint8_t>>();
    ASSERT_EQ(bytes_field.size(), 3);
    EXPECT_EQ(bytes_field[0], 1);
    EXPECT_EQ(bytes_field[1], 2);
    EXPECT_EQ(bytes_field[2], 3);
}

TEST(AvroTest, CelFieldTransformation) {
    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);
    
    // Create serializer configuration
    auto ser_config = SerializerConfig(
        false,  // auto_register_schemas
        std::make_optional(SchemaSelector::useLatestVersion()),  // use_schema
        true,   // normalize_schemas
        false,  // validate
        std::unordered_map<std::string, std::string>{}  // rule_config
    );
    auto deser_config = DeserializerConfig::createDefault();
    
    // Define the Avro schema
    const std::string schema_str = R"({
        "type": "record",
        "name": "test",
        "fields": [
            {"name": "intField", "type": "int"},
            {"name": "doubleField", "type": "double"},
            {"name": "stringField", "type": "string"},
            {"name": "booleanField", "type": "boolean"},
            {"name": "bytesField", "type": "bytes"}
        ]
    })";
    
    // Create CEL rule for field transformation
    Rule cel_rule;
    cel_rule.setName(std::make_optional<std::string>("test-cel"));
    cel_rule.setKind(std::make_optional<Kind>(Kind::Transform));
    cel_rule.setMode(std::make_optional<Mode>(Mode::Write));
    cel_rule.setType(std::make_optional<std::string>("CEL_FIELD"));
    cel_rule.setExpr(std::make_optional<std::string>("name == 'stringField' ; value + '-suffix'"));
    
    // Create rule set with domain rules
    RuleSet rule_set;
    std::vector<Rule> domain_rules = {cel_rule};
    rule_set.setDomainRules(std::make_optional<std::vector<Rule>>(domain_rules));
    
    // Create schema object with rule set
    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("AVRO"));
    schema.setSchema(std::make_optional<std::string>(schema_str));
    schema.setRuleSet(std::make_optional<RuleSet>(rule_set));
    
    // Register the schema
    auto registered_schema = client->registerSchema("test-value", schema, false);
    
    // Parse the Avro schema
    ::avro::ValidSchema avro_schema = AvroSerializer::compileJsonSchema(schema_str);
    
    // Create the Avro record with test data
    ::avro::GenericDatum datum(avro_schema);
    auto& record = datum.value<::avro::GenericRecord>();
    
    // Set field values
    record.setFieldAt(0, ::avro::GenericDatum(static_cast<int32_t>(123)));           // intField
    record.setFieldAt(1, ::avro::GenericDatum(45.67));                              // doubleField  
    record.setFieldAt(2, ::avro::GenericDatum(std::string("hi")));                  // stringField
    record.setFieldAt(3, ::avro::GenericDatum(true));                               // booleanField
    record.setFieldAt(4, ::avro::GenericDatum(std::vector<uint8_t>{1, 2, 3}));      // bytesField
    
    // Create rule registry and register CEL field executor
    auto rule_registry = std::make_shared<RuleRegistry>();
    auto cel_executor = std::make_shared<CelFieldExecutor>();
    rule_registry->registerExecutor(cel_executor);
    
    // Create serializer and deserializer
    AvroSerializer serializer(client, std::nullopt, rule_registry, ser_config);
    AvroDeserializer deserializer(client, rule_registry, deser_config);
    
    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Avro;
    
    // Serialize the record
    auto serialized_bytes = serializer.serialize(ser_ctx, datum);
    
    // Deserialize the record
    auto deserialized_value = deserializer.deserialize(ser_ctx, serialized_bytes);
    
    // Verify the deserialized record
    ASSERT_TRUE(deserialized_value.value.type() == ::avro::AVRO_RECORD);
    
    auto& deserialized_record = deserialized_value.value.value<::avro::GenericRecord>();
    ASSERT_EQ(deserialized_record.fieldCount(), 5);
    
    // Verify field values - stringField should be transformed with "-suffix"
    EXPECT_EQ(deserialized_record.fieldAt(0).value<int32_t>(), 123);
    EXPECT_DOUBLE_EQ(deserialized_record.fieldAt(1).value<double>(), 45.67);
    EXPECT_EQ(deserialized_record.fieldAt(2).value<std::string>(), "hi-suffix"); // Should be transformed
    EXPECT_EQ(deserialized_record.fieldAt(3).value<bool>(), true);
    
    auto bytes_field = deserialized_record.fieldAt(4).value<std::vector<uint8_t>>();
    ASSERT_EQ(bytes_field.size(), 3);
    EXPECT_EQ(bytes_field[0], 1);
    EXPECT_EQ(bytes_field[1], 2);
    EXPECT_EQ(bytes_field[2], 3);
}

TEST(AvroTest, JsonataWithCelField) {
    // JSONATA rule to transform "size" field to "height" field
    const std::string rule1_to_2 = 
        "$merge([$sift($, function($v, $k) {$k != 'size'}), {'height': $.'size'}])";

    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);
    
    // Set up server config with compatibility group for versioning
    ServerConfig server_config;
    server_config.setCompatibilityGroup(std::make_optional<std::string>("application.version"));
    
    // Update the server configuration for the subject
    client->updateConfig("test-value", server_config);
    
    // Define the old schema (v1) 
    const std::string old_schema_str = R"({
        "type": "record",
        "name": "old",
        "fields": [
            {"name": "name", "type": "string"},
            {"name": "size", "type": "int"},
            {"name": "version", "type": "int"}
        ]
    })";
    
    // Create metadata for v1 schema
    Metadata v1_metadata;
    std::map<std::string, std::string> v1_properties = {
        {"application.version", "v1"}
    };
    v1_metadata.setProperties(std::make_optional<std::map<std::string, std::string>>(v1_properties));
    
    // Create and register the old schema (v1)
    Schema old_schema;
    old_schema.setSchemaType(std::make_optional<std::string>("AVRO"));
    old_schema.setSchema(std::make_optional<std::string>(old_schema_str));
    old_schema.setMetadata(std::make_optional<Metadata>(v1_metadata));
    
    auto v1_registered = client->registerSchema("test-value", old_schema, false);
    
    // Define the new schema (v2)
    const std::string new_schema_str = R"({
        "type": "record",
        "name": "new",
        "fields": [
            {"name": "name", "type": "string"},
            {"name": "height", "type": "int"},
            {"name": "version", "type": "int"}
        ]
    })";
    
    // Create JSONATA migration rule
    Rule jsonata_rule;
    jsonata_rule.setName(std::make_optional<std::string>("test-jsonata"));
    jsonata_rule.setKind(std::make_optional<Kind>(Kind::Transform));
    jsonata_rule.setMode(std::make_optional<Mode>(Mode::Upgrade));
    jsonata_rule.setType(std::make_optional<std::string>("JSONATA"));
    jsonata_rule.setExpr(std::make_optional<std::string>(rule1_to_2));
    
    // Create CEL field transformation rule
    Rule cel_rule;
    cel_rule.setName(std::make_optional<std::string>("test-cel"));
    cel_rule.setKind(std::make_optional<Kind>(Kind::Transform));
    cel_rule.setMode(std::make_optional<Mode>(Mode::Read));
    cel_rule.setType(std::make_optional<std::string>("CEL_FIELD"));
    cel_rule.setExpr(std::make_optional<std::string>("name == 'name' ; value + '-suffix'"));
    
    // Create rule set with migration and domain rules
    RuleSet rule_set;
    std::vector<Rule> migration_rules = {jsonata_rule};
    std::vector<Rule> domain_rules = {cel_rule};
    rule_set.setMigrationRules(std::make_optional<std::vector<Rule>>(migration_rules));
    rule_set.setDomainRules(std::make_optional<std::vector<Rule>>(domain_rules));
    
    // Create metadata for v2 schema
    Metadata v2_metadata;
    std::map<std::string, std::string> v2_properties = {
        {"application.version", "v2"}
    };
    v2_metadata.setProperties(std::make_optional<std::map<std::string, std::string>>(v2_properties));
    
    // Create and register the new schema (v2) with rules
    Schema new_schema;
    new_schema.setSchemaType(std::make_optional<std::string>("AVRO"));
    new_schema.setSchema(std::make_optional<std::string>(new_schema_str));
    new_schema.setMetadata(std::make_optional<Metadata>(v2_metadata));
    new_schema.setRuleSet(std::make_optional<RuleSet>(rule_set));
    
    auto v2_registered = client->registerSchema("test-value", new_schema, false);
    
    // Parse the old schema for creating test data
    ::avro::ValidSchema avro_schema = AvroSerializer::compileJsonSchema(old_schema_str);
    
    // Create the Avro record with test data (using old schema format)
    ::avro::GenericDatum datum(avro_schema);
    ::avro::GenericRecord record(avro_schema.root());
    
    // Set field values using the old schema structure
    record.setFieldAt(0, ::avro::GenericDatum(std::string("alice")));        // name
    record.setFieldAt(1, ::avro::GenericDatum(static_cast<int32_t>(123)));    // size
    record.setFieldAt(2, ::avro::GenericDatum(static_cast<int32_t>(1)));      // version
    
    // Assign the record to the datum
    datum.value<::avro::GenericRecord>() = record;
    
    // Create rule registry and register executors
    auto rule_registry = std::make_shared<RuleRegistry>();
    auto cel_executor = std::make_shared<CelFieldExecutor>();
    auto jsonata_executor = std::make_shared<JsonataExecutor>();
    rule_registry->registerExecutor(cel_executor);
    rule_registry->registerExecutor(jsonata_executor);
    
    // Create serializer configuration to use v1 schema
    std::unordered_map<std::string, std::string> v1_selector_metadata = {
        {"application.version", "v1"}
    };
    auto ser_config = SerializerConfig(
        false,  // auto_register_schemas
        std::make_optional(SchemaSelector::useLatestWithMetadata(v1_selector_metadata)),
        false,  // normalize_schemas  
        false,  // validate
        std::unordered_map<std::string, std::string>{}  // rule_config
    );
    
    // Create deserializer configuration to use v2 schema
    std::unordered_map<std::string, std::string> v2_selector_metadata = {
        {"application.version", "v2"}
    };
    auto deser_config = DeserializerConfig(
        std::make_optional(SchemaSelector::useLatestWithMetadata(v2_selector_metadata)),
        false,  // validate
        std::unordered_map<std::string, std::string>{}  // rule_config
    );
    
    // Create serializer and deserializer
    AvroSerializer serializer(client, std::nullopt, rule_registry, ser_config);
    AvroDeserializer deserializer(client, rule_registry, deser_config);
    
    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Avro;
    
    // Serialize using v1 schema
    auto serialized_bytes = serializer.serialize(ser_ctx, datum);
    
    // Deserialize using v2 schema (should apply transformations)
    auto deserialized_value = deserializer.deserialize(ser_ctx, serialized_bytes);
    
    // Verify the deserialized record
    ASSERT_TRUE(deserialized_value.value.type() == ::avro::AVRO_RECORD);
    
    auto& deserialized_record = deserialized_value.value.value<::avro::GenericRecord>();
    ASSERT_EQ(deserialized_record.fieldCount(), 3);
    
    // Verify field transformations:
    // 1. "name" should be transformed by CEL rule: "alice" -> "alice-suffix" 
    // 2. "size" should be transformed to "height" by JSONATA rule: 123 -> 123
    // 3. "version" should remain unchanged: 1 -> 1
    EXPECT_EQ(deserialized_record.fieldAt(0).value<std::string>(), "alice-suffix");  // name with CEL suffix
    EXPECT_EQ(deserialized_record.fieldAt(1).value<int32_t>(), 123);                 // height (was size)
    EXPECT_EQ(deserialized_record.fieldAt(2).value<int32_t>(), 1);                   // version unchanged
}

TEST(AvroTest, FieldEncryption) {
    // Register local KMS driver
    LocalKmsDriver::registerDriver();

    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);
    
    // Create mock DEK registry client for encryption
    auto dek_client = std::make_shared<MockDekRegistryClient>(client_config);
    
    // Create rule configuration with secret
    std::unordered_map<std::string, std::string> rule_config = {
        {"secret", "mysecret"}
    };
    
    // Create serializer configuration
    auto ser_config = SerializerConfig(
        false,  // auto_register_schemas
        std::make_optional(SchemaSelector::useLatestVersion()),  // use_schema
        false,  // normalize_schemas
        false,  // validate
        rule_config  // rule_config
    );
    auto deser_config = DeserializerConfig::createDefault();
    
    // Define the Avro schema with PII tags
    const std::string schema_str = R"({
        "type": "record",
        "name": "test",
        "fields": [
            {"name": "intField", "type": "int"},
            {"name": "doubleField", "type": "double"},
            {"name": "stringField", "type": "string", "confluent:tags": ["PII"]},
            {"name": "booleanField", "type": "boolean"},
            {"name": "bytesField", "type": "bytes", "confluent:tags": ["PII"]}
        ]
    })";
    
    // Create encryption rule for PII fields
    Rule encrypt_rule;
    encrypt_rule.setName(std::make_optional<std::string>("test-encrypt"));
    encrypt_rule.setKind(std::make_optional<Kind>(Kind::Transform));
    encrypt_rule.setMode(std::make_optional<Mode>(Mode::WriteRead));
    encrypt_rule.setType(std::make_optional<std::string>("ENCRYPT"));
    
    // Set rule tags to target PII fields
    std::vector<std::string> tags = {"PII"};
    encrypt_rule.setTags(std::make_optional<std::vector<std::string>>(tags));
    
    // Set rule parameters for local KMS - use std::map instead of std::unordered_map
    std::map<std::string, std::string> params = {
        {"encrypt.kek.name", "kek1"},
        {"encrypt.kms.type", "local-kms"},
        {"encrypt.kms.key.id", "mykey"}
    };
    encrypt_rule.setParams(std::make_optional<std::map<std::string, std::string>>(params));
    encrypt_rule.setOnFailure(std::make_optional<std::string>("ERROR,NONE"));
    
    // Create rule set with domain rules
    RuleSet rule_set;
    std::vector<Rule> domain_rules = {encrypt_rule};
    rule_set.setDomainRules(std::make_optional<std::vector<Rule>>(domain_rules));
    
    // Create schema object with rule set
    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("AVRO"));
    schema.setSchema(std::make_optional<std::string>(schema_str));
    schema.setRuleSet(std::make_optional<RuleSet>(rule_set));
    
    // Register the schema
    auto registered_schema = client->registerSchema("test-value", schema, false);
    
    // Parse the Avro schema
    ::avro::ValidSchema avro_schema = AvroSerializer::compileJsonSchema(schema_str);
    
    // Create the Avro record with test data
    ::avro::GenericDatum datum(avro_schema);
    auto& record = datum.value<::avro::GenericRecord>();
    
    // Set field values
    record.setFieldAt(0, ::avro::GenericDatum(static_cast<int32_t>(123)));           // intField
    record.setFieldAt(1, ::avro::GenericDatum(45.67));                              // doubleField  
    record.setFieldAt(2, ::avro::GenericDatum(std::string("hi")));                  // stringField (PII)
    record.setFieldAt(3, ::avro::GenericDatum(true));                               // booleanField
    record.setFieldAt(4, ::avro::GenericDatum(std::vector<uint8_t>{1, 2, 3}));      // bytesField (PII)
    
    // Create rule registry and register field encryption executor
    auto rule_registry = std::make_shared<RuleRegistry>();
    auto clock = std::make_shared<SystemClock>();
    auto encryption_executor = std::make_shared<FieldEncryptionExecutor>(clock);
    rule_registry->registerExecutor(encryption_executor);
    
    // Create serializer and deserializer
    AvroSerializer serializer(client, std::nullopt, rule_registry, ser_config);
    AvroDeserializer deserializer(client, rule_registry, deser_config);
    
    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Avro;
    
    // Serialize the record (PII fields should be encrypted)
    auto serialized_bytes = serializer.serialize(ser_ctx, datum);
    
    // Deserialize the record (PII fields should be decrypted)
    auto deserialized_value = deserializer.deserialize(ser_ctx, serialized_bytes);
    
    // Verify the deserialized record
    ASSERT_TRUE(deserialized_value.value.type() == ::avro::AVRO_RECORD);
    
    auto& deserialized_record = deserialized_value.value.value<::avro::GenericRecord>();
    ASSERT_EQ(deserialized_record.fieldCount(), 5);
    
    // Verify field values - should match original after encryption/decryption round trip
    EXPECT_EQ(deserialized_record.fieldAt(0).value<int32_t>(), 123);
    EXPECT_DOUBLE_EQ(deserialized_record.fieldAt(1).value<double>(), 45.67);
    EXPECT_EQ(deserialized_record.fieldAt(2).value<std::string>(), "hi");  // PII field
    EXPECT_EQ(deserialized_record.fieldAt(3).value<bool>(), true);
    
    auto bytes_field = deserialized_record.fieldAt(4).value<std::vector<uint8_t>>(); // PII field
    ASSERT_EQ(bytes_field.size(), 3);
    EXPECT_EQ(bytes_field[0], 1);
    EXPECT_EQ(bytes_field[1], 2);
    EXPECT_EQ(bytes_field[2], 3);
}

TEST(AvroTest, PayloadEncryption) {
    // Register local KMS driver
    LocalKmsDriver::registerDriver();

    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);
    
    // Create rule configuration with secret
    std::unordered_map<std::string, std::string> rule_config = {
        {"secret", "mysecret"}
    };
    
    // Create serializer configuration
    auto ser_config = SerializerConfig(
        false,  // auto_register_schemas
        std::make_optional(SchemaSelector::useLatestVersion()),  // use_schema
        false,  // normalize_schemas
        false,  // validate
        rule_config  // rule_config
    );
    auto deser_config = DeserializerConfig::createDefault();
    
    // Define the Avro schema with PII tags
    const std::string schema_str = R"({
        "type": "record",
        "name": "test",
        "fields": [
            {"name": "intField", "type": "int"},
            {"name": "doubleField", "type": "double"},
            {"name": "stringField", "type": "string", "confluent:tags": ["PII"]},
            {"name": "booleanField", "type": "boolean"},
            {"name": "bytesField", "type": "bytes", "confluent:tags": ["PII"]}
        ]
    })";
    
    // Create payload encryption rule (encrypts entire message)
    Rule encrypt_rule;
    encrypt_rule.setName(std::make_optional<std::string>("test-encrypt"));
    encrypt_rule.setKind(std::make_optional<Kind>(Kind::Transform));
    encrypt_rule.setMode(std::make_optional<Mode>(Mode::WriteRead));
    encrypt_rule.setType(std::make_optional<std::string>("ENCRYPT_PAYLOAD"));
    
    // No tags needed for payload encryption (encrypts entire message)
    
    // Set rule parameters for local KMS
    std::map<std::string, std::string> params = {
        {"encrypt.kek.name", "kek1"},
        {"encrypt.kms.type", "local-kms"},
        {"encrypt.kms.key.id", "mykey"}
    };
    encrypt_rule.setParams(std::make_optional<std::map<std::string, std::string>>(params));
    encrypt_rule.setOnFailure(std::make_optional<std::string>("ERROR,NONE"));
    
    // Create rule set with encoding rules (for payload encryption)
    RuleSet rule_set;
    std::vector<Rule> encoding_rules = {encrypt_rule};
    rule_set.setEncodingRules(std::make_optional<std::vector<Rule>>(encoding_rules));
    
    // Create schema object with rule set
    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("AVRO"));
    schema.setSchema(std::make_optional<std::string>(schema_str));
    schema.setRuleSet(std::make_optional<RuleSet>(rule_set));
    
    // Register the schema
    auto registered_schema = client->registerSchema("test-value", schema, false);
    
    // Parse the Avro schema
    ::avro::ValidSchema avro_schema = AvroSerializer::compileJsonSchema(schema_str);
    
    // Create the Avro record with test data
    ::avro::GenericDatum datum(avro_schema);
    auto& record = datum.value<::avro::GenericRecord>();
    
    // Set field values
    record.setFieldAt(0, ::avro::GenericDatum(static_cast<int32_t>(123)));           // intField
    record.setFieldAt(1, ::avro::GenericDatum(45.67));                              // doubleField  
    record.setFieldAt(2, ::avro::GenericDatum(std::string("hi")));                  // stringField
    record.setFieldAt(3, ::avro::GenericDatum(true));                               // booleanField
    record.setFieldAt(4, ::avro::GenericDatum(std::vector<uint8_t>{1, 2, 3}));      // bytesField
    
    // Create rule registry and register payload encryption executor
    auto rule_registry = std::make_shared<RuleRegistry>();
    auto fake_clock = std::make_shared<FakeClock>(0);  // Use FakeClock with time 0 for consistent testing
    auto encryption_executor = std::make_shared<EncryptionExecutor>(fake_clock);
    rule_registry->registerExecutor(encryption_executor);
    
    // Create serializer and deserializer
    AvroSerializer serializer(client, std::nullopt, rule_registry, ser_config);
    AvroDeserializer deserializer(client, rule_registry, deser_config);
    
    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Avro;
    
    // Serialize the record (entire payload should be encrypted)
    auto serialized_bytes = serializer.serialize(ser_ctx, datum);
    
    // Deserialize the record (entire payload should be decrypted)
    auto deserialized_value = deserializer.deserialize(ser_ctx, serialized_bytes);
    
    // Verify the deserialized record
    ASSERT_TRUE(deserialized_value.value.type() == ::avro::AVRO_RECORD);
    
    auto& deserialized_record = deserialized_value.value.value<::avro::GenericRecord>();
    ASSERT_EQ(deserialized_record.fieldCount(), 5);
    
    // Verify field values - should match original after encryption/decryption round trip
    EXPECT_EQ(deserialized_record.fieldAt(0).value<int32_t>(), 123);
    EXPECT_DOUBLE_EQ(deserialized_record.fieldAt(1).value<double>(), 45.67);
    EXPECT_EQ(deserialized_record.fieldAt(2).value<std::string>(), "hi");
    EXPECT_EQ(deserialized_record.fieldAt(3).value<bool>(), true);
    
    auto bytes_field = deserialized_record.fieldAt(4).value<std::vector<uint8_t>>();
    ASSERT_EQ(bytes_field.size(), 3);
    EXPECT_EQ(bytes_field[0], 1);
    EXPECT_EQ(bytes_field[1], 2);
    EXPECT_EQ(bytes_field[2], 3);
}

TEST(AvroTest, EncryptionF1Preserialized) {
    // Register local KMS driver
    LocalKmsDriver::registerDriver();

    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);
    
    // Create rule configuration with secret
    std::unordered_map<std::string, std::string> rule_config = {
        {"secret", "mysecret"}
    };
    
    // Create deserializer configuration
    auto deser_config = DeserializerConfig(
        std::nullopt,  // use_schema  
        false,  // validate
        rule_config  // rule_config
    );
    
    // Define the Avro schema with PII tags for f1 field
    const std::string schema_str = R"({
        "type": "record",
        "name": "f1Schema",
        "fields": [
            {"name": "f1", "type": "string", "confluent:tags": ["PII"]}
        ]
    })";
    
    // Create field encryption rule for PII fields
    Rule encrypt_rule;
    encrypt_rule.setName(std::make_optional<std::string>("test-encrypt"));
    encrypt_rule.setKind(std::make_optional<Kind>(Kind::Transform));
    encrypt_rule.setMode(std::make_optional<Mode>(Mode::WriteRead));
    encrypt_rule.setType(std::make_optional<std::string>("ENCRYPT"));
    
    // Set rule tags to target PII fields
    std::vector<std::string> tags = {"PII"};
    encrypt_rule.setTags(std::make_optional<std::vector<std::string>>(tags));
    
    // Set rule parameters for local KMS
    std::map<std::string, std::string> params = {
        {"encrypt.kek.name", "kek1-f1"},
        {"encrypt.kms.type", "local-kms"},
        {"encrypt.kms.key.id", "mykey"}
    };
    encrypt_rule.setParams(std::make_optional<std::map<std::string, std::string>>(params));
    encrypt_rule.setOnFailure(std::make_optional<std::string>("ERROR,ERROR"));
    
    // Create rule set with domain rules
    RuleSet rule_set;
    std::vector<Rule> domain_rules = {encrypt_rule};
    rule_set.setDomainRules(std::make_optional<std::vector<Rule>>(domain_rules));
    
    // Create schema object with rule set
    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("AVRO"));
    schema.setSchema(std::make_optional<std::string>(schema_str));
    schema.setRuleSet(std::make_optional<RuleSet>(rule_set));
    
    // Register the schema
    auto registered_schema = client->registerSchema("test-value", schema, false);
    
    // Create rule registry and register field encryption executor
    auto rule_registry = std::make_shared<RuleRegistry>();
    auto fake_clock = std::make_shared<FakeClock>(0);  // Use FakeClock with time 0 for consistent testing
    auto encryption_executor = std::make_shared<FieldEncryptionExecutor>(fake_clock);
    rule_registry->registerExecutor(encryption_executor);
    
    // Create deserializer
    AvroDeserializer deserializer(client, rule_registry, deser_config);
    
    // Get the DEK client from the encryption executor to pre-register KEK and DEK
    auto dek_client =
        dynamic_cast<const FieldEncryptionExecutor*>(
            rule_registry->getExecutor("ENCRYPT").get()
        )->getClient();
    ASSERT_NE(dek_client, nullptr);
    
    // Register KEK
    CreateKekRequest kek_req("kek1-f1", "local-kms", "mykey", std::nullopt, std::nullopt, false);
    auto registered_kek = dek_client->registerKek(kek_req);
    
    // Register DEK with pre-encrypted key material
    const std::string encrypted_dek = 
        "07V2ndh02DA73p+dTybwZFm7DKQSZN1tEwQh+FoX1DZLk4Yj2LLu4omYjp/84tAg3BYlkfGSz+zZacJHIE4=";
    CreateDekRequest dek_req("test-value", std::nullopt, std::nullopt, 
                            std::make_optional<std::string>(encrypted_dek));
    auto registered_dek = dek_client->registerDek("kek1-f1", dek_req);
    
    // Pre-serialized encrypted bytes from Rust test
    std::vector<uint8_t> bytes = {
        0, 0, 0, 0, 1, 104, 122, 103, 121, 47, 106, 70, 78, 77, 86, 47, 101, 70, 105, 108, 97,
        72, 114, 77, 121, 101, 66, 103, 100, 97, 86, 122, 114, 82, 48, 117, 100, 71, 101, 111,
        116, 87, 56, 99, 65, 47, 74, 97, 108, 55, 117, 107, 114, 43, 77, 47, 121, 122
    };
    
    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Avro;
    
    // Deserialize the pre-encrypted bytes
    auto deserialized_value = deserializer.deserialize(ser_ctx, bytes);
    
    // Verify the deserialized record
    ASSERT_TRUE(deserialized_value.value.type() == ::avro::AVRO_RECORD);
    
    auto& deserialized_record = deserialized_value.value.value<::avro::GenericRecord>();
    ASSERT_EQ(deserialized_record.fieldCount(), 1);
    
    // Verify field value - should be decrypted to "hello world"
    EXPECT_EQ(deserialized_record.fieldAt(0).value<std::string>(), "hello world");
}

TEST(AvroTest, EncryptionDeterministicF1Preserialized) {
    // Register local KMS driver
    LocalKmsDriver::registerDriver();

    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);
    
    // Create rule configuration with secret
    std::unordered_map<std::string, std::string> rule_config = {
        {"secret", "mysecret"}
    };
    
    // Create deserializer configuration
    auto deser_config = DeserializerConfig(
        std::nullopt,  // use_schema  
        false,  // validate
        rule_config  // rule_config
    );
    
    // Define the Avro schema with PII tags for f1 field
    const std::string schema_str = R"({
        "type": "record",
        "name": "f1Schema",
        "fields": [
            {"name": "f1", "type": "string", "confluent:tags": ["PII"]}
        ]
    })";
    
    // Create field encryption rule for PII fields with AES256_SIV algorithm
    Rule encrypt_rule;
    encrypt_rule.setName(std::make_optional<std::string>("test-encrypt"));
    encrypt_rule.setKind(std::make_optional<Kind>(Kind::Transform));
    encrypt_rule.setMode(std::make_optional<Mode>(Mode::WriteRead));
    encrypt_rule.setType(std::make_optional<std::string>("ENCRYPT"));
    
    // Set rule tags to target PII fields
    std::vector<std::string> tags = {"PII"};
    encrypt_rule.setTags(std::make_optional<std::vector<std::string>>(tags));
    
    // Set rule parameters for local KMS with deterministic encryption (AES256_SIV)
    std::map<std::string, std::string> params = {
        {"encrypt.kek.name", "kek1-det-f1"},
        {"encrypt.kms.type", "local-kms"},
        {"encrypt.kms.key.id", "mykey"},
        {"encrypt.dek.algorithm", "AES256_SIV"}  // Deterministic encryption
    };
    encrypt_rule.setParams(std::make_optional<std::map<std::string, std::string>>(params));
    encrypt_rule.setOnFailure(std::make_optional<std::string>("ERROR,ERROR"));
    
    // Create rule set with domain rules
    RuleSet rule_set;
    std::vector<Rule> domain_rules = {encrypt_rule};
    rule_set.setDomainRules(std::make_optional<std::vector<Rule>>(domain_rules));
    
    // Create schema object with rule set
    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("AVRO"));
    schema.setSchema(std::make_optional<std::string>(schema_str));
    schema.setRuleSet(std::make_optional<RuleSet>(rule_set));
    
    // Register the schema
    auto registered_schema = client->registerSchema("test-value", schema, false);
    
    // Create rule registry and register field encryption executor
    auto rule_registry = std::make_shared<RuleRegistry>();
    auto fake_clock = std::make_shared<FakeClock>(0);  // Use FakeClock with time 0 for consistent testing
    auto encryption_executor = std::make_shared<FieldEncryptionExecutor>(fake_clock);
    rule_registry->registerExecutor(encryption_executor);
    
    // Create deserializer
    AvroDeserializer deserializer(client, rule_registry, deser_config);
    
    // Get the DEK client from the encryption executor to pre-register KEK and DEK
    auto dek_client =
        dynamic_cast<const FieldEncryptionExecutor*>(
            rule_registry->getExecutor("ENCRYPT").get()
        )->getClient();
    ASSERT_NE(dek_client, nullptr);
    
    // Register KEK
    CreateKekRequest kek_req("kek1-det-f1", "local-kms", "mykey", std::nullopt, std::nullopt, false);
    auto registered_kek = dek_client->registerKek(kek_req);
    
    // Register DEK with pre-encrypted key material for AES256_SIV
    const std::string encrypted_dek = 
        "YSx3DTlAHrmpoDChquJMifmPntBzxgRVdMzgYL82rgWBKn7aUSnG+WIu9ozBNS3y2vXd++mBtK07w4/W/G6w0da39X9hfOVZsGnkSvry/QRht84V8yz3dqKxGMOK5A==";
    CreateDekRequest dek_req("test-value", std::nullopt, 
                            std::make_optional<Algorithm>(Algorithm::Aes256Siv),
                            std::make_optional<std::string>(encrypted_dek));
    auto registered_dek = dek_client->registerDek("kek1-det-f1", dek_req);
    
    // Pre-serialized encrypted bytes from Rust test (AES256_SIV produces deterministic output)
    std::vector<uint8_t> bytes = {
        0, 0, 0, 0, 1, 72, 68, 54, 89, 116, 120, 114, 108, 66, 110, 107, 84, 87, 87, 57, 78,
        54, 86, 98, 107, 51, 73, 73, 110, 106, 87, 72, 56, 49, 120, 109, 89, 104, 51, 107, 52,
        100
    };
    
    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Avro;
    
    // Deserialize the pre-encrypted bytes
    auto deserialized_value = deserializer.deserialize(ser_ctx, bytes);
    
    // Verify the deserialized record
    ASSERT_TRUE(deserialized_value.value.type() == ::avro::AVRO_RECORD);
    
    auto& deserialized_record = deserialized_value.value.value<::avro::GenericRecord>();
    ASSERT_EQ(deserialized_record.fieldCount(), 1);
    
    // Verify field value - should be decrypted to "hello world"
    EXPECT_EQ(deserialized_record.fieldAt(0).value<std::string>(), "hello world");
}

TEST(AvroTest, EncryptionDekRotationF1Preserialized) {
    // Register local KMS driver
    LocalKmsDriver::registerDriver();

    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);
    
    // Create rule configuration with secret
    std::unordered_map<std::string, std::string> rule_config = {
        {"secret", "mysecret"}
    };
    
    // Create deserializer configuration
    auto deser_config = DeserializerConfig(
        std::nullopt,  // use_schema  
        false,  // validate
        rule_config  // rule_config
    );
    
    // Define the Avro schema with PII tags for f1 field
    const std::string schema_str = R"({
        "type": "record",
        "name": "f1Schema",
        "fields": [
            {"name": "f1", "type": "string", "confluent:tags": ["PII"]}
        ]
    })";
    
    // Create field encryption rule for PII fields with DEK rotation (1 day expiry)
    Rule encrypt_rule;
    encrypt_rule.setName(std::make_optional<std::string>("test-encrypt"));
    encrypt_rule.setKind(std::make_optional<Kind>(Kind::Transform));
    encrypt_rule.setMode(std::make_optional<Mode>(Mode::WriteRead));
    encrypt_rule.setType(std::make_optional<std::string>("ENCRYPT"));
    
    // Set rule tags to target PII fields
    std::vector<std::string> tags = {"PII"};
    encrypt_rule.setTags(std::make_optional<std::vector<std::string>>(tags));
    
    // Set rule parameters for local KMS with DEK rotation (1 day expiry)
    std::map<std::string, std::string> params = {
        {"encrypt.kek.name", "kek1-rot-f1"},
        {"encrypt.kms.type", "local-kms"},
        {"encrypt.kms.key.id", "mykey"},
        {"encrypt.dek.expiry.days", "1"}  // DEK expires after 1 day
    };
    encrypt_rule.setParams(std::make_optional<std::map<std::string, std::string>>(params));
    encrypt_rule.setOnFailure(std::make_optional<std::string>("ERROR,ERROR"));
    
    // Create rule set with domain rules
    RuleSet rule_set;
    std::vector<Rule> domain_rules = {encrypt_rule};
    rule_set.setDomainRules(std::make_optional<std::vector<Rule>>(domain_rules));
    
    // Create schema object with rule set
    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("AVRO"));
    schema.setSchema(std::make_optional<std::string>(schema_str));
    schema.setRuleSet(std::make_optional<RuleSet>(rule_set));
    
    // Register the schema
    auto registered_schema = client->registerSchema("test-value", schema, false);
    
    // Create rule registry and register field encryption executor
    auto rule_registry = std::make_shared<RuleRegistry>();
    auto fake_clock = std::make_shared<FakeClock>(0);  // Use FakeClock with time 0 for consistent testing
    auto encryption_executor = std::make_shared<FieldEncryptionExecutor>(fake_clock);
    rule_registry->registerExecutor(encryption_executor);
    
    // Create deserializer
    AvroDeserializer deserializer(client, rule_registry, deser_config);
    
    // Get the DEK client from the encryption executor to pre-register KEK and DEK
    auto dek_client =
        dynamic_cast<const FieldEncryptionExecutor*>(
            rule_registry->getExecutor("ENCRYPT").get()
        )->getClient();
    ASSERT_NE(dek_client, nullptr);
    
    // Register KEK
    CreateKekRequest kek_req("kek1-rot-f1", "local-kms", "mykey", std::nullopt, std::nullopt, false);
    auto registered_kek = dek_client->registerKek(kek_req);
    
    // Register DEK with pre-encrypted key material for AES256_GCM (default algorithm)
    const std::string encrypted_dek = 
        "W/v6hOQYq1idVAcs1pPWz9UUONMVZW4IrglTnG88TsWjeCjxmtRQ4VaNe/I5dCfm2zyY9Cu0nqdvqImtUk4=";
    CreateDekRequest dek_req("test-value", std::nullopt, 
                            std::make_optional<Algorithm>(Algorithm::Aes256Gcm),
                            std::make_optional<std::string>(encrypted_dek));
    auto registered_dek = dek_client->registerDek("kek1-rot-f1", dek_req);
    
    // Pre-serialized encrypted bytes from Rust test (includes version for DEK rotation)
    std::vector<uint8_t> bytes = {
        0, 0, 0, 0, 1, 120, 65, 65, 65, 65, 65, 65, 71, 52, 72, 73, 54, 98, 49, 110, 88, 80,
        88, 113, 76, 121, 71, 56, 99, 73, 73, 51, 53, 78, 72, 81, 115, 101, 113, 113, 85, 67,
        100, 43, 73, 101, 76, 101, 70, 86, 65, 101, 78, 112, 83, 83, 51, 102, 120, 80, 110, 74,
        51, 50, 65, 61
    };
    
    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Avro;
    
    // Deserialize the pre-encrypted bytes
    auto deserialized_value = deserializer.deserialize(ser_ctx, bytes);
    
    // Verify the deserialized record
    ASSERT_TRUE(deserialized_value.value.type() == ::avro::AVRO_RECORD);
    
    auto& deserialized_record = deserialized_value.value.value<::avro::GenericRecord>();
    ASSERT_EQ(deserialized_record.fieldCount(), 1);
    
    // Verify field value - should be decrypted to "hello world"
    EXPECT_EQ(deserialized_record.fieldAt(0).value<std::string>(), "hello world");
}

#endif