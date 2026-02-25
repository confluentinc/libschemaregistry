#include "schemaregistry/rest/model/Association.h"

namespace schemaregistry::rest::model {

// ── Association ──────────────────────────────────────────────────────────────

Association::Association() = default;

bool Association::operator==(const Association &rhs) const {
    return subject_ == rhs.subject_ && guid_ == rhs.guid_ &&
           resource_name_ == rhs.resource_name_ &&
           resource_namespace_ == rhs.resource_namespace_ &&
           resource_id_ == rhs.resource_id_ &&
           resource_type_ == rhs.resource_type_ &&
           association_type_ == rhs.association_type_ &&
           lifecycle_ == rhs.lifecycle_ && frozen_ == rhs.frozen_;
}

bool Association::operator!=(const Association &rhs) const {
    return !(*this == rhs);
}

// Getters
std::optional<std::string> Association::getSubject() const { return subject_; }
std::optional<std::string> Association::getGuid() const { return guid_; }
std::optional<std::string> Association::getResourceName() const { return resource_name_; }
std::optional<std::string> Association::getResourceNamespace() const { return resource_namespace_; }
std::optional<std::string> Association::getResourceId() const { return resource_id_; }
std::optional<std::string> Association::getResourceType() const { return resource_type_; }
std::optional<std::string> Association::getAssociationType() const { return association_type_; }
std::optional<LifecyclePolicy> Association::getLifecycle() const { return lifecycle_; }
std::optional<bool> Association::getFrozen() const { return frozen_; }

// Setters
void Association::setSubject(const std::optional<std::string> &value) { subject_ = value; }
void Association::setGuid(const std::optional<std::string> &value) { guid_ = value; }
void Association::setResourceName(const std::optional<std::string> &value) { resource_name_ = value; }
void Association::setResourceNamespace(const std::optional<std::string> &value) { resource_namespace_ = value; }
void Association::setResourceId(const std::optional<std::string> &value) { resource_id_ = value; }
void Association::setResourceType(const std::optional<std::string> &value) { resource_type_ = value; }
void Association::setAssociationType(const std::optional<std::string> &value) { association_type_ = value; }
void Association::setLifecycle(const std::optional<LifecyclePolicy> &value) { lifecycle_ = value; }
void Association::setFrozen(const std::optional<bool> &value) { frozen_ = value; }

// JSON serialization
void to_json(nlohmann::json &j, const Association &o) {
    j = nlohmann::json::object();
    if (o.subject_.has_value()) j["subject"] = o.subject_.value();
    if (o.guid_.has_value()) j["guid"] = o.guid_.value();
    if (o.resource_name_.has_value()) j["resourceName"] = o.resource_name_.value();
    if (o.resource_namespace_.has_value()) j["resourceNamespace"] = o.resource_namespace_.value();
    if (o.resource_id_.has_value()) j["resourceId"] = o.resource_id_.value();
    if (o.resource_type_.has_value()) j["resourceType"] = o.resource_type_.value();
    if (o.association_type_.has_value()) j["associationType"] = o.association_type_.value();
    if (o.lifecycle_.has_value()) j["lifecycle"] = o.lifecycle_.value();
    if (o.frozen_.has_value()) j["frozen"] = o.frozen_.value();
}

void from_json(const nlohmann::json &j, Association &o) {
    if (j.contains("subject") && !j["subject"].is_null())
        o.subject_ = j["subject"].get<std::string>();
    if (j.contains("guid") && !j["guid"].is_null())
        o.guid_ = j["guid"].get<std::string>();
    if (j.contains("resourceName") && !j["resourceName"].is_null())
        o.resource_name_ = j["resourceName"].get<std::string>();
    if (j.contains("resourceNamespace") && !j["resourceNamespace"].is_null())
        o.resource_namespace_ = j["resourceNamespace"].get<std::string>();
    if (j.contains("resourceId") && !j["resourceId"].is_null())
        o.resource_id_ = j["resourceId"].get<std::string>();
    if (j.contains("resourceType") && !j["resourceType"].is_null())
        o.resource_type_ = j["resourceType"].get<std::string>();
    if (j.contains("associationType") && !j["associationType"].is_null())
        o.association_type_ = j["associationType"].get<std::string>();
    if (j.contains("lifecycle") && !j["lifecycle"].is_null())
        o.lifecycle_ = j["lifecycle"].get<LifecyclePolicy>();
    if (j.contains("frozen") && !j["frozen"].is_null())
        o.frozen_ = j["frozen"].get<bool>();
}

// ── AssociationCreateOrUpdateInfo ────────────────────────────────────────────

AssociationCreateOrUpdateInfo::AssociationCreateOrUpdateInfo() = default;

bool AssociationCreateOrUpdateInfo::operator==(
    const AssociationCreateOrUpdateInfo &rhs) const {
    return subject_ == rhs.subject_ &&
           association_type_ == rhs.association_type_ &&
           lifecycle_ == rhs.lifecycle_ && frozen_ == rhs.frozen_;
}

bool AssociationCreateOrUpdateInfo::operator!=(
    const AssociationCreateOrUpdateInfo &rhs) const {
    return !(*this == rhs);
}

// Getters
std::optional<std::string> AssociationCreateOrUpdateInfo::getSubject() const { return subject_; }
std::optional<std::string> AssociationCreateOrUpdateInfo::getAssociationType() const { return association_type_; }
std::optional<std::string> AssociationCreateOrUpdateInfo::getLifecycle() const { return lifecycle_; }
std::optional<bool> AssociationCreateOrUpdateInfo::getFrozen() const { return frozen_; }

// Setters
void AssociationCreateOrUpdateInfo::setSubject(const std::optional<std::string> &value) { subject_ = value; }
void AssociationCreateOrUpdateInfo::setAssociationType(const std::optional<std::string> &value) { association_type_ = value; }
void AssociationCreateOrUpdateInfo::setLifecycle(const std::optional<std::string> &value) { lifecycle_ = value; }
void AssociationCreateOrUpdateInfo::setFrozen(const std::optional<bool> &value) { frozen_ = value; }

// JSON serialization
void to_json(nlohmann::json &j, const AssociationCreateOrUpdateInfo &o) {
    j = nlohmann::json::object();
    if (o.subject_.has_value()) j["subject"] = o.subject_.value();
    if (o.association_type_.has_value()) j["associationType"] = o.association_type_.value();
    if (o.lifecycle_.has_value()) j["lifecycle"] = o.lifecycle_.value();
    if (o.frozen_.has_value()) j["frozen"] = o.frozen_.value();
}

void from_json(const nlohmann::json &j, AssociationCreateOrUpdateInfo &o) {
    if (j.contains("subject") && !j["subject"].is_null())
        o.subject_ = j["subject"].get<std::string>();
    if (j.contains("associationType") && !j["associationType"].is_null())
        o.association_type_ = j["associationType"].get<std::string>();
    if (j.contains("lifecycle") && !j["lifecycle"].is_null())
        o.lifecycle_ = j["lifecycle"].get<std::string>();
    if (j.contains("frozen") && !j["frozen"].is_null())
        o.frozen_ = j["frozen"].get<bool>();
}

// ── AssociationCreateOrUpdateRequest ─────────────────────────────────────────

AssociationCreateOrUpdateRequest::AssociationCreateOrUpdateRequest() = default;

bool AssociationCreateOrUpdateRequest::operator==(
    const AssociationCreateOrUpdateRequest &rhs) const {
    return resource_name_ == rhs.resource_name_ &&
           resource_namespace_ == rhs.resource_namespace_ &&
           resource_id_ == rhs.resource_id_ &&
           resource_type_ == rhs.resource_type_ &&
           associations_ == rhs.associations_;
}

bool AssociationCreateOrUpdateRequest::operator!=(
    const AssociationCreateOrUpdateRequest &rhs) const {
    return !(*this == rhs);
}

// Getters
std::optional<std::string> AssociationCreateOrUpdateRequest::getResourceName() const { return resource_name_; }
std::optional<std::string> AssociationCreateOrUpdateRequest::getResourceNamespace() const { return resource_namespace_; }
std::optional<std::string> AssociationCreateOrUpdateRequest::getResourceId() const { return resource_id_; }
std::optional<std::string> AssociationCreateOrUpdateRequest::getResourceType() const { return resource_type_; }
std::optional<std::vector<AssociationCreateOrUpdateInfo>>
AssociationCreateOrUpdateRequest::getAssociations() const { return associations_; }

// Setters
void AssociationCreateOrUpdateRequest::setResourceName(const std::optional<std::string> &value) { resource_name_ = value; }
void AssociationCreateOrUpdateRequest::setResourceNamespace(const std::optional<std::string> &value) { resource_namespace_ = value; }
void AssociationCreateOrUpdateRequest::setResourceId(const std::optional<std::string> &value) { resource_id_ = value; }
void AssociationCreateOrUpdateRequest::setResourceType(const std::optional<std::string> &value) { resource_type_ = value; }
void AssociationCreateOrUpdateRequest::setAssociations(
    const std::optional<std::vector<AssociationCreateOrUpdateInfo>> &value) { associations_ = value; }

// JSON serialization
void to_json(nlohmann::json &j, const AssociationCreateOrUpdateRequest &o) {
    j = nlohmann::json::object();
    if (o.resource_name_.has_value()) j["resourceName"] = o.resource_name_.value();
    if (o.resource_namespace_.has_value()) j["resourceNamespace"] = o.resource_namespace_.value();
    if (o.resource_id_.has_value()) j["resourceId"] = o.resource_id_.value();
    if (o.resource_type_.has_value()) j["resourceType"] = o.resource_type_.value();
    if (o.associations_.has_value()) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto &assoc : o.associations_.value()) {
            nlohmann::json assoc_json;
            to_json(assoc_json, assoc);
            arr.push_back(assoc_json);
        }
        j["associations"] = arr;
    }
}

