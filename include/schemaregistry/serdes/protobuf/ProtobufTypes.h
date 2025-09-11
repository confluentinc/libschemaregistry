#pragma once

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>

#include <any>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <variant>
#include <vector>

#include "schemaregistry/rest/ISchemaRegistryClient.h"
#include "schemaregistry/serdes/SerdeError.h"
#include "schemaregistry/serdes/SerdeTypes.h"

namespace schemaregistry::serdes::protobuf {

/**
 * Protobuf schema caching and parsing class
 * Based on ProtobufSerde struct from protobuf.rs (converted to synchronous)
 */
class ProtobufSerde {
  public:
    ProtobufSerde();
    ~ProtobufSerde() = default;

    // Schema parsing and caching
    std::pair<const google::protobuf::FileDescriptor *,
              const google::protobuf::DescriptorPool *>
    getParsedSchema(
        const schemaregistry::rest::model::Schema &schema,
        std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client);

    // Clear cache
    void clear();

  private:
    // Cache for parsed schemas: Schema -> (FileDescriptor*, DescriptorPool)
    std::unordered_map<
        std::string,
        std::pair<const google::protobuf::FileDescriptor *,
                  std::unique_ptr<google::protobuf::DescriptorPool>>>
        parsed_schemas_cache_;

    mutable std::mutex cache_mutex_;

    // Helper methods
    void resolveNamedSchema(
        const schemaregistry::rest::model::Schema &schema,
        std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
        google::protobuf::DescriptorPool *pool,
        std::unordered_set<std::string> &visited);

    // Initialize descriptor pool with well-known types
    void initPool(google::protobuf::DescriptorPool *pool);

    // Add file descriptor to pool
    void addFileToPool(google::protobuf::DescriptorPool *pool,
                       const google::protobuf::FileDescriptor *file_descriptor);
};

// C++ variant representing protobuf values - type alias for map keys
using MapKey =
    std::variant<std::string, int32_t, int64_t, uint32_t, uint64_t, bool>;

/**
 * C++ variant-based structure representing protobuf values
 * Ported from Rust Value enum to use std::variant
 */
struct ProtobufVariant {
    enum class ValueType {
        Bool,
        I32,
        I64,
        U32,
        U64,
        F32,
        F64,
        String,
        Bytes,
        EnumNumber,
        Message,
        List,
        Map
    };

    using ValueVariant =
        std::variant<bool,                                        // Bool
                     int32_t,                                     // I32
                     int64_t,                                     // I64
                     uint32_t,                                    // U32
                     uint64_t,                                    // U64
                     float,                                       // F32
                     double,                                      // F64
                     std::string,                                 // String
                     std::vector<uint8_t>,                        // Bytes
                     std::unique_ptr<google::protobuf::Message>,  // Message
                     std::vector<ProtobufVariant>,                // List
                     std::map<MapKey, ProtobufVariant>            // Map
                     >;

    ValueVariant value;
    ValueType type;

    // Constructors for each type
    ProtobufVariant(bool v) : value(v), type(ValueType::Bool) {}
    ProtobufVariant(int32_t v, ValueType value_type = ValueType::I32)
        : value(v), type(value_type) {}
    ProtobufVariant(int64_t v) : value(v), type(ValueType::I64) {}
    ProtobufVariant(uint32_t v) : value(v), type(ValueType::U32) {}
    ProtobufVariant(uint64_t v) : value(v), type(ValueType::U64) {}
    ProtobufVariant(float v) : value(v), type(ValueType::F32) {}
    ProtobufVariant(double v) : value(v), type(ValueType::F64) {}
    ProtobufVariant(const std::string &v) : value(v), type(ValueType::String) {}
    ProtobufVariant(const std::vector<uint8_t> &v)
        : value(v), type(ValueType::Bytes) {}
    ProtobufVariant(std::unique_ptr<google::protobuf::Message> v)
        : value(std::move(v)), type(ValueType::Message) {}
    ProtobufVariant(const std::vector<ProtobufVariant> &v)
        : value(v), type(ValueType::List) {}
    ProtobufVariant(const std::map<MapKey, ProtobufVariant> &v)
        : value(v), type(ValueType::Map) {}

    // Special constructor for enum values
    static ProtobufVariant createEnum(int32_t value) {
        return ProtobufVariant(value, ValueType::EnumNumber);
    }

    // Copy constructor and assignment operator
    ProtobufVariant(const ProtobufVariant &other);
    ProtobufVariant &operator=(const ProtobufVariant &other);

    // Move constructor and assignment operator
    ProtobufVariant(ProtobufVariant &&other) = default;
    ProtobufVariant &operator=(ProtobufVariant &&other) = default;

    // Visitor helper methods
    template <typename T>
    bool is() const {
        return std::holds_alternative<T>(value);
    }

    template <typename T>
    const T &get() const {
        return std::get<T>(value);
    }

    template <typename T>
    T &get() {
        return std::get<T>(value);
    }
};

/**
 * Protobuf errors
 */
class ProtobufError : public SerdeError {
  public:
    explicit ProtobufError(const std::string &message)
        : SerdeError("Protobuf error: " + message) {}
};

/**
 * Protobuf reflection errors
 * Maps to SerdeError::ProtobufReflect variant
 */
class ProtobufReflectError : public SerdeError {
  public:
    explicit ProtobufReflectError(const std::string &message)
        : SerdeError("Protobuf reflect error: " + message) {}
};

/**
 * Protobuf implementation of SerdeValue
 */
class ProtobufValue : public SerdeValue {
  private:
    ProtobufVariant value_;

  public:
    explicit ProtobufValue(ProtobufVariant value);

    // SerdeValue interface implementation
    const void *getRawValue() const override;
    void *getMutableRawValue() override;
    SerdeFormat getFormat() const override;
    const std::type_info &getType() const override;
    std::unique_ptr<SerdeValue> clone() const override;
    void moveFrom(SerdeValue &&other) override;

    // Value extraction methods
    bool asBool() const override;
    std::string asString() const override;
    std::vector<uint8_t> asBytes() const override;
    nlohmann::json asJson() const override;

    // Direct access to ProtobufVariant
    const ProtobufVariant &getProtobufVariant() const;
    ProtobufVariant &getMutableProtobufVariant();
};

// Helper functions for creating Protobuf SerdeValue instances
inline std::unique_ptr<SerdeValue> makeProtobufValue(ProtobufVariant value) {
    return std::make_unique<ProtobufValue>(std::move(value));
}

// Utility functions for Protobuf value and schema extraction
ProtobufVariant &asProtobuf(const SerdeValue &value);

}  // namespace schemaregistry::serdes::protobuf