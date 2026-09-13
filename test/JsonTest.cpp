/**
 * JsonTest
 * Tests for the JSON serialization functionality
 */

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>

// Project includes
#include "schemaregistry/rest/MockSchemaRegistryClient.h"
#include "schemaregistry/rest/ClientConfiguration.h"
#include "schemaregistry/rest/SchemaRegistryClient.h"
#include "schemaregistry/serdes/SerdeConfig.h"
#include "schemaregistry/serdes/SerdeTypes.h"
#include "schemaregistry/serdes/RuleRegistry.h"
#include "schemaregistry/serdes/json/JsonSerializer.h"
#include "schemaregistry/serdes/json/JsonDeserializer.h"
#include "schemaregistry/rest/model/Schema.h"
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
#endif

using namespace schemaregistry::serdes;
using namespace schemaregistry::serdes::json;
using namespace schemaregistry::rest;
using namespace schemaregistry::rest::model;

#ifdef SCHEMAREGISTRY_USE_RULES
using namespace schemaregistry::rules::cel;
using namespace schemaregistry::rules::encryption;
using namespace schemaregistry::rules::encryption::localkms;
#endif

TEST(JsonTest, BasicSerialization) {
    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);
    
    // Create serializer configuration
    auto ser_conf = SerializerConfig::createDefault();
    
    // Define JSON schema string
    std::string schema_str = R"(
    {
        "type": "object",
        "properties": {
            "intField": {"type": "integer"},
            "doubleField": {"type": "number"},
            "stringField": {
                "type": "string",
                "confluent:tags": ["PII"]
            },
            "booleanField": {"type": "boolean"},
            "bytesField": {
                "type": "string",
                "contentEncoding": "base64",
                "confluent:tags": ["PII"]
            }
        }
    }
    )";
    
    // Create schema object
    Schema schema;
    schema.setSchemaType("JSON");
    schema.setSchema(schema_str);
    
    // Create test JSON object
    std::string obj_str = R"(
    {
        "intField": 123,
        "doubleField": 45.67,
        "stringField": "hi",
        "booleanField": true,
        "bytesField": "Zm9vYmFy"
    }
    )";
    
    nlohmann::json obj = nlohmann::json::parse(obj_str);
    
    // Create rule registry
    auto rule_registry = std::make_shared<RuleRegistry>();
    
    // Create JsonSerializer
    JsonSerializer serializer(client, schema, rule_registry, ser_conf);
    
    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Json;
    
    // Serialize the JSON object
    std::vector<uint8_t> bytes = serializer.serialize(ser_ctx, obj);
    
    // Create JsonDeserializer
    auto deser_conf = DeserializerConfig::createDefault();
    JsonDeserializer deserializer(client, rule_registry, deser_conf);
    
    // Deserialize back to JSON object
    nlohmann::json obj2 = deserializer.deserialize(ser_ctx, bytes);
    
    // Assert that the original and deserialized objects are equal
    ASSERT_EQ(obj2, obj);
}

TEST(JsonTest, GuidInHeader) {
    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);
    
    // Create serializer configuration with header schema ID serializer
    auto ser_conf = SerializerConfig::createDefault();
    ser_conf.schema_id_serializer = headerSchemaIdSerializer;
    
    // Define JSON schema string with PII tags
    std::string schema_str = R"(
    {
        "type": "object",
        "properties": {
            "intField": {"type": "integer"},
            "doubleField": {"type": "number"},
            "stringField": {
                "type": "string",
                "confluent:tags": ["PII"]
            },
            "booleanField": {"type": "boolean"},
            "bytesField": {
                "type": "string",
                "contentEncoding": "base64",
                "confluent:tags": ["PII"]
            }
        }
    }
    )";
    
    // Create schema object
    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("JSON"));
    schema.setSchema(std::make_optional<std::string>(schema_str));
    
    // Create test JSON object
    std::string obj_str = R"(
    {
        "intField": 123,
        "doubleField": 45.67,
        "stringField": "hi",
        "booleanField": true,
        "bytesField": "Zm9vYmFy"
    }
    )";
    
    nlohmann::json obj = nlohmann::json::parse(obj_str);
    
    // Create rule registry
    auto rule_registry = std::make_shared<RuleRegistry>();
    
    // Create JsonSerializer with schema
    JsonSerializer serializer(client, std::make_optional<Schema>(schema), rule_registry, ser_conf);
    
    // Create serialization context with headers (schema ID should be stored in header)
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Json;
    ser_ctx.headers = std::make_optional<SerdeHeaders>(SerdeHeaders());
    
    // Serialize the JSON object
    std::vector<uint8_t> bytes = serializer.serialize(ser_ctx, obj);
    
    // Create JsonDeserializer
    auto deser_conf = DeserializerConfig::createDefault();
    JsonDeserializer deserializer(client, rule_registry, deser_conf);
    
    // Deserialize back to JSON object
    nlohmann::json obj2 = deserializer.deserialize(ser_ctx, bytes);
    
    // Assert that the original and deserialized objects are equal
    ASSERT_EQ(obj2, obj);
}

TEST(JsonTest, SerializeReferences) {
    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);
    
    // Create serializer configuration
    auto ser_conf = SerializerConfig(
        false,  // auto_register_schemas
        std::make_optional(SchemaSelector::useLatestVersion()),  // use_schema
        true,   // normalize_schemas  
        true,  // validate
        {}      // rule_config
    );
    
    // Define reference schema string
    std::string ref_schema_str = R"(
    {
        "type": "object",
        "properties": {
            "intField": {"type": "integer"},
            "doubleField": {"type": "number"},
            "stringField": {
                "type": "string",
                "confluent:tags": ["PII"]
            },
            "booleanField": {"type": "boolean"},
            "bytesField": {
                "type": "string",
                "contentEncoding": "base64",
                "confluent:tags": ["PII"]
            }
        }
    }
    )";
    
    // Create reference schema object
    Schema ref_schema;
    ref_schema.setSchemaType(std::make_optional<std::string>("JSON"));
    ref_schema.setSchema(std::make_optional<std::string>(ref_schema_str));
    
    // Register the reference schema
    auto registered_ref_schema = client->registerSchema("ref", ref_schema, false);
    
    // Define main schema string that references the "ref" schema
    std::string schema_str = R"(
    {
        "type": "object",
        "properties": {
            "otherField": {"$ref": "ref"}
        }
    }
    )";
    
    // Create schema reference
    SchemaReference schema_ref;
    schema_ref.setName(std::make_optional<std::string>("ref"));
    schema_ref.setSubject(std::make_optional<std::string>("ref"));
    schema_ref.setVersion(std::make_optional<int32_t>(1));
    
    // Create vector of references
    std::vector<SchemaReference> refs = {schema_ref};
    
    // Create main schema object with references
    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("JSON"));
    schema.setReferences(std::make_optional<std::vector<SchemaReference>>(refs));
    schema.setSchema(std::make_optional<std::string>(schema_str));
    
    // Register the main schema
    auto registered_schema = client->registerSchema("test-value", schema, false);
    
    // Create test JSON object
    std::string obj_str = R"(
    {
        "otherField": {
            "intField": 123,
            "doubleField": 45.67,
            "stringField": "hi",
            "booleanField": true,
            "bytesField": "Zm9vYmFy"
        }
    }
    )";
    
    nlohmann::json obj = nlohmann::json::parse(obj_str);
    
    // Create rule registry
    auto rule_registry = std::make_shared<RuleRegistry>();
    
    // Create JsonSerializer with no specific schema (uses latest from registry)
    JsonSerializer serializer(client, std::nullopt, rule_registry, ser_conf);
    
    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Json;
    
    // Serialize the JSON object
    std::vector<uint8_t> bytes = serializer.serialize(ser_ctx, obj);
    
    // Create JsonDeserializer
    auto deser_conf = DeserializerConfig::createDefault();
    JsonDeserializer deserializer(client, rule_registry, deser_conf);
    
    // Deserialize back to JSON object
    nlohmann::json obj2 = deserializer.deserialize(ser_ctx, bytes);
    
    // Assert that the original and deserialized objects are equal
    ASSERT_EQ(obj2, obj);
}

