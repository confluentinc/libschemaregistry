/**
 * Kek
 * Key encryption key model
 */

#include "schemaregistry/rest/model/Kek.h"

namespace schemaregistry::rest::model {

Kek::Kek() : shared_(false), ts_(0) {}

Kek::Kek(
    const std::string &name, const std::string &kmsType,
    const std::string &kmsKeyId,
    const std::optional<std::unordered_map<std::string, std::string>> &kmsProps,
    const std::optional<std::string> &doc, bool shared, int64_t ts,
    const std::optional<bool> &deleted)
    : name_(name),
      kmsType_(kmsType),
      kmsKeyId_(kmsKeyId),
      kmsProps_(kmsProps),
      doc_(doc),
      shared_(shared),
      ts_(ts),
      deleted_(deleted) {}

bool Kek::operator==(const Kek &rhs) const {
    return name_ == rhs.name_ && kmsType_ == rhs.kmsType_ &&
           kmsKeyId_ == rhs.kmsKeyId_ && kmsProps_ == rhs.kmsProps_ &&
           doc_ == rhs.doc_ && shared_ == rhs.shared_ && ts_ == rhs.ts_ &&
           deleted_ == rhs.deleted_;
}

bool Kek::operator!=(const Kek &rhs) const { return !(*this == rhs); }

// Getters
std::string Kek::getName() const { return name_; }
std::string Kek::getKmsType() const { return kmsType_; }
std::string Kek::getKmsKeyId() const { return kmsKeyId_; }
std::optional<std::unordered_map<std::string, std::string>> Kek::getKmsProps()
    const {
    return kmsProps_;
}
std::optional<std::string> Kek::getDoc() const { return doc_; }
bool Kek::getShared() const { return shared_; }
int64_t Kek::getTs() const { return ts_; }
std::optional<bool> Kek::getDeleted() const { return deleted_; }

// Setters
void Kek::setName(const std::string &name) { name_ = name; }
void Kek::setKmsType(const std::string &kmsType) { kmsType_ = kmsType; }
void Kek::setKmsKeyId(const std::string &kmsKeyId) { kmsKeyId_ = kmsKeyId; }
void Kek::setKmsProps(
    const std::optional<std::unordered_map<std::string, std::string>>
        &kmsProps) {
    kmsProps_ = kmsProps;
}
void Kek::setDoc(const std::optional<std::string> &doc) { doc_ = doc; }
void Kek::setShared(bool shared) { shared_ = shared; }
void Kek::setTs(int64_t ts) { ts_ = ts; }
void Kek::setDeleted(const std::optional<bool> &deleted) { deleted_ = deleted; }

// JSON serialization
void to_json(nlohmann::json &j, const Kek &o) {
    j = nlohmann::json{{"name", o.name_},
                       {"kmsType", o.kmsType_},
                       {"kmsKeyId", o.kmsKeyId_},
                       {"shared", o.shared_},
                       {"ts", o.ts_}};

    if (o.kmsProps_.has_value()) {
        j["kmsProps"] = o.kmsProps_.value();
    }

    if (o.doc_.has_value()) {
        j["doc"] = o.doc_.value();
    }

    if (o.deleted_.has_value()) {
        j["deleted"] = o.deleted_.value();
    }
}

void from_json(const nlohmann::json &j, Kek &o) {
    j.at("name").get_to(o.name_);
    j.at("kmsType").get_to(o.kmsType_);
    j.at("kmsKeyId").get_to(o.kmsKeyId_);
    j.at("shared").get_to(o.shared_);
    j.at("ts").get_to(o.ts_);

    if (j.contains("kmsProps") && !j["kmsProps"].is_null()) {
        o.kmsProps_ =
            j["kmsProps"].get<std::unordered_map<std::string, std::string>>();
    }

    if (j.contains("doc") && !j["doc"].is_null()) {
        o.doc_ = j["doc"].get<std::string>();
    }

    if (j.contains("deleted") && !j["deleted"].is_null()) {
        o.deleted_ = j["deleted"].get<bool>();
    }
}

}  // namespace schemaregistry::rest::model