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
#include "schemaregistry/serdes/SerdeConfig.h"
#include "schemaregistry/serdes/SerdeTypes.h"
#include "schemaregistry/serdes/RuleRegistry.h"
#include "schemaregistry/serdes/protobuf/ProtobufSerializer.h"
#include "schemaregistry/serdes/protobuf/ProtobufDeserializer.h"
#include "schemaregistry/serdes/protobuf/ProtobufUtils.h"
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

#include "test/dep.pb.h"
#include "test/example.pb.h"
#include "test/test.pb.h"

using namespace schemaregistry::serdes;
using namespace schemaregistry::serdes::protobuf;
using namespace schemaregistry::rest;
using namespace schemaregistry::rest::model;

#ifdef SCHEMAREGISTRY_USE_RULES
using namespace schemaregistry::rules::cel;
using namespace schemaregistry::rules::encryption;
using namespace schemaregistry::rules::encryption::localkms;
#endif

TEST(ProtobufTest, BasicSerialization) {
    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = std::make_shared<MockSchemaRegistryClient>(client_config);
    
    // Create serializer configuration
    auto ser_conf = SerializerConfig::createDefault();
    
    // Create Author object with test data
    test::Author obj;
    obj.set_name("Kafka");
    obj.set_id(123);
    obj.set_picture(std::string({1, 2, 3})); // bytes field
    obj.add_works("Metamorphosis");
    obj.add_works("The Trial");
    obj.set_oneof_string("oneof");
    
    // Create rule registry
    auto rule_registry = std::make_shared<RuleRegistry>();
    
    // Create protobuf serializer with default reference subject name strategy
    ProtobufSerializer<test::Author> ser(
        client,
        std::nullopt, // schema
        rule_registry,
        ser_conf,
        defaultReferenceSubjectNameStrategy
    );
    
    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Protobuf;
    ser_ctx.headers = std::nullopt;

    // Serialize the Author object
    auto bytes = ser.serialize(ser_ctx, obj);
    
    // Create protobuf deserializer
    ProtobufDeserializer<test::Author> deser(
        client,
        rule_registry,
        DeserializerConfig::createDefault()
    );
    
    // Deserialize the bytes back to Author object
    auto obj2_ptr = deser.deserialize(ser_ctx, bytes);
    
    // Cast to the specific message type
    auto obj2 = dynamic_cast<const test::Author*>(obj2_ptr.get());
    ASSERT_NE(obj2, nullptr);
    
    // Assert objects are equal
    EXPECT_EQ(obj2->name(), obj.name());
    EXPECT_EQ(obj2->id(), obj.id());
    EXPECT_EQ(obj2->picture(), obj.picture());
    EXPECT_EQ(obj2->works_size(), obj.works_size());
    for (int i = 0; i < obj.works_size(); ++i) {
        EXPECT_EQ(obj2->works(i), obj.works(i));
    }
    EXPECT_EQ(obj2->oneof_string(), obj.oneof_string());
}

TEST(ProtobufTest, GuidInHeader) {
    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = std::make_shared<MockSchemaRegistryClient>(client_config);
    
    // Create serializer configuration with header schema ID serializer
    auto ser_conf = SerializerConfig::createDefault();
    ser_conf.schema_id_serializer = headerSchemaIdSerializer;
    
    // Create Author object with test data
    test::Author obj;
    obj.set_name("Kafka");
    obj.set_id(123);
    obj.set_picture(std::string({1, 2, 3})); // bytes field
    obj.add_works("Metamorphosis");
    obj.add_works("The Trial");
    obj.set_oneof_string("oneof");
    
    // Create rule registry
    auto rule_registry = std::make_shared<RuleRegistry>();
    
    // Create protobuf serializer with default reference subject name strategy
    ProtobufSerializer<test::Author> ser(
        client,
        std::nullopt, // schema
        rule_registry,
        ser_conf,
        defaultReferenceSubjectNameStrategy
    );
    
    // Create serialization context with headers (schema ID should be stored in header)
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Protobuf;
    ser_ctx.headers = std::make_optional<SerdeHeaders>(SerdeHeaders());

    // Serialize the Author object
    auto bytes = ser.serialize(ser_ctx, obj);
    
    // Create protobuf deserializer
    ProtobufDeserializer<test::Author> deser(
        client,
        rule_registry,
        DeserializerConfig::createDefault()
    );
    
    // Deserialize the bytes back to Author object
    auto obj2_ptr = deser.deserialize(ser_ctx, bytes);
    
    // Cast to the specific message type
    auto obj2 = dynamic_cast<const test::Author*>(obj2_ptr.get());
    ASSERT_NE(obj2, nullptr);
    
    // Assert objects are equal
    EXPECT_EQ(obj2->name(), obj.name());
    EXPECT_EQ(obj2->id(), obj.id());
    EXPECT_EQ(obj2->picture(), obj.picture());
    EXPECT_EQ(obj2->works_size(), obj.works_size());
    for (int i = 0; i < obj.works_size(); ++i) {
        EXPECT_EQ(obj2->works(i), obj.works(i));
    }
    EXPECT_EQ(obj2->oneof_string(), obj.oneof_string());
}


