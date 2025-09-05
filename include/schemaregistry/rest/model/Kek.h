/**
 * Kek
 * Key encryption key model
 */

#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>

namespace schemaregistry::rest::model {

/**
 * Kek class
 */
class Kek {
  public:
    Kek();
    Kek(const std::string &name, const std::string &kmsType,
        const std::string &kmsKeyId,
        const std::optional<std::unordered_map<std::string, std::string>>
            &kmsProps,
        const std::optional<std::string> &doc, bool shared, int64_t ts,
        const std::optional<bool> &deleted);

    virtual ~Kek() = default;

    bool operator==(const Kek &rhs) const;
    bool operator!=(const Kek &rhs) const;

    // Getters
    std::string getName() const;
    std::string getKmsType() const;
    std::string getKmsKeyId() const;
    std::optional<std::unordered_map<std::string, std::string>> getKmsProps()
        const;
    std::optional<std::string> getDoc() const;
    bool getShared() const;
    int64_t getTs() const;
    std::optional<bool> getDeleted() const;

    // Setters
    void setName(const std::string &name);
    void setKmsType(const std::string &kmsType);
    void setKmsKeyId(const std::string &kmsKeyId);
    void setKmsProps(
        const std::optional<std::unordered_map<std::string, std::string>>
            &kmsProps);
    void setDoc(const std::optional<std::string> &doc);
    void setShared(bool shared);
    void setTs(int64_t ts);
    void setDeleted(const std::optional<bool> &deleted);

    friend void to_json(nlohmann::json &j, const Kek &o);
    friend void from_json(const nlohmann::json &j, Kek &o);

  private:
    std::string name_;
    std::string kmsType_;
    std::string kmsKeyId_;
    std::optional<std::unordered_map<std::string, std::string>> kmsProps_;
    std::optional<std::string> doc_;
    bool shared_;
    int64_t ts_;
    std::optional<bool> deleted_;
};

}  // namespace schemaregistry::rest::model