#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "schemaregistry/rest/ISchemaRegistryClient.h"
#include "schemaregistry/serdes/RuleRegistry.h"
#include "schemaregistry/serdes/SerdeConfig.h"
#include "schemaregistry/serdes/SerdeError.h"
#include "schemaregistry/serdes/SerdeTypes.h"
#include "schemaregistry/serdes/WildcardMatcher.h"

// Forward declarations
namespace schemaregistry::rest {
class ISchemaRegistryClient;
}

namespace schemaregistry::rest::model {
class Schema;
}

namespace schemaregistry::serdes {

/**
 * Schema ID wrapper for different ID types
 * Based on SchemaId struct from serde.rs
 */
class SchemaId {
  private:
    SerdeFormat serde_format_;
    std::optional<int32_t> id_;
    std::optional<std::string> guid_;
    std::optional<std::vector<int32_t>> message_indexes_;

  public:
    SchemaId() = default;
    SchemaId(
        SerdeFormat serde_format, std::optional<int32_t> id = std::nullopt,
        std::optional<std::string> guid = std::nullopt,
        std::optional<std::vector<int32_t>> message_indexes = std::nullopt);

    // Accessors
    SerdeFormat getSerdeFormat() const { return serde_format_; }
    std::optional<int32_t> getId() const { return id_; }
    std::optional<std::string> getGuid() const { return guid_; }
    std::optional<std::vector<int32_t>> getMessageIndexes() const {
        return message_indexes_;
    }

    // Setters
    void setId(int32_t id) { id_ = id; }
    void setGuid(const std::string &guid) { guid_ = guid; }
    void setMessageIndexes(const std::vector<int32_t> &indexes) {
        message_indexes_ = indexes;
    }

    // Serialization methods (synchronous versions)
    size_t readFromBytes(const std::vector<uint8_t> &bytes);
    std::vector<uint8_t> idToBytes() const;
    std::vector<uint8_t> guidToBytes() const;

    // Copy/move constructors and assignment operators
    SchemaId(const SchemaId &) = default;
    SchemaId(SchemaId &&) = default;
    SchemaId &operator=(const SchemaId &) = default;
    SchemaId &operator=(SchemaId &&) = default;

  private:
    std::pair<std::vector<int32_t>, size_t> readIndexArrayAndData(
        const std::vector<uint8_t> &buf) const;
    std::optional<std::vector<uint8_t>> toEncodedIndexArray() const;
};

/**
 * Kafka message header
 * Based on SerdeHeader from serde.rs
 */
struct SerdeHeader {
    std::string key;
    std::optional<std::vector<uint8_t>> value;

    SerdeHeader() = default;
    SerdeHeader(const std::string &k,
                const std::optional<std::vector<uint8_t>> &v = std::nullopt)
        : key(k), value(v) {}

    bool operator==(const SerdeHeader &other) const {
        return key == other.key && value == other.value;
    }

    bool operator<(const SerdeHeader &other) const {
        if (key != other.key) return key < other.key;
        return value < other.value;
    }
};

/**
 * Kafka message headers collection
 * Based on SerdeHeaders from serde.rs
 */
class SerdeHeaders {
  private:
    std::shared_ptr<std::mutex> mutex_;
    std::shared_ptr<std::vector<SerdeHeader>> headers_;

  public:
    SerdeHeaders();
    explicit SerdeHeaders(const std::vector<SerdeHeader> &headers);

    // Size and access
    size_t count() const;
    SerdeHeader get(size_t idx) const;
    std::optional<SerdeHeader> tryGet(size_t idx) const;

    // Modification
    void insert(const SerdeHeader &header);
    std::optional<SerdeHeader> lastHeader(const std::string &key) const;
    std::optional<std::vector<uint8_t>> getLastHeaderValue(
        const std::string &key) const;
    void remove(const std::string &key);

    // Copy/move constructors and assignment operators
    SerdeHeaders(const SerdeHeaders &) = default;
    SerdeHeaders(SerdeHeaders &&) = default;
    SerdeHeaders &operator=(const SerdeHeaders &) = default;
    SerdeHeaders &operator=(SerdeHeaders &&) = default;
};

/**
 * Serialization context containing topic, type, format, and headers
 * Based on SerializationContext from serde.rs
 */
struct SerializationContext {
    std::string topic;
    SerdeType serde_type;
    SerdeFormat serde_format;
    std::optional<SerdeHeaders> headers;