TEST(ProtobufTest, SerializeReference) {
    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = std::make_shared<MockSchemaRegistryClient>(client_config);
    
    // Create serializer configuration
    auto ser_conf = SerializerConfig::createDefault();
    
    // Create TestMessage object with test data
    test::TestMessage msg;
    msg.set_test_string("hi");
    msg.set_test_bool(true);
    msg.set_test_bytes(std::string({1, 2, 3})); // bytes field
    msg.set_test_double(1.23);
    msg.set_test_float(3.45f);
    msg.set_test_fixed32(67);
    msg.set_test_fixed64(89);
    msg.set_test_int32(100);
    msg.set_test_int64(200);
    msg.set_test_sfixed32(300);
    msg.set_test_sfixed64(400);
    msg.set_test_sint32(500);
    msg.set_test_sint64(600);
    msg.set_test_uint32(700);
    msg.set_test_uint64(800);
    
    // Create DependencyMessage object with TestMessage
    test::DependencyMessage obj;
    obj.set_is_active(true);
    obj.mutable_test_message()->CopyFrom(msg);
    
    // Create rule registry
    auto rule_registry = std::make_shared<RuleRegistry>();
    
    // Create protobuf serializer with reference subject name strategy
    ProtobufSerializer<test::DependencyMessage> ser(
        client,
        std::nullopt, // schema
        rule_registry,
        ser_conf,
        defaultReferenceSubjectNameStrategy
    );
    
    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Protobuf;
    ser_ctx.headers = std::nullopt;

    // Serialize the DependencyMessage object
    auto bytes = ser.serialize(ser_ctx, obj);
    
    // Create protobuf deserializer
    ProtobufDeserializer<test::DependencyMessage> deser(
        client,
        rule_registry,
        DeserializerConfig::createDefault()
    );
    
    // Deserialize the bytes back to DependencyMessage object
    auto obj2_ptr = deser.deserialize(ser_ctx, bytes);
    
    // Cast to the specific message type
    auto obj2 = dynamic_cast<const test::DependencyMessage*>(obj2_ptr.get());
    ASSERT_NE(obj2, nullptr);
    
    // Assert objects are equal
    EXPECT_EQ(obj2->is_active(), obj.is_active());
    ASSERT_TRUE(obj2->has_test_message());
    ASSERT_TRUE(obj.has_test_message());
    
    const auto& test_msg2 = obj2->test_message();
    const auto& test_msg1 = obj.test_message();
    
    EXPECT_EQ(test_msg2.test_string(), test_msg1.test_string());
    EXPECT_EQ(test_msg2.test_bool(), test_msg1.test_bool());
    EXPECT_EQ(test_msg2.test_bytes(), test_msg1.test_bytes());
    EXPECT_DOUBLE_EQ(test_msg2.test_double(), test_msg1.test_double());
    EXPECT_FLOAT_EQ(test_msg2.test_float(), test_msg1.test_float());
    EXPECT_EQ(test_msg2.test_fixed32(), test_msg1.test_fixed32());
    EXPECT_EQ(test_msg2.test_fixed64(), test_msg1.test_fixed64());
    EXPECT_EQ(test_msg2.test_int32(), test_msg1.test_int32());
    EXPECT_EQ(test_msg2.test_int64(), test_msg1.test_int64());
    EXPECT_EQ(test_msg2.test_sfixed32(), test_msg1.test_sfixed32());
    EXPECT_EQ(test_msg2.test_sfixed64(), test_msg1.test_sfixed64());
    EXPECT_EQ(test_msg2.test_sint32(), test_msg1.test_sint32());
    EXPECT_EQ(test_msg2.test_sint64(), test_msg1.test_sint64());
    EXPECT_EQ(test_msg2.test_uint32(), test_msg1.test_uint32());
    EXPECT_EQ(test_msg2.test_uint64(), test_msg1.test_uint64());
}