void from_json(const nlohmann::json &j, AssociationCreateOrUpdateRequest &o) {
    if (j.contains("resourceName") && !j["resourceName"].is_null())
        o.resource_name_ = j["resourceName"].get<std::string>();
    if (j.contains("resourceNamespace") && !j["resourceNamespace"].is_null())
        o.resource_namespace_ = j["resourceNamespace"].get<std::string>();
    if (j.contains("resourceId") && !j["resourceId"].is_null())
        o.resource_id_ = j["resourceId"].get<std::string>();
    if (j.contains("resourceType") && !j["resourceType"].is_null())
        o.resource_type_ = j["resourceType"].get<std::string>();
    if (j.contains("associations") && !j["associations"].is_null()) {
        std::vector<AssociationCreateOrUpdateInfo> assocs;
        for (const auto &item : j["associations"]) {
            AssociationCreateOrUpdateInfo assoc;
            from_json(item, assoc);
            assocs.push_back(assoc);
        }
        o.associations_ = assocs;
    }
}

// ── AssociationResponse ───────────────────────────────────────────────────────

AssociationResponse::AssociationResponse() = default;

bool AssociationResponse::operator==(const AssociationResponse &rhs) const {
    return resource_id_ == rhs.resource_id_ &&
           associations_ == rhs.associations_;
}

