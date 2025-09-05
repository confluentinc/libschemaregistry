/**
 * CreateDekRequest
 * Create dek request model
 */

#include "schemaregistry/rest/model/CreateDekRequest.h"

namespace schemaregistry::rest::model {

CreateDekRequest::CreateDekRequest() {}

CreateDekRequest::CreateDekRequest(
    const std::string &subject, const std::optional<int32_t> &version,
    const std::optional<Algorithm> &algorithm,
    const std::optional<std::string> &encryptedKeyMaterial)
    : subject_(subject),
      version_(version),
      algorithm_(algorithm),
      encryptedKeyMaterial_(encryptedKeyMaterial) {}

bool CreateDekRequest::operator==(const CreateDekRequest &rhs) const {
    return subject_ == rhs.subject_ && version_ == rhs.version_ &&
           algorithm_ == rhs.algorithm_ &&
           encryptedKeyMaterial_ == rhs.encryptedKeyMaterial_;
}

bool CreateDekRequest::operator!=(const CreateDekRequest &rhs) const {
    return !(*this == rhs);
}

// Getters
std::string CreateDekRequest::getSubject() const { return subject_; }
std::optional<int32_t> CreateDekRequest::getVersion() const { return version_; }
std::optional<Algorithm> CreateDekRequest::getAlgorithm() const {
    return algorithm_;
}
std::optional<std::string> CreateDekRequest::getEncryptedKeyMaterial() const {
    return encryptedKeyMaterial_;
}

// Setters
void CreateDekRequest::setSubject(const std::string &subject) {
    subject_ = subject;
}
void CreateDekRequest::setVersion(const std::optional<int32_t> &version) {
    version_ = version;
}
void CreateDekRequest::setAlgorithm(const std::optional<Algorithm> &algorithm) {
    algorithm_ = algorithm;
}
void CreateDekRequest::setEncryptedKeyMaterial(
    const std::optional<std::string> &encryptedKeyMaterial) {
    encryptedKeyMaterial_ = encryptedKeyMaterial;
}

// JSON serialization
void to_json(nlohmann::json &j, const CreateDekRequest &o) {
    j = nlohmann::json{{"subject", o.subject_}};

    if (o.version_.has_value()) {
        j["version"] = o.version_.value();
    }

    if (o.algorithm_.has_value()) {
        j["algorithm"] = o.algorithm_.value();
    }

    if (o.encryptedKeyMaterial_.has_value()) {
        j["encryptedKeyMaterial"] = o.encryptedKeyMaterial_.value();
    }
}

void from_json(const nlohmann::json &j, CreateDekRequest &o) {
    j.at("subject").get_to(o.subject_);

    if (j.contains("version") && !j["version"].is_null()) {
        o.version_ = j["version"].get<int32_t>();
    }

    if (j.contains("algorithm") && !j["algorithm"].is_null()) {
        o.algorithm_ = j["algorithm"].get<Algorithm>();
    }

    if (j.contains("encryptedKeyMaterial") &&
        !j["encryptedKeyMaterial"].is_null()) {
        o.encryptedKeyMaterial_ = j["encryptedKeyMaterial"].get<std::string>();
    }
}

}  // namespace schemaregistry::rest::model