#ifdef SCHEMAREGISTRY_USE_RULES

TEST(JsonTest, CelField) {
    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);
    
    // Create serializer configuration to use latest schema from registry
    auto ser_conf = SerializerConfig(
        false,  // auto_register_schemas
        std::make_optional(SchemaSelector::useLatestVersion()),  // use_schema
        false,  // normalize_schemas
        true,  // validate
        {}  // rule_config
    );
    
    // Define JSON schema string with PII tags
    std::string schema_str = R"(
    {
        "type": "object",
        "properties": {
            "intField": {"type": "integer"},
            "doubleField": {"type": "number"},
            "stringField": {
                "type": "string",
                "confluent:tags": ["PII"]
            },
            "booleanField": {"type": "boolean"},
            "bytesField": {
                "type": "string",
                "contentEncoding": "base64",
                "confluent:tags": ["PII"]
            }
        }
    }
    )";
    
    // Create CEL rule
    Rule rule;
    rule.setName(std::make_optional<std::string>("test-cel"));
    rule.setKind(std::make_optional<Kind>(Kind::Transform));
    rule.setMode(std::make_optional<Mode>(Mode::Write));
    rule.setType(std::make_optional<std::string>("CEL_FIELD"));
    rule.setExpr(std::make_optional<std::string>("name == 'stringField' ; value + '-suffix'"));
    
    // Create rule set with domain rules
    RuleSet rule_set;
    std::vector<Rule> domain_rules = {rule};
    rule_set.setDomainRules(std::make_optional<std::vector<Rule>>(domain_rules));
    
    // Create schema object with rule set
    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("JSON"));
    schema.setSchema(std::make_optional<std::string>(schema_str));
    schema.setRuleSet(std::make_optional<RuleSet>(rule_set));
    
    // Register the schema
    auto registered_schema = client->registerSchema("test-value", schema, false);
    
    // Create original test JSON object
    std::string obj_str = R"(
    {
        "intField": 123,
        "doubleField": 45.67,
        "stringField": "hi",
        "booleanField": true,
        "bytesField": "Zm9vYmFy"
    }
    )";
    
    nlohmann::json obj = nlohmann::json::parse(obj_str);
    
    // Create rule registry and register CEL field executor
    auto rule_registry = std::make_shared<RuleRegistry>();
    auto cel_field_executor = std::make_shared<CelFieldExecutor>();
    rule_registry->registerExecutor(cel_field_executor);
    
    // Create JsonSerializer with rule registry - no schema needed since we use latest from registry
    JsonSerializer serializer(client, std::nullopt, rule_registry, ser_conf);
    
    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Json;
    
    // Serialize the JSON object
    std::vector<uint8_t> bytes = serializer.serialize(ser_ctx, obj);
    
    // Create JsonDeserializer with rule registry
    auto deser_conf = DeserializerConfig::createDefault();
    JsonDeserializer deserializer(client, rule_registry, deser_conf);
    
    // Create expected JSON object (with "-suffix" added to stringField)
    std::string expected_obj_str = R"(
    {
        "intField": 123,
        "doubleField": 45.67,
        "stringField": "hi-suffix",
        "booleanField": true,
        "bytesField": "Zm9vYmFy"
    }
    )";
    
    nlohmann::json expected_obj = nlohmann::json::parse(expected_obj_str);
    
    // Deserialize back to JSON object
    nlohmann::json obj2 = deserializer.deserialize(ser_ctx, bytes);
    
    // Assert that the deserialized object matches the expected object (with suffix)
    ASSERT_EQ(obj2, expected_obj);
}

TEST(JsonTest, CelFieldWithNullable) {
    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);

    // Create serializer configuration to use latest schema from registry
    auto ser_conf = SerializerConfig(
        false,  // auto_register_schemas
        std::make_optional(SchemaSelector::useLatestVersion()),  // use_schema
        false,  // normalize_schemas
        true,  // validate
        {}  // rule_config
    );

    // Define JSON schema string with PII tags
    std::string schema_str = R"(
    {
        "type": "object",
        "properties": {
            "intField": {"type": "integer"},
            "doubleField": {"type": "number"},
            "stringField": {
                "type": ["string", "null"],
                "confluent:tags": ["PII"]
            },
            "booleanField": {"type": "boolean"},
            "bytesField": {
                "type": "string",
                "contentEncoding": "base64",
                "confluent:tags": ["PII"]
            }
        }
    }
    )";

    // Create CEL rule
    Rule rule;
    rule.setName(std::make_optional<std::string>("test-cel"));
    rule.setKind(std::make_optional<Kind>(Kind::Transform));
    rule.setMode(std::make_optional<Mode>(Mode::Write));
    rule.setType(std::make_optional<std::string>("CEL_FIELD"));
    rule.setExpr(std::make_optional<std::string>("name == 'stringField' ; value + '-suffix'"));

    // Create rule set with domain rules
    RuleSet rule_set;
    std::vector<Rule> domain_rules = {rule};
    rule_set.setDomainRules(std::make_optional<std::vector<Rule>>(domain_rules));

    // Create schema object with rule set
    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("JSON"));
    schema.setSchema(std::make_optional<std::string>(schema_str));
    schema.setRuleSet(std::make_optional<RuleSet>(rule_set));

    // Register the schema
    auto registered_schema = client->registerSchema("test-value", schema, false);

    // Create original test JSON object
    std::string obj_str = R"(
    {
        "intField": 123,
        "doubleField": 45.67,
        "stringField": "hi",
        "booleanField": true,
        "bytesField": "Zm9vYmFy"
    }
    )";

    nlohmann::json obj = nlohmann::json::parse(obj_str);

    // Create rule registry and register CEL field executor
    auto rule_registry = std::make_shared<RuleRegistry>();
    auto cel_field_executor = std::make_shared<CelFieldExecutor>();
    rule_registry->registerExecutor(cel_field_executor);

    // Create JsonSerializer with rule registry - no schema needed since we use latest from registry
    JsonSerializer serializer(client, std::nullopt, rule_registry, ser_conf);

    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Json;

    // Serialize the JSON object
    std::vector<uint8_t> bytes = serializer.serialize(ser_ctx, obj);

    // Create JsonDeserializer with rule registry
    auto deser_conf = DeserializerConfig::createDefault();
    JsonDeserializer deserializer(client, rule_registry, deser_conf);

    // Create expected JSON object (with "-suffix" added to stringField)
    std::string expected_obj_str = R"(
    {
        "intField": 123,
        "doubleField": 45.67,
        "stringField": "hi-suffix",
        "booleanField": true,
        "bytesField": "Zm9vYmFy"
    }
    )";

    nlohmann::json expected_obj = nlohmann::json::parse(expected_obj_str);

    // Deserialize back to JSON object
    nlohmann::json obj2 = deserializer.deserialize(ser_ctx, bytes);

    // Assert that the deserialized object matches the expected object (with suffix)
    ASSERT_EQ(obj2, expected_obj);
}

