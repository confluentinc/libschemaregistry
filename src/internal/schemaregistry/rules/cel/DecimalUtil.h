/**
 * Copyright 2026 Confluent Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstdint>
#include <string>

#include "confluent/type/decimal.pb.h"
#include "decimal.hh"

namespace schemaregistry::rules::cel {

/**
 * Conversions between the CEL Decimal backing type (libmpdec++ `decimal::Decimal`) and the
 * `confluent.type.Decimal` proto message, plus its unscaled-bytes + scale wire form. This is
 * the C++ counterpart of Java's DecimalUtils / the other clients' decimal helpers.
 *
 * `confluent.type.Decimal.value` is the unscaled integer as big-endian two's-complement bytes
 * (BigInteger.toByteArray form); `scale` is the number of fractional digits. Coefficients are
 * handled up to 128 bits, which covers the 38-significant-digit CEL precision.
 */
class DecimalUtil {
   public:
    /** The shared arithmetic context: 38 significant digits, ROUND_HALF_UP (= Java MathContext).
     *  Used by div/sqrt and the rounding family. */
    static decimal::Context &context();

    /** An exact/(practically) unbounded-precision context for the operations Java computes
     *  exactly via java.math.BigDecimal defaults (add/sub/mul). Mirrors Python's
     *  _EXACT_CONTEXT (prec=MAX_PREC). Capping these at 38 digits would diverge from the
     *  other clients. */
    static decimal::Context &exactContext();

    /** Decode a confluent.type.Decimal message into a Decimal (scale preserved). */
    static decimal::Decimal fromProto(const confluent::type::Decimal &d);

    /** Encode a Decimal as a confluent.type.Decimal message (unscaled value + scale). */
    static confluent::type::Decimal toProto(const decimal::Decimal &d);

    /** Build a Decimal from raw unscaled two's-complement big-endian bytes plus a scale. */
    static decimal::Decimal fromUnscaledBytes(const std::string &unscaled, int32_t scale);

    /** Decode big-endian two's-complement bytes into a 128-bit sign/magnitude. */
    static void bytesToMagnitude(const std::string &bytes, uint8_t &sign, uint64_t &hi,
                                 uint64_t &lo);

    /** Encode a 128-bit sign/magnitude as minimal big-endian two's-complement bytes. */
    static std::string magnitudeToBytes(uint8_t sign, uint64_t hi, uint64_t lo);
};

}  // namespace schemaregistry::rules::cel
