// Copyright 2026 Confluent Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Refer to LICENSE for more information.

#include "schemaregistry/serdes/Variant.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "absl/strings/charconv.h"
#include "absl/strings/escaping.h"

namespace schemaregistry::serdes {

namespace {

// ---- format constants (see VariantFormat / Variant.cs) ----

// Basic types (low 2 bits of the header byte).
constexpr int kPrimitive = 0;
constexpr int kShortStr = 1;
constexpr int kObjectType = 2;
constexpr int kArrayType = 3;

// Primitive type codes (upper 6 bits when basic type == Primitive).
constexpr int kTNull = 0;
constexpr int kTTrue = 1;
constexpr int kTFalse = 2;
constexpr int kTInt1 = 3;
constexpr int kTInt2 = 4;
constexpr int kTInt4 = 5;
constexpr int kTInt8 = 6;
constexpr int kTDouble = 7;
constexpr int kTDecimal4 = 8;
constexpr int kTDecimal8 = 9;
constexpr int kTDecimal16 = 10;
constexpr int kTDate = 11;
constexpr int kTTimestamp = 12;
constexpr int kTTimestampNtz = 13;
constexpr int kTFloat = 14;
constexpr int kTBinary = 15;
constexpr int kTLongStr = 16;
constexpr int kTTime = 17;
constexpr int kTTimestampNanos = 18;
constexpr int kTTimestampNanosNtz = 19;
constexpr int kTUuid = 20;

constexpr int kBasicTypeMask = 0x3;
constexpr int kBasicTypeBits = 2;
constexpr int kTypeInfoMask = 0x3F;
constexpr int kMaxShortStrSize = 0x3F;
constexpr int kVersion = 1;
constexpr int kVersionMask = 0x0F;
constexpr int kU32Size = 4;
constexpr int kBinarySearchThreshold = 32;

// ---- low-level byte helpers ----

void checkIndex(int64_t pos, size_t length) {
    if (pos < 0 || static_cast<uint64_t>(pos) >= length) {
        throw VariantException("malformed variant: index out of bounds");
    }
}

/// Reads a little-endian unsigned integer, which has to fit a non-negative int32.
///
/// That bound is the JVM's own contract for `readUnsigned` ("The value must fit into a
/// non-negative int"), enforced there by a `result < 0` check because its accumulator *is* an
/// int. The accumulator here is 64-bit, so a 4-byte count with the high bit set came back as a
/// large positive value and every caller's `static_cast<int>` then silently turned it into a
/// negative one - a field count of 0xFFFFFFFF became -1, and the object simply reported itself
/// as empty. Both counts and field ids come through here, so one check covers them.
int64_t readUnsignedLE(const std::vector<uint8_t> &data, int64_t pos,
                       int numBytes) {
    checkIndex(pos, data.size());
    checkIndex(pos + numBytes - 1, data.size());
    int64_t result = 0;
    for (int i = numBytes - 1; i >= 0; i--) {
        result = (result << 8) | data[pos + i];
    }
    if (result > std::numeric_limits<int32_t>::max()) {
        throw VariantException(
            "malformed variant: unsigned value does not fit a non-negative int");
    }
    return result;
}

int64_t readSignedLong(const std::vector<uint8_t> &data, int64_t pos,
                       int numBytes) {
    checkIndex(pos, data.size());
    checkIndex(pos + numBytes - 1, data.size());
    uint64_t result = 0;
    for (int i = numBytes - 1; i >= 0; i--) {
        result = (result << 8) | data[pos + i];
    }
    if (numBytes < 8) {
        uint64_t signBit = 1ULL << (numBytes * 8 - 1);
        if ((result & signBit) != 0) {
            result |= (~0ULL) << (numBytes * 8);
        }
    }
    return static_cast<int64_t>(result);
}

float readFloatLE(const std::vector<uint8_t> &data, int64_t pos) {
    checkIndex(pos + 3, data.size());
    uint32_t bits = 0;
    for (int i = 0; i < 4; i++) {
        bits |= static_cast<uint32_t>(data[pos + i]) << (8 * i);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

double readDoubleLE(const std::vector<uint8_t> &data, int64_t pos) {
    checkIndex(pos + 7, data.size());
    uint64_t bits = 0;
    for (int i = 0; i < 8; i++) {
        bits |= static_cast<uint64_t>(data[pos + i]) << (8 * i);
    }
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
}

// ---- calendar (proleptic Gregorian, matches Java/Python) ----

// Howard Hinnant's civil_from_days: days since 1970-01-01 -> (y, m, d).
void civilFromDays(int64_t z, int64_t &year, int &month, int &day) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;                       // [0, 146096]
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);  // [0, 365]
    int64_t mp = (5 * doy + 2) / 153;                       // [0, 11]
    int64_t d = doy - (153 * mp + 2) / 5 + 1;               // [1, 31]
    int64_t m = mp < 10 ? mp + 3 : mp - 9;                  // [1, 12]
    year = y + (m <= 2 ? 1 : 0);
    month = static_cast<int>(m);
    day = static_cast<int>(d);
}

int64_t floorDiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

int64_t floorMod(int64_t a, int64_t b) {
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

// ---- formatting (cross-language contract) ----

std::string frac(int64_t nano) {
    char buf[16];
    if (nano == 0) return "";
    if (nano % 1000000 == 0) {
        std::snprintf(buf, sizeof(buf), ".%03lld",
                      static_cast<long long>(nano / 1000000));
        return buf;
    }
    if (nano % 1000 == 0) {
        std::snprintf(buf, sizeof(buf), ".%06lld",
                      static_cast<long long>(nano / 1000));
        return buf;
    }
    std::snprintf(buf, sizeof(buf), ".%09lld", static_cast<long long>(nano));
    return buf;
}

// The range a timestamp may occupy when rendered to JSON, in microseconds since the epoch:
// 0001-01-01T00:00:00 through 9999-12-31T23:59:59.999999. That is the four-digit-year form every
// client renders: for the zone-aware types it is RFC 3339 - also google.protobuf.Timestamp's range,
// and the range timestamp(...) enforces when constructing a CEL timestamp. The zone-less types
// carry no offset, so they are ISO-8601 local date-times rather than RFC 3339 (which has no
// zone-less form); they share the range so both stay readable by the same date parsers.
//
// A variant TIMESTAMP_TZ / TIMESTAMP_NTZ is an arbitrary int64 of microseconds - roughly
// +/-292,471 years - so it can hold instants outside that form. Those are refused rather than
// rendered: ISO-8601's expanded year ("+10000-01-01T00:00:00Z") is not RFC 3339 and would not parse
// back through parseJson. It is also exactly what Python's datetime and .NET's DateTime can hold,
// so every client can enforce it natively. The nanosecond-based types need no check, because an
// int64 of nanoseconds spans only 1677-2262, inside this range at both ends.
constexpr int64_t kMinTimestampMicros = -62135596800000000LL;
constexpr int64_t kMaxTimestampMicros = 253402300799999999LL;

void checkMicrosRange(int64_t micros) {
    if (micros < kMinTimestampMicros || micros > kMaxTimestampMicros) {
        throw VariantException(
            "timestamp microseconds (" + std::to_string(micros) + ") must be in range [" +
            std::to_string(kMinTimestampMicros) + ", " + std::to_string(kMaxTimestampMicros) + "]");
    }
}

std::string formatDateTimeParts(int64_t sec, int64_t nano, bool utc) {
    int64_t days = floorDiv(sec, 86400LL);
    int64_t secOfDay = floorMod(sec, 86400LL);
    int64_t year;
    int month, day;
    civilFromDays(days, year, month, day);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04lld-%02d-%02dT%02lld:%02lld:%02lld%s%s",
                  static_cast<long long>(year), month, day,
                  static_cast<long long>(secOfDay / 3600),
                  static_cast<long long>((secOfDay % 3600) / 60),
                  static_cast<long long>(secOfDay % 60), frac(nano).c_str(),
                  utc ? "Z" : "");
    return buf;
}

std::string formatInstantParts(int64_t sec, int64_t nano) {
    return formatDateTimeParts(sec, nano, true);
}

std::string formatLocalDateTimeParts(int64_t sec, int64_t nano) {
    return formatDateTimeParts(sec, nano, false);
}

std::string formatInstant(int64_t totalNanos) {
    return formatInstantParts(floorDiv(totalNanos, 1000000000LL),
                              floorMod(totalNanos, 1000000000LL));
}

std::string formatLocalDateTime(int64_t totalNanos) {
    return formatLocalDateTimeParts(floorDiv(totalNanos, 1000000000LL),
                                    floorMod(totalNanos, 1000000000LL));
}

// These split microseconds into whole seconds before scaling the remainder to nanoseconds.
// Converting to nanoseconds first overflowed int64 past 9223372036854775 micros
// (2262-04-11T23:47:16.854775Z) and wrapped silently to a plausible-looking date in the past -
// year 10000 rendered as 1816. The whole renderable range cannot be expressed in nanoseconds at
// all, so the split is required, not merely safer.
std::string formatInstantMicros(int64_t micros) {
    checkMicrosRange(micros);
    return formatInstantParts(floorDiv(micros, 1000000LL), floorMod(micros, 1000000LL) * 1000LL);
}

std::string formatLocalDateTimeMicros(int64_t micros) {
    checkMicrosRange(micros);
    return formatLocalDateTimeParts(floorDiv(micros, 1000000LL),
                                    floorMod(micros, 1000000LL) * 1000LL);
}

// The range a TIME may occupy, in microseconds since midnight: 00:00:00 through 23:59:59.999999.
// RFC 3339's partial-time requires time-hour = 2DIGIT in 00-23, so a value at or past 24 hours (or
// negative) has no valid form. A variant TIME is an int64 of microseconds, so those are reachable
// and are refused rather than rendered; checking also removes an overflow, since micros * 1000
// wraps for a large enough value.
constexpr int64_t kMinTimeMicros = 0;
constexpr int64_t kMaxTimeMicros = 86400000000LL - 1;

// The range a DATE may occupy, in days since the epoch: 0001-01-01 through 9999-12-31. RFC 3339's
// full-date requires date-fullyear = 4DIGIT, so an expanded or negative year is not a valid
// full-date. A variant DATE is an int32 of days - roughly +/-5.8 million years - so those are
// reachable and are refused too.
constexpr int64_t kMinDateEpochDay = -719162;
constexpr int64_t kMaxDateEpochDay = 2932896;

std::string formatLocalTime(int64_t micros) {
    if (micros < kMinTimeMicros || micros > kMaxTimeMicros) {
        throw VariantException(
            "time microseconds of day (" + std::to_string(micros) + ") must be in range [" +
            std::to_string(kMinTimeMicros) + ", " + std::to_string(kMaxTimeMicros) + "]");
    }
    int64_t nanoOfDay = micros * 1000LL;
    int64_t secs = floorDiv(nanoOfDay, 1000000000LL);
    int64_t nano = floorMod(nanoOfDay, 1000000000LL);
    int64_t hour = secs / 3600;
    int64_t rem = secs % 3600;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld%s",
                  static_cast<long long>(hour), static_cast<long long>(rem / 60),
                  static_cast<long long>(rem % 60), frac(nano).c_str());
    return buf;
}

std::string formatDate(int64_t days) {
    if (days < kMinDateEpochDay || days > kMaxDateEpochDay) {
        throw VariantException(
            "date epoch day (" + std::to_string(days) + ") must be in range [" +
            std::to_string(kMinDateEpochDay) + ", " + std::to_string(kMaxDateEpochDay) + "]");
    }
    int64_t year;
    int month, day;
    civilFromDays(days, year, month, day);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04lld-%02d-%02d",
                  static_cast<long long>(year), month, day);
    return buf;
}

