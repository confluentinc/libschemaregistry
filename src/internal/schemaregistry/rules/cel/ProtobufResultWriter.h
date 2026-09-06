#pragma once

#include "eval/public/cel_value.h"
#include "google/protobuf/message.h"
#include "schemaregistry/serdes/protobuf/ProtobufTypes.h"

namespace schemaregistry::rules::cel::utils {

/**
 * Rebuilds a protobuf message from the map a message-level `CEL` transform returned.
 *
 * A rule that returns a map is returning **the whole new message**: the transform has replace
 * semantics, not merge. Three consequences a rule author needs to know, and every client has to
 * match:
 *   - a field the rule does not name is **dropped**, so a rule naming only the field it changes
 *     discards the rest;
 *   - a `null` in the map **clears** its field;
 *   - echoing a field that was absent **materialises** it, because reading it produced a value.
 *     Preserve absence with `has(x) ? x : null`.
 *
 * Its own unit rather than a branch inside `toProtobufValue`, which is where it used to live. That
 * function's job is "convert this value against a template", and the template shape is what made
 * seeding the result from the input look natural on the Avro side - finding D7, where the same
 * fusion turned a message-level transform into a merge. Stating the contract here is what keeps
 * that reading from being available.
 *
 * Mechanism note: the JVM client rebuilds by rendering the result to JSON and parsing it back.
 * This builds the message directly through protobuf reflection, because a JSON round trip would
 * base64 every bytes field and format every timestamp only to parse them straight back. The
 * behaviours the JVM client gets free from the JSON mapping - null clearing a field, and a key
 * matching either the declared or the JSON name - are reproduced explicitly.
 */
schemaregistry::serdes::protobuf::ProtobufVariant messageFromCelMap(
    const google::protobuf::Message &original,
    const google::api::expr::runtime::CelValue &cel_value);

}  // namespace schemaregistry::rules::cel::utils
