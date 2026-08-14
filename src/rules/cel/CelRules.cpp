#include "schemaregistry/rules/cel/CelRules.h"

namespace schemaregistry::rules::cel::registration {

void registerAllCelExecutors() {
    registerCelExecutor();
    registerCelFieldExecutor();
    registerCelValidator();
}

void registerCelExecutor() { CelExecutor::registerExecutor(); }

void registerCelFieldExecutor() { CelFieldExecutor::registerExecutor(); }

void registerCelValidator() { CelValidator::registerExecutor(); }

}  // namespace schemaregistry::rules::cel::registration