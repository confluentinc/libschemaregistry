/**
 * CreateDekRequest
 * Create dek request model
 */

#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "Dek.h"

namespace schemaregistry::rest::model {

/**
 * CreateDekRequest class
 */
class CreateDekRequest {
  public:
    CreateDekRequest();
    CreateDekRequest(const std::string &subject,
                     const std::optional<int32_t> &version,
                     const std::optional<Algorithm> &algorithm,
                     const std::optional<std::string> &encryptedKeyMaterial);

    virtual ~CreateDekRequest() = default;

    bool operator==(const CreateDekRequest &rhs) const;
    bool operator!=(const CreateDekRequest &rhs) const;

    // Getters
    std::string getSubject() const;
    std::optional<int32_t> getVersion() const;
    std::optional<Algorithm> getAlgorithm() const;
    std::optional<std::string> getEncryptedKeyMaterial() const;

    // Setters
    void setSubject(const std::string &subject);
    void setVersion(const std::optional<int32_t> &version);
    void setAlgorithm(const std::optional<Algorithm> &algorithm);
    void setEncryptedKeyMaterial(
        const std::optional<std::string> &encryptedKeyMaterial);

    friend void to_json(nlohmann::json &j, const CreateDekRequest &o);
    friend void from_json(const nlohmann::json &j, CreateDekRequest &o);

  private:
    std::string subject_;
    std::optional<int32_t> version_;
    std::optional<Algorithm> algorithm_;
    std::optional<std::string> encryptedKeyMaterial_;
};

}  // namespace schemaregistry::rest::model