// Integral doubles render as N.0; other values use the shortest round-trip.
// (Scientific-notation edge cases for very large/small magnitudes are a known
// minor divergence from Java's Double.toString.)
std::string formatDouble(double d) {
    // Non-finite values render as bare (unquoted) JSON tokens, matching the
    // Java contract (which diverges from Spark's quoted rendering). std::to_chars
    // and snprintf("%g") emit lowercase nan/inf, so special-case explicitly.
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
    if (d == std::floor(d) && std::abs(d) < 1e16) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld.0",
                      static_cast<long long>(d));
        return buf;
    }
    // Shortest decimal that round-trips to the same double. std::to_chars with
    // no format guarantees the shortest round-trippable representation and is
    // locale-independent (unlike snprintf("%.*g")+strtod).
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), d);
    std::string s(buf, res.ptr);
    // Mirror the integer ".0" convention for any finite, non-scientific result
    // that lacks a decimal point (defensive; integers handled above).
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find('E') == std::string::npos) {
        s += ".0";
    }
    return s;
}

std::string formatFloat(float f) {
    // Non-finite values render as bare (unquoted) JSON tokens, matching the
    // Java contract (which diverges from Spark's quoted rendering). std::to_chars
    // and snprintf("%g") emit lowercase nan/inf, so special-case explicitly.
    if (std::isnan(f)) return "NaN";
    if (std::isinf(f)) return f > 0 ? "Infinity" : "-Infinity";
    if (f == std::floor(f) && std::abs(f) < 1e16f) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld.0",
                      static_cast<long long>(f));
        return buf;
    }
    // Shortest decimal that round-trips to the same float32. std::to_chars with
    // no format guarantees the shortest round-trippable representation.
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), f);
    std::string s(buf, res.ptr);
    // Mirror formatDouble's integer ".0" convention for any finite, non-scientific
    // result that lacks a decimal point (defensive; integers handled above).
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find('E') == std::string::npos) {
        s += ".0";
    }
    return s;
}

std::string formatUuid(const std::vector<uint8_t> &data, int64_t start) {
    static const char *hex = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        uint8_t b = data[start + i];
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0xF]);
    }
    return out;
}

// JSON-escape a string into a quoted JSON string literal.
std::string jsonQuote(const std::string &s) { return nlohmann::json(s).dump(); }

// Exact fixed-point string for magnitude digits * 10^-scale (never scientific).
std::string decimalPlainString(bool negative, std::string digits, int scale) {
    // Strip leading zeros (keep at least one digit).
    size_t nz = digits.find_first_not_of('0');
    if (nz == std::string::npos) {
        digits = "0";
    } else if (nz > 0) {
        digits = digits.substr(nz);
    }
    std::string sign = (negative && digits != "0") ? "-" : "";
    if (scale == 0) return sign + digits;
    if (static_cast<int>(digits.size()) <= scale) {
        digits = std::string(scale - digits.size() + 1, '0') + digits;
    }
    int point = static_cast<int>(digits.size()) - scale;
    return sign + digits.substr(0, point) + "." + digits.substr(point);
}

// Convert little-endian magnitude bytes to a base-10 digit string ("0" if 0).
std::string leMagnitudeToDecimal(const std::vector<uint8_t> &le) {
    std::vector<uint8_t> dec;  // little-endian decimal digits
    for (int i = static_cast<int>(le.size()) - 1; i >= 0; i--) {
        int carry = le[i];
        for (size_t j = 0; j < dec.size(); j++) {
            int v = dec[j] * 256 + carry;
            dec[j] = static_cast<uint8_t>(v % 10);
            carry = v / 10;
        }
        while (carry) {
            dec.push_back(static_cast<uint8_t>(carry % 10));
            carry /= 10;
        }
    }
    if (dec.empty()) return "0";
    std::string out;
    out.reserve(dec.size());
    for (int i = static_cast<int>(dec.size()) - 1; i >= 0; i--) {
        out.push_back(static_cast<char>('0' + dec[i]));
    }
    return out;
}

}  // namespace

// ---- Variant ----

Variant::Variant(std::vector<uint8_t> value, std::vector<uint8_t> metadata)
    : value_(std::make_shared<const std::vector<uint8_t>>(std::move(value))),
      metadata_(
          std::make_shared<const std::vector<uint8_t>>(std::move(metadata))),
      pos_(0) {
    checkVersion();
}

Variant::Variant(Buffer value, Buffer metadata, size_t pos)
    : value_(std::move(value)), metadata_(std::move(metadata)), pos_(pos) {
    checkVersion();
}

void Variant::checkVersion() const {
    checkIndex(0, metadata_->size());
    if (((*metadata_)[0] & kVersionMask) != kVersion) {
        throw VariantException("unsupported variant metadata version: " +
                               std::to_string((*metadata_)[0] & kVersionMask));
    }
}

std::vector<uint8_t> Variant::standaloneValueBytes() const {
    const auto &v = *value_;
    if (pos_ >= v.size()) {
        return {};
    }
    return std::vector<uint8_t>(v.begin() + pos_, v.end());
}

