#include "schemaregistry/serdes/RuleRegistry.h"

#include <algorithm>
#include <stdexcept>

#include "schemaregistry/rest/ClientConfiguration.h"
#include "schemaregistry/serdes/Serde.h"

namespace schemaregistry::serdes {

// RuleRegistry implementation

void RuleRegistry::registerExecutor(std::shared_ptr<RuleExecutor> executor) {
    if (!executor) {
        throw std::invalid_argument("Cannot register null executor");
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    rule_executors_[executor->getType()] = executor;
}

std::shared_ptr<RuleExecutor> RuleRegistry::getExecutor(
    const std::string &type) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = rule_executors_.find(type);
    if (it != rule_executors_.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<std::shared_ptr<RuleExecutor>> RuleRegistry::getExecutors() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<std::shared_ptr<RuleExecutor>> result;
    result.reserve(rule_executors_.size());

    for (const auto &pair : rule_executors_) {
        result.push_back(pair.second);
    }

    return result;
}

void RuleRegistry::registerAction(std::shared_ptr<RuleAction> action) {
    if (!action) {
        throw std::invalid_argument("Cannot register null action");
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    rule_actions_[action->getType()] = action;
}

std::shared_ptr<RuleAction> RuleRegistry::getAction(
    const std::string &type) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = rule_actions_.find(type);
    if (it != rule_actions_.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<std::shared_ptr<RuleAction>> RuleRegistry::getActions() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<std::shared_ptr<RuleAction>> result;
    result.reserve(rule_actions_.size());

    for (const auto &pair : rule_actions_) {
        result.push_back(pair.second);
    }

    return result;
}

void RuleRegistry::registerOverride(const RuleOverride &rule_override) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    rule_overrides_[rule_override.type] = rule_override;
}

std::optional<RuleOverride> RuleRegistry::getOverride(
    const std::string &type) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = rule_overrides_.find(type);
    if (it != rule_overrides_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<RuleOverride> RuleRegistry::getOverrides() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<RuleOverride> result;
    result.reserve(rule_overrides_.size());

    for (const auto &pair : rule_overrides_) {
        result.push_back(pair.second);
    }

    return result;
}

void RuleRegistry::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    rule_executors_.clear();
    rule_actions_.clear();
    rule_overrides_.clear();
}

// Global registry implementation

namespace global_registry {

RuleRegistry &getInstance() {
    static RuleRegistry instance;
    return instance;
}

void registerRuleExecutor(std::shared_ptr<RuleExecutor> executor) {
    getInstance().registerExecutor(executor);
}

std::shared_ptr<RuleExecutor> getRuleExecutor(const std::string &type) {
    return getInstance().getExecutor(type);
}

std::vector<std::shared_ptr<RuleExecutor>> getRuleExecutors() {
    return getInstance().getExecutors();
}

void registerRuleAction(std::shared_ptr<RuleAction> action) {
    getInstance().registerAction(action);
}

std::shared_ptr<RuleAction> getRuleAction(const std::string &type) {
    return getInstance().getAction(type);
}

std::vector<std::shared_ptr<RuleAction>> getRuleActions() {
    return getInstance().getActions();
}

void registerRuleOverride(const RuleOverride &rule_override) {
    getInstance().registerOverride(rule_override);
}

std::optional<RuleOverride> getRuleOverride(const std::string &type) {
    return getInstance().getOverride(type);
}

std::vector<RuleOverride> getRuleOverrides() {
    return getInstance().getOverrides();
}

void clearGlobalRegistry() { getInstance().clear(); }

}  // namespace global_registry

// FieldRuleExecutor implementation

std::unique_ptr<SerdeValue> FieldRuleExecutor::transform(
    RuleContext &ctx, const SerdeValue &msg) {
    Mode rule_mode = ctx.getRuleMode();

    // Check if we need to skip transformation based on mode and rule order
    if (rule_mode == Mode::Write || rule_mode == Mode::Upgrade) {
        // For Write/Upgrade mode, check earlier rules
        for (size_t i = 0; i < ctx.getIndex(); ++i) {
            const Rule &other_rule = ctx.getRules()[i];
            if (rule_utils::areTransformsWithSameTag(ctx.getRule(),
                                                     other_rule)) {
                return msg.clone();
            }
        }
    } else if (rule_mode == Mode::Read || rule_mode == Mode::Downgrade) {
        // For Read/Downgrade mode, check later rules
        const auto &rules = ctx.getRules();
        for (size_t i = ctx.getIndex() + 1; i < rules.size(); ++i) {
            const Rule &other_rule = rules[i];
            if (rule_utils::areTransformsWithSameTag(ctx.getRule(),
                                                     other_rule)) {
                return msg.clone();
            }
        }
    }

    // If we have a field transformer, use it
    auto field_transformer = ctx.getFieldTransformer();
    if (field_transformer) {
        return (*field_transformer)(ctx, getType(), msg);
    }

    // Default: return a clone of the original message
    return msg.clone();
}

// ErrorAction implementation

void ErrorAction::run(const RuleContext &ctx, const SerdeValue &msg,
                      std::optional<SerdeError> ex) {
    if (ex.has_value()) {
        // Re-throw the existing exception
        throw ex.value();
    } else {
        // Throw a generic error if no specific exception was provided
        throw SerdeError("Rule execution failed");
    }
}

// Rule utility functions

namespace rule_utils {

bool areTransformsWithSameTag(const Rule &rule1, const Rule &rule2) {
    // Check if both rules are transforms and have the same tags
    if (rule1.getKind() != Kind::Transform ||
        rule2.getKind() != Kind::Transform) {
        return false;
    }

    // Get tags from both rules and compare
    auto tags1 = rule1.getTags();
    auto tags2 = rule2.getTags();

    if (!tags1.has_value() || !tags2.has_value()) {
        return false;
    }

    // Check if any tag matches between the two sets
    for (const auto &tag1 : tags1.value()) {
        for (const auto &tag2 : tags2.value()) {
            if (tag1 == tag2) {
                return true;
            }
        }
    }

    return false;
}

std::optional<std::string> getRuleActionName(
    const Rule &rule, Mode mode, std::optional<std::string> action_name) {
    // If action_name is explicitly provided, use it
    if (action_name.has_value()) {
        return action_name;
    }

    // Based on the rule's on_success and on_failure settings and the current
    // mode
    switch (mode) {
        case Mode::Upgrade:
        case Mode::UpDown:
            // For upgrade operations, typically use on_success
            return rule.getOnSuccess();

        case Mode::Downgrade:
            // For downgrade operations, use on_failure if available
            if (rule.getOnFailure().has_value()) {
                return rule.getOnFailure();
            }
            return rule.getOnSuccess();

        case Mode::Write:
        case Mode::Read:
        case Mode::WriteRead:
            // For read/write operations, use on_failure if available
            if (rule.getOnFailure().has_value()) {
                return rule.getOnFailure();
            }
            return rule.getOnSuccess();

        default:
            return rule.getOnSuccess();
    }
}

}  // namespace rule_utils

}  // namespace schemaregistry::serdes