bool AssociationResponse::operator!=(const AssociationResponse &rhs) const {
    return !(*this == rhs);
}

// Getters
std::optional<std::string> AssociationResponse::getResourceId() const { return resource_id_; }
std::optional<std::vector<Association>> AssociationResponse::getAssociations() const { return associations_; }

// Setters
void AssociationResponse::setResourceId(const std::optional<std::string> &value) { resource_id_ = value; }
void AssociationResponse::setAssociations(
    const std::optional<std::vector<Association>> &value) { associations_ = value; }

// JSON serialization
void to_json(nlohmann::json &j, const AssociationResponse &o) {
    j = nlohmann::json::object();
    if (o.resource_id_.has_value()) j["resourceId"] = o.resource_id_.value();
    if (o.associations_.has_value()) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto &assoc : o.associations_.value()) {
            nlohmann::json assoc_json;
            to_json(assoc_json, assoc);
            arr.push_back(assoc_json);
        }
        j["associations"] = arr;
    }
}

void from_json(const nlohmann::json &j, AssociationResponse &o) {
    if (j.contains("resourceId") && !j["resourceId"].is_null())
        o.resource_id_ = j["resourceId"].get<std::string>();
    if (j.contains("associations") && !j["associations"].is_null()) {
        std::vector<Association> assocs;
        for (const auto &item : j["associations"]) {
            Association assoc;
            from_json(item, assoc);
            assocs.push_back(assoc);
        }
        o.associations_ = assocs;
    }
}

}  // namespace schemaregistry::rest::model
