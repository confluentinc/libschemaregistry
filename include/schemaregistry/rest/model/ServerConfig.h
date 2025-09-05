#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "Metadata.h"
#include "RuleSet.h"

namespace schemaregistry::rest::model {

enum class CompatibilityLevel {
    Backward,
    BackwardTransitive,
    Forward,
    ForwardTransitive,
    Full,
    FullTransitive,
    None,
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    CompatibilityLevel,
    {{CompatibilityLevel::Backward, "BACKWARD"},
     {CompatibilityLevel::BackwardTransitive, "BACKWARD_TRANSITIVE"},
     {CompatibilityLevel::Forward, "FORWARD"},
     {CompatibilityLevel::ForwardTransitive, "FORWARD_TRANSITIVE"},
     {CompatibilityLevel::Full, "FULL"},
     {CompatibilityLevel::FullTransitive, "FULL_TRANSITIVE"},
     {CompatibilityLevel::None, "NONE"}})

/// <summary>
/// Config
/// </summary>
class ServerConfig {
  public:
    ServerConfig();
    virtual ~ServerConfig() = default;

    bool operator==(const ServerConfig &rhs) const;
    bool operator!=(const ServerConfig &rhs) const;

    /////////////////////////////////////////////
    /// Config members

    /// <summary>
    /// Compatibility
    /// </summary>
    std::optional<CompatibilityLevel> getCompatibility() const;
    void setCompatibility(const std::optional<CompatibilityLevel> &value);
    /// <summary>
    /// Compatibility Level
    /// </summary>
    std::optional<CompatibilityLevel> getCompatibilityLevel() const;
    void setCompatibilityLevel(const std::optional<CompatibilityLevel> &value);
    /// <summary>
    ///
    /// </summary>
    std::optional<std::string> getAlias() const;
    void setAlias(const std::optional<std::string> &value);
    /// <summary>
    ///
    /// </summary>
    std::optional<bool> isNormalize() const;
    void setNormalize(const std::optional<bool> &value);
    /// <summary>
    ///
    /// </summary>
    std::optional<bool> isValidateFields() const;
    void setValidateFields(const std::optional<bool> &value);
    /// <summary>
    ///
    /// </summary>
    std::optional<bool> isValidateRules() const;
    void setValidateRules(const std::optional<bool> &value);
    /// <summary>
    ///
    /// </summary>
    std::optional<std::string> getCompatibilityGroup() const;
    void setCompatibilityGroup(const std::optional<std::string> &value);
    /// <summary>
    ///
    /// </summary>
    std::optional<schemaregistry::rest::model::Metadata> getDefaultMetadata()
        const;
    void setDefaultMetadata(
        const std::optional<schemaregistry::rest::model::Metadata> &value);
    /// <summary>
    ///
    /// </summary>
    std::optional<schemaregistry::rest::model::Metadata> getOverrideMetadata()
        const;
    void setOverrideMetadata(
        const std::optional<schemaregistry::rest::model::Metadata> &value);
    /// <summary>
    ///
    /// </summary>
    std::optional<schemaregistry::rest::model::RuleSet> getDefaultRuleSet()
        const;
    void setDefaultRuleSet(
        const std::optional<schemaregistry::rest::model::RuleSet> &value);
    /// <summary>
    ///
    /// </summary>
    std::optional<schemaregistry::rest::model::RuleSet> getOverrideRuleSet()
        const;
    void setOverrideRuleSet(
        const std::optional<schemaregistry::rest::model::RuleSet> &value);

    friend void to_json(nlohmann::json &j, const ServerConfig &o);
    friend void from_json(const nlohmann::json &j, ServerConfig &o);

  protected:
    std::optional<CompatibilityLevel> compatibility_;
    std::optional<CompatibilityLevel> compatibilityLevel_;
    std::optional<std::string> alias_;
    std::optional<bool> normalize_;
    std::optional<bool> validateFields_;
    std::optional<bool> validateRules_;
    std::optional<std::string> compatibilityGroup_;
    std::optional<schemaregistry::rest::model::Metadata> defaultMetadata_;
    std::optional<schemaregistry::rest::model::Metadata> overrideMetadata_;
    std::optional<schemaregistry::rest::model::RuleSet> defaultRuleSet_;
    std::optional<schemaregistry::rest::model::RuleSet> overrideRuleSet_;
};

}  // namespace schemaregistry::rest::model
