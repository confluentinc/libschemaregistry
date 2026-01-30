#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace schemaregistry::rest::model {

/**
 * LifecyclePolicy represents the lifecycle policy for an association.
 * Based on LifecyclePolicy from serde.rs
 */
enum class LifecyclePolicy { Strong, Weak };

/**
 * Association represents an association between a subject and a resource.
 * Based on Association from serde.rs
 */
struct Association {
    std::optional<std::string> subject;
    std::optional<std::string> guid;
    std::optional<std::string> resource_name;
    std::optional<std::string> resource_namespace;
    std::optional<std::string> resource_id;
    std::optional<std::string> resource_type;
    std::optional<std::string> association_type;
    std::optional<LifecyclePolicy> lifecycle;
    std::optional<bool> frozen;

    Association() = default;
};

// Custom JSON conversion for LifecyclePolicy
inline void to_json(nlohmann::json &j, const LifecyclePolicy &p) {
    switch (p) {
        case LifecyclePolicy::Strong:
            j = "STRONG";
            break;
        case LifecyclePolicy::Weak:
            j = "WEAK";
            break;
    }
}

inline void from_json(const nlohmann::json &j, LifecyclePolicy &p) {
    std::string s = j.get<std::string>();
    if (s == "STRONG") {
        p = LifecyclePolicy::Strong;
    } else if (s == "WEAK") {
        p = LifecyclePolicy::Weak;
    } else {
        p = LifecyclePolicy::Strong;  // Default
    }
}

// Manual JSON conversion for Association to handle optional fields and
// lifecycle
inline void to_json(nlohmann::json &j, const Association &a) {
    j = nlohmann::json::object();
    if (a.subject.has_value()) {
        j["subject"] = a.subject.value();
    }
    if (a.guid.has_value()) {
        j["guid"] = a.guid.value();
    }
    if (a.resource_name.has_value()) {
        j["resourceName"] = a.resource_name.value();
    }
    if (a.resource_namespace.has_value()) {
        j["resourceNamespace"] = a.resource_namespace.value();
    }
    if (a.resource_id.has_value()) {
        j["resourceId"] = a.resource_id.value();
    }
    if (a.resource_type.has_value()) {
        j["resourceType"] = a.resource_type.value();
    }
    if (a.association_type.has_value()) {
        j["associationType"] = a.association_type.value();
    }
    if (a.lifecycle.has_value()) {
        to_json(j["lifecycle"], a.lifecycle.value());
    }
    if (a.frozen.has_value()) {
        j["frozen"] = a.frozen.value();
    }
}

inline void from_json(const nlohmann::json &j, Association &a) {
    if (j.contains("subject") && !j["subject"].is_null()) {
        a.subject = j["subject"].get<std::string>();
    }
    if (j.contains("guid") && !j["guid"].is_null()) {
        a.guid = j["guid"].get<std::string>();
    }
    if (j.contains("resourceName") && !j["resourceName"].is_null()) {
        a.resource_name = j["resourceName"].get<std::string>();
    }
    if (j.contains("resourceNamespace") && !j["resourceNamespace"].is_null()) {
        a.resource_namespace = j["resourceNamespace"].get<std::string>();
    }
    if (j.contains("resourceId") && !j["resourceId"].is_null()) {
        a.resource_id = j["resourceId"].get<std::string>();
    }
    if (j.contains("resourceType") && !j["resourceType"].is_null()) {
        a.resource_type = j["resourceType"].get<std::string>();
    }
    if (j.contains("associationType") && !j["associationType"].is_null()) {
        a.association_type = j["associationType"].get<std::string>();
    }
    if (j.contains("lifecycle") && !j["lifecycle"].is_null()) {
        LifecyclePolicy lp;
        from_json(j["lifecycle"], lp);
        a.lifecycle = lp;
    }
    if (j.contains("frozen") && !j["frozen"].is_null()) {
        a.frozen = j["frozen"].get<bool>();
    }
}

/**
 * AssociationCreateOrUpdateInfo represents an association to create/update.
 */
struct AssociationCreateOrUpdateInfo {
    std::optional<std::string> subject;
    std::optional<std::string> association_type;
    std::optional<std::string> lifecycle;
    std::optional<bool> frozen;

    AssociationCreateOrUpdateInfo() = default;
};

