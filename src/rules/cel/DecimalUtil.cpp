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

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

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

namespace {

/// Long division of a decimal digit string by 256, in place; returns the remainder byte.
///
/// The coefficient is carried through its decimal digits rather than through
/// `mpd_uint128_triple_t`, which is what bounded this codec to 128 bits.
/// `carry * 10 + digit` peaks at 255*10+9 = 2559, so each step yields one digit and one byte.
unsigned divideDigitsBy256(std::string &digits) {
    std::string quotient;
    quotient.reserve(digits.size());
    unsigned carry = 0;
    for (char c : digits) {
        const unsigned cur = carry * 10u + static_cast<unsigned>(c - '0');
        const unsigned q = cur / 256u;
        carry = cur % 256u;
        if (!quotient.empty() || q != 0) {
            quotient.push_back(static_cast<char>('0' + q));
        }
    }
    digits = quotient.empty() ? std::string("0") : quotient;
    return carry;
}

/// `digits = digits * 256 + addend`, in place. The carry stays at or below 256, so
/// `digit * 256 + carry` peaks at 2560.
void multiplyDigitsBy256(std::string &digits, unsigned addend) {
    unsigned carry = addend;
    for (size_t i = digits.size(); i-- > 0;) {
        const unsigned cur = static_cast<unsigned>(digits[i] - '0') * 256u + carry;
        digits[i] = static_cast<char>('0' + cur % 10u);
        carry = cur / 10u;
    }
    while (carry != 0) {
        digits.insert(digits.begin(), static_cast<char>('0' + carry % 10u));
        carry /= 10u;
    }
}

/// Minimal big-endian two's-complement bytes for a magnitude given as decimal digits - the
/// same form `BigInteger.toByteArray()` produces, which is what the JVM writes into
/// `confluent.type.Decimal.value`.
std::string digitsToTwosComplement(std::string digits, bool negative) {
    std::vector<uint8_t> bytes;
    while (!(digits.size() == 1 && digits[0] == '0')) {
        bytes.push_back(static_cast<uint8_t>(divideDigitsBy256(digits)));
    }
    if (bytes.empty()) {
        bytes.push_back(0);  // BigInteger.ZERO.toByteArray() is a single 0x00
    }
    std::reverse(bytes.begin(), bytes.end());

    // A magnitude whose top bit is set needs a leading zero byte or it reads back as its own
    // negation. That is the corruption the old 128-bit ceiling was guarding against, handled
    // by widening the encoding instead of refusing the value.
    if ((bytes.front() & 0x80) != 0) {
        bytes.insert(bytes.begin(), 0x00);
    }
    if (!negative) {
        return std::string(bytes.begin(), bytes.end());
    }

    for (auto &byte : bytes) {
        byte = static_cast<uint8_t>(~byte);
    }
    for (size_t i = bytes.size(); i-- > 0;) {
        if (++bytes[i] != 0) {
            break;
        }
    }
    // Back to minimal, keeping the sign bit set. -128 is the case that matters: 0x00,0x80
    // negates to 0xFF,0x80 and has to come back as the single byte 0x80, which is what
    // BigInteger produces.
    while (bytes.size() > 1 && bytes.front() == 0xFF && (bytes[1] & 0x80) != 0) {
        bytes.erase(bytes.begin());
    }
    return std::string(bytes.begin(), bytes.end());
}

/// A big-endian two's-complement byte string as a sign and a decimal digit string.
void twosComplementToDigits(const std::string &value, bool &negative,
                            std::string &digits) {
    negative = !value.empty() && (static_cast<uint8_t>(value[0]) & 0x80) != 0;
    std::vector<uint8_t> bytes(value.begin(), value.end());
    if (bytes.empty()) {
        bytes.push_back(0);  // an empty value is zero, as BigInteger reads it
    }
    if (negative) {
        for (auto &byte : bytes) {
            byte = static_cast<uint8_t>(~byte);
        }
        for (size_t i = bytes.size(); i-- > 0;) {
            if (++bytes[i] != 0) {
                break;
            }
        }
    }
    digits = "0";
    for (uint8_t byte : bytes) {
        multiplyDigitsBy256(digits, byte);
    }
}

/// The digits of a Decimal's coefficient. `coeff()` carries an exponent of zero, which
/// `to_sci` prints as plain digits; the guard is here so a change to either cannot silently
/// yield a wrong coefficient.
std::string coefficientDigits(const decimal::Decimal &d) {
    const std::string text = d.coeff().to_sci();
    if (text.find_first_not_of("0123456789") != std::string::npos) {
        throw std::runtime_error("cannot read the coefficient of a decimal: " + text);
    }
    return text;
}

}  // namespace

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
    // Through the digits as well, so both directions take the same widths: reading was capped
    // at 16 bytes, which would have refused what the JVM routinely writes.
    bool negative = false;
    std::string digits;
    twosComplementToDigits(unscaled, negative, digits);
    decimal::Decimal value((negative ? "-" : "") + digits);
    return value.scaleb(decimal::Decimal(-static_cast<int64_t>(scale)), exactContext());
}

decimal::Decimal DecimalUtil::fromProto(const confluent::type::Decimal &d) {
    return fromUnscaledBytes(d.value(), d.scale());
}

confluent::type::Decimal DecimalUtil::toProto(const decimal::Decimal &d) {
    if (!d.isfinite()) {
        throw std::runtime_error(
            "cannot convert a non-finite decimal to confluent.type.Decimal");
    }
    // The scale is a signed int32 on the wire, and negating a wide exponent silently wrapped
    // it: decimal("1e-2147483648") needs scale 2147483648, which wrapped to -2147483648 and
    // turned a vanishingly small number into an enormous one. The accepted band matches the
    // JVM's, measured: BigDecimal refuses a literal at +/-2147483648 and takes +/-2147483647.
    const int64_t exponent = d.exponent();
    if (exponent < -static_cast<int64_t>(INT32_MAX) ||
        exponent > static_cast<int64_t>(INT32_MAX)) {
        throw std::out_of_range(
            "decimal scale does not fit the confluent.type.Decimal int32 scale field");
    }
    // The coefficient goes out at whatever width it needs. `value` is a variable-length bytes
    // field and the JVM fills it from BigInteger.toByteArray(), which has no ceiling -
    // measured against the JDK, 38 nines take 16 bytes, their sum 17 and their product 32,
    // and each round-trips. This codec used to read the coefficient out of an
    // mpd_uint128_triple_t and refuse anything past signed 128 bits, so `decimals.add` on two
    // values at CEL's own documented 38-digit precision was an error here and ordinary there.
    confluent::type::Decimal out;
    out.set_value(digitsToTwosComplement(coefficientDigits(d),
                                         !d.iszero() && d.issigned()));
    out.set_scale(static_cast<int32_t>(-exponent));
    return out;
}

}  // namespace schemaregistry::rules::cel
