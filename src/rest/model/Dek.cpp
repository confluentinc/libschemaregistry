/**
 * Dek
 * Data encryption key model
 */

#include "schemaregistry/rest/model/Dek.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

#include "absl/strings/escaping.h"

namespace schemaregistry::rest::model {

// Base64 encoding/decoding utilities using absl
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

Dek::Dek() : version_(0), algorithm_(Algorithm::Aes256Gcm), ts_(0) {}

Dek::Dek(const std::string &kekName, const std::string &subject,
         int32_t version, Algorithm algorithm,
         const std::optional<std::string> &encryptedKeyMaterial,
         const std::optional<std::string> &keyMaterial, int64_t ts,
         const std::optional<bool> &deleted)
    : kekName_(kekName),
      subject_(subject),
      version_(version),
      algorithm_(algorithm),
      encryptedKeyMaterial_(encryptedKeyMaterial),
      keyMaterial_(keyMaterial),
      ts_(ts),
      deleted_(deleted) {}

bool Dek::operator==(const Dek &rhs) const {
    return kekName_ == rhs.kekName_ && subject_ == rhs.subject_ &&
           version_ == rhs.version_ && algorithm_ == rhs.algorithm_ &&
           encryptedKeyMaterial_ == rhs.encryptedKeyMaterial_ &&
           keyMaterial_ == rhs.keyMaterial_ && ts_ == rhs.ts_ &&
           deleted_ == rhs.deleted_;
}

bool Dek::operator!=(const Dek &rhs) const { return !(*this == rhs); }

// Getters
std::string Dek::getKekName() const { return kekName_; }
std::string Dek::getSubject() const { return subject_; }
int32_t Dek::getVersion() const { return version_; }
Algorithm Dek::getAlgorithm() const { return algorithm_; }
std::optional<std::string> Dek::getEncryptedKeyMaterial() const {
    return encryptedKeyMaterial_;
}
std::optional<std::vector<uint8_t>> Dek::getEncryptedKeyMaterialBytes() const {
    if (!encryptedKeyMaterial_.has_value()) {
        return std::nullopt;
    }

    if (!encryptedKeyMaterialBytes_.has_value()) {
        encryptedKeyMaterialBytes_ =
            base64_decode(encryptedKeyMaterial_.value());
    }

    return encryptedKeyMaterialBytes_;
}
std::optional<std::string> Dek::getKeyMaterial() const { return keyMaterial_; }
std::optional<std::vector<uint8_t>> Dek::getKeyMaterialBytes() const {
    if (!keyMaterial_.has_value()) {
        return std::nullopt;
    }

    if (!keyMaterialBytes_.has_value()) {
        keyMaterialBytes_ = base64_decode(keyMaterial_.value());
    }

    return keyMaterialBytes_;
}
int64_t Dek::getTs() const { return ts_; }
std::optional<bool> Dek::getDeleted() const { return deleted_; }

// Setters
void Dek::setKekName(const std::string &kekName) { kekName_ = kekName; }
void Dek::setSubject(const std::string &subject) { subject_ = subject; }
void Dek::setVersion(int32_t version) { version_ = version; }
void Dek::setAlgorithm(Algorithm algorithm) { algorithm_ = algorithm; }
void Dek::setEncryptedKeyMaterial(
    const std::optional<std::string> &encryptedKeyMaterial) {
    encryptedKeyMaterial_ = encryptedKeyMaterial;
}
void Dek::setEncryptedKeyMaterialBytes(
    const std::optional<std::vector<uint8_t>> &encryptedKeyMaterialBytes) {
    encryptedKeyMaterialBytes_ = encryptedKeyMaterialBytes;
}
void Dek::setKeyMaterial(const std::optional<std::string> &keyMaterial) {
    keyMaterial_ = keyMaterial;
}
void Dek::setKeyMaterial(
    const std::optional<std::vector<uint8_t>> &keyMaterial) {
    keyMaterial_ = base64_encode(keyMaterial.value());
}
void Dek::setKeyMaterialBytes(
    const std::optional<std::vector<uint8_t>> &keyMaterialBytes) {
    keyMaterialBytes_ = keyMaterialBytes;
}
void Dek::setTs(int64_t ts) { ts_ = ts; }
void Dek::setDeleted(const std::optional<bool> &deleted) { deleted_ = deleted; }

// Utility methods
void Dek::populateKeyMaterialBytes() {
    getEncryptedKeyMaterialBytes();
    getKeyMaterialBytes();
}

// JSON serialization
void to_json(nlohmann::json &j, const Dek &o) {
    j = nlohmann::json{{"kekName", o.kekName_},
                       {"subject", o.subject_},
                       {"version", o.version_},
                       {"algorithm", o.algorithm_},
                       {"ts", o.ts_}};

    if (o.encryptedKeyMaterial_.has_value()) {
        j["encryptedKeyMaterial"] = o.encryptedKeyMaterial_.value();
    }

    if (o.keyMaterial_.has_value()) {
        j["keyMaterial"] = o.keyMaterial_.value();
    }

    if (o.deleted_.has_value()) {
        j["deleted"] = o.deleted_.value();
    }
}

void from_json(const nlohmann::json &j, Dek &o) {
    j.at("kekName").get_to(o.kekName_);
    j.at("subject").get_to(o.subject_);
    j.at("version").get_to(o.version_);
    j.at("algorithm").get_to(o.algorithm_);
    j.at("ts").get_to(o.ts_);

    if (j.contains("encryptedKeyMaterial") &&
        !j["encryptedKeyMaterial"].is_null()) {
        o.encryptedKeyMaterial_ = j["encryptedKeyMaterial"].get<std::string>();
    }

    if (j.contains("keyMaterial") && !j["keyMaterial"].is_null()) {
        o.keyMaterial_ = j["keyMaterial"].get<std::string>();
    }

    if (j.contains("deleted") && !j["deleted"].is_null()) {
        o.deleted_ = j["deleted"].get<bool>();
    }
}

}  // namespace schemaregistry::rest::model