VariantType Variant::getType() const {
    const auto &value = *value_;
    checkIndex(pos_, value.size());
    int basicType = value[pos_] & kBasicTypeMask;
    int typeInfo = (value[pos_] >> kBasicTypeBits) & kTypeInfoMask;
    switch (basicType) {
        case kShortStr:
            return VariantType::String;
        case kObjectType:
            return VariantType::Object;
        case kArrayType:
            return VariantType::Array;
        default:
            break;
    }
    switch (typeInfo) {
        case kTNull:
            return VariantType::Null;
        case kTTrue:
        case kTFalse:
            return VariantType::Boolean;
        case kTInt1:
            return VariantType::Byte;
        case kTInt2:
            return VariantType::Short;
        case kTInt4:
            return VariantType::Int;
        case kTInt8:
            return VariantType::Long;
        case kTDouble:
            return VariantType::Double;
        case kTDecimal4:
            return VariantType::Decimal4;
        case kTDecimal8:
            return VariantType::Decimal8;
        case kTDecimal16:
            return VariantType::Decimal16;
        case kTDate:
            return VariantType::Date;
        case kTTimestamp:
            return VariantType::TimestampTz;
        case kTTimestampNtz:
            return VariantType::TimestampNtz;
        case kTFloat:
            return VariantType::Float;
        case kTBinary:
            return VariantType::Binary;
        case kTLongStr:
            return VariantType::String;
        case kTTime:
            return VariantType::Time;
        case kTTimestampNanos:
            return VariantType::TimestampNanosTz;
        case kTTimestampNanosNtz:
            return VariantType::TimestampNanosNtz;
        case kTUuid:
            return VariantType::Uuid;
        default:
            throw VariantException("unknown variant primitive type: " +
                                   std::to_string(typeInfo));
    }
}

int Variant::primitiveInfo() const {
    const auto &value = *value_;
    checkIndex(pos_, value.size());
    int basicType = value[pos_] & kBasicTypeMask;
    if (basicType != kPrimitive) {
        throw VariantException("expected a primitive variant value");
    }
    return (value[pos_] >> kBasicTypeBits) & kTypeInfoMask;
}

bool Variant::getBoolean() const {
    int ti = primitiveInfo();
    if (ti != kTTrue && ti != kTFalse) {
        throw VariantException("variant is not a boolean");
    }
    return ti == kTTrue;
}

int8_t Variant::getByte() const {
    int ti = primitiveInfo();
    const auto &value = *value_;
    if (ti != kTInt1) throw VariantException("variant is not a byte");
    return static_cast<int8_t>(readSignedLong(value, pos_ + 1, 1));
}

int16_t Variant::getShort() const {
    int ti = primitiveInfo();
    const auto &value = *value_;
    switch (ti) {
        case kTInt1:
            return static_cast<int16_t>(readSignedLong(value, pos_ + 1, 1));
        case kTInt2:
            return static_cast<int16_t>(readSignedLong(value, pos_ + 1, 2));
        default:
            throw VariantException("variant is not a short");
    }
}

int32_t Variant::getInt() const {
    int ti = primitiveInfo();
    const auto &value = *value_;
    switch (ti) {
        case kTInt1:
            return static_cast<int32_t>(readSignedLong(value, pos_ + 1, 1));
        case kTInt2:
            return static_cast<int32_t>(readSignedLong(value, pos_ + 1, 2));
        case kTInt4:
            return static_cast<int32_t>(readSignedLong(value, pos_ + 1, 4));
        default:
            throw VariantException("variant is not an int");
    }
}

int64_t Variant::getLong() const {
    int ti = primitiveInfo();
    const auto &value = *value_;
    switch (ti) {
        case kTInt1:
            return readSignedLong(value, pos_ + 1, 1);
        case kTInt2:
            return readSignedLong(value, pos_ + 1, 2);
        case kTInt4:
        case kTDate:
            return readSignedLong(value, pos_ + 1, 4);
        case kTInt8:
        case kTTimestamp:
        case kTTimestampNtz:
        case kTTime:
        case kTTimestampNanos:
        case kTTimestampNanosNtz:
            return readSignedLong(value, pos_ + 1, 8);
        default:
            throw VariantException("variant is not an integer-backed type");
    }
}

float Variant::getFloat() const {
    int ti = primitiveInfo();
    const auto &value = *value_;
    if (ti != kTFloat) throw VariantException("variant is not a float");
    return readFloatLE(value, pos_ + 1);
}

double Variant::getDouble() const {
    int ti = primitiveInfo();
    const auto &value = *value_;
    if (ti == kTDouble) return readDoubleLE(value, pos_ + 1);
    throw VariantException("variant is not a double");
}

void Variant::getDecimalParts(std::vector<uint8_t> &unscaledBigEndian,
                              int &scale) const {
    int ti = primitiveInfo();
    const auto &value = *value_;
    checkIndex(pos_ + 1, value.size());
    scale = value[pos_ + 1];
    int width;
    if (ti == kTDecimal4)
        width = 4;
    else if (ti == kTDecimal8)
        width = 8;
    else if (ti == kTDecimal16)
        width = 16;
    else
        throw VariantException("variant is not a decimal");
    checkIndex(pos_ + 2 + width - 1, value.size());
    // Value bytes are little-endian two's-complement; reverse for big-endian.
    unscaledBigEndian.resize(width);
    for (int i = 0; i < width; i++) {
        unscaledBigEndian[i] = value[pos_ + 2 + (width - 1 - i)];
    }
}

std::string Variant::getDecimalString() const {
    std::vector<uint8_t> be;
    int scale;
    getDecimalParts(be, scale);
    bool negative = !be.empty() && (be[0] & 0x80) != 0;
    // Little-endian magnitude bytes.
    std::vector<uint8_t> le(be.rbegin(), be.rend());
    if (negative) {
        // Two's-complement negate to get the magnitude.
        int carry = 1;
        for (size_t i = 0; i < le.size(); i++) {
            int v = static_cast<uint8_t>(~le[i]) + carry;
            le[i] = static_cast<uint8_t>(v & 0xFF);
            carry = v >> 8;
        }
    }
    std::string digits = leMagnitudeToDecimal(le);
    return decimalPlainString(negative, digits, scale);
}

std::vector<uint8_t> Variant::getBinary() const {
    int ti = primitiveInfo();
    const auto &value = *value_;
    if (ti != kTBinary) throw VariantException("variant is not binary");
    int64_t length = readUnsignedLE(value, pos_ + 1, kU32Size);
    int64_t start = pos_ + 1 + kU32Size;
    checkIndex(start + length - 1, value.size());
    return std::vector<uint8_t>(value.begin() + start,
                                value.begin() + start + length);
}

std::string Variant::getUuid() const {
    int ti = primitiveInfo();
    const auto &value = *value_;
    if (ti != kTUuid) throw VariantException("variant is not a uuid");
    int64_t start = pos_ + 1;
    checkIndex(start + 15, value.size());
    return formatUuid(value, start);
}

std::string Variant::getString() const {
    const auto &value = *value_;
    checkIndex(pos_, value.size());
    int basicType = value[pos_] & kBasicTypeMask;
    int typeInfo = (value[pos_] >> kBasicTypeBits) & kTypeInfoMask;
    int64_t start, length;
    if (basicType == kShortStr) {
        start = pos_ + 1;
        length = typeInfo;
    } else if (basicType == kPrimitive && typeInfo == kTLongStr) {
        length = readUnsignedLE(value, pos_ + 1, kU32Size);
        start = pos_ + 1 + kU32Size;
    } else {
        throw VariantException("variant is not a string");
    }
    if (length == 0) return "";
    checkIndex(start + length - 1, value.size());
    return std::string(value.begin() + start, value.begin() + start + length);
}

void Variant::objectInfo(int &numFields, int &idSize, int &offsetSize,
                         int &idStart, int &offsetStart, int &dataStart) const {
    const auto &value = *value_;
    checkIndex(pos_, value.size());
    int basicType = value[pos_] & kBasicTypeMask;
    int typeInfo = (value[pos_] >> kBasicTypeBits) & kTypeInfoMask;
    if (basicType != kObjectType) {
        throw VariantException("variant is not an object");
    }
    bool largeSize = ((typeInfo >> 4) & 0x1) != 0;
    int sizeBytes = largeSize ? kU32Size : 1;
    // In 64 bits, then range-checked. A count near int32 max times an id size of 4 overflows
    // int, and where the JVM's int arithmetic wraps to a negative that its checkIndex then
    // rejects, the same wrap in C++ is undefined behaviour. Computing wide and rejecting a
    // table that does not fit the buffer reaches the JVM's outcome - an exception - without
    // relying on it.
    const int64_t count = readUnsignedLE(value, pos_ + 1, sizeBytes);
    const int64_t ids = pos_ + 1 + sizeBytes;
    const int64_t offsets = ids + count * (((typeInfo >> 2) & 0x3) + 1);
    const int64_t data = offsets + (count + 1) * ((typeInfo & 0x3) + 1);
    if (data > static_cast<int64_t>(value.size())) {
        throw VariantException(
            "malformed variant: object id and offset tables do not fit the value");
    }
    numFields = static_cast<int>(count);
    idSize = ((typeInfo >> 2) & 0x3) + 1;
    offsetSize = (typeInfo & 0x3) + 1;
    idStart = static_cast<int>(ids);
    offsetStart = static_cast<int>(offsets);
    dataStart = static_cast<int>(data);
}

