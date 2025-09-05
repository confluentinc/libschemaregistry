/**
 * CreateKekRequest
 * Create kek request model
 */

#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>

namespace schemaregistry::rest::model {

/**
 * CreateKekRequest class
 */
class CreateKekRequest {
  public:
    CreateKekRequest();
    CreateKekRequest(
        const std::string &name, const std::string &kmsType,
        const std::string &kmsKeyId,
        const std::optional<std::unordered_map<std::string, std::string>>
            &kmsProps,
        const std::optional<std::string> &doc, bool shared);

    virtual ~CreateKekRequest() = default;

    bool operator==(const CreateKekRequest &rhs) const;
    bool operator!=(const CreateKekRequest &rhs) const;

    // Getters
    std::string getName() const;
    std::string getKmsType() const;
    std::string getKmsKeyId() const;
    std::optional<std::unordered_map<std::string, std::string>> getKmsProps()
        const;
    std::optional<std::string> getDoc() const;
    bool getShared() const;

    // Setters
    void setName(const std::string &name);
    void setKmsType(const std::string &kmsType);
    void setKmsKeyId(const std::string &kmsKeyId);
    void setKmsProps(
        const std::optional<std::unordered_map<std::string, std::string>>
            &kmsProps);
    void setDoc(const std::optional<std::string> &doc);
    void setShared(bool shared);

    friend void to_json(nlohmann::json &j, const CreateKekRequest &o);
    friend void from_json(const nlohmann::json &j, CreateKekRequest &o);

  private:
    std::string name_;
    std::string kmsType_;
    std::string kmsKeyId_;
    std::optional<std::unordered_map<std::string, std::string>> kmsProps_;
    std::optional<std::string> doc_;
    bool shared_;
};

}  // namespace schemaregistry::rest::model