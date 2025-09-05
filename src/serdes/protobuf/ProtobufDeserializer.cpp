#define schemaregistry_PROTOBUF_SKIP_TEMPLATE_IMPL
#include "schemaregistry/serdes/protobuf/ProtobufDeserializer.h"

#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/util/json_util.h>

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>

#include "schemaregistry/serdes/protobuf/ProtobufTypes.h"
#include "schemaregistry/serdes/protobuf/ProtobufUtils.h"

// Forward declaration for transformFields function from ProtobufUtils.cpp
namespace schemaregistry::serdes::protobuf::utils {
std::unique_ptr<schemaregistry::serdes::SerdeValue> transformFields(
    schemaregistry::serdes::RuleContext &ctx,
    const std::string &field_executor_type,
    const schemaregistry::serdes::SerdeValue &value);
}

namespace schemaregistry::serdes::protobuf {
}  // namespace schemaregistry::serdes::protobuf