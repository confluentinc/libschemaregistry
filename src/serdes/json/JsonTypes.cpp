#include "schemaregistry/serdes/json/JsonTypes.h"

namespace schemaregistry::serdes::json {

// Utility functions for JSON value and schema extraction
jsoncons::ojson asOJson(const SerdeValue &value) {
    if (value.getFormat() != SerdeFormat::Json) {
        throw std::invalid_argument("SerdeValue is not JSON");
    }
    return jsoncons::ojson::parse(value.getValue<nlohmann::json>().dump());
}

std::unique_ptr<SerdeValue> makeJsonValue(const jsoncons::ojson &value) {
    return std::make_unique<JsonValue>(
        nlohmann::json::parse(value.to_string()));
}

std::unique_ptr<SerdeValue> makeJsonValue(jsoncons::ojson &&value) {
    return std::make_unique<JsonValue>(
        nlohmann::json::parse(value.to_string()));
}

}  // namespace schemaregistry::serdes::json