TEST(JsonTest, CelFieldWithUnionOfRefs) {
    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);
    
    // Create serializer configuration to use latest schema from registry
    auto ser_conf = SerializerConfig(
        false,  // auto_register_schemas
        std::make_optional(SchemaSelector::useLatestVersion()),  // use_schema
        false,  // normalize_schemas
        true,  // validate
        {}  // rule_config
    );
    
    // Define JSON schema string with PII tags
    std::string schema_str = R"(
    {
        "type": "object",
        "properties": {
            "messageType": {
                "type": "string"
            },
            "version": {
                "type": "string"
            },
            "payload": {
                "type": "object",
                "oneOf": [
                {
                    "$ref": "#/$defs/authentication_request"
                },
                {
                    "$ref": "#/$defs/authentication_status"
                }
                ]
            }
        },
        "required": [
        "payload",
        "messageType",
        "version"
        ],
        "$defs": {
            "authentication_request": {
                "properties": {
                    "messageId": {
                        "type": "string",
                        "confluent:tags": ["PII"]
                    },
                    "timestamp": {
                        "type": "integer",
                        "minimum": 0
                    },
                    "requestId": {
                        "type": "string"
                    }
                },
                "required": [
                "messageId",
                "timestamp"
                ]
            },
            "authentication_status": {
                "properties": {
                    "messageId": {
                        "type": "string",
                        "confluent:tags": ["PII"]
                    },
                    "authType": {
                        "type": [
                        "string",
                        "null"
                        ]
                    }
                },
                "required": [
                "messageId",
                "authType"
                ]
            }
        }
    }
    )";
    
    // Create CEL rule
    Rule rule;
    rule.setName(std::make_optional<std::string>("test-cel"));
    rule.setKind(std::make_optional<Kind>(Kind::Transform));
    rule.setMode(std::make_optional<Mode>(Mode::Write));
    rule.setType(std::make_optional<std::string>("CEL_FIELD"));
    rule.setExpr(std::make_optional<std::string>("name == 'messageId' ; value + '-suffix'"));
    
    // Create rule set with domain rules
    RuleSet rule_set;
    std::vector<Rule> domain_rules = {rule};
    rule_set.setDomainRules(std::make_optional<std::vector<Rule>>(domain_rules));
    
    // Create schema object with rule set
    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("JSON"));
    schema.setSchema(std::make_optional<std::string>(schema_str));
    schema.setRuleSet(std::make_optional<RuleSet>(rule_set));
    
    // Register the schema
    auto registered_schema = client->registerSchema("test-value", schema, false);
    
    // Create original test JSON object
    std::string obj_str = R"(
    {
        "messageType": "authentication_request",
        "version": "1.0",
        "payload": {
            "messageId": "12345",
            "timestamp": 1757410647
        }
    }
    )";

    nlohmann::json obj = nlohmann::json::parse(obj_str);
    
    // Create rule registry and register CEL field executor
    auto rule_registry = std::make_shared<RuleRegistry>();
    auto cel_field_executor = std::make_shared<CelFieldExecutor>();
    rule_registry->registerExecutor(cel_field_executor);
    
    // Create JsonSerializer with rule registry - no schema needed since we use latest from registry
    JsonSerializer serializer(client, std::nullopt, rule_registry, ser_conf);
    
    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Json;
    
    // Serialize the JSON object
    std::vector<uint8_t> bytes = serializer.serialize(ser_ctx, obj);
    
    // Create JsonDeserializer with rule registry
    auto deser_conf = DeserializerConfig::createDefault();
    JsonDeserializer deserializer(client, rule_registry, deser_conf);
    
    // Create expected JSON object (with "-suffix" added to messageId)
    std::string expected_obj_str = R"(
    {
        "messageType": "authentication_request",
        "version": "1.0",
        "payload": {
            "messageId": "12345-suffix",
            "timestamp": 1757410647
        }
    }
    )";

    nlohmann::json expected_obj = nlohmann::json::parse(expected_obj_str);
    
    // Deserialize back to JSON object
    nlohmann::json obj2 = deserializer.deserialize(ser_ctx, bytes);

    // Assert that the deserialized object matches the expected object (with suffix)
    ASSERT_EQ(obj2, expected_obj);
}

TEST(JsonTest, CelFieldTransformAllOf) {
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);

    auto ser_conf = SerializerConfig(
        false,
        std::make_optional(SchemaSelector::useLatestVersion()),
        false,
        true,
        {}
    );

    std::string schema_str = R"(
    {
        "type": "object",
        "properties": {
            "pins": {
                "type": "object",
                "allOf": [
                    {
                        "properties": {
                            "pin": {
                                "confluent:tags": ["PII"],
                                "type": ["string", "null"]
                            }
                        }
                    },
                    {
                        "properties": {
                            "npin": {
                                "confluent:tags": ["PII"],
                                "type": ["string", "null"]
                            }
                        }
                    }
                ]
            }
        }
    }
    )";

    Rule rule;
    rule.setName(std::make_optional<std::string>("test-cel"));
    rule.setKind(std::make_optional<Kind>(Kind::Transform));
    rule.setMode(std::make_optional<Mode>(Mode::Write));
    rule.setType(std::make_optional<std::string>("CEL_FIELD"));
    std::vector<std::string> tags = {"PII"};
    rule.setTags(std::make_optional<std::vector<std::string>>(tags));
    rule.setExpr(std::make_optional<std::string>("value + '-suffix'"));

    RuleSet rule_set;
    std::vector<Rule> domain_rules = {rule};
    rule_set.setDomainRules(std::make_optional<std::vector<Rule>>(domain_rules));

    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("JSON"));
    schema.setSchema(std::make_optional<std::string>(schema_str));
    schema.setRuleSet(std::make_optional<RuleSet>(rule_set));

    auto registered_schema = client->registerSchema("test-value", schema, false);

    std::string obj_str = R"(
    { "pins": { "pin": "P123456789", "npin": "NP00012345678" } }
    )";
    nlohmann::json obj = nlohmann::json::parse(obj_str);

    auto rule_registry = std::make_shared<RuleRegistry>();
    auto cel_field_executor = std::make_shared<CelFieldExecutor>();
    rule_registry->registerExecutor(cel_field_executor);

    JsonSerializer serializer(client, std::nullopt, rule_registry, ser_conf);

    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Json;

    std::vector<uint8_t> bytes = serializer.serialize(ser_ctx, obj);

    auto deser_conf = DeserializerConfig::createDefault();
    JsonDeserializer deserializer(client, rule_registry, deser_conf);

    std::string expected_obj_str = R"(
    { "pins": { "pin": "P123456789-suffix", "npin": "NP00012345678-suffix" } }
    )";
    nlohmann::json expected_obj = nlohmann::json::parse(expected_obj_str);

    nlohmann::json obj2 = deserializer.deserialize(ser_ctx, bytes);
    ASSERT_EQ(obj2, expected_obj);
}

TEST(JsonTest, CelFieldTransformNestedAnyOf) {
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);

    auto ser_conf = SerializerConfig(
        false,
        std::make_optional(SchemaSelector::useLatestVersion()),
        false,
        true,
        {}
    );

    std::string schema_str = R"(
    {
        "type": "object",
        "properties": {
            "pins": {
                "type": "object",
                "anyOf": [
                    {
                        "properties": {
                            "pin": {
                                "confluent:tags": ["PII"],
                                "type": ["string", "null"]
                            }
                        }
                    },
                    {
                        "properties": {
                            "npin": {
                                "confluent:tags": ["PII"],
                                "type": ["string", "null"]
                            }
                        }
                    }
                ]
            }
        }
    }
    )";

    Rule rule;
    rule.setName(std::make_optional<std::string>("test-cel"));
    rule.setKind(std::make_optional<Kind>(Kind::Transform));
    rule.setMode(std::make_optional<Mode>(Mode::Write));
    rule.setType(std::make_optional<std::string>("CEL_FIELD"));
    std::vector<std::string> tags = {"PII"};
    rule.setTags(std::make_optional<std::vector<std::string>>(tags));
    rule.setExpr(std::make_optional<std::string>("value + '-suffix'"));

    RuleSet rule_set;
    std::vector<Rule> domain_rules = {rule};
    rule_set.setDomainRules(std::make_optional<std::vector<Rule>>(domain_rules));

    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("JSON"));
    schema.setSchema(std::make_optional<std::string>(schema_str));
    schema.setRuleSet(std::make_optional<RuleSet>(rule_set));

    auto registered_schema = client->registerSchema("test-value", schema, false);

    std::string obj_str = R"(
    { "pins": { "pin": "P123456789", "npin": "NP00012345678" } }
    )";
    nlohmann::json obj = nlohmann::json::parse(obj_str);

    auto rule_registry = std::make_shared<RuleRegistry>();
    auto cel_field_executor = std::make_shared<CelFieldExecutor>();
    rule_registry->registerExecutor(cel_field_executor);

    JsonSerializer serializer(client, std::nullopt, rule_registry, ser_conf);

    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Json;

    std::vector<uint8_t> bytes = serializer.serialize(ser_ctx, obj);

    auto deser_conf = DeserializerConfig::createDefault();
    JsonDeserializer deserializer(client, rule_registry, deser_conf);

    std::string expected_obj_str = R"(
    { "pins": { "pin": "P123456789-suffix", "npin": "NP00012345678-suffix" } }
    )";
    nlohmann::json expected_obj = nlohmann::json::parse(expected_obj_str);

    nlohmann::json obj2 = deserializer.deserialize(ser_ctx, bytes);
    ASSERT_EQ(obj2, expected_obj);
}

