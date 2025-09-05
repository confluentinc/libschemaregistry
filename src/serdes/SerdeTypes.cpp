#include "schemaregistry/serdes/SerdeTypes.h"

#include <google/protobuf/message.h>

#include <avro/Compiler.hh>
#include <avro/ValidSchema.hh>
#include <sstream>

#include "absl/strings/escaping.h"
#include "schemaregistry/rest/model/RegisteredSchema.h"
#include "schemaregistry/rest/model/Schema.h"
#ifdef SCHEMAREGISTRY_USE_AVRO
#include "schemaregistry/serdes/avro/AvroTypes.h"
#endif
#include "schemaregistry/serdes/json/JsonTypes.h"
#ifdef SCHEMAREGISTRY_USE_PROTOBUF
#include "schemaregistry/serdes/protobuf/ProtobufTypes.h"
#endif

namespace schemaregistry::serdes {

// SchemaSelector static factory methods
SchemaSelector SchemaSelector::useSchemaId(int32_t id) {
    SchemaSelector data;
    data.type = SchemaSelectorType::SchemaId;
    data.schema_id = id;
    return data;
}

SchemaSelector SchemaSelector::useLatestVersion() {
    SchemaSelector data;
    data.type = SchemaSelectorType::LatestVersion;
    return data;
}

SchemaSelector SchemaSelector::useLatestWithMetadata(
    const std::unordered_map<std::string, std::string> &metadata) {
    SchemaSelector data;
    data.type = SchemaSelectorType::LatestWithMetadata;
    data.metadata = metadata;
    return data;
}

// Migration implementation
Migration::Migration(Mode mode, std::optional<RegisteredSchema> src,
                     std::optional<RegisteredSchema> tgt)
    : rule_mode(mode), source(src), target(tgt) {}

// FieldType to string conversion
std::string fieldTypeToString(FieldType type) {
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

// ParsedSchemaCache template implementation
template <typename T>
void ParsedSchemaCache<T>::set(const Schema &schema, const T &parsed_schema) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key = getSchemaKey(schema);
    cache_[key] = parsed_schema;
}

template <typename T>
std::optional<T> ParsedSchemaCache<T>::get(const Schema &schema) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key = getSchemaKey(schema);
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        return it->second;
    }
    return std::nullopt;
}

template <typename T>
void ParsedSchemaCache<T>::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
}

template <typename T>
std::string ParsedSchemaCache<T>::getSchemaKey(const Schema &schema) const {
    // Us the JSON representation of the schema to create a hash
    nlohmann::json j;
    to_json(j, schema);
    return j.dump();
}

// Explicit template instantiations for common types
template class ParsedSchemaCache<std::string>;
template class ParsedSchemaCache<int>;

// Base64 encoding/decoding utilities
namespace {
std::string base64_encode(const std::vector<uint8_t> &bytes) {
    std::string input(reinterpret_cast<const char *>(bytes.data()),
                      bytes.size());
    return absl::Base64Escape(input);
}

std::vector<uint8_t> base64_decode(const std::string &encoded_string) {
    std::string decoded;
    if (!absl::Base64Unescape(encoded_string, &decoded)) {
        // Return empty vector on decode failure
        return std::vector<uint8_t>();
    }
    return std::vector<uint8_t>(decoded.begin(), decoded.end());
}
}  // namespace

// SerdeValue static factory method implementations
std::unique_ptr<SerdeValue> SerdeValue::newString(SerdeFormat format,
                                                  const std::string &value) {
    switch (format) {
#ifdef SCHEMAREGISTRY_USE_AVRO
        case SerdeFormat::Avro: {
            ::avro::GenericDatum datum(value);
            return std::make_unique<avro::AvroValue>(datum);
        }
#endif
        case SerdeFormat::Json: {
            nlohmann::json json_value = value;
            return std::make_unique<json::JsonValue>(json_value);
        }
#ifdef SCHEMAREGISTRY_USE_PROTOBUF
        case SerdeFormat::Protobuf: {
            protobuf::ProtobufVariant variant(value);
            return protobuf::makeProtobufValue(std::move(variant));
        }
#endif
        default:
            throw SerdeError("Unsupported SerdeFormat");
    }
}

