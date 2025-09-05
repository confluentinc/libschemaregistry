#pragma once

#include "schemaregistry/rules/cel/CelExecutor.h"
#include "schemaregistry/rules/cel/CelFieldExecutor.h"

namespace schemaregistry::rules::cel {

/**
 * Convenience functions for registering all CEL rule executors
 * with the global rule registry
 */
namespace registration {

/**
 * Register all CEL rule executors (both CelExecutor and CelFieldExecutor)
 * Call this function during application initialization to make CEL
 * rules available for use.
 */
void registerAllCelExecutors();

/**
 * Register only the main CEL executor (for message-level transformations)
 */
void registerCelExecutor();

/**
 * Register only the CEL field executor (for field-level transformations)
 */
void registerCelFieldExecutor();

}  // namespace registration

}  // namespace schemaregistry::rules::cel