    SerializationContext() = default;
    SerializationContext(const std::string &t, SerdeType st, SerdeFormat sf,
                         std::optional<SerdeHeaders> h = std::nullopt)
        : topic(t), serde_type(st), serde_format(sf), headers(h) {}
};

/**
 * Field context for rule processing
 * Based on FieldContext from serde.rs
 */
class FieldContext {
  private:
    const SerdeValue &containing_message_;
    std::string full_name_;
    std::string name_;
    mutable std::mutex field_type_mutex_;
    FieldType field_type_;
    std::unordered_set<std::string> tags_;

  public:
    FieldContext(const SerdeValue &containing_message,
                 const std::string &full_name, const std::string &name,
                 FieldType field_type,
                 const std::unordered_set<std::string> &tags);

    // Accessors
    const SerdeValue &getContainingMessage() const {
        return containing_message_;
    }
    const std::string &getFullName() const { return full_name_; }
    const std::string &getName() const { return name_; }
    FieldType getFieldType() const;
    void setFieldType(FieldType field_type);
    const std::unordered_set<std::string> &getTags() const { return tags_; }

    // Utility methods
    bool isPrimitive() const;
    std::string typeName() const;

    // Copy/move constructors and assignment operators are deleted due to mutex
    FieldContext(const FieldContext &) = delete;
    FieldContext(FieldContext &&) = delete;
    FieldContext &operator=(const FieldContext &) = delete;
    FieldContext &operator=(FieldContext &&) = delete;
};

/**
 * Rule execution context
 * Based on RuleContext from serde.rs
 */
class RuleContext {
  private:
    SerializationContext ser_ctx_;
    std::optional<Schema> source_;
    std::optional<Schema> target_;
    std::string subject_;
    Mode rule_mode_;
    Rule rule_;
    size_t index_;
    std::vector<Rule> rules_;
    std::unordered_map<std::string, std::unordered_set<std::string>>
        inline_tags_;
    std::vector<std::unique_ptr<FieldContext>> field_contexts_;
    std::shared_ptr<FieldTransformer> field_transformer_;
    std::shared_ptr<RuleRegistry> rule_registry_;

  public:
    RuleContext(const SerializationContext &ser_ctx,
                std::optional<Schema> source, std::optional<Schema> target,
                const std::string &subject, Mode rule_mode, const Rule &rule,
                size_t index, const std::vector<Rule> &rules,
                std::unordered_map<std::string, std::unordered_set<std::string>>
                    inline_tags,
                std::shared_ptr<FieldTransformer> field_transformer = nullptr,
                std::shared_ptr<RuleRegistry> rule_registry = nullptr);

    // Accessors
    const SerializationContext &getSerializationContext() const {
        return ser_ctx_;
    }
    const std::optional<Schema> &getSource() const { return source_; }
    const std::optional<Schema> &getTarget() const { return target_; }
    const std::string &getSubject() const { return subject_; }
    Mode getRuleMode() const { return rule_mode_; }
    const Rule &getRule() const { return rule_; }
    size_t getIndex() const { return index_; }
    const std::vector<Rule> &getRules() const { return rules_; }
    std::shared_ptr<FieldTransformer> getFieldTransformer() const {
        return field_transformer_;
    }
    std::shared_ptr<RuleRegistry> getRuleRegistry() const {
        return rule_registry_;
    }

    // Parameter handling
    std::optional<std::string> getParameter(const std::string &name) const;

    std::optional<std::unordered_set<std::string>> getInlineTags(
        const std::string &name) const;

    // Field context management
    std::optional<FieldContext> currentField() const;
    void enterField(const SerdeValue &containing_message,
                    const std::string &full_name, const std::string &name,
                    FieldType field_type,
                    const std::unordered_set<std::string> &tags);
    void exitField();

    // Tag handling
    std::unordered_set<std::string> getTags(const std::string &full_name) const;

    // Copy/move constructors and assignment operators are deleted due to
    // unique_ptr
    RuleContext(const RuleContext &) = delete;
    RuleContext(RuleContext &&) = default;
    RuleContext &operator=(const RuleContext &) = delete;
    RuleContext &operator=(RuleContext &&) = default;
};

/**
 * Base serialization/deserialization class
 * Based on Serde struct from serde.rs (converted to synchronous)
 */
class Serde {
  private:
    std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client_;
    std::shared_ptr<RuleRegistry> rule_registry_;

  public:
    Serde(std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
          std::shared_ptr<RuleRegistry> rule_registry = nullptr);

    // Schema retrieval (synchronous versions)
    std::optional<RegisteredSchema> getReaderSchema(
        const std::string &subject, std::optional<std::string> format,
        const std::optional<SchemaSelector> &use_schema) const;

