/**
 * CreateKekRequest
 * Create kek request model
 */

#include "schemaregistry/rest/model/CreateKekRequest.h"

namespace schemaregistry::rest::model {

CreateKekRequest::CreateKekRequest() : shared_(false) {}

CreateKekRequest::CreateKekRequest(
    const std::string &name, const std::string &kmsType,
    const std::string &kmsKeyId,
    const std::optional<std::unordered_map<std::string, std::string>> &kmsProps,
    const std::optional<std::string> &doc, bool shared)
    : name_(name),
      kmsType_(kmsType),
      kmsKeyId_(kmsKeyId),
      kmsProps_(kmsProps),
      doc_(doc),
      shared_(shared) {}

bool CreateKekRequest::operator==(const CreateKekRequest &rhs) const {
    return name_ == rhs.name_ && kmsType_ == rhs.kmsType_ &&
           kmsKeyId_ == rhs.kmsKeyId_ && kmsProps_ == rhs.kmsProps_ &&
           doc_ == rhs.doc_ && shared_ == rhs.shared_;
}

bool CreateKekRequest::operator!=(const CreateKekRequest &rhs) const {
    return !(*this == rhs);
}

// Getters
std::string CreateKekRequest::getName() const { return name_; }
std::string CreateKekRequest::getKmsType() const { return kmsType_; }
std::string CreateKekRequest::getKmsKeyId() const { return kmsKeyId_; }
std::optional<std::unordered_map<std::string, std::string>>
CreateKekRequest::getKmsProps() const {
    return kmsProps_;
}
std::optional<std::string> CreateKekRequest::getDoc() const { return doc_; }
bool CreateKekRequest::getShared() const { return shared_; }

// Setters
void CreateKekRequest::setName(const std::string &name) { name_ = name; }
void CreateKekRequest::setKmsType(const std::string &kmsType) {
    kmsType_ = kmsType;
}
void CreateKekRequest::setKmsKeyId(const std::string &kmsKeyId) {
    kmsKeyId_ = kmsKeyId;
}
void CreateKekRequest::setKmsProps(
    const std::optional<std::unordered_map<std::string, std::string>>
        &kmsProps) {
    kmsProps_ = kmsProps;
}
void CreateKekRequest::setDoc(const std::optional<std::string> &doc) {
    doc_ = doc;
}
void CreateKekRequest::setShared(bool shared) { shared_ = shared; }

// JSON serialization
void to_json(nlohmann::json &j, const CreateKekRequest &o) {
    j = nlohmann::json{{"name", o.name_},
                       {"kmsType", o.kmsType_},
                       {"kmsKeyId", o.kmsKeyId_},
                       {"shared", o.shared_}};

    if (o.kmsProps_.has_value()) {
        j["kmsProps"] = o.kmsProps_.value();
    }

    if (o.doc_.has_value()) {
        j["doc"] = o.doc_.value();
    }
}

void from_json(const nlohmann::json &j, CreateKekRequest &o) {
    j.at("name").get_to(o.name_);
    j.at("kmsType").get_to(o.kmsType_);
    j.at("kmsKeyId").get_to(o.kmsKeyId_);
    j.at("shared").get_to(o.shared_);

    if (j.contains("kmsProps") && !j["kmsProps"].is_null()) {
        o.kmsProps_ =
            j["kmsProps"].get<std::unordered_map<std::string, std::string>>();
    }

    if (j.contains("doc") && !j["doc"].is_null()) {
        o.doc_ = j["doc"].get<std::string>();
    }
}

}  // namespace schemaregistry::rest::model