inline void to_json(nlohmann::json &j, const AssociationCreateOrUpdateInfo &a) {
    j = nlohmann::json::object();
    if (a.subject.has_value()) {
        j["subject"] = a.subject.value();
    }
    if (a.association_type.has_value()) {
        j["associationType"] = a.association_type.value();
    }
    if (a.lifecycle.has_value()) {
        j["lifecycle"] = a.lifecycle.value();
    }
    if (a.frozen.has_value()) {
        j["frozen"] = a.frozen.value();
    }
}

inline void from_json(const nlohmann::json &j, AssociationCreateOrUpdateInfo &a) {
    if (j.contains("subject") && !j["subject"].is_null()) {
        a.subject = j["subject"].get<std::string>();
    }
    if (j.contains("associationType") && !j["associationType"].is_null()) {
        a.association_type = j["associationType"].get<std::string>();
    }
    if (j.contains("lifecycle") && !j["lifecycle"].is_null()) {
        a.lifecycle = j["lifecycle"].get<std::string>();
    }
    if (j.contains("frozen") && !j["frozen"].is_null()) {
        a.frozen = j["frozen"].get<bool>();
    }
}

/**
 * AssociationCreateOrUpdateRequest represents a request to create/update
 * associations.
 */
struct AssociationCreateOrUpdateRequest {
    std::optional<std::string> resource_name;
    std::optional<std::string> resource_namespace;
    std::optional<std::string> resource_id;
    std::optional<std::string> resource_type;
    std::optional<std::vector<AssociationCreateOrUpdateInfo>> associations;

    AssociationCreateOrUpdateRequest() = default;
};

inline void to_json(nlohmann::json &j,
                    const AssociationCreateOrUpdateRequest &r) {
    j = nlohmann::json::object();
    if (r.resource_name.has_value()) {
        j["resourceName"] = r.resource_name.value();
    }
    if (r.resource_namespace.has_value()) {
        j["resourceNamespace"] = r.resource_namespace.value();
    }
    if (r.resource_id.has_value()) {
        j["resourceId"] = r.resource_id.value();
    }
    if (r.resource_type.has_value()) {
        j["resourceType"] = r.resource_type.value();
    }
    if (r.associations.has_value()) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto &assoc : r.associations.value()) {
            nlohmann::json assoc_json;
            to_json(assoc_json, assoc);
            arr.push_back(assoc_json);
        }
        j["associations"] = arr;
    }
}

inline void from_json(const nlohmann::json &j,
                      AssociationCreateOrUpdateRequest &r) {
    if (j.contains("resourceName") && !j["resourceName"].is_null()) {
        r.resource_name = j["resourceName"].get<std::string>();
    }
    if (j.contains("resourceNamespace") && !j["resourceNamespace"].is_null()) {
        r.resource_namespace = j["resourceNamespace"].get<std::string>();
    }
    if (j.contains("resourceId") && !j["resourceId"].is_null()) {
        r.resource_id = j["resourceId"].get<std::string>();
    }
    if (j.contains("resourceType") && !j["resourceType"].is_null()) {
        r.resource_type = j["resourceType"].get<std::string>();
    }
    if (j.contains("associations") && !j["associations"].is_null()) {
        std::vector<AssociationCreateOrUpdateInfo> assocs;
        for (const auto &item : j["associations"]) {
            AssociationCreateOrUpdateInfo assoc;
            from_json(item, assoc);
            assocs.push_back(assoc);
        }
        r.associations = assocs;
    }
}

/**
 * AssociationResponse represents a response from creating/updating
 * associations.
 */
struct AssociationResponse {
    std::optional<std::string> resource_id;
    std::optional<std::vector<Association>> associations;

    AssociationResponse() = default;
};

inline void to_json(nlohmann::json &j, const AssociationResponse &r) {
    j = nlohmann::json::object();
    if (r.resource_id.has_value()) {
        j["resourceId"] = r.resource_id.value();
    }
    if (r.associations.has_value()) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto &assoc : r.associations.value()) {
            nlohmann::json assoc_json;
            to_json(assoc_json, assoc);
            arr.push_back(assoc_json);
        }
        j["associations"] = arr;
    }
}

inline void from_json(const nlohmann::json &j, AssociationResponse &r) {
    if (j.contains("resourceId") && !j["resourceId"].is_null()) {
        r.resource_id = j["resourceId"].get<std::string>();
    }
    if (j.contains("associations") && !j["associations"].is_null()) {
        std::vector<Association> assocs;
        for (const auto &item : j["associations"]) {
            Association assoc;
            from_json(item, assoc);
            assocs.push_back(assoc);
        }
        r.associations = assocs;
    }
}

}  // namespace schemaregistry::rest::model
