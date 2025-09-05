#pragma once

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>

#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "schemaregistry/rest/ISchemaRegistryClient.h"
#include "schemaregistry/rest/model/Schema.h"
#include "schemaregistry/serdes/Serde.h"
#include "schemaregistry/serdes/SerdeError.h"
#include "schemaregistry/serdes/SerdeTypes.h"
#include "schemaregistry/serdes/protobuf/ProtobufTypes.h"

namespace schemaregistry::serdes::protobuf::utils {

// Import types from ProtobufTypes.h for convenience
using schemaregistry::serdes::protobuf::MapKey;
using schemaregistry::serdes::protobuf::ProtobufVariant;

/**
 * Transform protobuf fields using field execution context (synchronous version)
 * Ported from Rust async implementation
 */
std::unique_ptr<SerdeValue> transformFields(
    RuleContext &ctx, const google::protobuf::Descriptor *descriptor,
    const SerdeValue &value);

/**
 * Transform protobuf values recursively (synchronous version)
 * Ported from Rust async implementation
 */
ProtobufVariant transformRecursive(
    RuleContext &ctx, const google::protobuf::Descriptor *descriptor,
    const ProtobufVariant &message);

/**
 * Transform field with rule context (synchronous version)
 * Ported from Rust async implementation
 */
std::optional<ProtobufVariant> transformFieldWithContext(
    RuleContext &ctx, const google::protobuf::FieldDescriptor *fd,
    const google::protobuf::Descriptor *desc,
    const google::protobuf::Message *message);

/**
 * Extract field value from protobuf message
 */
ProtobufVariant getMessageFieldValue(
    const google::protobuf::Message *message,
    const google::protobuf::FieldDescriptor *fd);

/**
 * Set field value in protobuf message
 */
void setMessageField(google::protobuf::Message *message,
                     const google::protobuf::FieldDescriptor *fd,
                     const ProtobufVariant &value);

/**
 * Convert a FileDescriptor to base64-encoded string
 */
std::string schemaToString(const google::protobuf::FileDescriptor *file_desc);

/**
 * Parse base64-encoded schema string to FileDescriptor
 */
const google::protobuf::FileDescriptor *stringToSchema(
    google::protobuf::DescriptorPool *pool, const std::string &name,
    const std::string &schema_string);

/**
 * Decode FileDescriptorProto with proper name handling
 */
void decodeFileDescriptorProtoWithName(google::protobuf::DescriptorPool *pool,
                                       const std::string &name,
                                       const std::vector<uint8_t> &bytes);

/**
 * Check if a proto file name is a built-in type
 */
bool isBuiltin(const std::string &name);

/**
 * Get field type from FieldDescriptor
 */
FieldType getFieldType(const google::protobuf::FieldDescriptor *field_desc);

/**
 * Extract inline tags from field options (confluent.field_meta)
 */
std::unordered_set<std::string> getInlineTags(
    const google::protobuf::FieldDescriptor *field_desc);

/**
 * Protobuf Message to JSON conversion
 */
nlohmann::json messageToJson(const google::protobuf::Message &message);

/**
 * JSON to Protobuf Message conversion
 */
std::unique_ptr<google::protobuf::Message> jsonToMessage(
    const nlohmann::json &json, const google::protobuf::Descriptor *descriptor,
    const google::protobuf::DescriptorPool *pool);

/**
 * Copy compatible fields between protobuf messages
 */
void copyCompatibleFields(const google::protobuf::Message &source,
                          google::protobuf::Message &target);

/**
 * Get message descriptor from pool using message index array
 */
const google::protobuf::Descriptor *getMessageDescriptorByIndex(
    const google::protobuf::DescriptorPool *pool,
    const google::protobuf::FileDescriptor *file_desc,
    const std::vector<int32_t> &msg_index);

/**
 * Create message index array for nested message types
 */
std::vector<int32_t> createMessageIndexArray(
    const google::protobuf::Descriptor *descriptor);

/**
 * Schema resolution utilities
 */
namespace schema_resolution {

/**
 * Resolve named schemas recursively
 */
void resolveNamedSchema(
    const schemaregistry::rest::model::Schema &schema,
    std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
    google::protobuf::DescriptorPool *pool,
    std::unordered_set<std::string> &visited);

/**
 * Build dependencies list for a FileDescriptor
 */
std::vector<schemaregistry::rest::model::SchemaReference> buildDependencies(
    const google::protobuf::FileDescriptor *file_desc,
    const std::function<std::string(const std::string &, SerdeType)>
        &reference_strategy,
    SerdeType serde_type);
}  // namespace schema_resolution

}  // namespace schemaregistry::serdes::protobuf::utils