TEST(JsonTest, CelFieldTransformSiblingAnyOf) {
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);

    auto ser_conf = SerializerConfig(
        false,
        std::make_optional(SchemaSelector::useLatestVersion()),
        false,
        true,
        {}
    );

    std::string schema_str = R"(
    {
        "type": "object",
        "properties": {
            "pins": {
                "type": "object",
                "anyOf": [
                    { "required": ["pin"] },
                    { "required": ["npin"] }
                ],
                "properties": {
                    "pin": {
                        "confluent:tags": ["PII"],
                        "type": ["string", "null"]
                    },
                    "npin": {
                        "confluent:tags": ["PII"],
                        "type": ["string", "null"]
                    }
                }
            }
        }
    }
    )";

    Rule rule;
    rule.setName(std::make_optional<std::string>("test-cel"));
    rule.setKind(std::make_optional<Kind>(Kind::Transform));
    rule.setMode(std::make_optional<Mode>(Mode::Write));
    rule.setType(std::make_optional<std::string>("CEL_FIELD"));
    std::vector<std::string> tags = {"PII"};
    rule.setTags(std::make_optional<std::vector<std::string>>(tags));
    rule.setExpr(std::make_optional<std::string>("value + '-suffix'"));

    RuleSet rule_set;
    std::vector<Rule> domain_rules = {rule};
    rule_set.setDomainRules(std::make_optional<std::vector<Rule>>(domain_rules));

    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("JSON"));
    schema.setSchema(std::make_optional<std::string>(schema_str));
    schema.setRuleSet(std::make_optional<RuleSet>(rule_set));

    auto registered_schema = client->registerSchema("test-value", schema, false);

    std::string obj_str = R"(
    { "pins": { "pin": "P123456789", "npin": "NP00012345678" } }
    )";
    nlohmann::json obj = nlohmann::json::parse(obj_str);

    auto rule_registry = std::make_shared<RuleRegistry>();
    auto cel_field_executor = std::make_shared<CelFieldExecutor>();
    rule_registry->registerExecutor(cel_field_executor);

    JsonSerializer serializer(client, std::nullopt, rule_registry, ser_conf);

    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Json;

    std::vector<uint8_t> bytes = serializer.serialize(ser_ctx, obj);

    auto deser_conf = DeserializerConfig::createDefault();
    JsonDeserializer deserializer(client, rule_registry, deser_conf);

    std::string expected_obj_str = R"(
    { "pins": { "pin": "P123456789-suffix", "npin": "NP00012345678-suffix" } }
    )";
    nlohmann::json expected_obj = nlohmann::json::parse(expected_obj_str);

    nlohmann::json obj2 = deserializer.deserialize(ser_ctx, bytes);
    ASSERT_EQ(obj2, expected_obj);
}

TEST(JsonTest, CelWithReferences) {
    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);

    // Create serializer configuration
    auto ser_conf = SerializerConfig(
        false,  // auto_register_schemas
        std::make_optional(SchemaSelector::useLatestVersion()),  // use_schema
        false,  // normalize_schemas
        true,   // validate
        {}
    );

    // Define reference schema string with PII tags
    std::string ref_schema_str = R"(
    {
        "type": "object",
        "properties": {
            "intField": {"type": "integer"},
            "doubleField": {"type": "number"},
            "stringField": {
                "type": "string",
                "confluent:tags": ["PII"]
            },
            "booleanField": {"type": "boolean"},
            "bytesField": {
                "type": "string",
                "contentEncoding": "base64",
                "confluent:tags": ["PII"]
            }
        }
    }
    )";

    // Create reference schema object
    Schema ref_schema;
    ref_schema.setSchemaType(std::make_optional<std::string>("JSON"));
    ref_schema.setSchema(std::make_optional<std::string>(ref_schema_str));

    // Register the reference schema
    auto registered_ref_schema = client->registerSchema("ref", ref_schema, false);

    // Define main schema string that references the "ref" schema
    std::string schema_str = R"(
    {
        "type": "object",
        "properties": {
            "otherField": {"$ref": "ref"}
        }
    }
    )";

    // Create encryption rule for PII fields
    Rule rule;
    rule.setName(std::make_optional<std::string>("test-cel"));
    rule.setKind(std::make_optional<Kind>(Kind::Transform));
    rule.setMode(std::make_optional<Mode>(Mode::Write));
    rule.setType(std::make_optional<std::string>("CEL_FIELD"));
    rule.setExpr(std::make_optional<std::string>("name == 'stringField' ; value + '-suffix'"));

    // Create rule set with domain rules
    RuleSet rule_set;
    std::vector<Rule> domain_rules = {rule};
    rule_set.setDomainRules(std::make_optional<std::vector<Rule>>(domain_rules));

    // Create schema reference
    SchemaReference schema_ref;
    schema_ref.setName(std::make_optional<std::string>("ref"));
    schema_ref.setSubject(std::make_optional<std::string>("ref"));
    schema_ref.setVersion(std::make_optional<int32_t>(1));

    // Create vector of references
    std::vector<SchemaReference> refs = {schema_ref};

    // Create main schema object with references and rule set
    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("JSON"));
    schema.setReferences(std::make_optional<std::vector<SchemaReference>>(refs));
    schema.setRuleSet(std::make_optional<RuleSet>(rule_set));
    schema.setSchema(std::make_optional<std::string>(schema_str));

    // Register the main schema
    auto registered_schema = client->registerSchema("test-value", schema, false);

    // Create test JSON object
    std::string obj_str = R"(
    {
        "otherField": {
            "intField": 123,
            "doubleField": 45.67,
            "stringField": "hi",
            "booleanField": true,
            "bytesField": "Zm9vYmFy"
        }
    }
    )";

    nlohmann::json obj = nlohmann::json::parse(obj_str);

    // Create rule registry and register field encryption executor
    auto rule_registry = std::make_shared<RuleRegistry>();
    auto cel_field_executor = std::make_shared<CelFieldExecutor>();
    rule_registry->registerExecutor(cel_field_executor);

    // Create JsonSerializer with rule registry - no schema needed since we use latest from registry
    JsonSerializer serializer(client, std::nullopt, rule_registry, ser_conf);

    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Json;

    // Serialize the JSON object (PII fields should be encrypted)
    std::vector<uint8_t> bytes = serializer.serialize(ser_ctx, obj);

    // Create JsonDeserializer with rule registry
    auto deser_conf = DeserializerConfig::createDefault();
    JsonDeserializer deserializer(client, rule_registry, deser_conf);

    // Create test JSON object
    std::string expected_obj_str = R"(
    {
        "otherField": {
            "intField": 123,
            "doubleField": 45.67,
            "stringField": "hi-suffix",
            "booleanField": true,
            "bytesField": "Zm9vYmFy"
        }
    }
    )";

    nlohmann::json expected_obj = nlohmann::json::parse(expected_obj_str);

    // Deserialize back to JSON object (PII fields should be decrypted)
    nlohmann::json obj2 = deserializer.deserialize(ser_ctx, bytes);

    // Assert that the original and deserialized objects are equal
    ASSERT_EQ(obj2, expected_obj);
}

