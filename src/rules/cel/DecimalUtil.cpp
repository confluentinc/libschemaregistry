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

#include "schemaregistry/rules/cel/DecimalUtil.h"

#include <cstring>
#include <stdexcept>

namespace schemaregistry::rules::cel {

// 38 significant digits, HALF_UP — matches Java's MathContext(38, HALF_UP) and the other
// clients, and reproduces CPython's decimal results exactly (libmpdec is the same engine).
decimal::Context &DecimalUtil::context() {
    static thread_local decimal::Context ctx = [] {
        decimal::Context c;
        c.prec(38);
        c.round(decimal::ROUND_HALF_UP);
        return c;
    }();
    return ctx;
}

// (Practically) unbounded precision for add/sub/mul — java.math.BigDecimal computes these
// exactly (no MathContext), and Python uses decimal.Context(prec=MAX_PREC, Emax=MAX_EMAX,
// Emin=MIN_EMIN). Using the 38-digit context() here would cap the result and diverge from
// the other clients. MaxContext() traps only on Invalid_operation, so exact add/sub/mul
// never raise.
decimal::Context &DecimalUtil::exactContext() {
    static thread_local decimal::Context ctx = [] {
        decimal::Context c = decimal::MaxContext();
        c.round(decimal::ROUND_HALF_UP);
        return c;
    }();
    return ctx;
}

void DecimalUtil::bytesToMagnitude(const std::string &bytes, uint8_t &sign, uint64_t &hi,
                                   uint64_t &lo) {
    const bool negative =
        !bytes.empty() && (static_cast<uint8_t>(bytes[0]) & 0x80) != 0;

    // Right-align the bytes into a sign-extended 16-byte big-endian buffer.
    uint8_t buf[16];
    std::memset(buf, negative ? 0xFF : 0x00, sizeof(buf));
    const size_t len = bytes.size();
    if (len <= sizeof(buf)) {
        const size_t off = sizeof(buf) - len;
        for (size_t i = 0; i < len; ++i) {
            buf[off + i] = static_cast<uint8_t>(bytes[i]);
        }
    } else {
        // The coefficient is carried in a 128-bit triple (hi/lo), so anything wider than
        // 16 bytes (> ~38 digits) cannot be represented. Fail fast rather than silently
        // truncate to a wrong value.
        throw std::out_of_range(
            "decimal coefficient exceeds 128 bits (max 16 bytes) and cannot be represented");
    }

    uint64_t h = 0;
    uint64_t l = 0;
    for (int i = 0; i < 8; ++i) {
        h = (h << 8) | buf[i];
    }
    for (int i = 8; i < 16; ++i) {
        l = (l << 8) | buf[i];
    }

    if (negative) {
        // magnitude = -(two's-complement value) = ~value + 1
        l = ~l;
        h = ~h;
        if (++l == 0) {
            ++h;
        }
        sign = 1;
    } else {
        sign = 0;
    }
    hi = h;
    lo = l;
}

std::string DecimalUtil::magnitudeToBytes(uint8_t sign, uint64_t hi, uint64_t lo) {
    uint64_t h = hi;
    uint64_t l = lo;
    if (sign) {
        // Negative representation = two's complement of the magnitude.
        l = ~l;
        h = ~h;
        if (++l == 0) {
            ++h;
        }
    }

    uint8_t buf[16];
    for (int i = 7; i >= 0; --i) {
        buf[i] = static_cast<uint8_t>(h & 0xFF);
        h >>= 8;
    }
    for (int i = 15; i >= 8; --i) {
        buf[i] = static_cast<uint8_t>(l & 0xFF);
        l >>= 8;
    }

    // Strip redundant leading pad bytes while the sign bit is preserved.
    const uint8_t pad = sign ? 0xFF : 0x00;
    const uint8_t signBit = sign ? 0x80 : 0x00;
    size_t start = 0;
    while (start < 15 && buf[start] == pad && (buf[start + 1] & 0x80) == signBit) {
        ++start;
    }
    return std::string(reinterpret_cast<char *>(buf + start), sizeof(buf) - start);
}

decimal::Decimal DecimalUtil::fromUnscaledBytes(const std::string &unscaled, int32_t scale) {
    uint8_t sign;
    uint64_t hi;
    uint64_t lo;
    bytesToMagnitude(unscaled, sign, hi, lo);
    mpd_uint128_triple_t triple;
    triple.tag = MPD_TRIPLE_NORMAL;
    triple.sign = sign;
    triple.hi = hi;
    triple.lo = lo;
    triple.exp = -static_cast<int64_t>(scale);
    return decimal::Decimal(triple);
}

decimal::Decimal DecimalUtil::fromProto(const confluent::type::Decimal &d) {
    return fromUnscaledBytes(d.value(), d.scale());
}

confluent::type::Decimal DecimalUtil::toProto(const decimal::Decimal &d) {
    mpd_uint128_triple_t triple = d.as_uint128_triple();
    if (triple.tag != MPD_TRIPLE_NORMAL) {
        throw std::runtime_error(
            "cannot convert non-finite or out-of-range decimal to confluent.type.Decimal");
    }
    confluent::type::Decimal out;
    out.set_value(magnitudeToBytes(triple.sign, triple.hi, triple.lo));
    out.set_scale(static_cast<int32_t>(-triple.exp));
    return out;
}

}  // namespace schemaregistry::rules::cel
