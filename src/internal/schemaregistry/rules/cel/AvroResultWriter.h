#pragma once

#include "avro/Generic.hh"
#include "eval/public/cel_value.h"

namespace schemaregistry::rules::cel::utils {

/**
 * Rebuilds an Avro record from the map a message-level `CEL` transform returned.
 *
 * A rule that returns a map is returning **the whole new record**: the transform has replace
 * semantics, not merge. A field the rule does not name is *not* carried over from the input - it
 * takes the schema's declared default, or the rebuild fails, which is what
 * `GenericRecordBuilder.build()` does on the JVM. Avro has no notion of an absent field, so there
 * is no third option.
 *
 * Its own unit rather than a branch inside `toAvroValue`, which is where it used to live and where
 * it went wrong. That function's job is "convert this value against a template", so seeding the
 * result from the template reads as the natural thing to write - and it silently made every
 * message-level Avro transform a merge, in the one client that fused the two (finding D7). The
 * contract above is the whole point of the separation; the code is the same code.
 *
 * `original` supplies the *shape*: its schema, and per field the logical type, scale and unit that
 * the conversion of each value needs. Its values are not read.
 *
 * Child values are converted by calling `toAvroValue` back, so the two are mutually recursive.
 * That is the cost of extracting this: protobuf's counterpart has no such dependency, because its
 * nested-message case writes through reflection rather than recursing.
 */
::avro::GenericDatum recordFromCelMap(
    const ::avro::GenericDatum &original,
    const google::api::expr::runtime::CelMap &cel_map);

}  // namespace schemaregistry::rules::cel::utils