void Variant::arrayInfo(int &numFields, int &offsetSize, int &offsetStart,
                        int &dataStart) const {
    const auto &value = *value_;
    checkIndex(pos_, value.size());
    int basicType = value[pos_] & kBasicTypeMask;
    int typeInfo = (value[pos_] >> kBasicTypeBits) & kTypeInfoMask;
    if (basicType != kArrayType) {
        throw VariantException("variant is not an array");
    }
    bool largeSize = ((typeInfo >> 2) & 0x1) != 0;
    int sizeBytes = largeSize ? kU32Size : 1;
    // Wide arithmetic then a range check, for the reason given in objectInfo.
    const int64_t count = readUnsignedLE(value, pos_ + 1, sizeBytes);
    const int64_t offsets = pos_ + 1 + sizeBytes;
    const int64_t data = offsets + (count + 1) * ((typeInfo & 0x3) + 1);
    if (data > static_cast<int64_t>(value.size())) {
        throw VariantException(
            "malformed variant: array offset table does not fit the value");
    }
    numFields = static_cast<int>(count);
    offsetSize = (typeInfo & 0x3) + 1;
    offsetStart = static_cast<int>(offsets);
    dataStart = static_cast<int>(data);
}

int Variant::numObjectFields() const {
    int n, idSize, offsetSize, idStart, offsetStart, dataStart;
    objectInfo(n, idSize, offsetSize, idStart, offsetStart, dataStart);
    return n;
}

int Variant::numArrayElements() const {
    int n, offsetSize, offsetStart, dataStart;
    arrayInfo(n, offsetSize, offsetStart, dataStart);
    return n;
}

std::optional<Variant> Variant::getFieldByKey(const std::string &key) const {
    int numFields, idSize, offsetSize, idStart, offsetStart, dataStart;
    objectInfo(numFields, idSize, offsetSize, idStart, offsetStart, dataStart);
    const auto &value = *value_;
    if (numFields < kBinarySearchThreshold) {
        for (int i = 0; i < numFields; i++) {
            int id = static_cast<int>(
                readUnsignedLE(value, idStart + idSize * i, idSize));
            if (getMetadataKey(id) == key) {
                int offset = static_cast<int>(readUnsignedLE(
                    value, offsetStart + offsetSize * i, offsetSize));
                return Variant(value_, metadata_, dataStart + offset);
            }
        }
        return std::nullopt;
    }
    int low = 0, high = numFields - 1;
    while (low <= high) {
        int mid = (low + high) >> 1;
        int midId = static_cast<int>(
            readUnsignedLE(value, idStart + idSize * mid, idSize));
        int cmp = getMetadataKey(midId).compare(key);
        if (cmp < 0) {
            low = mid + 1;
        } else if (cmp > 0) {
            high = mid - 1;
        } else {
            int offset = static_cast<int>(readUnsignedLE(
                value, offsetStart + offsetSize * mid, offsetSize));
            return Variant(value_, metadata_, dataStart + offset);
        }
    }
    return std::nullopt;
}

std::pair<std::string, Variant> Variant::getFieldAtIndex(int idx) const {
    int numFields, idSize, offsetSize, idStart, offsetStart, dataStart;
    objectInfo(numFields, idSize, offsetSize, idStart, offsetStart, dataStart);
    const auto &value = *value_;
    int id = static_cast<int>(
        readUnsignedLE(value, idStart + idSize * idx, idSize));
    int offset = static_cast<int>(
        readUnsignedLE(value, offsetStart + offsetSize * idx, offsetSize));
    return std::pair<std::string, Variant>(
        getMetadataKey(id), Variant(value_, metadata_, dataStart + offset));
}

std::optional<Variant> Variant::getElementAtIndex(int index) const {
    int numFields, offsetSize, offsetStart, dataStart;
    arrayInfo(numFields, offsetSize, offsetStart, dataStart);
    if (index < 0 || index >= numFields) return std::nullopt;
    const auto &value = *value_;
    int offset = static_cast<int>(
        readUnsignedLE(value, offsetStart + offsetSize * index, offsetSize));
    return Variant(value_, metadata_, dataStart + offset);
}

std::string Variant::getMetadataKey(int id) const {
    const auto &metadata = *metadata_;
    checkIndex(0, metadata.size());
    int offsetSize = ((metadata[0] >> 6) & 0x3) + 1;
    const int64_t dictSize = readUnsignedLE(metadata, 1, offsetSize);
    // `id` is read through readUnsignedLE too, so it cannot be negative here; the check is
    // kept explicit because a negative one would otherwise pass the upper bound.
    if (id < 0 || id >= dictSize) {
        throw VariantException("malformed variant: field id out of range");
    }
    const int64_t stringTable = 1 + (dictSize + 2) * static_cast<int64_t>(offsetSize);
    if (stringTable > static_cast<int64_t>(metadata.size())) {
        throw VariantException(
            "malformed variant: metadata offset table does not fit the metadata");
    }
    int stringStart = static_cast<int>(stringTable);
    int offset = static_cast<int>(
        readUnsignedLE(metadata, 1 + (id + 1) * offsetSize, offsetSize));
    int nextOffset = static_cast<int>(
        readUnsignedLE(metadata, 1 + (id + 2) * offsetSize, offsetSize));
    if (offset > nextOffset) {
        throw VariantException(
            "malformed variant: non-monotonic metadata offsets");
    }
    if (nextOffset == offset) return "";
    checkIndex(stringStart + nextOffset - 1, metadata.size());
    return std::string(metadata.begin() + stringStart + offset,
                       metadata.begin() + stringStart + nextOffset);
}

std::string Variant::toJson() const {
    std::string out;
    writeJson(out);
    return out;
}

void Variant::writeJson(std::string &out) const {
    VariantType t = getType();
    switch (t) {
        case VariantType::Object: {
            out.push_back('{');
            int n = numObjectFields();
            for (int i = 0; i < n; i++) {
                if (i > 0) out.push_back(',');
                auto field = getFieldAtIndex(i);
                out.append(jsonQuote(field.first));
                out.push_back(':');
                field.second.writeJson(out);
            }
            out.push_back('}');
            break;
        }
        case VariantType::Array: {
            out.push_back('[');
            int n = numArrayElements();
            for (int i = 0; i < n; i++) {
                if (i > 0) out.push_back(',');
                getElementAtIndex(i)->writeJson(out);
            }
            out.push_back(']');
            break;
        }
        case VariantType::Null:
            out.append("null");
            break;
        case VariantType::Boolean:
            out.append(getBoolean() ? "true" : "false");
            break;
        case VariantType::String:
            out.append(jsonQuote(getString()));
            break;
        case VariantType::Byte:
        case VariantType::Short:
        case VariantType::Int:
        case VariantType::Long:
            out.append(std::to_string(getLong()));
            break;
        case VariantType::Float:
            out.append(formatFloat(getFloat()));
            break;
        case VariantType::Double:
            out.append(formatDouble(getDouble()));
            break;
        case VariantType::Decimal4:
        case VariantType::Decimal8:
        case VariantType::Decimal16:
            out.append(getDecimalString());
            break;
        case VariantType::Date:
            out.push_back('"');
            out.append(formatDate(getLong()));
            out.push_back('"');
            break;
        case VariantType::TimestampTz:
            out.push_back('"');
            out.append(formatInstantMicros(getLong()));
            out.push_back('"');
            break;
        case VariantType::TimestampNtz:
            out.push_back('"');
            out.append(formatLocalDateTimeMicros(getLong()));
            out.push_back('"');
            break;
        case VariantType::TimestampNanosTz:
            out.push_back('"');
            out.append(formatInstant(getLong()));
            out.push_back('"');
            break;
        case VariantType::TimestampNanosNtz:
            out.push_back('"');
            out.append(formatLocalDateTime(getLong()));
            out.push_back('"');
            break;
        case VariantType::Time:
            out.push_back('"');
            out.append(formatLocalTime(getLong()));
            out.push_back('"');
            break;
        case VariantType::Binary: {
            out.push_back('"');
            auto bin = getBinary();
            std::string b64;
            absl::Base64Escape(
                absl::string_view(reinterpret_cast<const char *>(bin.data()),
                                  bin.size()),
                &b64);
            out.append(b64);
            out.push_back('"');
            break;
        }
        case VariantType::Uuid:
            out.push_back('"');
            out.append(getUuid());
            out.push_back('"');
            break;
        default:
            throw VariantException("unsupported variant type for JSON");
    }
}

// ---- builder (streaming writer + JSON bridge) ----

