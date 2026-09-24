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
 * arbitrary width in both directions, as BigInteger's are.
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

    /** The width ceiling, in decimal digits.
     *
     * Deliberately *not* BigDecimal's - BigInteger tops out at Integer.MAX_VALUE bits, which
     * is 646456993 digits, and reproducing that bound across six decimal libraries is neither
     * achievable nor the point. This is a round number chosen so no single rule evaluation can
     * exhaust memory. libmpdec's own MAX_PREC is around 1e18 digits, so it will happily try:
     * measured, a 2**31-digit rescale costs 918 MB and rendering one costs several GB. Java is
     * the only client in the family that fails cleanly on width; this stands in for that, as a
     * bound rather than as a model of BigDecimal's domain.
     */
    static constexpr int64_t kSaneWidth = 10000000;

    /** A far tighter ceiling on what can be *encoded*, which bounds a different resource.
     *
     * `confluent.type.Decimal.value` is the unscaled integer in base 256, and decimal <->
     * binary radix conversion is quadratic. Measured on this codec: 0.04 s at 10^4 digits,
     * 4.2 s at 10^5 and ~420 s at 10^6, in each direction. mpdecimal's own
     * `mpd_qexport_u32` is about 10x faster with the same quadratic shape (0.44 s at 10^5,
     * 43.7 s at 10^6), so there is no cheap way out of it. A value can therefore be cheap to
     * hold, cheap to compute with, and still unserialisable - which is a bound on *time*,
     * not memory, and needs its own number.
     *
     * 4300 is CPython's `int_max_str_digits`, the cap it puts on str <-> int conversion for
     * exactly this reason. Taking the same number keeps this client and the Python one
     * agreeing on which decimals can be written, and both an order of magnitude inside a
     * cost a rule evaluation can absorb. CEL's documented decimal precision is 38 digits, so
     * this leaves two orders of headroom over anything a rule is meant to produce.
     */
    static constexpr int64_t kSaneCoefficient = 4300;

    /** Digits in the coefficient `d` would have at `target_scale`.
     *
     * Only *expanding* a scale costs anything - the coefficient grows by the difference.
     * Coarsening one is free at any distance and yields a single digit, so an
     * `abs(shift) + digits` estimate refuses it wrongly.
     *
     * Computed from the exponent and the adjusted exponent, so the digits the caller is about
     * to refuse are never built - materialising the coefficient is the allocation being
     * guarded.
     */
    static int64_t rescaledDigits(int64_t target_scale, const decimal::Decimal &d);

    /** Characters in `d`'s plain (non-scientific) rendering, to within a couple.
     *
     * Unlike a rescale this pays for the exponent in *both* directions: a positive exponent
     * writes that many trailing zeros and a negative one that many leading zeros, so a
     * one-digit coefficient at an extreme scale still renders enormous.
     */
    static int64_t plainFormLength(const decimal::Decimal &d);

    /** Throw unless `needed` digits fit within kSaneWidth.
     *
     * Three unrelated-looking things reduce to this one quantity, because each has to
     * materialise a value in positional form: aligning two exponents (`add`, `sub`; `rem` is
     * the same family but bounded by its integral quotient), rescaling (the rounding family
     * and the Avro write-back), and rendering (`string`, and `double`, which parses the plain
     * form). `mul`, `div`, comparison, negation and `abs` are absent deliberately - each is
     * measurably cheap at any width, because none of them aligns.
     */
    static void requireSaneWidth(int64_t needed, const std::string &fn, const std::string &what,
                                 int64_t limit = kSaneWidth);

    /** Digits `d` needs once expanded to `target_scale`; a zero needs one, at any distance. */
    static int64_t operandWidth(int64_t target_scale, const decimal::Decimal &d);

    /** Guard A: the frame `add`/`sub` align their operands in. */
    static void requireAlignable(const decimal::Decimal &a, const decimal::Decimal &b,
                                 const std::string &fn);
};

}  // namespace schemaregistry::rules::cel