#ifdef SCHEMAREGISTRY_USE_RULES

TEST(ProtobufTest, CelFieldTransformation) {
    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = std::make_shared<MockSchemaRegistryClient>(client_config);
    
    // Create serializer configuration
    std::unordered_map<std::string, std::string> rule_config;
    auto ser_conf = SerializerConfig(
        false,  // auto_register_schemas
        std::make_optional(SchemaSelector::useLatestVersion()),  // use_schema
        false,  // normalize_schemas
        false,  // validate
        rule_config  // rule_config
    );
    
    // Create Author object with test data
    test::Author obj;
    obj.set_name("Kafka");
    obj.set_id(123);
    obj.set_picture(std::string({1, 2, 3})); // bytes field
    obj.add_works("Metamorphosis");
    obj.add_works("The Trial");
    obj.set_oneof_string("oneof");
    
    // Create CEL field transformation rule
    Rule rule;
    rule.setName("test-cel");
    rule.setKind(Kind::Transform);
    rule.setMode(Mode::Write);
    rule.setType("CEL_FIELD");
    rule.setExpr("typeName == 'STRING' ; value + '-suffix'");
    
    // Create rule set with the domain rule
    RuleSet rule_set;
    std::vector<Rule> domain_rules = {rule};
    rule_set.setDomainRules(domain_rules);
    
    // Create schema with rule set
    Schema schema;
    schema.setSchemaType("PROTOBUF");
    schema.setRuleSet(rule_set);
    
    // Get the protobuf schema string from the test::Author descriptor
    std::string schema_str = protobuf::utils::schemaToString(obj.GetDescriptor()->file());
    schema.setSchema(schema_str);
    
    // Register the schema
    client->registerSchema("test-value", schema, false);
    
    // Create rule registry and register CEL field executor
    auto rule_registry = std::make_shared<RuleRegistry>();
    rule_registry->registerExecutor(std::make_shared<CelFieldExecutor>());
    
    // Create protobuf serializer with rule registry
    ProtobufSerializer<test::Author> ser(
        client,
        std::nullopt, // schema
        rule_registry,
        ser_conf
    );
    
    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Protobuf;
    ser_ctx.headers = std::nullopt;

    // Serialize the Author object (CEL transformation will be applied)
    auto bytes = ser.serialize(ser_ctx, obj);
    
    // Create protobuf deserializer with rule registry
    ProtobufDeserializer<test::Author> deser(
        client,
        rule_registry,
        DeserializerConfig::createDefault()
    );
    
    // Deserialize the bytes back to Author object
    auto obj2_ptr = deser.deserialize(ser_ctx, bytes);
    auto obj2 = dynamic_cast<const test::Author*>(obj2_ptr.get());
    ASSERT_NE(obj2, nullptr);
    
    // Create expected object with transformed string fields (should have "-suffix" appended)
    test::Author expected_obj;
    expected_obj.set_name("Kafka-suffix");
    expected_obj.set_id(123);
    expected_obj.set_picture(std::string({1, 2, 3}));
    expected_obj.add_works("Metamorphosis-suffix");
    expected_obj.add_works("The Trial-suffix");
    expected_obj.set_oneof_string("oneof-suffix");
    
    // Assert the transformed object matches expected values
    EXPECT_EQ(obj2->name(), expected_obj.name());
    EXPECT_EQ(obj2->id(), expected_obj.id());
    EXPECT_EQ(obj2->picture(), expected_obj.picture());
    EXPECT_EQ(obj2->works_size(), expected_obj.works_size());
    for (int i = 0; i < expected_obj.works_size(); ++i) {
        EXPECT_EQ(obj2->works(i), expected_obj.works(i));
    }
    EXPECT_EQ(obj2->oneof_string(), expected_obj.oneof_string());
}

