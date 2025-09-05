#pragma once

#include <google/protobuf/any.pb.h>
#include <google/protobuf/api.pb.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/duration.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/empty.pb.h>
#include <google/protobuf/field_mask.pb.h>
#include <google/protobuf/message.h>
#include <google/protobuf/source_context.pb.h>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/type.pb.h>
#include <google/protobuf/wrappers.pb.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "schemaregistry/serdes/Serde.h"
#include "schemaregistry/serdes/SerdeConfig.h"
#include "schemaregistry/serdes/SerdeError.h"
#include "schemaregistry/serdes/SerdeTypes.h"
#include "schemaregistry/serdes/protobuf/ProtobufTypes.h"
#include "schemaregistry/serdes/protobuf/ProtobufUtils.h"

namespace schemaregistry::serdes::protobuf {

class ProtobufSerde;

/**
 * Reference subject name strategy function type
 * Converts reference name and serde type to subject name
 */
using ReferenceSubjectNameStrategy = std::function<std::string(
    const std::string &ref_name, SerdeType serde_type)>;

/**
 * Default reference subject name strategy implementation
 */
std::string defaultReferenceSubjectNameStrategy(const std::string &ref_name,
                                                SerdeType serde_type);

/**
 * Protobuf serializer class template
 * Based on ProtobufSerializer from protobuf.rs (converted to synchronous)
 */
template <typename T = google::protobuf::Message>
class ProtobufSerializer {
  public:
    /**
     * Constructor with default reference subject name strategy
     */
    ProtobufSerializer(
        std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
        std::optional<schemaregistry::rest::model::Schema> schema,
        std::shared_ptr<RuleRegistry> rule_registry,
        const SerializerConfig &config);

    /**
     * Constructor with custom reference subject name strategy
     */
    ProtobufSerializer(
        std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
        std::optional<schemaregistry::rest::model::Schema> schema,
        std::shared_ptr<RuleRegistry> rule_registry,
        const SerializerConfig &config, ReferenceSubjectNameStrategy strategy);

    /**
     * Serialize a protobuf message
     */
    std::vector<uint8_t> serialize(const SerializationContext &ctx,
                                   const T &message);

    /**
     * Serialize with file descriptor set
     */
    std::vector<uint8_t> serializeWithFileDescriptorSet(
        const SerializationContext &ctx, const T &message,
        const std::string &message_type_name,
        const google::protobuf::FileDescriptorSet &fds);

    /**
     * Serialize with message descriptor
     */
    std::vector<uint8_t> serializeWithMessageDescriptor(
        const SerializationContext &ctx, const T &message,
        const google::protobuf::Descriptor *descriptor);

  private:
    std::optional<schemaregistry::rest::model::Schema> schema_;
    std::shared_ptr<BaseSerializer> base_;
    std::unique_ptr<ProtobufSerde> serde_;
    ReferenceSubjectNameStrategy reference_subject_name_strategy_;

    // Helper methods
    std::vector<int32_t> toIndexArray(
        const google::protobuf::Descriptor *descriptor);

    std::vector<schemaregistry::rest::model::SchemaReference>
    resolveDependencies(const SerializationContext &ctx,
                        const google::protobuf::FileDescriptor *file_desc);

    void validateSchema(const schemaregistry::rest::model::Schema &schema);
};

}  // namespace schemaregistry::serdes::protobuf

// Begin template method implementations moved from ProtobufSerializer.cpp so
// that users can instantiate the serializer for custom protobuf message types
// from translation units that do not directly compile `ProtobufSerializer.cpp`.
// Keeping the implementations in the header avoids undefined symbol errors
// during linking when the serializer/deserializer are used with types other
// than `google::protobuf::Message` (e.g. the unit-test messages in
// `test/example.proto`).

#ifndef schemaregistry_PROTOBUF_SKIP_TEMPLATE_IMPL

namespace schemaregistry::serdes::protobuf {
// ---------------------- ProtobufSerializer -------------------------------

template <typename T>
inline ProtobufSerializer<T>::ProtobufSerializer(
    std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
    std::optional<schemaregistry::rest::model::Schema> schema,
    std::shared_ptr<RuleRegistry> rule_registry, const SerializerConfig &config)
    : schema_(std::move(schema)),
      base_(std::make_shared<BaseSerializer>(
          Serde(std::move(client), rule_registry), config)),
      serde_(std::make_unique<ProtobufSerde>()),
      reference_subject_name_strategy_(defaultReferenceSubjectNameStrategy) {
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
            throw ProtobufError("Failed to configure rule executor: " +
                                std::string(e.what()));
        }
    }
}

