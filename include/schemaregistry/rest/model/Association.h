#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace schemaregistry::rest::model {

/**
 * LifecyclePolicy represents the lifecycle policy for an association.
 */
enum class LifecyclePolicy { Strong, Weak };

NLOHMANN_JSON_SERIALIZE_ENUM(LifecyclePolicy,
                             {{LifecyclePolicy::Strong, "STRONG"},
                              {LifecyclePolicy::Weak, "WEAK"}})

/**
 * Association represents an association between a subject and a resource.
 */
class Association {
  public:
    Association();
    virtual ~Association() = default;

    bool operator==(const Association &rhs) const;
    bool operator!=(const Association &rhs) const;

    // Getters
    std::optional<std::string> getSubject() const;
    std::optional<std::string> getGuid() const;
    std::optional<std::string> getResourceName() const;
    std::optional<std::string> getResourceNamespace() const;
    std::optional<std::string> getResourceId() const;
    std::optional<std::string> getResourceType() const;
    std::optional<std::string> getAssociationType() const;
    std::optional<LifecyclePolicy> getLifecycle() const;
    std::optional<bool> getFrozen() const;

    // Setters
    void setSubject(const std::optional<std::string> &value);
    void setGuid(const std::optional<std::string> &value);
    void setResourceName(const std::optional<std::string> &value);
    void setResourceNamespace(const std::optional<std::string> &value);
    void setResourceId(const std::optional<std::string> &value);
    void setResourceType(const std::optional<std::string> &value);
    void setAssociationType(const std::optional<std::string> &value);
    void setLifecycle(const std::optional<LifecyclePolicy> &value);
    void setFrozen(const std::optional<bool> &value);

    friend void to_json(nlohmann::json &j, const Association &o);
    friend void from_json(const nlohmann::json &j, Association &o);

  private:
    std::optional<std::string> subject_;
    std::optional<std::string> guid_;
    std::optional<std::string> resource_name_;
    std::optional<std::string> resource_namespace_;
    std::optional<std::string> resource_id_;
    std::optional<std::string> resource_type_;
    std::optional<std::string> association_type_;
    std::optional<LifecyclePolicy> lifecycle_;
    std::optional<bool> frozen_;
};

/**
 * AssociationCreateOrUpdateInfo represents an association to create/update.
 */
class AssociationCreateOrUpdateInfo {
  public:
    AssociationCreateOrUpdateInfo();
    virtual ~AssociationCreateOrUpdateInfo() = default;

    bool operator==(const AssociationCreateOrUpdateInfo &rhs) const;
    bool operator!=(const AssociationCreateOrUpdateInfo &rhs) const;

    // Getters
    std::optional<std::string> getSubject() const;
    std::optional<std::string> getAssociationType() const;
    std::optional<std::string> getLifecycle() const;
    std::optional<bool> getFrozen() const;

    // Setters
    void setSubject(const std::optional<std::string> &value);
    void setAssociationType(const std::optional<std::string> &value);
    void setLifecycle(const std::optional<std::string> &value);
    void setFrozen(const std::optional<bool> &value);

    friend void to_json(nlohmann::json &j,
                        const AssociationCreateOrUpdateInfo &o);
    friend void from_json(const nlohmann::json &j,
                          AssociationCreateOrUpdateInfo &o);

  private:
    std::optional<std::string> subject_;
    std::optional<std::string> association_type_;
    std::optional<std::string> lifecycle_;
    std::optional<bool> frozen_;
};

/**
 * AssociationCreateOrUpdateRequest represents a request to create/update
 * associations.
 */
class AssociationCreateOrUpdateRequest {
  public:
    AssociationCreateOrUpdateRequest();
    virtual ~AssociationCreateOrUpdateRequest() = default;

    bool operator==(const AssociationCreateOrUpdateRequest &rhs) const;
    bool operator!=(const AssociationCreateOrUpdateRequest &rhs) const;

    // Getters
    std::optional<std::string> getResourceName() const;
    std::optional<std::string> getResourceNamespace() const;
    std::optional<std::string> getResourceId() const;
    std::optional<std::string> getResourceType() const;
    std::optional<std::vector<AssociationCreateOrUpdateInfo>>
    getAssociations() const;

    // Setters
    void setResourceName(const std::optional<std::string> &value);
    void setResourceNamespace(const std::optional<std::string> &value);
    void setResourceId(const std::optional<std::string> &value);
    void setResourceType(const std::optional<std::string> &value);
    void setAssociations(
        const std::optional<std::vector<AssociationCreateOrUpdateInfo>> &value);

    friend void to_json(nlohmann::json &j,
                        const AssociationCreateOrUpdateRequest &o);
    friend void from_json(const nlohmann::json &j,
                          AssociationCreateOrUpdateRequest &o);

  private:
    std::optional<std::string> resource_name_;
    std::optional<std::string> resource_namespace_;
    std::optional<std::string> resource_id_;
    std::optional<std::string> resource_type_;
    std::optional<std::vector<AssociationCreateOrUpdateInfo>> associations_;
};

/**
 * AssociationResponse represents a response from creating/updating
 * associations.
 */
class AssociationResponse {
  public:
    AssociationResponse();
    virtual ~AssociationResponse() = default;

    bool operator==(const AssociationResponse &rhs) const;
    bool operator!=(const AssociationResponse &rhs) const;

    // Getters
    std::optional<std::string> getResourceId() const;
    std::optional<std::vector<Association>> getAssociations() const;

    // Setters
    void setResourceId(const std::optional<std::string> &value);
    void setAssociations(const std::optional<std::vector<Association>> &value);

    friend void to_json(nlohmann::json &j, const AssociationResponse &o);
    friend void from_json(const nlohmann::json &j, AssociationResponse &o);

  private:
    std::optional<std::string> resource_id_;
    std::optional<std::vector<Association>> associations_;
};

}  // namespace schemaregistry::rest::model
