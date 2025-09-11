#pragma once

#include <any>
#include <avro/Compiler.hh>
#include <avro/Decoder.hh>
#include <avro/Encoder.hh>
#include <avro/Exception.hh>
#include <avro/Generic.hh>
#include <avro/Specific.hh>
#include <avro/ValidSchema.hh>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <string>

#include "schemaregistry/rest/SchemaRegistryClient.h"
#include "schemaregistry/serdes/SerdeError.h"
#include "schemaregistry/serdes/SerdeTypes.h"

namespace schemaregistry::serdes::avro {

/**
 * Shared cache for parsed Avro schemas
 * Thread-safe storage of compiled schemas
 */
class AvroSerde {
  public:
    AvroSerde() = default;
    ~AvroSerde() = default;

    /**
     * Get cached parsed schema or parse and cache it
     * @param schema Schema to parse
     * @param client Client for resolving references
     * @return Tuple of main schema and named schemas
     */
    std::pair<::avro::ValidSchema, std::vector<::avro::ValidSchema>>
    getParsedSchema(
        const schemaregistry::rest::model::Schema &schema,
        std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client);

    /**
     * Clear all cached schemas
     */
    void clear();

  private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::pair<::avro::ValidSchema,
                                              std::vector<::avro::ValidSchema>>>
        parsed_schemas_;

    /**
     * Resolve schema references recursively
     * @param schema Schema to resolve references for
     * @param client Client for fetching referenced schemas
     * @param schemas Output vector for resolved schema strings
     * @param visited Set of already visited schemas to prevent cycles
     */
    void resolveNamedSchema(
        const schemaregistry::rest::model::Schema &schema,
        std::shared_ptr<schemaregistry::rest::ISchemaRegistryClient> client,
        std::vector<std::string> &schemas,
        std::unordered_set<std::string> &visited);
};

/**
 * Avro-specific serialization errors
 */
class AvroError : public SerdeError {
  public:
    explicit AvroError(const std::string &message)
        : SerdeError("Avro error: " + message) {}

    explicit AvroError(const ::avro::Exception &avro_ex)
        : SerdeError("Avro error: " + std::string(avro_ex.what())) {}
};

/**
 * Avro implementation of SerdeValue
 */
class AvroValue : public SerdeValue {
  private:
    ::avro::GenericDatum value_;

  public:
    explicit AvroValue(const ::avro::GenericDatum &value) : value_(value) {}
    explicit AvroValue(::avro::GenericDatum &&value)
        : value_(std::move(value)) {}

    // SerdeValue interface implementation
    const void *getRawValue() const override { return &value_; }
    void *getMutableRawValue() override { return &value_; }
    SerdeFormat getFormat() const override { return SerdeFormat::Avro; }
    const std::type_info &getType() const override {
        return typeid(::avro::GenericDatum);
    }

    std::unique_ptr<SerdeValue> clone() const override {
        return std::make_unique<AvroValue>(value_);
    }

    void moveFrom(SerdeValue &&other) override {
        if (other.getFormat() == SerdeFormat::Avro) {
            value_ = std::move(*static_cast<::avro::GenericDatum *>(
                other.getMutableRawValue()));
        }
    }

    // Value extraction methods
    bool asBool() const override;
    std::string asString() const override;
    std::vector<uint8_t> asBytes() const override;
    nlohmann::json asJson() const override;
};

// Helper functions for creating Avro SerdeValue instances
inline std::unique_ptr<SerdeValue> makeAvroValue(
    const ::avro::GenericDatum &value) {
    return std::make_unique<AvroValue>(value);
}

inline std::unique_ptr<SerdeValue> makeAvroValue(::avro::GenericDatum &&value) {
    return std::make_unique<AvroValue>(std::move(value));
}

// Utility functions for Avro value and schema extraction
::avro::GenericDatum asAvro(const SerdeValue &value);

}  // namespace schemaregistry::serdes::avro