std::unique_ptr<SerdeValue> SerdeValue::newBytes(
    SerdeFormat format, const std::vector<uint8_t> &value) {
    switch (format) {
#ifdef SCHEMAREGISTRY_USE_AVRO
        case SerdeFormat::Avro: {
            ::avro::GenericDatum datum(value);
            return std::make_unique<avro::AvroValue>(datum);
        }
#endif
        case SerdeFormat::Json: {
            // For JSON, encode bytes as base64 string
            std::string base64_value = base64_encode(value);
            nlohmann::json json_value = base64_value;
            return std::make_unique<json::JsonValue>(json_value);
        }
#ifdef SCHEMAREGISTRY_USE_PROTOBUF
        case SerdeFormat::Protobuf: {
            protobuf::ProtobufVariant variant(value);
            return protobuf::makeProtobufValue(std::move(variant));
        }
#endif
        default:
            throw SerdeError("Unsupported SerdeFormat");
    }
}

std::unique_ptr<SerdeValue> SerdeValue::newJson(SerdeFormat format,
                                                const nlohmann::json &value) {
    switch (format) {
#ifdef SCHEMAREGISTRY_USE_AVRO
        case SerdeFormat::Avro: {
            // TODO
            throw SerdeError("TODO");
        }
#endif
        case SerdeFormat::Json: {
            return std::make_unique<json::JsonValue>(value);
        }
#ifdef SCHEMAREGISTRY_USE_PROTOBUF
        case SerdeFormat::Protobuf: {
            // TODO
            throw SerdeError("TODO");
        }
#endif
        default:
            throw SerdeError("Unsupported SerdeFormat");
    }
}

namespace type_utils {

std::string formatToString(SerdeFormat format) {
    switch (format) {
        case SerdeFormat::Avro:
            return "AVRO";
        case SerdeFormat::Json:
            return "JSON";
        case SerdeFormat::Protobuf:
            return "PROTOBUF";
        default:
            return "UNKNOWN";
    }
}

SerdeFormat stringToFormat(const std::string &format) {
    if (format == "AVRO" || format == "avro") {
        return SerdeFormat::Avro;
    } else if (format == "JSON" || format == "json") {
        return SerdeFormat::Json;
    } else if (format == "PROTOBUF" || format == "protobuf") {
        return SerdeFormat::Protobuf;
    }
    // Default to JSON if unknown
    return SerdeFormat::Json;
}

std::string typeToString(SerdeType type) {
    switch (type) {
        case SerdeType::Key:
            return "KEY";
        case SerdeType::Value:
            return "VALUE";
        default:
            return "UNKNOWN";
    }
}

std::string modeToString(Mode mode) {
    switch (mode) {
        case Mode::Read:
            return "READ";
        case Mode::Write:
            return "WRITE";
        case Mode::WriteRead:
            return "WRITEREAD";
        case Mode::Upgrade:
            return "UPGRADE";
        case Mode::Downgrade:
            return "DOWNGRADE";
        case Mode::UpDown:
            return "UPDOWN";
        default:
            return "UNKNOWN";
    }
}

std::string phaseToString(Phase phase) {
    switch (phase) {
        case Phase::Migration:
            return "MIGRATION";
        case Phase::Domain:
            return "DOMAIN";
        case Phase::Encoding:
            return "ENCODING";
        default:
            return "UNKNOWN";
    }
}

std::string kindToString(Kind kind) {
    switch (kind) {
        case Kind::Transform:
            return "TRANSFORM";
        case Kind::Condition:
            return "CONDITION";
        default:
            return "UNKNOWN";
    }
}

}  // namespace type_utils

}  // namespace schemaregistry::serdes