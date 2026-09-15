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
//
// Emax/Emin are widened alongside, exactly as the Python client's _DIV_CONTEXT does. A
// default-constructed Context carries Emax 999999, which is far narrower than the exponent
// range the constructor accepts (the int32 scale of confluent.type.Decimal), so dividing a
// legitimately constructed value overflowed here where Java and Python both return a result:
// `decimals.div(decimal("1e1000000"), decimal("1"))` was `[Overflow]`. Only the precision is
// part of the cross-client contract; the exponent range is each library's own, and clamping
// it below what this client's own constructor accepts made the two disagree.
decimal::Context &DecimalUtil::context() {
    static thread_local decimal::Context ctx = [] {
        decimal::Context c;
        c.prec(38);
        c.round(decimal::ROUND_HALF_UP);
        c.emax(MPD_MAX_EMAX);
        c.emin(MPD_MIN_EMIN);
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

decimal::Decimal DecimalUtil::fromUnscaledBytes(const std::string &unscaled, int32_t scale) {
    // Bounded here, not only at the decimal(bytes, scale) constructor: the protobuf and Avro
    // decoders reach this with producer-controlled bytes, and the base-256 to decimal
    // conversion below is quadratic in their length. Measured: 16 KiB takes 545 ms, 64 KiB
    // 8.2 s and 128 KiB 32.6 s. Checked from the byte count, before any digit is built - one
    // byte carries about 2.41 decimal digits. The reference needs no such bound: BigInteger
    // keeps the coefficient binary and never materialises its digits (decimals.md 4b).
    requireSaneWidth(static_cast<int64_t>(unscaled.size() * 241 / 100) + 1,
                     "decimal", "the coefficient", kSaneCoefficient);
    // Through the digits as well, so both directions take the same widths: reading was capped
    // at 16 bytes, which would have refused what the JVM routinely writes.
    bool negative = false;
    std::string digits;
    twosComplementToDigits(unscaled, negative, digits);
    decimal::Decimal value((negative ? "-" : "") + digits);
    return value.scaleb(decimal::Decimal(-static_cast<int64_t>(scale)), exactContext());
}

int64_t DecimalUtil::rescaledDigits(int64_t target_scale, const decimal::Decimal &d) {
    if (!d.isfinite()) {
        throw std::runtime_error("cannot measure a non-finite decimal");
    }
    const int64_t exponent = d.exponent();
    const int64_t digits = d.adjexp() - exponent + 1;
    return std::max<int64_t>(1, digits + target_scale + exponent);
}

int64_t DecimalUtil::plainFormLength(const decimal::Decimal &d) {
    if (!d.isfinite()) {
        throw std::runtime_error("cannot measure a non-finite decimal");
    }
    const int64_t exponent = d.exponent();
    const int64_t digits = d.adjexp() - exponent + 1;
    return digits + (exponent < 0 ? -exponent : exponent);
}

void DecimalUtil::requireSaneWidth(int64_t needed, const std::string &fn,
                                   const std::string &what, int64_t limit) {
    if (needed > limit) {
        throw std::out_of_range(fn + ": " + what + " needs " + std::to_string(needed) +
                                " digits, past this client's " + std::to_string(limit) +
                                "-digit limit");
    }
}

int64_t DecimalUtil::operandWidth(int64_t target_scale, const decimal::Decimal &d) {
    if (!d.isfinite()) {
        throw std::runtime_error("cannot measure a non-finite decimal");
    }
    // A zero contributes one digit whatever the distance: expanding a zero appends none. That
    // decides several cases outright, because alignment expands only the operand whose scale is
    // coarser. Measured on libmpdec, and the JDK agrees on every row:
    //
    //   0E+2e9 + 0E-2e9      free, 1 digit             precision 1
    //   0E+2e9 + 1           free, 1 digit             precision 1
    //   0E+2e9 mod 1E-2e9    free, 1 digit             precision 1
    //   0E-2e9 + 1           1601 MB, 2e9+1 digits     ArithmeticException
    //
    // Only the last must be refused, and the difference is purely which operand expands.
    if (d.iszero()) {
        return 1;
    }
    const int64_t exponent = d.exponent();
    const int64_t digits = d.adjexp() - exponent + 1;
    return digits + target_scale + exponent;
}

void DecimalUtil::requireAlignable(const decimal::Decimal &a, const decimal::Decimal &b,
                                   const std::string &fn) {
    // Addition and subtraction align both operands on the finer scale, so the frame is the
    // widest either of them needs there - computed per operand, because of the zero case
    // above.
    if (!a.isfinite() || !b.isfinite()) {
        throw std::runtime_error("cannot measure a non-finite decimal");
    }
    const int64_t target_scale = -std::min(a.exponent(), b.exponent());
    const int64_t needed =
        std::max(operandWidth(target_scale, a), operandWidth(target_scale, b)) + 1;
    requireSaneWidth(needed, fn, "aligning the operands");
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
    // turned a vanishingly small number into an enormous one.
    //
    // The band is on the *exponent* and is deliberately one short of the int32 scale range,
    // since scale = -exponent: scale INT32_MIN needs exponent +2147483648 and is refused.
    // Three things to know before widening it, because each is measured and the first two
    // contradict each other:
    //
    //   - It is narrower than this client's own rule. context() above widens emax/emin to
    //     MPD_MAX_EMAX precisely so the context is not "narrower than the exponent range the
    //     constructor accepts (the int32 scale of confluent.type.Decimal)". This guard is.
    //   - It is load-bearing for two other paths. The JVM accepts that scale from
    //     (unscaled, scale) - new BigDecimal(BigInteger.ONE, Integer.MIN_VALUE) is scale
    //     -2147483648 - but refuses it from parsing ("Exponent overflow" for
    //     "1e2147483648") and from rescaling ("Underflow" for setScale(-2147483648)).
    //     Parsing and decimals.round rely on *this* check for those refusals, so widening it
    //     here alone trades one false rejection for two false acceptances. Measured: it breaks
    //     WideExponentIsRefusedNotWrapped and CoarseningAScaleIsNeverRefused.
    //   - The value is not portable anyway. Go cannot represent it at all (apd's exponent is
    //     an int32, so scale INT32_MIN is unreachable there, unfixably), and JS refuses both
    //     extremes on its plain-form width ceiling.
    //
    // So: not a native-library limit, and not decimals.md 4a. Doing it properly means moving
    // the parse and rescale refusals to their own sites first.
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
    const std::string digits = coefficientDigits(d);
    // The coefficient goes out in base 256, and this codec's radix conversion is quadratic:
    // 0.04 s at 10^4 digits, 4.2 s at 10^5, ~420 s at 10^6. Nothing upstream bounded it, so a
    // rule as ordinary as `decimals.round(x, 1000000)` did not fail - it ground. That is the
    // resource this ceiling protects, and it is time rather than memory, so it is a separate
    // and much lower number than kSaneWidth. Checked from the digit string's length, before
    // the conversion runs.
    DecimalUtil::requireSaneWidth(static_cast<int64_t>(digits.size()),
                                  "confluent.type.Decimal", "the coefficient",
                                  kSaneCoefficient);
    // sign() rather than issigned(): mpdecimal implements both as `flags & MPD_NEG`, but the
    // Windows DLL does not export mpd_issigned, so that spelling failed to link (LNK2019).
    // sign() routes through mpd_isnegative, which ExtraFunc.cpp already links.
    out.set_value(digitsToTwosComplement(digits, !d.iszero() && d.sign() < 0));
    // The unscaled value's digit count, which is what BigDecimal.precision() reports. This
    // client set it nowhere, so every decimal it produced carried 0 - a value the reference
    // cannot produce, since precision() is never less than 1 (zero's precision is 1) - and a
    // JVM consumer rewrites such a message on its next touch. Every decimal that reaches the
    // wire from here originates in this function, so this one line covers the serde and the
    // CEL write-back alike. Taken from the digit string that was just encoded, so a rescale
    // upstream cannot leave it stale.
    out.set_precision(static_cast<uint32_t>(digits.size()));
    out.set_scale(static_cast<int32_t>(-exponent));
    return out;
}

}  // namespace schemaregistry::rules::cel