namespace {

int integerSize(int64_t v) {
    if (v <= 0xFF) return 1;
    if (v <= 0xFFFF) return 2;
    if (v <= 0xFFFFFF) return 3;
    return 4;
}

void appendUintLE(std::vector<uint8_t> &out, int64_t v, int numBytes) {
    for (int i = 0; i < numBytes; i++) {
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

void appendLongLE(std::vector<uint8_t> &out, int64_t v, int width) {
    for (int i = 0; i < width; i++) {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        v >>= 8;
    }
}

uint8_t primitiveHeader(int typeCode) {
    return static_cast<uint8_t>((typeCode << 2) | kPrimitive);
}

struct FieldEntry {
    std::string key;
    int id;
    int offset;
};

}  // namespace

// VariantBuilder::Impl holds the streaming core: the value byte stream, the
// metadata key dictionary, and the nesting stack. Each append*() records its
// slot in the parent container (or the root) via beforeValue(), then emits the
// value bytes; a container's header is inserted on end*(). This is the same
// machinery the JSON path uses - Variant::parseJson drives it through a SAX
// handler, so builder output is byte-identical to parseJson.
struct VariantBuilder::Impl {
    struct Ctx {
        bool isObject = false;
        int start = 0;
        std::vector<FieldEntry> fields;  // objects
        std::vector<int> offsets;        // arrays
        bool hasPendingKey = false;
        std::string pendingKey;
        int pendingId = 0;
    };

    std::vector<uint8_t> value;
    std::unordered_map<std::string, int> dictionary;
    std::vector<std::vector<uint8_t>> dictionaryKeys;
    std::vector<Ctx> stack;
    bool rootWritten = false;

    // ---- nesting-stack bookkeeping ----

    // Register the slot the value about to be written occupies in its parent
    // container (or the root). Must be called before emitting any value bytes.
    void beforeValue() {
        if (stack.empty()) {
            if (rootWritten) {
                throw VariantException("VariantBuilder: multiple root values");
            }
            rootWritten = true;
            return;
        }
        Ctx &c = stack.back();
        if (c.isObject) {
            if (!c.hasPendingKey) {
                throw VariantException(
                    "VariantBuilder: value appended without a preceding "
                    "appendKey");
            }
            c.fields.push_back(FieldEntry{
                c.pendingKey, c.pendingId,
                static_cast<int>(value.size()) - c.start});
            c.hasPendingKey = false;
        } else {
            c.offsets.push_back(static_cast<int>(value.size()) - c.start);
        }
    }

    int addKey(const std::string &key) {
        auto it = dictionary.find(key);
        if (it != dictionary.end()) return it->second;
        int id = static_cast<int>(dictionaryKeys.size());
        dictionary[key] = id;
        dictionaryKeys.push_back(std::vector<uint8_t>(key.begin(), key.end()));
        return id;
    }

    // ---- pure emit helpers (no slot bookkeeping) ----

    void writeIntWidth(int code, int64_t v, int width) {
        value.push_back(primitiveHeader(code));
        appendLongLE(value, v, width);
    }

    void writeFloat(float f) {
        value.push_back(primitiveHeader(kTFloat));
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        for (int i = 0; i < 4; i++) {
            value.push_back(static_cast<uint8_t>((bits >> (8 * i)) & 0xFF));
        }
    }

    void writeDouble(double d) {
        value.push_back(primitiveHeader(kTDouble));
        uint64_t bits;
        std::memcpy(&bits, &d, sizeof(bits));
        for (int i = 0; i < 8; i++) {
            value.push_back(static_cast<uint8_t>((bits >> (8 * i)) & 0xFF));
        }
    }

    void writeString(const std::string &s) {
        if (static_cast<int>(s.size()) > kMaxShortStrSize) {
            value.push_back(primitiveHeader(kTLongStr));
            appendUintLE(value, static_cast<int64_t>(s.size()), kU32Size);
        } else {
            value.push_back(static_cast<uint8_t>((s.size() << 2) | kShortStr));
        }
        value.insert(value.end(), s.begin(), s.end());
    }

    // Smallest-width integer (the JSON classification).
    void writeIntAuto(int64_t i) {
        if (i >= -128 && i <= 127) {
            writeIntWidth(kTInt1, i, 1);
        } else if (i >= -32768 && i <= 32767) {
            writeIntWidth(kTInt2, i, 2);
        } else if (i >= -2147483648LL && i <= 2147483647LL) {
            writeIntWidth(kTInt4, i, 4);
        } else {
            writeIntWidth(kTInt8, i, 8);
        }
    }

    // Emit a decimal from a magnitude digit string + sign at the given scale.
    void writeDecimalDigits(bool negative, std::string digits, int scale) {
        // The encoding stores the scale in a single unsigned byte, so a negative scale would
        // wrap (-1 becomes 255) and change the value on decode.
        if (scale < 0) {
            throw VariantException("decimal scale must be non-negative");
        }
        // Normalize leading zeros (BigInteger.ToString has none).
        size_t nz = digits.find_first_not_of('0');
        if (nz == std::string::npos) {
            digits = "0";
        } else if (nz > 0) {
            digits = digits.substr(nz);
        }
        int numDigits = static_cast<int>(digits.size());
        int code, width;
        if (scale <= 9 && numDigits <= 9) {
            code = kTDecimal4;
            width = 4;
        } else if (scale <= 18 && numDigits <= 18) {
            code = kTDecimal8;
            width = 8;
        } else if (scale <= 38 && numDigits <= 38) {
            code = kTDecimal16;
            width = 16;
        } else {
            throw VariantException("decimal exceeds maximum precision (38)");
        }
        value.push_back(primitiveHeader(code));
        value.push_back(static_cast<uint8_t>(scale));
        appendTwosComplementLE(negative, digits, width);
    }

    // Append width bytes of little-endian two's-complement for (+/-)digits.
    void appendTwosComplementLE(bool negative, const std::string &digits,
                                int width) {
        std::vector<uint8_t> le;  // little-endian magnitude bytes
        for (char c : digits) {
            int carry = c - '0';
            for (size_t j = 0; j < le.size(); j++) {
                int v = le[j] * 10 + carry;
                le[j] = static_cast<uint8_t>(v & 0xFF);
                carry = v >> 8;
            }
            while (carry) {
                le.push_back(static_cast<uint8_t>(carry & 0xFF));
                carry >>= 8;
            }
        }
        std::vector<uint8_t> out(width, 0);
        for (size_t i = 0; i < le.size() && i < static_cast<size_t>(width);
             i++) {
            out[i] = le[i];
        }
        if (negative) {
            int carry = 1;
            for (int i = 0; i < width; i++) {
                int v = static_cast<uint8_t>(~out[i]) + carry;
                out[i] = static_cast<uint8_t>(v & 0xFF);
                carry = v >> 8;
            }
        }
        value.insert(value.end(), out.begin(), out.end());
    }

    // ---- streaming scalar appends (slot + emit) ----

    void appendNull() {
        beforeValue();
        value.push_back(primitiveHeader(kTNull));
    }
    void appendBoolean(bool b) {
        beforeValue();
        value.push_back(primitiveHeader(b ? kTTrue : kTFalse));
    }
    void appendByte(int8_t v) { beforeValue(); writeIntWidth(kTInt1, v, 1); }
    void appendShort(int16_t v) { beforeValue(); writeIntWidth(kTInt2, v, 2); }
    void appendInt(int32_t v) { beforeValue(); writeIntWidth(kTInt4, v, 4); }
    void appendLong(int64_t v) { beforeValue(); writeIntWidth(kTInt8, v, 8); }
    void appendFloat(float f) { beforeValue(); writeFloat(f); }
    void appendDouble(double d) { beforeValue(); writeDouble(d); }
    void appendString(const std::string &s) { beforeValue(); writeString(s); }

    void appendBinary(const std::vector<uint8_t> &bytes) {
        beforeValue();
        value.push_back(primitiveHeader(kTBinary));
        appendUintLE(value, static_cast<int64_t>(bytes.size()), kU32Size);
        value.insert(value.end(), bytes.begin(), bytes.end());
    }

    void appendUuid(const std::vector<uint8_t> &uuid16) {
        if (uuid16.size() != 16) {
            throw VariantException("uuid must be 16 bytes");
        }
        beforeValue();
        value.push_back(primitiveHeader(kTUuid));
        value.insert(value.end(), uuid16.begin(), uuid16.end());
    }

    void appendDate(int32_t d) { beforeValue(); writeIntWidth(kTDate, d, 4); }
    void appendTime(int64_t m) { beforeValue(); writeIntWidth(kTTime, m, 8); }
    void appendTimestampTz(int64_t m) {
        beforeValue();
        writeIntWidth(kTTimestamp, m, 8);
    }
    void appendTimestampNtz(int64_t m) {
        beforeValue();
        writeIntWidth(kTTimestampNtz, m, 8);
    }
    void appendTimestampNanosTz(int64_t n) {
        beforeValue();
        writeIntWidth(kTTimestampNanos, n, 8);
    }
    void appendTimestampNanosNtz(int64_t n) {
        beforeValue();
        writeIntWidth(kTTimestampNanosNtz, n, 8);
    }

    // Decimal from two's-complement big-endian bytes + scale.
    void appendDecimalBytes(const std::vector<uint8_t> &be, int scale) {
        beforeValue();
        bool negative = !be.empty() && (be[0] & 0x80) != 0;
        std::vector<uint8_t> le(be.rbegin(), be.rend());
        if (negative) {
            int carry = 1;
            for (size_t i = 0; i < le.size(); i++) {
                int v = static_cast<uint8_t>(~le[i]) + carry;
                le[i] = static_cast<uint8_t>(v & 0xFF);
                carry = v >> 8;
            }
        }
        std::string digits = leMagnitudeToDecimal(le);
        writeDecimalDigits(negative, digits, scale);
    }

    // JSON-path helpers (slot + emit) for auto-width int and decimal digits.
    void appendIntAutoJson(int64_t i) { beforeValue(); writeIntAuto(i); }
    void appendDecimalDigitsJson(bool negative, std::string digits, int scale) {
        beforeValue();
        writeDecimalDigits(negative, std::move(digits), scale);
    }

    // ---- containers ----

    void startObject() {
        beforeValue();
        Ctx c;
        c.isObject = true;
        c.start = static_cast<int>(value.size());
        stack.push_back(std::move(c));
    }

    void appendKey(const std::string &key) {
        if (stack.empty() || !stack.back().isObject) {
            throw VariantException(
                "VariantBuilder: appendKey called outside an object");
        }
        Ctx &c = stack.back();
        if (c.hasPendingKey) {
            throw VariantException(
                "VariantBuilder: appendKey called twice without a value");
        }
        c.pendingId = addKey(key);
        c.pendingKey = key;
        c.hasPendingKey = true;
    }

    void endObject() {
        if (stack.empty() || !stack.back().isObject) {
            throw VariantException(
                "VariantBuilder: endObject with no matching startObject");
        }
        if (stack.back().hasPendingKey) {
            throw VariantException(
                "VariantBuilder: endObject with a dangling appendKey");
        }
        Ctx c = std::move(stack.back());
        stack.pop_back();
        finishWritingObject(c.start, c.fields);
    }

    void startArray() {
        beforeValue();
        Ctx c;
        c.isObject = false;
        c.start = static_cast<int>(value.size());
        stack.push_back(std::move(c));
    }

    void endArray() {
        if (stack.empty() || stack.back().isObject) {
            throw VariantException(
                "VariantBuilder: endArray with no matching startArray");
        }
        Ctx c = std::move(stack.back());
        stack.pop_back();
        finishWritingArray(c.start, c.offsets);
    }

    // ---- container header insertion + finalize ----

    void finishWritingArray(int start, const std::vector<int> &offsets) {
        int dataSize = static_cast<int>(value.size()) - start;
        int numOffsets = static_cast<int>(offsets.size());
        bool largeSize = numOffsets > 0xFF;
        int sizeBytes = largeSize ? kU32Size : 1;
        int offsetSize = integerSize(dataSize);
        std::vector<uint8_t> header;
        header.push_back(static_cast<uint8_t>(
            ((largeSize ? 1 : 0) << (kBasicTypeBits + 2)) |
            ((offsetSize - 1) << kBasicTypeBits) | kArrayType));
        appendUintLE(header, numOffsets, sizeBytes);
        for (int offset : offsets) appendUintLE(header, offset, offsetSize);
        appendUintLE(header, dataSize, offsetSize);
        value.insert(value.begin() + start, header.begin(), header.end());
    }

    // Collapse duplicate keys (last-wins), compacting retained values leftward
    // in the shared value buffer. Called after fields are sorted by key (so
    // duplicates are adjacent) and before the object header is computed. Values
    // are laid out in the value buffer in insertion order; each
    // FieldEntry.offset is the byte position (relative to `start`) where that
    // field's value begins, recorded before the value was written, so a field's
    // value length is the gap to the next-inserted offset (the last runs to the
    // end of the object's data). The SAX JSON parser passes duplicate object
    // keys through, so without this an object could carry duplicate field ids -
    // a spec violation that strict readers reject.
    void dedupObjectFields(int start, std::vector<FieldEntry> &fields) {
        int n = static_cast<int>(fields.size());
        if (n <= 1) return;
        int dataSize = static_cast<int>(value.size()) - start;
        // value length per (unique) offset, from the sorted set of all offsets
        std::vector<int> offsets;
        offsets.reserve(n);
        for (const auto &f : fields) offsets.push_back(f.offset);
        std::sort(offsets.begin(), offsets.end());
        std::unordered_map<int, int> lenAt;
        for (int i = 0; i < n; i++) {
            int next = (i + 1 < n) ? offsets[i + 1] : dataSize;
            lenAt[offsets[i]] = next - offsets[i];
        }
        // collapse adjacent equal ids, keeping the entry with the greater
        // offset (the last write for that key)
        int distinctPos = 0;
        for (int i = 1; i < n; i++) {
            if (fields[i].id == fields[distinctPos].id) {
                if (fields[distinctPos].offset < fields[i].offset) {
                    fields[distinctPos] = fields[i];
                }
            } else {
                distinctPos++;
                fields[distinctPos] = fields[i];
            }
        }
        if (distinctPos + 1 == n) return;  // no duplicates
        fields.resize(distinctPos + 1);
        // compact retained values leftward, recompute offsets, truncate buffer
        std::sort(fields.begin(), fields.end(),
                  [](const FieldEntry &a, const FieldEntry &b) {
                      return a.offset < b.offset;
                  });
        int curr = 0;
        for (auto &f : fields) {
            int o = f.offset;
            int l = lenAt[o];
            if (curr != o) {
                // leftward move (curr <= o); forward std::copy is overlap-safe
                std::copy(value.begin() + start + o,
                          value.begin() + start + o + l,
                          value.begin() + start + curr);
            }
            f.offset = curr;
            curr += l;
        }
        value.resize(start + curr);
        // restore key order for header emission
        std::sort(fields.begin(), fields.end(),
                  [](const FieldEntry &a, const FieldEntry &b) {
                      return a.key < b.key;
                  });
    }

    void finishWritingObject(int start, std::vector<FieldEntry> &fields) {
        std::sort(fields.begin(), fields.end(),
                  [](const FieldEntry &a, const FieldEntry &b) {
                      return a.key < b.key;
                  });
        dedupObjectFields(start, fields);
        int numFields = static_cast<int>(fields.size());
        int maxId = 0;
        for (const auto &f : fields) maxId = std::max(maxId, f.id);
        int dataSize = static_cast<int>(value.size()) - start;
        bool largeSize = numFields > 0xFF;
        int sizeBytes = largeSize ? kU32Size : 1;
        int idSize = integerSize(maxId);
        int offsetSize = integerSize(dataSize);
        std::vector<uint8_t> header;
        header.push_back(static_cast<uint8_t>(
            ((largeSize ? 1 : 0) << (kBasicTypeBits + 4)) |
            ((idSize - 1) << (kBasicTypeBits + 2)) |
            ((offsetSize - 1) << kBasicTypeBits) | kObjectType));
        appendUintLE(header, numFields, sizeBytes);
        for (const auto &f : fields) appendUintLE(header, f.id, idSize);
        for (const auto &f : fields) appendUintLE(header, f.offset, offsetSize);
        appendUintLE(header, dataSize, offsetSize);
        value.insert(value.begin() + start, header.begin(), header.end());
    }

    void finish(std::vector<uint8_t> &valueOut,
                std::vector<uint8_t> &metadataOut) {
        int numKeys = static_cast<int>(dictionaryKeys.size());
        int dictStringSize = 0;
        for (const auto &k : dictionaryKeys)
            dictStringSize += static_cast<int>(k.size());
        int offsetSize = integerSize(std::max(dictStringSize, numKeys));

        std::vector<uint8_t> metadata;
        metadata.push_back(
            static_cast<uint8_t>(kVersion | ((offsetSize - 1) << 6)));
        appendUintLE(metadata, numKeys, offsetSize);
        int currentOffset = 0;
        for (const auto &k : dictionaryKeys) {
            appendUintLE(metadata, currentOffset, offsetSize);
            currentOffset += static_cast<int>(k.size());
        }
        appendUintLE(metadata, currentOffset, offsetSize);
        for (const auto &k : dictionaryKeys)
            metadata.insert(metadata.end(), k.begin(), k.end());

        valueOut = value;
        metadataOut = std::move(metadata);
    }
};

// SAX handler that drives a VariantBuilder::Impl from an nlohmann JSON stream,
// so parseJson reuses the exact streaming machinery.
struct VariantJsonSaxHandler {
    VariantBuilder::Impl &impl;

    // Set by parseJson when the document contained non-finite barewords, which nlohmann's lexer
    // cannot read: each was rewritten to `nonFinitePlaceholder`, and the values they stand for
    // are listed in document order. The placeholder is chosen so that it occurs nowhere in the
    // original text, so a number_float carrying exactly that literal is always one of ours, and
    // SAX events arrive in document order, so the index tracks the list.
    std::string nonFinitePlaceholder;
    std::vector<double> nonFiniteValues;
    std::size_t nonFiniteIndex = 0;

    explicit VariantJsonSaxHandler(VariantBuilder &b) : impl(*b.impl_) {}

    bool null() {
        impl.appendNull();
        return true;
    }
    bool boolean(bool val) {
        impl.appendBoolean(val);
        return true;
    }
    bool number_integer(std::int64_t val) {
        impl.appendIntAutoJson(val);
        return true;
    }
    bool number_unsigned(std::uint64_t val) {
        if (val <= static_cast<uint64_t>(INT64_MAX)) {
            impl.appendIntAutoJson(static_cast<int64_t>(val));
        } else {
            impl.appendDecimalDigitsJson(false, std::to_string(val), 0);
        }
        return true;
    }
    bool number_float(double val, const std::string &s) {
        if (!nonFinitePlaceholder.empty() && s == nonFinitePlaceholder &&
            nonFiniteIndex < nonFiniteValues.size()) {
            impl.appendDouble(nonFiniteValues[nonFiniteIndex++]);
            return true;
        }
        // nlohmann emits number_float for integer literals that overflow 64
        // bits; the raw token distinguishes them from true fractional numbers.
        bool fractional = s.find('.') != std::string::npos ||
                          s.find('e') != std::string::npos ||
                          s.find('E') != std::string::npos;
        if (!fractional) {
            bool negative = !s.empty() && s[0] == '-';
            std::string digits = negative ? s.substr(1) : s;
            impl.appendDecimalDigitsJson(negative, digits, 0);
        } else {
            impl.appendDouble(val);
        }
        return true;
    }
    bool string(std::string &val) {
        impl.appendString(val);
        return true;
    }
    bool binary(nlohmann::json::binary_t & /*val*/) {
        throw VariantException("unsupported JSON value: binary");
    }
    bool start_object(std::size_t /*elements*/) {
        impl.startObject();
        return true;
    }
    bool key(std::string &val) {
        impl.appendKey(val);
        return true;
    }
    bool end_object() {
        impl.endObject();
        return true;
    }
    bool start_array(std::size_t /*elements*/) {
        impl.startArray();
        return true;
    }
    bool end_array() {
        impl.endArray();
        return true;
    }
    bool parse_error(std::size_t /*position*/, const std::string & /*token*/,
                     const nlohmann::json::exception & /*ex*/) {
        return false;
    }
};

// ---- VariantBuilder public API (forwards to Impl) ----

VariantBuilder::VariantBuilder() : impl_(std::make_unique<Impl>()) {}
VariantBuilder::~VariantBuilder() = default;

void VariantBuilder::appendNull() { impl_->appendNull(); }
void VariantBuilder::appendBoolean(bool b) { impl_->appendBoolean(b); }
void VariantBuilder::appendByte(int8_t v) { impl_->appendByte(v); }
void VariantBuilder::appendShort(int16_t v) { impl_->appendShort(v); }
void VariantBuilder::appendInt(int32_t v) { impl_->appendInt(v); }
void VariantBuilder::appendLong(int64_t v) { impl_->appendLong(v); }
void VariantBuilder::appendFloat(float f) { impl_->appendFloat(f); }
void VariantBuilder::appendDouble(double d) { impl_->appendDouble(d); }
void VariantBuilder::appendDecimal(const std::vector<uint8_t> &unscaledBigEndian,
                                   int scale) {
    impl_->appendDecimalBytes(unscaledBigEndian, scale);
}
void VariantBuilder::appendString(const std::string &s) {
    impl_->appendString(s);
}
void VariantBuilder::appendBinary(const std::vector<uint8_t> &bytes) {
    impl_->appendBinary(bytes);
}
void VariantBuilder::appendUuid(const std::vector<uint8_t> &uuid16) {
    impl_->appendUuid(uuid16);
}
void VariantBuilder::appendDate(int32_t daysSinceEpoch) {
    impl_->appendDate(daysSinceEpoch);
}
void VariantBuilder::appendTime(int64_t microsSinceMidnight) {
    impl_->appendTime(microsSinceMidnight);
}
void VariantBuilder::appendTimestampTz(int64_t micros) {
    impl_->appendTimestampTz(micros);
}
void VariantBuilder::appendTimestampNtz(int64_t micros) {
    impl_->appendTimestampNtz(micros);
}
void VariantBuilder::appendTimestampNanosTz(int64_t nanos) {
    impl_->appendTimestampNanosTz(nanos);
}
void VariantBuilder::appendTimestampNanosNtz(int64_t nanos) {
    impl_->appendTimestampNanosNtz(nanos);
}
void VariantBuilder::startObject() { impl_->startObject(); }
void VariantBuilder::appendKey(const std::string &key) {
    impl_->appendKey(key);
}
void VariantBuilder::endObject() { impl_->endObject(); }
void VariantBuilder::startArray() { impl_->startArray(); }
void VariantBuilder::endArray() { impl_->endArray(); }

Variant VariantBuilder::build() {
    if (!impl_->stack.empty()) {
        throw VariantException(
            "VariantBuilder: build called with an open container");
    }
    if (!impl_->rootWritten) {
        throw VariantException("VariantBuilder: build called with no value");
    }
    std::vector<uint8_t> value, metadata;
    impl_->finish(value, metadata);
    return Variant(std::move(value), std::move(metadata));
}

namespace {

// The bare non-finite tokens every other client's JSON parser accepts: Jackson (Java) under
// ALLOW_NON_NUMERIC_NUMBERS, Python's json module, System.Text.Json (C#) and serde_json (Rust)
// all read these, and every client's toJson writes them back out (see the isnan/isinf arms of the
// double and float renderers above). nlohmann's lexer rejects them and offers no option to allow
// them, so they are rewritten before parsing and restored by the SAX handler.
//
// Matching is case-sensitive and whole-token, exactly as Jackson has it: `nan` and `INFINITY` are
// rejected by Java, so they must be rejected here too.
struct NonFiniteLiteral {
    const char *text;
    double value;
};

const NonFiniteLiteral kNonFiniteLiterals[] = {
    {"-Infinity", -std::numeric_limits<double>::infinity()},
    {"Infinity", std::numeric_limits<double>::infinity()},
    {"NaN", std::numeric_limits<double>::quiet_NaN()},
};

bool isBarewordChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Matches one of the non-finite barewords at `i`, as a whole token. Both boundaries are
// checked. A trailing letter or digit means some longer (and invalid) literal such as `NaNny`,
// which must be left for the parser to reject rather than silently truncated.
//
// The leading boundary matters just as much, because the scanner retries at every byte: with
// only the trailing check, `1NaN` matched `NaN` at offset 1 and was rewritten to the *valid*
// number `10.0`, so malformed input parsed as a wrong value instead of being rejected. A
// bareword can only begin where a JSON value may begin.
bool isValueStart(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '[' || c == '{' ||
           c == ':' || c == ',';
}

bool matchNonFiniteBareword(const std::string &json, std::size_t i, std::size_t &len,
                            double &value) {
    if (i > 0 && !isValueStart(json[i - 1])) {
        return false;
    }
    for (const auto &lit : kNonFiniteLiterals) {
        std::size_t n = std::strlen(lit.text);
        if (json.compare(i, n, lit.text) != 0) {
            continue;
        }
        if (i + n < json.size() && isBarewordChar(json[i + n])) {
            continue;
        }
        len = n;
        value = lit.value;
        return true;
    }
    return false;
}

// Matches a JSON number literal at `i` whose magnitude overflows a double. nlohmann rejects those
// outright (error 406, "number overflow") where Java, Go and JavaScript all read `1e400` as
// +Infinity.
//
// Only a *fractional* literal is eligible, because that is the only kind Java stores as a DOUBLE:
// an integer literal becomes an int or a scale-0 decimal, so an enormous one must keep reaching
// the existing decimal path (and be rejected there for exceeding precision 38) rather than
// quietly becoming Infinity here.
// Scans a JSON number at `i` per RFC 8259: -? (0 | [1-9][0-9]*) ('.' [0-9]+)? ([eE][+-]?[0-9]+)?
//
// The scan used to accept C's number grammar instead, which is broader: `01e400` was consumed
// whole, judged to overflow, and rewritten to the placeholder, so a malformed document parsed
// successfully. nlohmann applies the JSON grammar, so anything it would reject has to be left
// for it rather than replaced here.
bool scanJsonNumber(const std::string &json, std::size_t i, std::size_t &len, bool &fractional,
                    bool &hasExponent) {
    std::size_t p = i;
    if (p < json.size() && json[p] == '-') {
        ++p;
    }
    if (p >= json.size() || !isDigit(json[p])) {
        return false;
    }
    if (json[p] == '0') {
        // A leading zero stands alone: "0", "0.5", "0e1" are numbers, "01" is not.
        ++p;
        if (p < json.size() && isDigit(json[p])) {
            return false;
        }
    } else {
        while (p < json.size() && isDigit(json[p])) {
            ++p;
        }
    }
    fractional = false;
    hasExponent = false;
    if (p < json.size() && json[p] == '.') {
        ++p;
        if (p >= json.size() || !isDigit(json[p])) {
            return false;  // a trailing '.' is not a JSON number
        }
        while (p < json.size() && isDigit(json[p])) {
            ++p;
        }
        fractional = true;
    }
    if (p < json.size() && (json[p] == 'e' || json[p] == 'E')) {
        ++p;
        if (p < json.size() && (json[p] == '+' || json[p] == '-')) {
            ++p;
        }
        if (p >= json.size() || !isDigit(json[p])) {
            return false;  // an exponent needs at least one digit
        }
        while (p < json.size() && isDigit(json[p])) {
            ++p;
        }
        hasExponent = true;
        fractional = true;
    }
    len = p - i;
    return true;
}

// The base-10 exponent of a JSON number token's leading digit, i.e. `adjusted` such that the
// value is d.ddd x 10^adjusted. Used only to tell an overflow from an underflow, because
// std::from_chars reports both as result_out_of_range and leaves the value unspecified.
// Returns false for a token whose value is exactly zero.
bool adjustedExponent(const std::string &token, long long &adjusted) {
    std::size_t p = token.empty() || token[0] != '-' ? 0 : 1;
    std::string intDigits;
    for (; p < token.size() && isDigit(token[p]); ++p) {
        intDigits.push_back(token[p]);
    }
    std::string fracDigits;
    if (p < token.size() && token[p] == '.') {
        for (++p; p < token.size() && isDigit(token[p]); ++p) {
            fracDigits.push_back(token[p]);
        }
    }
    // from_chars rather than strtoll, and clamped: strtoll saturates to LLONG_MIN/MAX for an
    // out-of-range exponent, and the additions below would then overflow signed long long -
    // undefined behaviour on a value parsed from untrusted JSON ("10e999999999999999999999").
    // Only the sign of the result matters to the caller, so any exponent past the double range
    // is clamped to a sentinel well inside it.
    constexpr long long kExponentClamp = 1LL << 40;
    long long exponent = 0;
    if (p < token.size() && (token[p] == 'e' || token[p] == 'E')) {
        const char *first = token.data() + p + 1;
        const char *last = token.data() + token.size();
        const bool negative = first < last && *first == '-';
        const auto parsed = std::from_chars(first, last, exponent);
        if (parsed.ec == std::errc::result_out_of_range) {
            exponent = negative ? -kExponentClamp : kExponentClamp;
        } else if (parsed.ec != std::errc()) {
            exponent = 0;
        }
    }
    if (exponent > kExponentClamp) {
        exponent = kExponentClamp;
    } else if (exponent < -kExponentClamp) {
        exponent = -kExponentClamp;
    }

    const std::size_t firstIntDigit = intDigits.find_first_not_of('0');
    if (firstIntDigit != std::string::npos) {
        adjusted = exponent +
                   static_cast<long long>(intDigits.size() - firstIntDigit) - 1;
        return true;
    }
    const std::size_t firstFracDigit = fracDigits.find_first_not_of('0');
    if (firstFracDigit == std::string::npos) {
        return false;  // the value is zero
    }
    adjusted = exponent - static_cast<long long>(firstFracDigit) - 1;
    return true;
}

bool matchOverflowingNumber(const std::string &json, std::size_t i, std::size_t &len,
                            double &value) {
    std::size_t n = 0;
    bool fractional = false;
    bool hasExponent = false;
    if (!scanJsonNumber(json, i, n, fractional, hasExponent)) {
        return false;
    }
    // DBL_MAX is ~1.8e308, so a literal with no exponent and fewer than 309 integer digits
    // cannot overflow. Skipping those keeps the ordinary document free of any parse call.
    const std::size_t kOverflowFreeDigits = 309;
    if (!fractional || (!hasExponent && n < kOverflowFreeDigits)) {
        return false;
    }

    // from_chars, not strtod: strtod reads its radix character from LC_NUMERIC, so under a
    // comma-radix locale "1.5e400" was not consumed whole and the rewrite silently stopped
    // happening - parseJson became locale-dependent. from_chars ignores the locale.
    //
    // absl's, not std's: libc++ marks the floating-point std::from_chars overloads unavailable
    // before macOS 26, so this did not compile on older deployment targets. absl::from_chars is
    // a documented workalike for double/float with the same error codes.
    const char *first = json.data() + i;
    double parsed = 0.0;
    const auto result = absl::from_chars(first, first + n, parsed);
    if (result.ptr != first + n) {
        return false;
    }
    if (result.ec == std::errc::result_out_of_range) {
        // Both overflow and underflow report this, and the value is left unspecified, so the
        // direction comes from the token. Only an overflow is rewritten: an underflow is a
        // number nlohmann reads natively as zero.
        long long adjusted = 0;
        if (!adjustedExponent(std::string(first, n), adjusted) || adjusted < 0) {
            return false;
        }
        value = json[i] == '-' ? -std::numeric_limits<double>::infinity()
                               : std::numeric_limits<double>::infinity();
        len = n;
        return true;
    }
    if (result.ec != std::errc() || !std::isinf(parsed)) {
        return false;
    }
    len = n;
    value = parsed;
    return true;
}

// Rewrites each non-finite bareword, and each number literal that overflows to +/-Infinity, to a
// placeholder number, appending the value it stands for to `values` in document order. Returns
// false when there was nothing to rewrite - the overwhelmingly common case - leaving `out`
// untouched so the caller parses the original text.
//
// `out` is materialized only once the first substitution is found, so an ordinary document costs
// a single scan and no allocation. The placeholder is grown until it occurs nowhere in the
// original document, which is what makes the association exact: a number_float carrying that
// literal can only be one this rewrite wrote, never one the document already held.
bool rewriteNonFinite(const std::string &json, std::string &out, std::string &placeholder,
                      std::vector<double> &values) {
    bool rewriting = false;
    bool inString = false;
    for (std::size_t i = 0; i < json.size();) {
        char c = json[i];
        if (inString) {
            // A backslash escape is copied whole so an escaped quote does not read as the end of
            // the string.
            if (c == '\\' && i + 1 < json.size()) {
                if (rewriting) {
                    out.append(json, i, 2);
                }
                i += 2;
                continue;
            }
            if (c == '"') {
                inString = false;
            }
        } else if (c == '"') {
            inString = true;
        } else {
            std::size_t len = 0;
            double value = 0.0;
            if (matchNonFiniteBareword(json, i, len, value) ||
                matchOverflowingNumber(json, i, len, value)) {
                if (!rewriting) {
                    placeholder = "0.0";
                    while (json.find(placeholder) != std::string::npos) {
                        placeholder += '0';
                    }
                    out.assign(json, 0, i);
                    rewriting = true;
                }
                out += placeholder;
                values.push_back(value);
                i += len;
                continue;
            }
        }
        if (rewriting) {
            out += c;
        }
        ++i;
    }
    return rewriting;
}

}  // namespace

Variant Variant::parseJson(const std::string &json) {
    VariantBuilder builder;
    VariantJsonSaxHandler handler(builder);
    std::string rewritten;
    std::string placeholder;
    const std::string *text = &json;
    if (rewriteNonFinite(json, rewritten, placeholder, handler.nonFiniteValues)) {
        handler.nonFinitePlaceholder = placeholder;
        text = &rewritten;
    }
    bool ok = nlohmann::json::sax_parse(*text, &handler);
    if (!ok) {
        throw VariantException("malformed JSON for variant");
    }
    return builder.build();
}

}  // namespace schemaregistry::serdes