    // Rule execution (synchronous versions)
    std::unique_ptr<SerdeValue> executeRules(
        const SerializationContext &ser_ctx, const std::string &subject,
        Mode rule_mode, std::optional<Schema> source,
        std::optional<Schema> target, const SerdeValue &msg,
        std::unordered_map<std::string, std::unordered_set<std::string>>
            inline_tags,
        std::shared_ptr<FieldTransformer> field_transformer = nullptr) const;

    std::unique_ptr<SerdeValue> executeRulesWithPhase(
        const SerializationContext &ser_ctx, const std::string &subject,
        Phase rule_phase, Mode rule_mode, std::optional<Schema> source,
        std::optional<Schema> target, const SerdeValue &msg,
        std::unordered_map<std::string, std::unordered_set<std::string>>
            inline_tags,
        std::shared_ptr<FieldTransformer> field_transformer = nullptr) const;

    // Migration support (synchronous versions)
    std::vector<Migration> getMigrations(
        const std::string &subject, const Schema &source_info,
        const RegisteredSchema &target,
        std::optional<std::string> format = std::nullopt) const;

    std::vector<RegisteredSchema> getSchemasBetween(
        const std::string &subject, const RegisteredSchema &first,
        const RegisteredSchema &last,
        std::optional<std::string> format = std::nullopt) const;

    std::unique_ptr<SerdeValue> executeMigrations(
        const SerializationContext &ser_ctx, const std::string &subject,
        const std::vector<Migration> &migrations, const SerdeValue &msg) const;

    // Accessors
    std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> getClient()
        const {
        return client_;
    }
    std::shared_ptr<RuleRegistry> getRuleRegistry() const {
        return rule_registry_;
    }

  private:
    // Helper methods for rule processing (synchronous versions)
    std::vector<Rule> getMigrationRules(std::optional<Schema> schema) const;
    std::vector<Rule> getDomainRules(std::optional<Schema> schema) const;
    std::vector<Rule> getEncodingRules(std::optional<Schema> schema) const;

    std::optional<std::string> getOnSuccess(const Rule &rule) const;
    std::optional<std::string> getOnFailure(const Rule &rule) const;
    bool isDisabled(const Rule &rule) const;

    void runAction(const RuleContext &ctx, Mode rule_mode, const Rule &rule,
                   std::optional<std::string> action, const SerdeValue &msg,
                   std::optional<SerdeError> ex,
                   const std::string &default_action) const;

    std::optional<std::string> getRuleActionName(
        const Rule &rule, Mode mode,
        std::optional<std::string> action_name) const;

    std::shared_ptr<RuleAction> getRuleAction(
        const RuleContext &ctx, const std::string &action_name) const;

    bool hasRules(std::optional<RuleSet> rule_set, Phase phase,
                  Mode mode) const;
};

/**
 * Base serializer class
 * Based on BaseSerializer from serde.rs
 */
class BaseSerializer {
  private:
    Serde serde_;
    SerializerConfig config_;

  public:
    BaseSerializer(Serde serde, const SerializerConfig &config);

    // Accessors
    const Serde &getSerde() const { return serde_; }
    const SerializerConfig &getConfig() const { return config_; }

    // Copy/move constructors and assignment operators - deleted due to Serde
    // containing non-copyable RuleRegistry
    BaseSerializer(const BaseSerializer &) = delete;
    BaseSerializer(BaseSerializer &&) = delete;
    BaseSerializer &operator=(const BaseSerializer &) = delete;
    BaseSerializer &operator=(BaseSerializer &&) = delete;
};

/**
 * Base deserializer class
 * Based on BaseDeserializer from serde.rs
 */
class BaseDeserializer {
  private:
    Serde serde_;
    DeserializerConfig config_;

  public:
    BaseDeserializer(Serde serde, const DeserializerConfig &config);

    // Schema retrieval (synchronous version)
    Schema getWriterSchema(
        const SchemaId &schema_id,
        std::optional<std::string> subject = std::nullopt,
        std::optional<std::string> format = std::nullopt) const;

    // Accessors
    const Serde &getSerde() const { return serde_; }
    const DeserializerConfig &getConfig() const { return config_; }

    // Copy/move constructors and assignment operators - deleted due to Serde
    // containing non-copyable RuleRegistry
    BaseDeserializer(const BaseDeserializer &) = delete;
    BaseDeserializer(BaseDeserializer &&) = delete;
    BaseDeserializer &operator=(const BaseDeserializer &) = delete;
    BaseDeserializer &operator=(BaseDeserializer &&) = delete;
};

}  // namespace schemaregistry::serdes