template <typename T>
inline ProtobufSerializer<T>::ProtobufSerializer(
    std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
    std::optional<schemaregistry::rest::model::Schema> schema,
    std::shared_ptr<RuleRegistry> rule_registry, const SerializerConfig &config,
    ReferenceSubjectNameStrategy strategy)
    : schema_(std::move(schema)),
      base_(std::make_shared<BaseSerializer>(
          Serde(std::move(client), rule_registry), config)),
      serde_(std::make_unique<ProtobufSerde>()),
      reference_subject_name_strategy_(std::move(strategy)) {
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
            throw ProtobufError("Failed to configure rule executor: " +
                                std::string(e.what()));
        }
    }
}

template <typename T>
inline std::vector<uint8_t> ProtobufSerializer<T>::serialize(
    const SerializationContext &ctx, const T &message) {
    return serializeWithMessageDescriptor(ctx, message,
                                          message.GetDescriptor());
}

template <typename T>
inline std::vector<uint8_t>
ProtobufSerializer<T>::serializeWithFileDescriptorSet(
    const SerializationContext &ctx, const T &message,
    const std::string &message_type_name,
    const google::protobuf::FileDescriptorSet &fds) {
    google::protobuf::DescriptorPool pool;
    for (const auto &file_desc : fds.file()) {
        const auto *file = pool.BuildFile(file_desc);
        if (!file) {
            throw ProtobufError("Failed to build file descriptor from set");
        }
    }

    const auto *descriptor = pool.FindMessageTypeByName(message_type_name);
    if (!descriptor) {
        throw ProtobufError("Message descriptor " + message_type_name +
                            " not found");
    }
    return serializeWithMessageDescriptor(ctx, message, descriptor);
}

// Forward declaration for transformFields helper that lives in
// ProtobufUtils.cpp
namespace utils {
std::unique_ptr<schemaregistry::serdes::SerdeValue> transformFields(
    schemaregistry::serdes::RuleContext &ctx,
    const std::string &field_executor_type,
    const schemaregistry::serdes::SerdeValue &value);
}

template <typename T>
inline std::vector<uint8_t>
ProtobufSerializer<T>::serializeWithMessageDescriptor(
    const SerializationContext &ctx, const T &message,
    const google::protobuf::Descriptor *descriptor) {
    using namespace schemaregistry::serdes;
    using namespace schemaregistry::serdes::protobuf;
    using schemaregistry::rest::model::RegisteredSchema;

    // Resolve the subject name.
    auto strategy = base_->getConfig().subject_name_strategy;
    auto subject_opt = strategy(ctx.topic, ctx.serde_type, schema_);
    if (!subject_opt.has_value()) {
        throw ProtobufError("Subject name strategy returned no subject");
    }
    std::string subject = *subject_opt;

    // Retrieve (or register) the schema in the registry.
    SchemaId schema_id(SerdeFormat::Protobuf);
    std::optional<RegisteredSchema> latest_schema;
    std::vector<uint8_t> encoded_bytes;

    try {
        latest_schema = base_->getSerde().getReaderSchema(
            subject, "serialized", base_->getConfig().use_schema);
    } catch (const std::exception &) {
        // Not found – handled below.
    }

    if (latest_schema) {
        // Path when writer schema is known already to the registry.
        schema_id = SchemaId(SerdeFormat::Protobuf, latest_schema->getId(),
                             latest_schema->getGuid(), std::nullopt);

        auto schema = latest_schema->toSchema();
        auto [fd, pool] =
            serde_->getParsedSchema(schema, base_->getSerde().getClient());

        auto field_tf = [descriptor](RuleContext &rctx,
                                     const std::string &rule_type,
                                     const SerdeValue &val) {
            return utils::transformFields(rctx, descriptor, val);
        };

        google::protobuf::DynamicMessageFactory msg_factory;
        auto *dynamic_proto = msg_factory.GetPrototype(descriptor);
        auto dynamic_msg =
            std::unique_ptr<google::protobuf::Message>(dynamic_proto->New());
        dynamic_msg->CopyFrom(message);

        auto dynamic_msg_copy =
            std::unique_ptr<google::protobuf::Message>(dynamic_msg->New());
        dynamic_msg_copy->CopyFrom(*dynamic_msg);
        auto protobuf_value = protobuf::makeProtobufValue(
            ProtobufVariant(std::move(dynamic_msg_copy)));

        auto serde_value = base_->getSerde().executeRules(
            ctx, subject, Mode::Write, std::nullopt, std::make_optional(schema),
            *protobuf_value, {}, std::make_shared<FieldTransformer>(field_tf));

        if (serde_value->getFormat() != SerdeFormat::Protobuf) {
            throw ProtobufError(
                "Unexpected serde value type after rule execution");
        }

        // Convert the serde_value to a Protobuf message
        auto &proto_variant = asProtobuf(*serde_value);
        auto &transformed_msg =
            *proto_variant
                 .template get<std::unique_ptr<google::protobuf::Message>>();
        encoded_bytes.resize(
            static_cast<size_t>(transformed_msg.ByteSizeLong()));
        if (!transformed_msg.SerializeToArray(
                encoded_bytes.data(), static_cast<int>(encoded_bytes.size()))) {
            throw ProtobufError("Failed to serialize protobuf message");
        }
    } else {
        // Schema not present in registry – create & register or look it up.
        auto refs = resolveDependencies(ctx, descriptor->file());

        schemaregistry::rest::model::Schema schema;
        schema.setSchemaType("PROTOBUF");
        schema.setReferences(refs);
        schema.setSchema(utils::schemaToString(descriptor->file()));

        if (base_->getConfig().auto_register_schemas) {
            auto reg = base_->getSerde().getClient()->registerSchema(
                subject, schema, base_->getConfig().normalize_schemas);
            schema_id = SchemaId(SerdeFormat::Protobuf, reg.getId(),
                                 reg.getGuid(), std::nullopt);
        } else {
            auto reg = base_->getSerde().getClient()->getBySchema(
                subject, schema, base_->getConfig().normalize_schemas, false);
            schema_id = SchemaId(SerdeFormat::Protobuf, reg.getId(),
                                 reg.getGuid(), std::nullopt);
        }

        encoded_bytes.resize(static_cast<size_t>(message.ByteSizeLong()));
        if (!message.SerializeToArray(encoded_bytes.data(),
                                      static_cast<int>(encoded_bytes.size()))) {
            throw ProtobufError("Failed to serialize protobuf message");
        }
    }

    // Apply encoding-phase rules if they exist.
    if (latest_schema) {
        auto schema = latest_schema->toSchema();
        if (schema.getRuleSet().has_value()) {
            auto rule_set = schema.getRuleSet().value();
            if (rule_set.getEncodingRules().has_value()) {
                auto bytes_value =
                    SerdeValue::newBytes(SerdeFormat::Protobuf, encoded_bytes);
                auto result = base_->getSerde().executeRulesWithPhase(
                    ctx, subject, Phase::Encoding, Mode::Write, std::nullopt,
                    std::make_optional(schema), *bytes_value, {});
                encoded_bytes = result->asBytes();
            }
        }
    }

    // Store message index information (for nested msgs).
    schema_id.setMessageIndexes(toIndexArray(descriptor));

    // Final framing (schema id serialization).
    auto id_serializer = base_->getConfig().schema_id_serializer;
    return id_serializer(encoded_bytes, ctx, schema_id);
}