TEST(JsonTest, Encryption) {
    // Register LocalKmsDriver
    LocalKmsDriver::registerDriver();

    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);

    // Create rule configuration with secret
    std::unordered_map<std::string, std::string> rule_config;
    rule_config["secret"] = "mysecret";

    // Create serializer configuration to use latest schema from registry with rule config
    auto ser_conf = SerializerConfig(
        false,  // auto_register_schemas
        std::make_optional(SchemaSelector::useLatestVersion()),  // use_schema
        false,  // normalize_schemas
        false,  // validate
        rule_config  // rule_config
    );

    // Define JSON schema string with PII tags
    std::string schema_str = R"(
    {
        "type": "object",
        "properties": {
            "intField": {"type": "integer"},
            "doubleField": {"type": "number"},
            "stringField": {
                "type": "string",
                "confluent:tags": ["PII"]
            },
            "booleanField": {"type": "boolean"},
            "bytesField": {
                "type": "string",
                "contentEncoding": "base64",
                "confluent:tags": ["PII"]
            }
        }
    }
    )";

    // Create encryption rule
    Rule rule;
    rule.setName(std::make_optional<std::string>("test-encrypt"));
    rule.setKind(std::make_optional<Kind>(Kind::Transform));
    rule.setMode(std::make_optional<Mode>(Mode::WriteRead));
    rule.setType(std::make_optional<std::string>("ENCRYPT"));

    // Set tags for PII fields
    std::vector<std::string> tags = {"PII"};
    rule.setTags(std::make_optional<std::vector<std::string>>(tags));

    // Set encryption parameters - use std::map instead of std::unordered_map
    std::map<std::string, std::string> params;
    params["encrypt.kek.name"] = "kek1";
    params["encrypt.kms.type"] = "local-kms";
    params["encrypt.kms.key.id"] = "mykey";
    rule.setParams(std::make_optional<std::map<std::string, std::string>>(params));

    // Set on_failure
    rule.setOnFailure(std::make_optional<std::string>("ERROR,NONE"));

    // Create rule set with domain rules
    RuleSet rule_set;
    std::vector<Rule> domain_rules = {rule};
    rule_set.setDomainRules(std::make_optional<std::vector<Rule>>(domain_rules));

    // Create schema object with rule set
    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("JSON"));
    schema.setSchema(std::make_optional<std::string>(schema_str));
    schema.setRuleSet(std::make_optional<RuleSet>(rule_set));

    // Register the schema
    auto registered_schema = client->registerSchema("test-value", schema, false);

    // Create original test JSON object
    std::string obj_str = R"(
    {
        "intField": 123,
        "doubleField": 45.67,
        "stringField": "hi",
        "booleanField": true,
        "bytesField": "Zm9vYmFy"
    }
    )";

    nlohmann::json obj = nlohmann::json::parse(obj_str);

    // Create rule registry and register field encryption executor
    auto rule_registry = std::make_shared<RuleRegistry>();
    auto dek_client = std::make_shared<MockDekRegistryClient>(client_config);
    auto field_encryption_executor = std::make_shared<FieldEncryptionExecutor>();
    rule_registry->registerExecutor(field_encryption_executor);

    // Create JsonSerializer with rule registry - no schema needed since we use latest from registry
    JsonSerializer serializer(client, std::nullopt, rule_registry, ser_conf);

    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Json;

    // Serialize the JSON object
    std::vector<uint8_t> bytes = serializer.serialize(ser_ctx, obj);

    // Create JsonDeserializer with rule registry
    auto deser_conf = DeserializerConfig::createDefault();
    JsonDeserializer deserializer(client, rule_registry, deser_conf);

    // Deserialize back to JSON object
    nlohmann::json obj2 = deserializer.deserialize(ser_ctx, bytes);

    // Assert that the original and deserialized objects are equal
    ASSERT_EQ(obj2, obj);
}

TEST(JsonTest, PayloadEncryption) {
    // Register LocalKmsDriver
    LocalKmsDriver::registerDriver();
    
    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);
    
    // Create rule configuration with secret
    std::unordered_map<std::string, std::string> rule_config;
    rule_config["secret"] = "mysecret";
    
    // Create serializer configuration to use latest schema from registry with rule config
    auto ser_conf = SerializerConfig(
        false,  // auto_register_schemas
        std::make_optional(SchemaSelector::useLatestVersion()),  // use_schema
        false,  // normalize_schemas
        false,  // validate
        rule_config  // rule_config
    );
    
    // Define JSON schema string with PII tags
    std::string schema_str = R"(
    {
        "type": "object",
        "properties": {
            "intField": {"type": "integer"},
            "doubleField": {"type": "number"},
            "stringField": {
                "type": "string",
                "confluent:tags": ["PII"]
            },
            "booleanField": {"type": "boolean"},
            "bytesField": {
                "type": "string",
                "contentEncoding": "base64",
                "confluent:tags": ["PII"]
            }
        }
    }
    )";
    
    // Create encryption rule
    Rule rule;
    rule.setName(std::make_optional<std::string>("test-encrypt"));
    rule.setKind(std::make_optional<Kind>(Kind::Transform));
    rule.setMode(std::make_optional<Mode>(Mode::WriteRead));
    rule.setType(std::make_optional<std::string>("ENCRYPT_PAYLOAD"));

    // Set tags for PII fields
    std::vector<std::string> tags = {"PII"};
    rule.setTags(std::make_optional<std::vector<std::string>>(tags));
    
    // Set encryption parameters - use std::map instead of std::unordered_map
    std::map<std::string, std::string> params;
    params["encrypt.kek.name"] = "kek1";
    params["encrypt.kms.type"] = "local-kms";
    params["encrypt.kms.key.id"] = "mykey";
    rule.setParams(std::make_optional<std::map<std::string, std::string>>(params));
    
    // Set on_failure
    rule.setOnFailure(std::make_optional<std::string>("ERROR,NONE"));
    
    // Create rule set with domain rules
    RuleSet rule_set;
    std::vector<Rule> encoding_rules = {rule};
    rule_set.setEncodingRules(std::make_optional<std::vector<Rule>>(encoding_rules));
    
    // Create schema object with rule set
    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("JSON"));
    schema.setSchema(std::make_optional<std::string>(schema_str));
    schema.setRuleSet(std::make_optional<RuleSet>(rule_set));
    
    // Register the schema
    auto registered_schema = client->registerSchema("test-value", schema, false);
    
    // Create original test JSON object
    std::string obj_str = R"(
    {
        "intField": 123,
        "doubleField": 45.67,
        "stringField": "hi",
        "booleanField": true,
        "bytesField": "Zm9vYmFy"
    }
    )";
    
    nlohmann::json obj = nlohmann::json::parse(obj_str);
    
    // Create rule registry and register field encryption executor
    auto rule_registry = std::make_shared<RuleRegistry>();
    auto dek_client = std::make_shared<MockDekRegistryClient>(client_config);
    auto encryption_executor = std::make_shared<EncryptionExecutor>();
    rule_registry->registerExecutor(encryption_executor);
    
    // Create JsonSerializer with rule registry - no schema needed since we use latest from registry
    JsonSerializer serializer(client, std::nullopt, rule_registry, ser_conf);
    
    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Json;
    
    // Serialize the JSON object
    std::vector<uint8_t> bytes = serializer.serialize(ser_ctx, obj);
    
    // Create JsonDeserializer with rule registry
    auto deser_conf = DeserializerConfig::createDefault();
    JsonDeserializer deserializer(client, rule_registry, deser_conf);
    
    // Deserialize back to JSON object
    nlohmann::json obj2 = deserializer.deserialize(ser_ctx, bytes);
    
    // Assert that the original and deserialized objects are equal
    ASSERT_EQ(obj2, obj);
}

