#define schemaregistry_PROTOBUF_SKIP_TEMPLATE_IMPL
#include "schemaregistry/serdes/protobuf/ProtobufSerializer.h"

#include <google/protobuf/any.pb.h>
#include <google/protobuf/api.pb.h>
#include <google/protobuf/duration.pb.h>
#include <google/protobuf/empty.pb.h>
#include <google/protobuf/field_mask.pb.h>
#include <google/protobuf/source_context.pb.h>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/type.pb.h>
#include <google/protobuf/wrappers.pb.h>

#include "confluent/meta.pb.h"
#include "confluent/type/decimal.pb.h"
#include "schemaregistry/serdes/protobuf/ProtobufUtils.h"

// Forward declaration for transformFields function from ProtobufUtils.cpp
namespace schemaregistry::serdes::protobuf::utils {
std::unique_ptr<schemaregistry::serdes::SerdeValue> transformFields(
    schemaregistry::serdes::RuleContext &ctx,
    const std::string &field_executor_type,
    const schemaregistry::serdes::SerdeValue &value);
}

namespace schemaregistry::serdes::protobuf {

using namespace utils;

// Default reference subject name strategy implementation
std::string defaultReferenceSubjectNameStrategy(const std::string &ref_name,
                                                SerdeType serde_type) {
    return ref_name;
}

// ProtobufSerde implementation
ProtobufSerde::ProtobufSerde() {}

std::pair<const google::protobuf::FileDescriptor *,
          const google::protobuf::DescriptorPool *>
ProtobufSerde::getParsedSchema(
    const schemaregistry::rest::model::Schema &schema,
    std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client) {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    // Create cache key from schema content
    auto schema_str = schema.getSchema();
    std::string cache_key = schema_str.value_or("");

    auto it = parsed_schemas_cache_.find(cache_key);
    if (it != parsed_schemas_cache_.end()) {
        return {it->second.first, it->second.second.get()};
    }

    // Parse new schema
    auto pool = std::make_unique<google::protobuf::DescriptorPool>();
    initPool(pool.get());
    std::unordered_set<std::string> visited;

    // Resolve dependencies first
    auto references = schema.getReferences();
    if (references.has_value()) {
        for (const auto &ref : references.value()) {
            resolveNamedSchema(schema, client, pool.get(), visited);
        }
    }

    // Parse main schema
    auto file_desc = stringToSchema(pool.get(), "main.proto", cache_key);

    // Store in cache with raw FileDescriptor pointer (owned by pool)
    parsed_schemas_cache_[cache_key] =
        std::make_pair(file_desc, std::move(pool));

    return {file_desc, parsed_schemas_cache_[cache_key].second.get()};
}

void ProtobufSerde::addFileToPool(
    google::protobuf::DescriptorPool *pool,
    const google::protobuf::FileDescriptor *file_descriptor) {
    google::protobuf::FileDescriptorProto file_descriptor_proto;
    file_descriptor->CopyTo(&file_descriptor_proto);
    pool->BuildFile(file_descriptor_proto);
}

void ProtobufSerde::initPool(google::protobuf::DescriptorPool *pool) {
    // Add Google's well-known types to the descriptor pool using BuildFile
    addFileToPool(pool, google::protobuf::Any::descriptor()->file());
    // Source_context needed by api
    addFileToPool(pool, google::protobuf::SourceContext::descriptor()->file());
    // Type needed by api
    addFileToPool(pool, google::protobuf::Type::descriptor()->file());
    addFileToPool(pool, google::protobuf::Api::descriptor()->file());
    addFileToPool(pool,
                  google::protobuf::DescriptorProto::descriptor()->file());
    addFileToPool(pool, google::protobuf::Duration::descriptor()->file());
    addFileToPool(pool, google::protobuf::Empty::descriptor()->file());
    addFileToPool(pool, google::protobuf::FieldMask::descriptor()->file());
    addFileToPool(pool, google::protobuf::Struct::descriptor()->file());
    addFileToPool(pool, google::protobuf::Timestamp::descriptor()->file());

    // Add wrapper types
    addFileToPool(pool, google::protobuf::DoubleValue::descriptor()->file());
    addFileToPool(pool, google::protobuf::FloatValue::descriptor()->file());
    addFileToPool(pool, google::protobuf::Int64Value::descriptor()->file());
    addFileToPool(pool, google::protobuf::UInt64Value::descriptor()->file());
    addFileToPool(pool, google::protobuf::Int32Value::descriptor()->file());
    addFileToPool(pool, google::protobuf::UInt32Value::descriptor()->file());
    addFileToPool(pool, google::protobuf::BoolValue::descriptor()->file());
    addFileToPool(pool, google::protobuf::StringValue::descriptor()->file());
    addFileToPool(pool, google::protobuf::BytesValue::descriptor()->file());

    addFileToPool(pool, confluent::Meta::descriptor()->file());
    addFileToPool(pool, confluent::type::Decimal::descriptor()->file());
}

void ProtobufSerde::clear() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    parsed_schemas_cache_.clear();
}

void ProtobufSerde::resolveNamedSchema(
    const schemaregistry::rest::model::Schema &schema,
    std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
    google::protobuf::DescriptorPool *pool,
    std::unordered_set<std::string> &visited) {
    // Implement dependency resolution
    // This recursively resolves schema references
    auto references = schema.getReferences();
    if (references.has_value()) {
        for (const auto &ref : references.value()) {
            auto name = ref.getName().value_or("");
            if (isBuiltin(name) || visited.find(name) != visited.end()) {
                continue;
            }
            visited.insert(name);

            auto subject = ref.getSubject().value_or("");
            auto version = ref.getVersion().value_or(-1);

            try {
                auto ref_schema =
                    client->getVersion(subject, version, true, "serialized");
                auto schema_obj = ref_schema.toSchema();
                resolveNamedSchema(schema_obj, client, pool, visited);
                auto schema_content = ref_schema.getSchema().value_or("");
                stringToSchema(pool, name, schema_content);
            } catch (const std::exception &e) {
                throw ProtobufError("Failed to resolve schema reference: " +
                                    name + " - " + e.what());
            }
        }
    }
}

}  // namespace schemaregistry::serdes::protobuf