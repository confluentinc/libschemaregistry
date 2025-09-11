#include "schemaregistry/serdes/Serde.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "schemaregistry/rest/ClientConfiguration.h"
#include "schemaregistry/serdes/RuleRegistry.h"
#include "schemaregistry/serdes/SerdeConfig.h"
#include "schemaregistry/serdes/WildcardMatcher.h"

namespace schemaregistry::serdes {

// SchemaId implementation

SchemaId::SchemaId(SerdeFormat serde_format, std::optional<int32_t> id,
                   std::optional<std::string> guid,
                   std::optional<std::vector<int32_t>> message_indexes)
    : serde_format_(serde_format), id_(id), message_indexes_(message_indexes) {
    if (guid.has_value()) {
        guid_ = guid.value();
    }
}

size_t SchemaId::readFromBytes(const std::vector<uint8_t> &bytes) {
    if (bytes.empty()) {
        throw SerdeError("Empty byte array");
    }

    size_t total_bytes_read = 0;
    uint8_t magic = bytes[0];

    if (magic == MAGIC_BYTE_V0) {
        if (bytes.size() < 5) {
            throw SerdeError("Insufficient bytes for schema ID");
        }

        // Read 4-byte big-endian integer
        int32_t id = static_cast<int32_t>((bytes[1] << 24) | (bytes[2] << 16) |
                                          (bytes[3] << 8) | bytes[4]);
        id_ = id;
        total_bytes_read = 5;
    } else if (magic == MAGIC_BYTE_V1) {
        if (bytes.size() < 17) {
            throw SerdeError("Insufficient bytes for schema GUID");
        }

        // Read 16-byte UUID
        std::stringstream uuid_stream;
        uuid_stream << std::hex << std::setfill('0');

        // Format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
        for (int i = 1; i <= 16; ++i) {
            uuid_stream << std::setw(2) << static_cast<unsigned>(bytes[i]);
            if (i == 4 || i == 6 || i == 8 || i == 10) {
                uuid_stream << "-";
            }
        }

        guid_ = uuid_stream.str();
        total_bytes_read = 17;
    } else {
        throw SerdeError("Invalid magic byte");
    }

    if (serde_format_ == SerdeFormat::Protobuf &&
        total_bytes_read < bytes.size()) {
        auto [msg_indexes, bytes_read] =
            readIndexArrayAndData(std::vector<uint8_t>(
                bytes.begin() + total_bytes_read, bytes.end()));
        message_indexes_ = msg_indexes;
        total_bytes_read += bytes_read;
    }

    return total_bytes_read;
}

std::pair<std::vector<int32_t>, size_t> SchemaId::readIndexArrayAndData(
    const std::vector<uint8_t> &buf) const {
    if (buf.empty() || buf[0] == 0) {
        return {std::vector<int32_t>{0}, 1};
    }

    std::vector<int32_t> msg_idx;
    size_t pos = 0;

    // Read variable-length encoded array length
    int32_t len = 0;
    int shift = 0;
    while (pos < buf.size()) {
        uint8_t byte = buf[pos++];
        len |= (byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) break;
        shift += 7;
    }

    // Read variable-length encoded values
    for (int i = 0; i < len && pos < buf.size(); ++i) {
        int32_t value = 0;
        shift = 0;
        while (pos < buf.size()) {
            uint8_t byte = buf[pos++];
            value |= (byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) break;
            shift += 7;
        }
        msg_idx.push_back(value);
    }

    return {msg_idx, pos};
}

std::vector<uint8_t> SchemaId::idToBytes() const {
    if (!id_.has_value()) {
        throw SerdeError("Schema ID is not set");
    }

    std::vector<uint8_t> bytes;
    bytes.push_back(MAGIC_BYTE_V0);

    // Write 4-byte big-endian integer
    int32_t id = id_.value();
    bytes.push_back((id >> 24) & 0xFF);
    bytes.push_back((id >> 16) & 0xFF);
    bytes.push_back((id >> 8) & 0xFF);
    bytes.push_back(id & 0xFF);

    if (auto encoded_idx = toEncodedIndexArray()) {
        bytes.insert(bytes.end(), encoded_idx->begin(), encoded_idx->end());
    }

    return bytes;
}

std::vector<uint8_t> SchemaId::guidToBytes() const {
    if (!guid_.has_value()) {
        throw SerdeError("Schema GUID is not set");
    }

    std::vector<uint8_t> bytes;
    bytes.push_back(MAGIC_BYTE_V1);

    // Parse UUID string and convert to bytes
    std::string guid_str = guid_.value();
    std::string hex_str;
    for (char c : guid_str) {
        if (c != '-') {
            hex_str += c;
        }
    }

    if (hex_str.length() != 32) {
        throw SerdeError("Invalid UUID format");
    }

    for (size_t i = 0; i < hex_str.length(); i += 2) {
        std::string byte_str = hex_str.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
        bytes.push_back(byte);
    }

    if (auto encoded_idx = toEncodedIndexArray()) {
        bytes.insert(bytes.end(), encoded_idx->begin(), encoded_idx->end());
    }

    return bytes;
}

std::optional<std::vector<uint8_t>> SchemaId::toEncodedIndexArray() const {
    if (!message_indexes_.has_value()) {
        return std::nullopt;
    }

    const auto &msg_idx = message_indexes_.value();

    if (msg_idx.size() == 1 && msg_idx[0] == 0) {
        return std::vector<uint8_t>{0};
    }

    std::vector<uint8_t> result;

    // Encode array length using variable-length encoding
    int32_t len = static_cast<int32_t>(msg_idx.size());
    while (len >= 0x80) {
        result.push_back((len & 0x7F) | 0x80);
        len >>= 7;
    }
    result.push_back(len & 0x7F);

    // Encode each value using variable-length encoding
    for (int32_t value : msg_idx) {
        while (value >= 0x80) {
            result.push_back((value & 0x7F) | 0x80);
            value >>= 7;
        }
        result.push_back(value & 0x7F);
    }

    return result;
}

// SerdeHeaders implementation

SerdeHeaders::SerdeHeaders()
    : mutex_(std::make_shared<std::mutex>()),
      headers_(std::make_shared<std::vector<SerdeHeader>>()) {}

SerdeHeaders::SerdeHeaders(const std::vector<SerdeHeader> &headers)
    : mutex_(std::make_shared<std::mutex>()),
      headers_(std::make_shared<std::vector<SerdeHeader>>(headers)) {}

size_t SerdeHeaders::count() const {
    std::lock_guard<std::mutex> lock(*mutex_);
    return headers_->size();
}

SerdeHeader SerdeHeaders::get(size_t idx) const {
    auto header = tryGet(idx);
    if (!header.has_value()) {
        throw std::out_of_range("headers index out of bounds: the count is " +
                                std::to_string(count()) + " but the index is " +
                                std::to_string(idx));
    }
    return header.value();
}

std::optional<SerdeHeader> SerdeHeaders::tryGet(size_t idx) const {
    std::lock_guard<std::mutex> lock(*mutex_);
    if (idx < headers_->size()) {
        return (*headers_)[idx];
    }
    return std::nullopt;
}

void SerdeHeaders::insert(const SerdeHeader &header) {
    std::lock_guard<std::mutex> lock(*mutex_);
    headers_->push_back(header);
}

std::optional<SerdeHeader> SerdeHeaders::lastHeader(
    const std::string &key) const {
    std::lock_guard<std::mutex> lock(*mutex_);
    for (auto it = headers_->rbegin(); it != headers_->rend(); ++it) {
        if (it->key == key) {
            return *it;
        }
    }
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> SerdeHeaders::getLastHeaderValue(
    const std::string &key) const {
    auto header = lastHeader(key);
    if (header.has_value()) {
        return header->value;
    }
    return std::nullopt;
}

void SerdeHeaders::remove(const std::string &key) {
    std::lock_guard<std::mutex> lock(*mutex_);
    headers_->erase(
        std::remove_if(headers_->begin(), headers_->end(),
                       [&key](const SerdeHeader &h) { return h.key == key; }),
        headers_->end());
}

// FieldContext implementation

FieldContext::FieldContext(const SerdeValue &containing_message,
                           const std::string &full_name,
                           const std::string &name, FieldType field_type,
                           const std::unordered_set<std::string> &tags)
    : containing_message_(containing_message),
      full_name_(full_name),
      name_(name),
      field_type_(field_type),
      tags_(tags) {}

FieldType FieldContext::getFieldType() const {
    std::lock_guard<std::mutex> lock(field_type_mutex_);
    return field_type_;
}

void FieldContext::setFieldType(FieldType field_type) {
    std::lock_guard<std::mutex> lock(field_type_mutex_);
    field_type_ = field_type;
}

bool FieldContext::isPrimitive() const {
    FieldType type = getFieldType();
    return type == FieldType::String || type == FieldType::Bytes ||
           type == FieldType::Int || type == FieldType::Long ||
           type == FieldType::Float || type == FieldType::Double ||
           type == FieldType::Boolean || type == FieldType::Null;
}

std::string FieldContext::typeName() const {
    FieldType type = getFieldType();
    switch (type) {
        case FieldType::Record:
            return "RECORD";
        case FieldType::Enum:
            return "ENUM";
        case FieldType::Array:
            return "ARRAY";
        case FieldType::Map:
            return "MAP";
        case FieldType::Combined:
            return "COMBINED";
        case FieldType::Fixed:
            return "FIXED";
        case FieldType::String:
            return "STRING";
        case FieldType::Bytes:
            return "BYTES";
        case FieldType::Int:
            return "INT";
        case FieldType::Long:
            return "LONG";
        case FieldType::Float:
            return "FLOAT";
        case FieldType::Double:
            return "DOUBLE";
        case FieldType::Boolean:
            return "BOOLEAN";
        case FieldType::Null:
            return "NULL";
        default:
            return "UNKNOWN";
    }
}

// RuleContext implementation

RuleContext::RuleContext(
    const SerializationContext &ser_ctx, std::optional<Schema> source,
    std::optional<Schema> target, const std::string &subject, Mode rule_mode,
    const Rule &rule, size_t index, const std::vector<Rule> &rules,
    std::unordered_map<std::string, std::unordered_set<std::string>>
        inline_tags,
    std::shared_ptr<FieldTransformer> field_transformer,
    std::shared_ptr<RuleRegistry> rule_registry)
    : ser_ctx_(ser_ctx),
      source_(source),
      target_(target),
      subject_(subject),
      rule_mode_(rule_mode),
      rule_(rule),
      index_(index),
      rules_(rules),
      inline_tags_(inline_tags),
      field_transformer_(field_transformer),
      rule_registry_(rule_registry) {}

std::optional<std::string> RuleContext::getParameter(
    const std::string &name) const {
    // First check rule parameters
    if (rule_.getParams().has_value()) {
        auto params = rule_.getParams()
                          .value();  // Store copy to avoid dangling reference
        auto it = params.find(name);
        if (it != params.end()) {
            return it->second;
        }
    }

    // Then check target schema metadata properties
    if (target_.has_value() && target_->getMetadata().has_value()) {
        auto metadata = target_->getMetadata()
                            .value();  // Store copy to avoid dangling reference
        if (metadata.getProperties().has_value()) {
            auto properties =
                metadata.getProperties()
                    .value();  // Store copy to avoid dangling reference
            auto it = properties.find(name);
            if (it != properties.end()) {
                return it->second;
            }
        }
    }

    return std::nullopt;
}

std::optional<std::unordered_set<std::string>> RuleContext::getInlineTags(
    const std::string &name) const {
    auto it = inline_tags_.find(name);
    if (it != inline_tags_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<FieldContext> RuleContext::currentField() const {
    if (field_contexts_.empty()) {
        return std::nullopt;
    }
    // Since FieldContext can't be copied/moved, return a copy constructed from
    // the back element
    const auto &back = *field_contexts_.back();
    return std::make_optional<FieldContext>(
        back.getContainingMessage(), back.getFullName(), back.getName(),
        back.getFieldType(), back.getTags());
}

void RuleContext::enterField(const SerdeValue &containing_message,
                             const std::string &full_name,
                             const std::string &name, FieldType field_type,
                             const std::unordered_set<std::string> &tags) {
    std::unordered_set<std::string> all_tags = tags;
    if (all_tags.empty()) {
        auto inline_tags = getInlineTags(full_name);
        if (inline_tags.has_value()) {
            all_tags = inline_tags.value();
        }
    }
    auto schema_tags = getTags(full_name);
    all_tags.insert(schema_tags.begin(), schema_tags.end());

    // Use unique_ptr to avoid copy/move issues with FieldContext
    field_contexts_.push_back(std::make_unique<FieldContext>(
        containing_message, full_name, name, field_type, all_tags));
}

void RuleContext::exitField() {
    if (!field_contexts_.empty()) {
        field_contexts_.pop_back();
    }
}

std::unordered_set<std::string> RuleContext::getTags(
    const std::string &full_name) const {
    std::unordered_set<std::string> result;

    if (target_.has_value() && target_->getMetadata().has_value()) {
        auto metadata = target_->getMetadata()
                            .value();  // Store copy to avoid dangling reference
        if (metadata.getTags().has_value()) {
            auto tags_map =
                metadata.getTags()
                    .value();  // Store copy to avoid dangling reference
            for (const auto &[pattern, tag] : tags_map) {
                if (wildcardMatch(full_name, pattern)) {  // Fixed function name
                    result.insert(pattern);
                }
            }
        }
    }

    return result;
}

// Serde implementation

Serde::Serde(
    std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
    std::shared_ptr<RuleRegistry> rule_registry)
    : client_(client), rule_registry_(rule_registry) {}

std::optional<RegisteredSchema> Serde::getReaderSchema(
    const std::string &subject, std::optional<std::string> format,
    const std::optional<SchemaSelector> &use_schema) const {
    if (!use_schema.has_value()) {
        return std::nullopt;
    }

    const auto &selector = use_schema.value();

    switch (selector.type) {
        case SchemaSelectorType::SchemaId: {
            if (!selector.schema_id.has_value()) {
                throw SerdeError(
                    "Schema ID not provided for SchemaId selector");
            }
            auto schema = client_->getBySubjectAndId(
                subject, selector.schema_id.value(), format);
            return client_->getBySchema(subject, schema, false, true);
        }

        case SchemaSelectorType::LatestVersion: {
            return client_->getLatestVersion(subject, format);
        }

        case SchemaSelectorType::LatestWithMetadata: {
            return client_->getLatestWithMetadata(subject, selector.metadata,
                                                  true, format);
        }

        default:
            throw SerdeError("Unknown schema selector type");
    }
}

std::unique_ptr<SerdeValue> Serde::executeRules(
    const SerializationContext &ser_ctx, const std::string &subject,
    Mode rule_mode, std::optional<Schema> source, std::optional<Schema> target,
    const SerdeValue &msg,
    std::unordered_map<std::string, std::unordered_set<std::string>>
        inline_tags,
    std::shared_ptr<FieldTransformer> field_transformer) const {
    return executeRulesWithPhase(ser_ctx, subject, Phase::Domain, rule_mode,
                                 source, target, msg, inline_tags,
                                 field_transformer);
}

std::unique_ptr<SerdeValue> Serde::executeRulesWithPhase(
    const SerializationContext &ser_ctx, const std::string &subject,
    Phase rule_phase, Mode rule_mode, std::optional<Schema> source,
    std::optional<Schema> target, const SerdeValue &msg,
    std::unordered_map<std::string, std::unordered_set<std::string>>
        inline_tags,
    std::shared_ptr<FieldTransformer> field_transformer) const {
    std::vector<Rule> rules;

    switch (rule_mode) {
        case Mode::Upgrade:
            rules = getMigrationRules(target);
            break;
        case Mode::Downgrade:
            rules = getMigrationRules(source);
            std::reverse(rules.begin(), rules.end());
            break;
        default:
            if (rule_phase == Phase::Encoding) {
                rules = getEncodingRules(target);
            } else {
                rules = getDomainRules(target);
            }
            if (rule_mode == Mode::Read) {
                std::reverse(rules.begin(), rules.end());
            }
            break;
    }

    if (rules.empty()) {
        return msg.clone();
    }

    // Create a local variable to track the current message state
    auto current_msg = msg.clone();

    for (size_t index = 0; index < rules.size(); ++index) {
        const auto &rule = rules[index];

        if (isDisabled(rule)) {
            continue;
        }

        Mode mode = rule.getMode().value_or(Mode::Write);
        switch (mode) {
            case Mode::WriteRead:
                if (rule_mode != Mode::Read && rule_mode != Mode::Write) {
                    continue;
                }
                break;
            case Mode::UpDown:
                if (rule_mode != Mode::Upgrade &&
                    rule_mode != Mode::Downgrade) {
                    continue;
                }
                break;
            default:
                if (mode != rule_mode) {
                    continue;
                }
                break;
        }

        RuleContext ctx(ser_ctx, source, target, subject, rule_mode, rule,
                        index, rules, inline_tags, field_transformer,
                        rule_registry_);

        // Fix: Check if rule type is available before using it
        if (!rule.getType().has_value()) {
            runAction(ctx, rule_mode, rule, getOnFailure(rule), *current_msg,
                      SerdeError("Rule type not specified"), "ERROR");
            return std::move(current_msg);
        }

        std::string rule_type =
            rule.getType().value();  // Store copy to avoid dangling reference

        auto executor = rule_registry_
                            ? rule_registry_->getExecutor(rule_type)
                            : global_registry::getRuleExecutor(rule_type);

        if (!executor) {
            runAction(ctx, rule_mode, rule, getOnFailure(rule), *current_msg,
                      SerdeError("Rule executor " + rule_type + " not found"),
                      "ERROR");
            return std::move(current_msg);
        }

        try {
            auto result = executor->transform(
                ctx, *current_msg);  // Now returns unique_ptr<SerdeValue>

            Kind kind = rule.getKind().value_or(Kind::Transform);
            if (kind == Kind::Condition) {
                // For condition rules, check if result is true
                if (!result->asBool()) {
                    runAction(ctx, rule_mode, rule, getOnFailure(rule),
                              *current_msg,
                              RuleConditionError(std::make_shared<Rule>(rule)),
                              "ERROR");
                }
            } else {
                // replace current_msg with result
                current_msg = std::move(result);
            }

            runAction(ctx, rule_mode, rule, getOnSuccess(rule), *current_msg,
                      std::nullopt, "NONE");
        } catch (const SerdeError &e) {
            runAction(ctx, rule_mode, rule, getOnFailure(rule), *current_msg, e,
                      "ERROR");
            return std::move(current_msg);
        } catch (const std::exception &e) {
            runAction(ctx, rule_mode, rule, getOnFailure(rule), *current_msg,
                      SerdeError(e.what()), "ERROR");
            return std::move(current_msg);
        }
    }

    return std::move(current_msg);
}

std::vector<Migration> Serde::getMigrations(
    const std::string &subject, const Schema &source_info,
    const RegisteredSchema &target, std::optional<std::string> format) const {
    auto source = client_->getBySchema(subject, source_info, false, true);
    std::vector<Migration> migrations;

    Mode migration_mode;
    const RegisteredSchema *first;
    const RegisteredSchema *last;

    int source_version = source.getVersion().value_or(0);
    int target_version = target.getVersion().value_or(0);

    if (source_version < target_version) {
        migration_mode = Mode::Upgrade;
        first = &source;
        last = &target;
    } else if (source_version > target_version) {
        migration_mode = Mode::Downgrade;
        first = &target;
        last = &source;
    } else {
        return migrations;  // No migration needed
    }

    auto versions = getSchemasBetween(subject, *first, *last, format);

    const RegisteredSchema *previous = nullptr;
    for (size_t i = 0; i < versions.size(); ++i) {
        const auto &version = versions[i];

        if (i == 0) {
            previous = &version;
            continue;
        }

        if (hasRules(version.getRuleSet(), Phase::Migration, migration_mode)) {
            Migration migration(migration_mode);
            if (migration_mode == Mode::Upgrade) {
                migration.source = *previous;
                migration.target = version;
            } else {
                migration.source = version;
                migration.target = *previous;
            }
            migrations.push_back(migration);
        }

        previous = &version;
    }

    if (migration_mode == Mode::Downgrade) {
        std::reverse(migrations.begin(), migrations.end());
    }

    return migrations;
}

std::vector<RegisteredSchema> Serde::getSchemasBetween(
    const std::string &subject, const RegisteredSchema &first,
    const RegisteredSchema &last, std::optional<std::string> format) const {
    int first_version = first.getVersion().value_or(0);
    int last_version = last.getVersion().value_or(0);

    if (last_version - first_version < 2) {
        return {first, last};
    }

    std::vector<RegisteredSchema> result = {first};

    for (int i = first_version + 1; i < last_version; ++i) {
        auto schema = client_->getVersion(subject, i, true, format);
        result.push_back(schema);
    }

    result.push_back(last);
    return result;
}

std::unique_ptr<SerdeValue> Serde::executeMigrations(
    const SerializationContext &ser_ctx, const std::string &subject,
    const std::vector<Migration> &migrations, const SerdeValue &msg) const {
    auto current_msg = msg.clone();
    for (const auto &migration : migrations) {
        std::optional<Schema> source =
            migration.source.has_value()
                ? std::make_optional(migration.source->toSchema())
                : std::nullopt;
        std::optional<Schema> target =
            migration.target.has_value()
                ? std::make_optional(migration.target->toSchema())
                : std::nullopt;

        current_msg = executeRulesWithPhase(ser_ctx, subject, Phase::Migration,
                                            migration.rule_mode, source, target,
                                            *current_msg, {});
    }

    return current_msg;
}

// Helper methods

std::vector<Rule> Serde::getMigrationRules(std::optional<Schema> schema) const {
    if (!schema.has_value() || !schema->getRuleSet().has_value()) {
        return {};
    }

    auto rule_set = schema->getRuleSet().value();
    if (!rule_set.getMigrationRules().has_value()) {
        return {};
    }

    return rule_set.getMigrationRules().value();
}

std::vector<Rule> Serde::getDomainRules(std::optional<Schema> schema) const {
    if (!schema.has_value() || !schema->getRuleSet().has_value()) {
        return {};
    }

    auto rule_set = schema->getRuleSet().value();
    if (!rule_set.getDomainRules().has_value()) {
        return {};
    }

    return rule_set.getDomainRules().value();
}

std::vector<Rule> Serde::getEncodingRules(std::optional<Schema> schema) const {
    if (!schema.has_value() || !schema->getRuleSet().has_value()) {
        return {};
    }

    auto rule_set = schema->getRuleSet().value();
    if (!rule_set.getEncodingRules().has_value()) {
        return {};
    }

    return rule_set.getEncodingRules().value();
}

std::optional<std::string> Serde::getOnSuccess(const Rule &rule) const {
    if (!rule.getType().has_value()) {
        return rule.getOnSuccess();
    }

    std::string rule_type =
        rule.getType().value();  // Store copy to avoid dangling reference

    if (rule_registry_) {
        auto override_opt = rule_registry_->getOverride(rule_type);
        if (override_opt.has_value() && override_opt->on_success.has_value()) {
            return override_opt->on_success;
        }
    } else {
        auto override_opt = global_registry::getRuleOverride(rule_type);
        if (override_opt.has_value() && override_opt->on_success.has_value()) {
            return override_opt->on_success;
        }
    }

    return rule.getOnSuccess();
}

std::optional<std::string> Serde::getOnFailure(const Rule &rule) const {
    if (!rule.getType().has_value()) {
        return rule.getOnFailure();
    }

    std::string rule_type =
        rule.getType().value();  // Store copy to avoid dangling reference

    if (rule_registry_) {
        auto override_opt = rule_registry_->getOverride(rule_type);
        if (override_opt.has_value() && override_opt->on_failure.has_value()) {
            return override_opt->on_failure;
        }
    } else {
        auto override_opt = global_registry::getRuleOverride(rule_type);
        if (override_opt.has_value() && override_opt->on_failure.has_value()) {
            return override_opt->on_failure;
        }
    }

    return rule.getOnFailure();
}

bool Serde::isDisabled(const Rule &rule) const {
    if (!rule.getType().has_value()) {
        return false;
    }

    std::string rule_type =
        rule.getType().value();  // Store copy to avoid dangling reference

    if (rule_registry_) {
        auto override_opt = rule_registry_->getOverride(rule_type);
        if (override_opt.has_value() && override_opt->disabled.has_value()) {
            return override_opt->disabled.value();
        }
    } else {
        auto override_opt = global_registry::getRuleOverride(rule_type);
        if (override_opt.has_value() && override_opt->disabled.has_value()) {
            return override_opt->disabled.value();
        }
    }

    return false;
}

void Serde::runAction(const RuleContext &ctx, Mode rule_mode, const Rule &rule,
                      std::optional<std::string> action, const SerdeValue &msg,
                      std::optional<SerdeError> ex,
                      const std::string &default_action) const {
    auto action_name = getRuleActionName(rule, rule_mode, action);
    if (!action_name.has_value()) {
        action_name = default_action;
    }

    auto rule_action = getRuleAction(ctx, action_name.value());
    if (!rule_action) {
        throw SerdeError("Rule action " + action_name.value() + " not found");
    }

    rule_action->run(ctx, msg, ex);
}

std::optional<std::string> Serde::getRuleActionName(
    const Rule &rule, Mode mode, std::optional<std::string> action_name) const {
    if (!action_name.has_value()) {
        return std::nullopt;
    }

    if (!rule.getMode().has_value()) {
        return action_name;
    }

    Mode rule_mode = rule.getMode().value();
    std::string action_str = action_name.value();

    if ((rule_mode == Mode::WriteRead || rule_mode == Mode::UpDown) &&
        action_str.find(",") != std::string::npos) {
        size_t comma_pos = action_str.find(",");

        if (mode == Mode::Write || mode == Mode::Upgrade) {
            return action_str.substr(0, comma_pos);
        } else if (mode == Mode::Read || mode == Mode::Downgrade) {
            return action_str.substr(comma_pos + 1);
        }
    }

    return action_str;
}

std::shared_ptr<RuleAction> Serde::getRuleAction(
    const RuleContext &ctx, const std::string &action_name) const {
    if (action_name == "ERROR") {
        return std::make_shared<ErrorAction>();
    } else if (action_name == "NONE") {
        return std::make_shared<NoneAction>();
    }

    if (rule_registry_) {
        return rule_registry_->getAction(action_name);
    } else {
        return global_registry::getRuleAction(action_name);
    }
}

bool Serde::hasRules(std::optional<RuleSet> rule_set, Phase phase,
                     Mode mode) const {
    if (!rule_set.has_value()) {
        return false;
    }

    std::optional<std::vector<Rule>> rules;
    switch (phase) {
        case Phase::Migration:
            rules = rule_set->getMigrationRules();
            break;
        case Phase::Domain:
            rules = rule_set->getDomainRules();
            break;
        case Phase::Encoding:
            rules = rule_set->getEncodingRules();
            break;
    }

    if (!rules.has_value()) {
        return false;
    }

    for (const auto &rule : rules.value()) {
        if (!rule.getMode().has_value()) {
            continue;
        }

        Mode rule_mode = rule.getMode().value();

        switch (mode) {
            case Mode::Upgrade:
            case Mode::Downgrade:
                if (rule_mode == mode || rule_mode == Mode::UpDown) {
                    return true;
                }
                break;
            case Mode::Write:
            case Mode::Read:
                if (rule_mode == mode || rule_mode == Mode::WriteRead) {
                    return true;
                }
                break;
            default:
                if (rule_mode == mode) {
                    return true;
                }
                break;
        }
    }

    return false;
}

// BaseSerializer implementation

BaseSerializer::BaseSerializer(Serde serde, const SerializerConfig &config)
    : serde_(std::move(serde)), config_(config) {}

// BaseDeserializer implementation

BaseDeserializer::BaseDeserializer(Serde serde,
                                   const DeserializerConfig &config)
    : serde_(std::move(serde)), config_(config) {}

Schema BaseDeserializer::getWriterSchema(
    const SchemaId &schema_id, std::optional<std::string> subject,
    std::optional<std::string> format) const {
    if (schema_id.getId().has_value()) {
        return serde_.getClient()->getBySubjectAndId(
            subject.value_or(""), schema_id.getId().value(), format);
    } else if (schema_id.getGuid().has_value()) {
        return serde_.getClient()->getByGuid(schema_id.getGuid().value(),
                                             format);
    } else {
        throw SerdeError("Schema ID or GUID are not set");
    }
}

// Note: ErrorAction and NoneAction implementations are in RuleRegistry.cpp

}  // namespace schemaregistry::serdes