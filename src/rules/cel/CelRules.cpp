#include "schemaregistry/rules/cel/CelRules.h"

namespace schemaregistry::rules::cel::registration {

void registerAllCelExecutors() {
    registerCelExecutor();
    registerCelFieldExecutor();
}

void registerCelExecutor() { CelExecutor::registerExecutor(); }

void registerCelFieldExecutor() { CelFieldExecutor::registerExecutor(); }

}  // namespace schemaregistry::rules::cel::registration