#pragma once

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "schemaregistry/serdes/SerdeError.h"
#include "schemaregistry/serdes/SerdeTypes.h"

namespace schemaregistry::serdes {

// Forward declarations
class RuleExecutor;
class RuleAction;

/**
 * Registry for managing rule executors, actions, and overrides
 * Based on RuleRegistry from rule_registry.rs
 */
class RuleRegistry {
  private:
    std::unordered_map<std::string, std::shared_ptr<RuleExecutor>>
        rule_executors_;
    std::unordered_map<std::string, std::shared_ptr<RuleAction>> rule_actions_;
    std::unordered_map<std::string, RuleOverride> rule_overrides_;
    mutable std::shared_mutex mutex_;

  public:
    RuleRegistry() = default;
    ~RuleRegistry() = default;

    // Rule executor management
    void registerExecutor(std::shared_ptr<RuleExecutor> executor);
    std::shared_ptr<RuleExecutor> getExecutor(const std::string &type) const;
    std::vector<std::shared_ptr<RuleExecutor>> getExecutors() const;

    // Rule action management
    void registerAction(std::shared_ptr<RuleAction> action);
    std::shared_ptr<RuleAction> getAction(const std::string &type) const;
    std::vector<std::shared_ptr<RuleAction>> getActions() const;

    // Rule override management
    void registerOverride(const RuleOverride &rule_override);
    std::optional<RuleOverride> getOverride(const std::string &type) const;
    std::vector<RuleOverride> getOverrides() const;

    // Clear all registrations
    void clear();

    // Copy/move operations - deleted due to mutex
    RuleRegistry(const RuleRegistry &) = delete;
    RuleRegistry &operator=(const RuleRegistry &) = delete;
    RuleRegistry(RuleRegistry &&) = delete;
    RuleRegistry &operator=(RuleRegistry &&) = delete;
};

/**
 * Global rule registry functions
 * Based on the global functions from rule_registry.rs
 */
namespace global_registry {
/**
 * Get the global rule registry instance
 */
RuleRegistry &getInstance();

/**
 * Register a rule executor globally
 */
void registerRuleExecutor(std::shared_ptr<RuleExecutor> executor);

/**
 * Get a rule executor from global registry
 */
std::shared_ptr<RuleExecutor> getRuleExecutor(const std::string &type);

/**
 * Get all rule executors from global registry
 */
std::vector<std::shared_ptr<RuleExecutor>> getRuleExecutors();

/**
 * Register a rule action globally
 */
void registerRuleAction(std::shared_ptr<RuleAction> action);

/**
 * Get a rule action from global registry
 */
std::shared_ptr<RuleAction> getRuleAction(const std::string &type);

/**
 * Get all rule actions from global registry
 */
std::vector<std::shared_ptr<RuleAction>> getRuleActions();

/**
 * Register a rule override globally
 */
void registerRuleOverride(const RuleOverride &rule_override);

/**
 * Get a rule override from global registry
 */
std::optional<RuleOverride> getRuleOverride(const std::string &type);

/**
 * Get all rule overrides from global registry
 */
std::vector<RuleOverride> getRuleOverrides();

/**
 * Clear the global registry
 */
void clearGlobalRegistry();
}  // namespace global_registry

/**
 * Base interface for all rule-related components
 * Based on traits from serde.rs
 */
class RuleBase {
  public:
    virtual ~RuleBase() = default;

    /**
     * Configure the rule with client and rule-specific configuration
     */
    virtual void configure(
        std::shared_ptr<const ClientConfiguration> client_config,
        const std::unordered_map<std::string, std::string> &rule_config) {}

    /**
     * Get the type identifier for this rule
     */
    virtual std::string getType() const = 0;

    /**
     * Close and cleanup resources
     */
    virtual void close() {}
};

/**
 * Interface for rule executors
 * Based on RuleExecutor trait from serde.rs
 */
class RuleExecutor : public RuleBase {
  public:
    virtual ~RuleExecutor() = default;

    /**
     * Transform a message value according to the rule
     * Synchronous version of the async transform method
     */
    virtual std::unique_ptr<SerdeValue> transform(RuleContext &ctx,
                                                  const SerdeValue &msg) = 0;
};

/**
 * Interface for field-level rule executors
 * Based on FieldRuleExecutor trait from serde.rs
 */
class FieldRuleExecutor : public RuleExecutor {
  public:
    virtual ~FieldRuleExecutor() = default;

    /**
     * Transform a field value according to the rule
     * Synchronous version of the async transform_field method
     */
    virtual std::unique_ptr<SerdeValue> transformField(
        RuleContext &ctx, const SerdeValue &field_value) = 0;

    /**
     * Implementation of RuleExecutor::transform for field executors
     */
    std::unique_ptr<SerdeValue> transform(RuleContext &ctx,
                                          const SerdeValue &msg) override;
};

/**
 * Interface for rule actions
 * Based on RuleAction trait from serde.rs
 */
class RuleAction : public RuleBase {
  public:
    virtual ~RuleAction() = default;

    /**
     * Execute the action with the given context, message, and optional
     * exception Synchronous version of the async run method
     */
    virtual void run(const RuleContext &ctx, const SerdeValue &msg,
                     std::optional<SerdeError> ex = std::nullopt) = 0;
};

/**
 * Built-in error action
 * Based on ErrorAction from serde.rs
 */
class ErrorAction : public RuleAction {
  public:
    std::string getType() const override { return "ERROR"; }

    void run(const RuleContext &ctx, const SerdeValue &msg,
             std::optional<SerdeError> ex = std::nullopt) override;
};

/**
 * Built-in none action (no-op)
 * Based on NoneAction from serde.rs
 */
class NoneAction : public RuleAction {
  public:
    std::string getType() const override { return "NONE"; }

    void run(const RuleContext &ctx, const SerdeValue &msg,
             std::optional<SerdeError> ex = std::nullopt) override {}
};

/**
 * Utility functions for rule management
 */
namespace rule_utils {
/**
 * Check if two rules are transforms with the same tag
 * Based on are_transforms_with_same_tag from serde.rs
 */
bool areTransformsWithSameTag(const Rule &rule1, const Rule &rule2);

/**
 * Get rule action name based on rule mode
 * Based on logic from serde.rs
 */
std::optional<std::string> getRuleActionName(
    const Rule &rule, Mode mode, std::optional<std::string> action_name);
}  // namespace rule_utils

}  // namespace schemaregistry::serdes