template <typename T>
inline std::vector<schemaregistry::rest::model::SchemaReference>
ProtobufSerializer<T>::resolveDependencies(
    const SerializationContext &ctx,
    const google::protobuf::FileDescriptor *file_desc) {
    std::vector<schemaregistry::rest::model::SchemaReference> refs;

    for (int i = 0; i < file_desc->dependency_count(); ++i) {
        const auto *dep = file_desc->dependency(i);
        if (utils::isBuiltin(dep->name())) {
            continue;
        }

        auto dep_refs = resolveDependencies(ctx, dep);
        auto subject =
            reference_subject_name_strategy_(dep->name(), ctx.serde_type);

        schemaregistry::rest::model::Schema schema;
        schema.setSchemaType("PROTOBUF");
        schema.setReferences(dep_refs);
        schema.setSchema(utils::schemaToString(dep));

        if (base_->getConfig().auto_register_schemas) {
            base_->getSerde().getClient()->registerSchema(
                subject, schema, base_->getConfig().normalize_schemas);
        }

        auto reference = base_->getSerde().getClient()->getBySchema(
            subject, schema, base_->getConfig().normalize_schemas, false);

        schemaregistry::rest::model::SchemaReference sr;
        sr.setName(dep->name());
        sr.setSubject(subject);
        sr.setVersion(reference.getVersion());

        refs.push_back(sr);
    }
    return refs;
}

template <typename T>
inline std::vector<int32_t> ProtobufSerializer<T>::toIndexArray(
    const google::protobuf::Descriptor *descriptor) {
    std::vector<int32_t> indices;
    const auto *file = descriptor->file();
    for (int i = 0; i < file->message_type_count(); ++i) {
        if (file->message_type(i) == descriptor) {
            indices.push_back(i);
            break;
        }
    }
    return indices;
}

template <typename T>
inline void ProtobufSerializer<T>::validateSchema(
    const schemaregistry::rest::model::Schema &schema) {
    auto schema_str = schema.getSchema();
    if (!schema_str || schema_str->empty()) {
        throw ProtobufError("Schema content is empty");
    }
    auto schema_type = schema.getSchemaType();
    if (schema_type && schema_type.value() != "PROTOBUF") {
        throw ProtobufError("Schema type must be PROTOBUF");
    }
}

}  // namespace schemaregistry::serdes::protobuf

// End of template method implementations
#endif  // schemaregistry_PROTOBUF_SKIP_TEMPLATE_IMPL