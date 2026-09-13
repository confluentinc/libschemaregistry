#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#ifdef SCHEMAREGISTRY_USE_AVRO
// Guarded like the Avro declarations below: rules and Avro are independent features (only
// Protobuf is auto-enabled with rules), so a rules-without-Avro build has no avro headers.
#include "avro/Generic.hh"
#endif
#include "eval/public/cel_value.h"
#include "google/protobuf/arena.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "nlohmann/json.hpp"
#include "schemaregistry/serdes/protobuf/ProtobufTypes.h"

namespace schemaregistry::rules::cel::utils {

/// The CEL/protobuf timestamp range: 0001-01-01T00:00:00Z .. 9999-12-31T23:59:59.999999999Z.
/// Same values as Java's TimestampUtils.MIN_EPOCH_SECOND / MAX_EPOCH_SECOND. Declared here
/// rather than per-translation-unit so the constructor and the Avro binding cannot drift.
constexpr int64_t kMinEpochSecond = -62135596800LL;
constexpr int64_t kMaxEpochSecond = 253402300799LL;

/// The name of the CEL type a value carries, for a rule error message. Shared so the Avro and
/// protobuf writers report a mismatch in the same words.
const char *celTypeName(const google::api::expr::runtime::CelValue &value);

google::api::expr::runtime::CelValue fromJsonValue(
    const nlohmann::json &json, google::protobuf::Arena *arena);

nlohmann::json toJsonValue(
    const nlohmann::json &original,
    const google::api::expr::runtime::CelValue &cel_value);

#ifdef SCHEMAREGISTRY_USE_AVRO

google::api::expr::runtime::CelValue fromAvroValue(
    const ::avro::GenericDatum &avro, google::protobuf::Arena *arena);

::avro::GenericDatum toAvroValue(
    const ::avro::GenericDatum &original,
    const google::api::expr::runtime::CelValue &cel_value);

#endif

schemaregistry::serdes::protobuf::ProtobufVariant toProtobufValue(
    const schemaregistry::serdes::protobuf::ProtobufVariant &original,
    const google::api::expr::runtime::CelValue &cel_value);

google::api::expr::runtime::CelValue fromProtobufValue(
    const schemaregistry::serdes::protobuf::ProtobufVariant &variant,
    google::protobuf::Arena *arena);

google::api::expr::runtime::CelValue convertProtobufFieldToCel(
    const google::protobuf::Message &message,
    const google::protobuf::FieldDescriptor *field,
    const google::protobuf::Reflection *reflection,
    google::protobuf::Arena *arena, int index);

google::api::expr::runtime::CelValue convertProtobufMapToCel(
    const google::protobuf::Message &map_entry,
    const google::protobuf::FieldDescriptor *map_field,
    google::protobuf::Arena *arena);

}  // namespace schemaregistry::rules::cel::utils
