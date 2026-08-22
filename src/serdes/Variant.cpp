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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unordered_map>

#include <nlohmann/json.hpp>

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

int64_t readUnsignedLE(const std::vector<uint8_t> &data, int64_t pos,
                       int numBytes) {
    checkIndex(pos, data.size());
    checkIndex(pos + numBytes - 1, data.size());
    int64_t result = 0;
    for (int i = numBytes - 1; i >= 0; i--) {
        result = (result << 8) | data[pos + i];
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

std::string formatInstant(int64_t totalNanos) {
    int64_t sec = floorDiv(totalNanos, 1000000000LL);
    int64_t nano = floorMod(totalNanos, 1000000000LL);
    int64_t days = floorDiv(sec, 86400LL);
    int64_t secOfDay = floorMod(sec, 86400LL);
    int64_t year;
    int month, day;
    civilFromDays(days, year, month, day);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04lld-%02d-%02dT%02lld:%02lld:%02lld%sZ",
                  static_cast<long long>(year), month, day,
                  static_cast<long long>(secOfDay / 3600),
                  static_cast<long long>((secOfDay % 3600) / 60),
                  static_cast<long long>(secOfDay % 60), frac(nano).c_str());
    return buf;
}

std::string formatLocalDateTime(int64_t totalNanos) {
    int64_t sec = floorDiv(totalNanos, 1000000000LL);
    int64_t nano = floorMod(totalNanos, 1000000000LL);
    int64_t days = floorDiv(sec, 86400LL);
    int64_t secOfDay = floorMod(sec, 86400LL);
    int64_t year;
    int month, day;
    civilFromDays(days, year, month, day);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04lld-%02d-%02dT%02lld:%02lld:%02lld%s",
                  static_cast<long long>(year), month, day,
                  static_cast<long long>(secOfDay / 3600),
                  static_cast<long long>((secOfDay % 3600) / 60),
                  static_cast<long long>(secOfDay % 60), frac(nano).c_str());
    return buf;
}

std::string formatLocalTime(int64_t micros) {
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
    if (std::isnan(d) || std::isinf(d)) {
        throw VariantException("cannot render non-finite double as JSON");
    }
    if (d == std::floor(d) && std::abs(d) < 1e16) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld.0",
                      static_cast<long long>(d));
        return buf;
    }
    // Shortest round-tripping decimal representation.
    char buf[64];
    for (int prec = 1; prec <= 17; prec++) {
        std::snprintf(buf, sizeof(buf), "%.*g", prec, d);
        if (std::strtod(buf, nullptr) == d) break;
    }
    return buf;
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
    numFields = static_cast<int>(readUnsignedLE(value, pos_ + 1, sizeBytes));
    idSize = ((typeInfo >> 2) & 0x3) + 1;
    offsetSize = (typeInfo & 0x3) + 1;
    idStart = static_cast<int>(pos_) + 1 + sizeBytes;
    offsetStart = idStart + numFields * idSize;
    dataStart = offsetStart + (numFields + 1) * offsetSize;
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
    numFields = static_cast<int>(readUnsignedLE(value, pos_ + 1, sizeBytes));
    offsetSize = (typeInfo & 0x3) + 1;
    offsetStart = static_cast<int>(pos_) + 1 + sizeBytes;
    dataStart = offsetStart + (numFields + 1) * offsetSize;
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
    int dictSize = static_cast<int>(readUnsignedLE(metadata, 1, offsetSize));
    if (id >= dictSize) {
        throw VariantException("malformed variant: field id out of range");
    }
    int stringStart = 1 + (dictSize + 2) * offsetSize;
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
            out.append(formatDouble(getFloat()));
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
            out.append(formatInstant(getLong() * 1000LL));
            out.push_back('"');
            break;
        case VariantType::TimestampNtz:
            out.push_back('"');
            out.append(formatLocalDateTime(getLong() * 1000LL));
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

    void finishWritingObject(int start, std::vector<FieldEntry> &fields) {
        int numFields = static_cast<int>(fields.size());
        std::sort(fields.begin(), fields.end(),
                  [](const FieldEntry &a, const FieldEntry &b) {
                      return a.key < b.key;
                  });
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

Variant Variant::parseJson(const std::string &json) {
    VariantBuilder builder;
    VariantJsonSaxHandler handler(builder);
    bool ok = nlohmann::json::sax_parse(json, &handler);
    if (!ok) {
        throw VariantException("malformed JSON for variant");
    }
    return builder.build();
}

}  // namespace schemaregistry::serdes
