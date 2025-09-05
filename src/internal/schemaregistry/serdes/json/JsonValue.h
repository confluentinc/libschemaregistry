#pragma once

#include <memory>
#include <nlohmann/json.hpp>

#include "schemaregistry/serdes/SerdeTypes.h"

namespace schemaregistry::serdes::json {

/**
 * JSON implementation of SerdeValue
 */
class JsonValue : public SerdeValue {
  private:
    nlohmann::json value_;

  public:
    explicit JsonValue(const nlohmann::json &value) : value_(value) {}
    explicit JsonValue(nlohmann::json &&value) : value_(std::move(value)) {}

    // SerdeValue interface implementation
    const void *getRawValue() const override { return &value_; }
    void *getMutableRawValue() override { return &value_; }
    SerdeFormat getFormat() const override { return SerdeFormat::Json; }
    const std::type_info &getType() const override {
        return typeid(nlohmann::json);
    }

    std::unique_ptr<SerdeValue> clone() const override {
        return std::make_unique<JsonValue>(value_);
    }

    void moveFrom(SerdeValue &&other) override {
        if (other.getFormat() == SerdeFormat::Json) {
            value_ = std::move(
                *static_cast<nlohmann::json *>(other.getMutableRawValue()));
        }
    }

    // Value extraction methods
    bool asBool() const override;
    std::string asString() const override;
    std::vector<uint8_t> asBytes() const override;
    nlohmann::json asJson() const override;
};

nlohmann::json asJson(const SerdeValue &value);

std::unique_ptr<SerdeValue> makeJsonValue(const nlohmann::json &value);

std::unique_ptr<SerdeValue> makeJsonValue(nlohmann::json &&value);

}  // namespace schemaregistry::serdes::json
