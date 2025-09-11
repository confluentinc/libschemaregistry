/**
 * Dek
 * Data encryption key model
 */

#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace schemaregistry::rest::model {

/**
 * Algorithm of the dek
 */
enum class Algorithm { Aes128Gcm, Aes256Gcm, Aes256Siv };

NLOHMANN_JSON_SERIALIZE_ENUM(Algorithm, {{Algorithm::Aes128Gcm, "AES128_GCM"},
                                         {Algorithm::Aes256Gcm, "AES256_GCM"},
                                         {Algorithm::Aes256Siv, "AES256_SIV"}})

/**
 * Dek class
 */
class Dek {
  public:
    Dek();
    Dek(const std::string &kekName, const std::string &subject, int32_t version,
        Algorithm algorithm,
        const std::optional<std::string> &encryptedKeyMaterial,
        const std::optional<std::string> &keyMaterial, int64_t ts,
        const std::optional<bool> &deleted);

    virtual ~Dek() = default;

    bool operator==(const Dek &rhs) const;
    bool operator!=(const Dek &rhs) const;

    // Getters
    std::string getKekName() const;
    std::string getSubject() const;
    int32_t getVersion() const;
    Algorithm getAlgorithm() const;
    std::optional<std::string> getEncryptedKeyMaterial() const;
    std::optional<std::vector<uint8_t>> getEncryptedKeyMaterialBytes() const;
    std::optional<std::string> getKeyMaterial() const;
    std::optional<std::vector<uint8_t>> getKeyMaterialBytes() const;
    int64_t getTs() const;
    std::optional<bool> getDeleted() const;

    // Setters
    void setKekName(const std::string &kekName);
    void setSubject(const std::string &subject);
    void setVersion(int32_t version);
    void setAlgorithm(Algorithm algorithm);
    void setEncryptedKeyMaterial(
        const std::optional<std::string> &encryptedKeyMaterial);
    void setEncryptedKeyMaterialBytes(
        const std::optional<std::vector<uint8_t>> &encryptedKeyMaterialBytes);
    void setKeyMaterial(const std::optional<std::string> &keyMaterial);
    void setKeyMaterial(const std::optional<std::vector<uint8_t>> &keyMaterial);
    void setKeyMaterialBytes(
        const std::optional<std::vector<uint8_t>> &keyMaterialBytes);
    void setTs(int64_t ts);
    void setDeleted(const std::optional<bool> &deleted);

    // Utility methods
    void populateKeyMaterialBytes();

    friend void to_json(nlohmann::json &j, const Dek &o);
    friend void from_json(const nlohmann::json &j, Dek &o);
    friend void to_json(nlohmann::json &j, const Algorithm &o);
    friend void from_json(const nlohmann::json &j, Algorithm &o);

  private:
    std::string kekName_;
    std::string subject_;
    int32_t version_;
    Algorithm algorithm_;
    std::optional<std::string> encryptedKeyMaterial_;
    mutable std::optional<std::vector<uint8_t>> encryptedKeyMaterialBytes_;
    std::optional<std::string> keyMaterial_;
    mutable std::optional<std::vector<uint8_t>> keyMaterialBytes_;
    int64_t ts_;
    std::optional<bool> deleted_;
};

}  // namespace schemaregistry::rest::model