TEST(JsonTest, PayloadEncryptionUsesContextFromSubject) {
    // Register LocalKmsDriver
    LocalKmsDriver::registerDriver();

    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);

    // Create rule configuration with secret
    std::unordered_map<std::string, std::string> rule_config;
    rule_config["secret"] = "mysecret";

    auto ser_conf = SerializerConfig(
        false,  // auto_register_schemas
        std::make_optional(SchemaSelector::useLatestVersion()),  // use_schema
        false,  // normalize_schemas
        false,  // validate
        rule_config  // rule_config
    );

    std::string schema_str = R"(
    {
        "type": "object",
        "properties": {
            "stringField": {
                "type": "string",
                "confluent:tags": ["PII"]
            }
        }
    }
    )";

    Rule rule;
    rule.setName(std::make_optional<std::string>("test-encrypt"));
    rule.setKind(std::make_optional<Kind>(Kind::Transform));
    rule.setMode(std::make_optional<Mode>(Mode::WriteRead));
    rule.setType(std::make_optional<std::string>("ENCRYPT_PAYLOAD"));
    std::vector<std::string> tags = {"PII"};
    rule.setTags(std::make_optional<std::vector<std::string>>(tags));
    // Deliberately omit encrypt.kms.type/encrypt.kms.key.id: both keks below
    // are pre-registered, so getOrCreateKek always takes the "found" path,
    // and omitting these params skips the kms type/key id validation that
    // would otherwise reject whichever kek doesn't match a hardcoded value.
    std::map<std::string, std::string> params;
    params["encrypt.kek.name"] = "kek1";
    rule.setParams(std::make_optional<std::map<std::string, std::string>>(params));
    rule.setOnFailure(std::make_optional<std::string>("ERROR,NONE"));

    RuleSet rule_set;
    std::vector<Rule> encoding_rules = {rule};
    rule_set.setEncodingRules(std::make_optional<std::vector<Rule>>(encoding_rules));

    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("JSON"));
    schema.setSchema(std::make_optional<std::string>(schema_str));
    schema.setRuleSet(std::make_optional<RuleSet>(rule_set));

    std::string obj_str = R"({"stringField": "hi"})";
    nlohmann::json obj = nlohmann::json::parse(obj_str);

    auto rule_registry = std::make_shared<RuleRegistry>();
    auto encryption_executor = std::make_shared<EncryptionExecutor>();
    rule_registry->registerExecutor(encryption_executor);

    JsonSerializer serializer(client, std::nullopt, rule_registry, ser_conf);
    auto deser_conf = DeserializerConfig::createDefault();
    JsonDeserializer deserializer(client, rule_registry, deser_conf);

    // Constructing the serializer/deserializer configures the executor with
    // the mock:// client; retrieve that same client to pre-register keks.
    auto *dek_client = encryption_executor->getClient();
    ASSERT_NE(dek_client, nullptr);

    // Pre-register the same kek name under two different contexts, with a
    // different kmsKeyId each, so a wrong (or dropped) context shows up as a
    // mismatched kmsKeyId rather than just "it didn't throw".
    CreateKekRequest ctx_kek_req("kek1", "local-kms", "myctxkey", std::nullopt,
                                std::nullopt, false);
    dek_client->registerKek(ctx_kek_req, std::make_optional<std::string>(".myctx"));
    CreateKekRequest default_kek_req("kek1", "local-kms", "defaultkey",
                                     std::nullopt, std::nullopt, false);
    dek_client->registerKek(default_kek_req, std::nullopt);

    auto roundTrip = [&](const std::string &topic,
                        const std::optional<std::string> &context,
                        const std::string &expected_kms_key_id) {
        std::string subject = topic + "-value";
        client->registerSchema(subject, schema, false);

        SerializationContext ser_ctx;
        ser_ctx.topic = topic;
        ser_ctx.serde_type = SerdeType::Value;
        ser_ctx.serde_format = SerdeFormat::Json;

        std::vector<uint8_t> bytes = serializer.serialize(ser_ctx, obj);
        nlohmann::json obj2 = deserializer.deserialize(ser_ctx, bytes);
        ASSERT_EQ(obj2, obj);

        // Confirm the kek actually used for this context matches expectation
        // (not just that the round trip happened to succeed).
        auto kek = dek_client->getKek("kek1", false, context);
        ASSERT_EQ(kek.getKmsKeyId(), expected_kms_key_id);
    };

    // Context-qualified subject: the context should be parsed out of the
    // subject and threaded through to the dek registry client, not dropped,
    // so the "myctxkey" kek is the one actually used.
    roundTrip(":.myctx:test", std::make_optional<std::string>(".myctx"), "myctxkey");

    // Unqualified subject (default context): should use the "defaultkey" kek.
    roundTrip("test2", std::nullopt, "defaultkey");

    // Explicitly-qualified default context (":.:subject"): should behave
    // identically to an unqualified subject, using the "defaultkey" kek
    // rather than creating a new one under the literal "." context.
    roundTrip(":.:test3", std::nullopt, "defaultkey");

    // No kek should ever have been created under the literal "." context --
    // that would indicate the "." normalization was skipped.
    ASSERT_THROW(dek_client->getKek("kek1", false, std::make_optional<std::string>(".")),
                schemaregistry::rest::RestException);
}