TEST(ProtobufTest, PayloadEncryption) {
    // Register LocalKmsDriver
    LocalKmsDriver::registerDriver();

    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = std::make_shared<MockSchemaRegistryClient>(client_config);
    auto dek_client = std::make_shared<MockDekRegistryClient>(client_config);

    // Create rule configuration with secret
    std::unordered_map<std::string, std::string> rule_config;
    rule_config["secret"] = "mysecret";

    // Create serializer and deserializer configurations
    auto ser_conf = SerializerConfig(
        false,  // auto_register_schemas
        std::make_optional(SchemaSelector::useLatestVersion()),  // use_schema
        true,   // normalize_schemas
        false,  // validate
        rule_config  // rule_config
    );

    auto deser_conf = DeserializerConfig(
        std::nullopt,  // use_schema
        false,  // validate
        rule_config  // rule_config
    );

    // Create Author object with test data
    test::Author obj;
    obj.set_name("Kafka");
    obj.set_id(123);
    obj.set_picture(std::string({1, 2, 3})); // bytes field
    obj.add_works("Metamorphosis");
    obj.add_works("The Trial");
    obj.set_oneof_string("oneof");

    // Create encryption rule
    Rule rule;
    rule.setName("test-encrypt");
    rule.setKind(Kind::Transform);
    rule.setMode(Mode::WriteRead);
    rule.setType("ENCRYPT_PAYLOAD");

    // Set rule tags for PII
    std::vector<std::string> tags = {"PII"};
    rule.setTags(tags);

    // Set rule parameters
    std::map<std::string, std::string> params;
    params["encrypt.kek.name"] = "kek1";
    params["encrypt.kms.type"] = "local-kms";
    params["encrypt.kms.key.id"] = "mykey";
    rule.setParams(params);

    // Set on_failure parameter
    rule.setOnFailure("ERROR,NONE");

    // Create rule set with the domain rule
    RuleSet rule_set;
    std::vector<Rule> encoding_rules = {rule};
    rule_set.setEncodingRules(encoding_rules);

    // Create schema with rule set
    Schema schema;
    schema.setSchemaType("PROTOBUF");
    schema.setRuleSet(rule_set);

    // Get the protobuf schema string from the test::Author descriptor
    std::string schema_str = protobuf::utils::schemaToString(obj.GetDescriptor()->file());
    schema.setSchema(schema_str);

    // Register the schema
    client->registerSchema("test-value", schema, false);

    // Create rule registry and register field encryption executor
    auto rule_registry = std::make_shared<RuleRegistry>();
    rule_registry->registerExecutor(std::make_shared<EncryptionExecutor>());

    // Create protobuf serializer with rule registry
    ProtobufSerializer<test::Author> ser(
        client,
        std::nullopt, // schema
        rule_registry,
        ser_conf
    );

    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Protobuf;
    ser_ctx.headers = std::nullopt;

    // Serialize the Author object (encryption will be applied)
    auto bytes = ser.serialize(ser_ctx, obj);

    // Create protobuf deserializer with rule registry
    ProtobufDeserializer<test::Author> deser(
        client,
        rule_registry,
        deser_conf
    );

    // Deserialize the bytes back to Author object
    auto obj2_ptr = deser.deserialize(ser_ctx, bytes);
    auto obj2 = dynamic_cast<const test::Author*>(obj2_ptr.get());
    ASSERT_NE(obj2, nullptr);

    // Create expected object (should be the same as original after decryption)
    test::Author expected_obj;
    expected_obj.set_name("Kafka");
    expected_obj.set_id(123);
    expected_obj.set_picture(std::string({1, 2, 3}));
    expected_obj.add_works("Metamorphosis");
    expected_obj.add_works("The Trial");
    expected_obj.set_oneof_string("oneof");

    // Assert the decrypted object matches expected values
    EXPECT_EQ(obj2->name(), expected_obj.name());
    EXPECT_EQ(obj2->id(), expected_obj.id());
    EXPECT_EQ(obj2->picture(), expected_obj.picture());
    EXPECT_EQ(obj2->works_size(), expected_obj.works_size());
    for (int i = 0; i < expected_obj.works_size(); ++i) {
        EXPECT_EQ(obj2->works(i), expected_obj.works(i));
    }
    EXPECT_EQ(obj2->oneof_string(), expected_obj.oneof_string());
}
TEST(ProtobufTest, FieldEncryption) {
    // Register LocalKmsDriver
    LocalKmsDriver::registerDriver();
    
    // Create client configuration with mock URL
    std::vector<std::string> urls = {"mock://"};
    auto client_config = std::make_shared<const ClientConfiguration>(urls);
    auto client = std::make_shared<MockSchemaRegistryClient>(client_config);
    auto dek_client = std::make_shared<MockDekRegistryClient>(client_config);
    
    // Create rule configuration with secret
    std::unordered_map<std::string, std::string> rule_config;
    rule_config["secret"] = "mysecret";
    
    // Create serializer and deserializer configurations
    auto ser_conf = SerializerConfig(
        false,  // auto_register_schemas
        std::make_optional(SchemaSelector::useLatestVersion()),  // use_schema
        true,   // normalize_schemas
        false,  // validate
        rule_config  // rule_config
    );
    
    auto deser_conf = DeserializerConfig(
        std::nullopt,  // use_schema
        false,  // validate
        rule_config  // rule_config
    );
    
    // Create Author object with test data
    test::Author obj;
    obj.set_name("Kafka");
    obj.set_id(123);
    obj.set_picture(std::string({1, 2, 3})); // bytes field
    obj.add_works("Metamorphosis");
    obj.add_works("The Trial");
    obj.set_oneof_string("oneof");
    
    // Create encryption rule
    Rule rule;
    rule.setName("test-encrypt");
    rule.setKind(Kind::Transform);
    rule.setMode(Mode::WriteRead);
    rule.setType("ENCRYPT");
    
    // Set rule tags for PII
    std::vector<std::string> tags = {"PII"};
    rule.setTags(tags);
    
    // Set rule parameters
    std::map<std::string, std::string> params;
    params["encrypt.kek.name"] = "kek1";
    params["encrypt.kms.type"] = "local-kms";
    params["encrypt.kms.key.id"] = "mykey";
    rule.setParams(params);
    
    // Set on_failure parameter
    rule.setOnFailure("ERROR,NONE");
    
    // Create rule set with the domain rule
    RuleSet rule_set;
    std::vector<Rule> domain_rules = {rule};
    rule_set.setDomainRules(domain_rules);
    
    // Create schema with rule set
    Schema schema;
    schema.setSchemaType("PROTOBUF");
    schema.setRuleSet(rule_set);
    
    // Get the protobuf schema string from the test::Author descriptor
    std::string schema_str = protobuf::utils::schemaToString(obj.GetDescriptor()->file());
    schema.setSchema(schema_str);
    
    // Register the schema
    client->registerSchema("test-value", schema, false);
    
    // Create rule registry and register field encryption executor
    auto rule_registry = std::make_shared<RuleRegistry>();
    rule_registry->registerExecutor(std::make_shared<FieldEncryptionExecutor>());
    
    // Create protobuf serializer with rule registry
    ProtobufSerializer<test::Author> ser(
        client,
        std::nullopt, // schema
        rule_registry,
        ser_conf
    );
    
    // Create serialization context
    SerializationContext ser_ctx;
    ser_ctx.topic = "test";
    ser_ctx.serde_type = SerdeType::Value;
    ser_ctx.serde_format = SerdeFormat::Protobuf;
    ser_ctx.headers = std::nullopt;

    // Serialize the Author object (encryption will be applied)
    auto bytes = ser.serialize(ser_ctx, obj);
    
    // Create protobuf deserializer with rule registry
    ProtobufDeserializer<test::Author> deser(
        client,
        rule_registry,
        deser_conf
    );
    
    // Deserialize the bytes back to Author object
    auto obj2_ptr = deser.deserialize(ser_ctx, bytes);
    auto obj2 = dynamic_cast<const test::Author*>(obj2_ptr.get());
    ASSERT_NE(obj2, nullptr);
    
    // Create expected object (should be the same as original after decryption)
    test::Author expected_obj;
    expected_obj.set_name("Kafka");
    expected_obj.set_id(123);
    expected_obj.set_picture(std::string({1, 2, 3}));
    expected_obj.add_works("Metamorphosis");
    expected_obj.add_works("The Trial");
    expected_obj.set_oneof_string("oneof");
    
    // Assert the decrypted object matches expected values
    EXPECT_EQ(obj2->name(), expected_obj.name());
    EXPECT_EQ(obj2->id(), expected_obj.id());
    EXPECT_EQ(obj2->picture(), expected_obj.picture());
    EXPECT_EQ(obj2->works_size(), expected_obj.works_size());
    for (int i = 0; i < expected_obj.works_size(); ++i) {
        EXPECT_EQ(obj2->works(i), expected_obj.works(i));
    }
    EXPECT_EQ(obj2->oneof_string(), expected_obj.oneof_string());
}

#endif