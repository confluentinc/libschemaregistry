#pragma once

#include "schemaregistry/rules/cel/CelExecutor.h"
#include "schemaregistry/rules/cel/CelFieldExecutor.h"
#include "schemaregistry/rules/cel/CelValidator.h"

namespace schemaregistry::rules::cel {

/**
 * Convenience functions for registering all CEL rule executors
 * with the global rule registry
 */
namespace registration {

/**
 * Register all CEL rule executors (CelExecutor, CelFieldExecutor and the
 * CelValidator used for inline validation rules)
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

/**
 * Register only the CEL validator (for inline validation rules)
 */
void registerCelValidator();

}  // namespace registration

}  // namespace schemaregistry::rules::cel