TEST(JsonTest, EncryptionWithReferences) {
    // Register LocalKmsDriver
    LocalKmsDriver::registerDriver();

    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);

    // Create rule configuration with secret
    std::unordered_map<std::string, std::string> rule_config;
    rule_config["secret"] = "mysecret";

    // Create serializer configuration
    auto ser_conf = SerializerConfig(
        false,  // auto_register_schemas
        std::make_optional(SchemaSelector::useLatestVersion()),  // use_schema
        false,  // normalize_schemas
        true,   // validate
        rule_config  // rule_config
    );

    // Define reference schema string with PII tags
    std::string ref_schema_str = R"(
    {
        "type": "object",
        "properties": {
            "intField": {"type": "integer"},
            "doubleField": {"type": "number"},
            "stringField": {
                "type": "string",
                "confluent:tags": ["PII"]
            },
            "booleanField": {"type": "boolean"},
            "bytesField": {
                "type": "string",
                "contentEncoding": "base64",
                "confluent:tags": ["PII"]
            }
        }
    }
    )";

    // Create reference schema object
    Schema ref_schema;
    ref_schema.setSchemaType(std::make_optional<std::string>("JSON"));
    ref_schema.setSchema(std::make_optional<std::string>(ref_schema_str));

    // Register the reference schema
    auto registered_ref_schema = client->registerSchema("ref", ref_schema, false);

    // Define main schema string that references the "ref" schema
    std::string schema_str = R"(
    {
        "type": "object",
        "properties": {
            "otherField": {"$ref": "ref"}
        }
    }
    )";

    // Create encryption rule for PII fields
    Rule rule;
    rule.setName(std::make_optional<std::string>("test-encrypt"));
    rule.setKind(std::make_optional<Kind>(Kind::Transform));
    rule.setMode(std::make_optional<Mode>(Mode::WriteRead));
    rule.setType(std::make_optional<std::string>("ENCRYPT"));

    // Set rule tags to target PII fields
    std::vector<std::string> tags = {"PII"};
    rule.setTags(std::make_optional<std::vector<std::string>>(tags));

    // Set rule parameters for local KMS - use std::map instead of std::unordered_map
    std::map<std::string, std::string> params = {
        {"encrypt.kek.name", "kek1"},
        {"encrypt.kms.type", "local-kms"},
        {"encrypt.kms.key.id", "mykey"}
    };
    rule.setParams(std::make_optional<std::map<std::string, std::string>>(params));
    rule.setOnFailure(std::make_optional<std::string>("ERROR,NONE"));

    // Create rule set with domain rules
    RuleSet rule_set;
    std::vector<Rule> domain_rules = {rule};
    rule_set.setDomainRules(std::make_optional<std::vector<Rule>>(domain_rules));

    // Create schema reference
    SchemaReference schema_ref;
    schema_ref.setName(std::make_optional<std::string>("ref"));
    schema_ref.setSubject(std::make_optional<std::string>("ref"));
    schema_ref.setVersion(std::make_optional<int32_t>(1));

    // Create vector of references
    std::vector<SchemaReference> refs = {schema_ref};

    // Create main schema object with references and rule set
    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("JSON"));
    schema.setReferences(std::make_optional<std::vector<SchemaReference>>(refs));
    schema.setRuleSet(std::make_optional<RuleSet>(rule_set));
    schema.setSchema(std::make_optional<std::string>(schema_str));

    // Register the main schema
    auto registered_schema = client->registerSchema("test-value", schema, false);

    // Create test JSON object
    std::string obj_str = R"(
    {
        "otherField": {
            "intField": 123,
            "doubleField": 45.67,
            "stringField": "hi",
            "booleanField": true,
            "bytesField": "Zm9vYmFy"
        }
    }
    )";

    nlohmann::json obj = nlohmann::json::parse(obj_str);

    // Create rule registry and register field encryption executor
    auto rule_registry = std::make_shared<RuleRegistry>();
    auto dek_client = std::make_shared<MockDekRegistryClient>(client_config);
    auto fake_clock = std::make_shared<FakeClock>(0);  // Use FakeClock with time 0 for consistent testing
    auto field_encryption_executor = std::make_shared<FieldEncryptionExecutor>(fake_clock);
    rule_registry->registerExecutor(field_encryption_executor);

    // Create JsonSerializer with rule registry - no schema needed since we use latest from registry
    JsonSerializer serializer(client, std::nullopt, rule_registry, ser_conf);

    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Json;

    // Serialize the JSON object (PII fields should be encrypted)
    std::vector<uint8_t> bytes = serializer.serialize(ser_ctx, obj);

    // Create JsonDeserializer with rule registry
    auto deser_conf = DeserializerConfig::createDefault();
    JsonDeserializer deserializer(client, rule_registry, deser_conf);

    // Deserialize back to JSON object (PII fields should be decrypted)
    nlohmann::json obj2 = deserializer.deserialize(ser_ctx, bytes);

    // Assert that the original and deserialized objects are equal
    ASSERT_EQ(obj2, obj);
}

#endif

// AssociatedNameStrategy Tests
// Ported from TestJsonSerdeWithAssociated* in confluent-kafka-go

namespace {
const std::string kJsonDemoSchema = R"({
    "title": "DemoSchema",
    "type": "object",
    "properties": {
        "intField":    {"type": "integer"},
        "doubleField": {"type": "number"},
        "stringField": {"type": "string"},
        "boolField":   {"type": "boolean"}
    }
})";

nlohmann::json makeJsonDemoDatum() {
    return {{"intField", 123}, {"doubleField", 45.67},
            {"stringField", "hi"}, {"boolField", true}};
}

void verifyJsonDemoDatum(const nlohmann::json &datum) {
    EXPECT_EQ(datum["intField"].get<int>(), 123);
    EXPECT_DOUBLE_EQ(datum["doubleField"].get<double>(), 45.67);
    EXPECT_EQ(datum["stringField"].get<std::string>(), "hi");
    EXPECT_EQ(datum["boolField"].get<bool>(), true);
}
}  // namespace

TEST(JsonTest, JsonSerdeWithAssociatedNameStrategy) {
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);

    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("JSON"));
    schema.setSchema(std::make_optional<std::string>(kJsonDemoSchema));
    client->registerSchema("my-custom-subject", schema, false);

    AssociationCreateOrUpdateRequest req;
    req.setResourceName("topic1");
    req.setResourceNamespace("-");
    req.setResourceId("lkc-123:topic1");
    req.setResourceType("topic");
    AssociationCreateOrUpdateInfo assoc;
    assoc.setSubject("my-custom-subject");
    assoc.setAssociationType("value");
    req.setAssociations(std::vector<AssociationCreateOrUpdateInfo>{assoc});
    client->createAssociation(req);

    auto ser_config = SerializerConfig::createDefault();
    ser_config.auto_register_schemas = false;
    ser_config.use_schema = SchemaSelector::useLatestVersion();
    ser_config.subject_name_strategy_type = SubjectNameStrategyType::Associated;

    auto rule_registry = std::make_shared<RuleRegistry>();
    JsonSerializer serializer(client, std::nullopt, rule_registry, ser_config);
    SerializationContext ser_ctx;
    ser_ctx.topic = "topic1";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Json;
    auto bytes = serializer.serialize(ser_ctx, makeJsonDemoDatum());

    auto deser_config = DeserializerConfig::createDefault();
    deser_config.subject_name_strategy_type = SubjectNameStrategyType::Associated;
    JsonDeserializer deserializer(client, rule_registry, deser_config);
    verifyJsonDemoDatum(deserializer.deserialize(ser_ctx, bytes));
}

TEST(JsonTest, JsonSerdeWithAssociatedNameStrategyFallbackToTopic) {
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);

    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("JSON"));
    schema.setSchema(std::make_optional<std::string>(kJsonDemoSchema));
    client->registerSchema("topic1-value", schema, false);

    auto ser_config = SerializerConfig::createDefault();
    ser_config.auto_register_schemas = false;
    ser_config.use_schema = SchemaSelector::useLatestVersion();
    ser_config.subject_name_strategy_type = SubjectNameStrategyType::Associated;

    auto rule_registry = std::make_shared<RuleRegistry>();
    JsonSerializer serializer(client, std::nullopt, rule_registry, ser_config);
    SerializationContext ser_ctx;
    ser_ctx.topic = "topic1";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Json;
    auto bytes = serializer.serialize(ser_ctx, makeJsonDemoDatum());

    auto deser_config = DeserializerConfig::createDefault();
    JsonDeserializer deserializer(client, rule_registry, deser_config);
    verifyJsonDemoDatum(deserializer.deserialize(ser_ctx, bytes));
}

TEST(JsonTest, JsonSerdeWithAssociatedNameStrategyFallbackNone) {
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);

    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("JSON"));
    schema.setSchema(std::make_optional<std::string>(kJsonDemoSchema));
    client->registerSchema("topic1-value", schema, false);

    auto ser_config = SerializerConfig::createDefault();
    ser_config.auto_register_schemas = false;
    ser_config.use_schema = SchemaSelector::useLatestVersion();
    ser_config.subject_name_strategy_type = SubjectNameStrategyType::Associated;
    ser_config.subject_name_strategy_config = {
        {FALLBACK_TYPE_CONFIG, "NONE"}};

    auto rule_registry = std::make_shared<RuleRegistry>();
    JsonSerializer serializer(client, std::nullopt, rule_registry, ser_config);
    SerializationContext ser_ctx;
    ser_ctx.topic = "topic1";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Json;
    EXPECT_THROW(serializer.serialize(ser_ctx, makeJsonDemoDatum()),
                 SerializationError);
}

TEST(JsonTest, JsonSerdeWithAssociatedNameStrategyMultipleAssociations) {
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);

    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("JSON"));
    schema.setSchema(std::make_optional<std::string>(kJsonDemoSchema));
    client->registerSchema("subject1", schema, false);
    client->registerSchema("subject2", schema, false);

    auto makeReq = [](const std::string &resource_id,
                      const std::string &subject_name) {
        AssociationCreateOrUpdateRequest r;
        r.setResourceName("topic1");
        r.setResourceNamespace("-");
        r.setResourceId(resource_id);
        r.setResourceType("topic");
        AssociationCreateOrUpdateInfo a;
        a.setSubject(subject_name);
        a.setAssociationType("value");
        r.setAssociations(std::vector<AssociationCreateOrUpdateInfo>{a});
        return r;
    };
    client->createAssociation(makeReq("lkc-123:topic1", "subject1"));
    client->createAssociation(makeReq("lkc-456:topic1", "subject2"));

    auto ser_config = SerializerConfig::createDefault();
    ser_config.auto_register_schemas = false;
    ser_config.use_schema = SchemaSelector::useLatestVersion();
    ser_config.subject_name_strategy_type = SubjectNameStrategyType::Associated;

    auto rule_registry = std::make_shared<RuleRegistry>();
    JsonSerializer serializer(client, std::nullopt, rule_registry, ser_config);
    SerializationContext ser_ctx;
    ser_ctx.topic = "topic1";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Json;
    EXPECT_THROW(serializer.serialize(ser_ctx, makeJsonDemoDatum()),
                 SerializationError);
}

TEST(JsonTest, JsonSerdeWithAssociatedNameStrategyWithKafkaClusterID) {
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);

    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("JSON"));
    schema.setSchema(std::make_optional<std::string>(kJsonDemoSchema));
    client->registerSchema("my-custom-subject", schema, false);

    AssociationCreateOrUpdateRequest req;
    req.setResourceName("topic1");
    req.setResourceNamespace("lkc-my-cluster");
    req.setResourceId("lkc-my-cluster:topic1");
    req.setResourceType("topic");
    AssociationCreateOrUpdateInfo assoc;
    assoc.setSubject("my-custom-subject");
    assoc.setAssociationType("value");
    req.setAssociations(std::vector<AssociationCreateOrUpdateInfo>{assoc});
    client->createAssociation(req);

    auto ser_config = SerializerConfig::createDefault();
    ser_config.auto_register_schemas = false;
    ser_config.use_schema = SchemaSelector::useLatestVersion();
    ser_config.subject_name_strategy_type = SubjectNameStrategyType::Associated;
    ser_config.subject_name_strategy_config = {
        {KAFKA_CLUSTER_ID_CONFIG, "lkc-my-cluster"}};

    auto rule_registry = std::make_shared<RuleRegistry>();
    JsonSerializer serializer(client, std::nullopt, rule_registry, ser_config);
    SerializationContext ser_ctx;
    ser_ctx.topic = "topic1";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Json;
    auto bytes = serializer.serialize(ser_ctx, makeJsonDemoDatum());

    auto deser_config = DeserializerConfig::createDefault();
    deser_config.subject_name_strategy_type = SubjectNameStrategyType::Associated;
    deser_config.subject_name_strategy_config = {
        {KAFKA_CLUSTER_ID_CONFIG, "lkc-my-cluster"}};
    JsonDeserializer deserializer(client, rule_registry, deser_config);
    verifyJsonDemoDatum(deserializer.deserialize(ser_ctx, bytes));
}

TEST(JsonTest, JsonSerdeWithAssociatedNameStrategyCaching) {
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);

    Schema schema;
    schema.setSchemaType(std::make_optional<std::string>("JSON"));
    schema.setSchema(std::make_optional<std::string>(kJsonDemoSchema));
    client->registerSchema("my-cached-subject", schema, false);

    AssociationCreateOrUpdateRequest req;
    req.setResourceName("topic1");
    req.setResourceNamespace("-");
    req.setResourceId("lkc-123:topic1");
    req.setResourceType("topic");
    AssociationCreateOrUpdateInfo assoc;
    assoc.setSubject("my-cached-subject");
    assoc.setAssociationType("value");
    req.setAssociations(std::vector<AssociationCreateOrUpdateInfo>{assoc});
    client->createAssociation(req);

    auto ser_config = SerializerConfig::createDefault();
    ser_config.auto_register_schemas = false;
    ser_config.use_schema = SchemaSelector::useLatestVersion();
    ser_config.subject_name_strategy_type = SubjectNameStrategyType::Associated;

    auto deser_config = DeserializerConfig::createDefault();
    deser_config.subject_name_strategy_type = SubjectNameStrategyType::Associated;

    auto rule_registry = std::make_shared<RuleRegistry>();
    JsonSerializer serializer(client, std::nullopt, rule_registry, ser_config);
    JsonDeserializer deserializer(client, rule_registry, deser_config);
    SerializationContext ser_ctx;
    ser_ctx.topic = "topic1";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Json;

    for (int i = 0; i < 5; ++i) {
        auto bytes = serializer.serialize(ser_ctx, makeJsonDemoDatum());
        verifyJsonDemoDatum(deserializer.deserialize(ser_ctx, bytes));
    }
}


/**
 * The `message` binding inside a `CEL_FIELD` rule is the *containing object*, not the field
 * under the rule - which is already bound as `value`. This walk passed the field's own value,
 * so `message` was a second name for `value` and no sibling was reachable from a rule.
 *
 * The reference binds the containing message here too, converting a `JsonNode` to a map first.
 */
TEST(JsonTest, CelFieldMessageBindingIsTheContainingObject) {
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = SchemaRegistryClient::newClient(client_config);
    auto ser_conf = SerializerConfig(
        false, std::make_optional(SchemaSelector::useLatestVersion()), false, true, {});
    std::string schema_str = R"(
    {
        "type": "object",
        "properties": {
            "intField": {"type": "integer"},
            "stringField": {"type": "string", "confluent:tags": ["PII"]}
        }
    })";

    // Writes what the rule produced into stringField, so the expression's result is readable.
    auto run = [&](const std::string &subject, const std::string &expr) {
        Rule rule;
        rule.setName(std::make_optional<std::string>("r"));
        rule.setKind(std::make_optional<Kind>(Kind::Transform));
        rule.setMode(std::make_optional<Mode>(Mode::Write));
        rule.setType(std::make_optional<std::string>("CEL_FIELD"));
        rule.setExpr(std::make_optional<std::string>(expr));
        RuleSet rule_set;
        std::vector<Rule> domain_rules = {rule};
        rule_set.setDomainRules(std::make_optional<std::vector<Rule>>(domain_rules));
        Schema schema;
        schema.setSchemaType(std::make_optional<std::string>("JSON"));
        schema.setSchema(std::make_optional<std::string>(schema_str));
        schema.setRuleSet(std::make_optional<RuleSet>(rule_set));
        client->registerSchema(subject + "-value", schema, false);

        nlohmann::json obj = nlohmann::json::parse(R"({"intField":123,"stringField":"hi"})");
        auto reg = std::make_shared<RuleRegistry>();
        reg->registerExecutor(std::make_shared<CelFieldExecutor>());
        JsonSerializer serializer(client, std::nullopt, reg, ser_conf);
        SerializationContext sc;
        sc.topic = subject;
        sc.serde_type = SerdeType::Value;
        sc.serde_format = SerdeFormat::Json;
        auto bytes = serializer.serialize(sc, obj);
        auto deser_conf = DeserializerConfig::createDefault();
        JsonDeserializer deserializer(client, reg, deser_conf);
        return deserializer.deserialize(sc, bytes)["stringField"].get<std::string>();
    };

    EXPECT_EQ(run("jsonmsg1", "name == 'stringField' ; string(message.intField)"), "123")
        << "a sibling field must be reachable from a field rule";
    // The must-pass twin: `value` is still the field the rule is applied to.
    EXPECT_EQ(run("jsonmsg2", "name == 'stringField' ; value + '!'"), "hi!");
}
