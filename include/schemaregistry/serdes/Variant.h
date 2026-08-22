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

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace schemaregistry::serdes {

/**
 * The value type of a Variant, mirroring Java's Variant.Type. Integer, decimal,
 * and timestamp widths are kept distinct here; the CEL layer collapses them.
 *
 * This is the C++ counterpart of the Spark/Parquet Variant binary format (a
 * metadata key-dictionary plus a self-describing value stream), a self-contained
 * codec ported from the .NET Confluent.SchemaRegistry Variant / VariantBuilder.
 */
enum class VariantType {
    Object,
    Array,
    Null,
    Boolean,
    Byte,
    Short,
    Int,
    Long,
    String,
    Double,
    Decimal4,
    Decimal8,
    Decimal16,
    Date,
    TimestampTz,
    TimestampNtz,
    Float,
    Binary,
    Time,
    TimestampNanosTz,
    TimestampNanosNtz,
    Uuid
};

/** Raised for a malformed or unsupported Variant binary value. */
class VariantException : public std::runtime_error {
  public:
    explicit VariantException(const std::string &message)
        : std::runtime_error(message) {}
};

/**
 * A read-only view over a Variant (a metadata key-dictionary plus a
 * self-describing value stream) at a byte position. Navigation
 * (getFieldByKey / getElementAtIndex) returns a sub-Variant sharing the same
 * buffers.
 *
 * toJson() renders temporal types as ISO-8601 with the seconds field always
 * present (0/3/6/9-digit fractional grouping) and decimals in fixed-point - the
 * cross-language contract. parseJson() follows Java number handling: a
 * fractional JSON number becomes a DOUBLE, an integer wider than 64 bits a
 * scale-0 decimal.
 */
class Variant {
  public:
    /** Construct from raw value + metadata byte arrays (value first). */
    Variant(std::vector<uint8_t> value, std::vector<uint8_t> metadata);

    /** The raw value bytes (the whole buffer, shared across sub-variants). */
    const std::vector<uint8_t> &valueBytes() const { return *value_; }

    /** The raw metadata bytes (the key dictionary). */
    const std::vector<uint8_t> &metadataBytes() const { return *metadata_; }

    /**
     * The value buffer sliced from this node's start offset - a self-contained
     * value encoding for this node (trailing sibling bytes are harmless, the
     * decoder reads only what it needs). A sub-variant re-encodes as
     * (metadataBytes(), standaloneValueBytes()). For a root Variant this equals
     * valueBytes().
     */
    std::vector<uint8_t> standaloneValueBytes() const;

    VariantType getType() const;

    bool getBoolean() const;

    /** The signed 8-bit integer of an INT8 value (exact width; no widening). */
    int8_t getByte() const;

    /** The signed 16-bit integer of an INT8/INT16 value (widens within 16 bits). */
    int16_t getShort() const;

    /** The signed 32-bit integer of an INT8/INT16/INT32 value (widens within 32 bits). */
    int32_t getInt() const;

    /**
     * The raw integer for any integer-backed type (byte/short/int/long, date
     * days, timestamp micros, time micros, timestamp-nanos) - mirrors Java
     * getLong.
     */
    int64_t getLong() const;

    /** The 32-bit float of a FLOAT value (exact; does not read DOUBLE). */
    float getFloat() const;

    /** The 64-bit double of a DOUBLE value (exact; does not widen FLOAT). */
    double getDouble() const;

    /**
     * The unscaled integer (two's-complement big-endian bytes) and scale of a
     * decimal value (scale preserved).
     */
    void getDecimalParts(std::vector<uint8_t> &unscaledBigEndian,
                         int &scale) const;

    /**
     * The plain decimal string (fixed-point, never scientific), i.e. .NET's
     * BigDecimal.ToPlainString / Java's toPlainString.
     */
    std::string getDecimalString() const;

    std::vector<uint8_t> getBinary() const;

    /** The UUID as its canonical big-endian hex string. */
    std::string getUuid() const;

    std::string getString() const;

    /** The object field with the given key, or nullopt if absent. */
    std::optional<Variant> getFieldByKey(const std::string &key) const;

    /** The (key, value) of the field at idx (key-sorted). */
    std::pair<std::string, Variant> getFieldAtIndex(int idx) const;

    /** The array element at index, or nullopt if out of bounds. */
    std::optional<Variant> getElementAtIndex(int index) const;

    int numObjectFields() const;

    int numArrayElements() const;

    /** Serialize to a JSON string, matching Java's VariantUtils.toJsonString. */
    std::string toJson() const;

    /** Parse a JSON string into a Variant (matches Java VariantUtils). */
    static Variant parseJson(const std::string &json);

  private:
    using Buffer = std::shared_ptr<const std::vector<uint8_t>>;

    Variant(Buffer value, Buffer metadata, size_t pos);

    void checkVersion() const;

    int primitiveInfo() const;

    void objectInfo(int &numFields, int &idSize, int &offsetSize, int &idStart,
                    int &offsetStart, int &dataStart) const;

    void arrayInfo(int &numFields, int &offsetSize, int &offsetStart,
                   int &dataStart) const;

    std::string getMetadataKey(int id) const;

    void writeJson(std::string &out) const;

    Buffer value_;
    Buffer metadata_;
    size_t pos_;
};

/**
 * A flat, streaming writer that builds a Variant programmatically. A single
 * builder maintains an internal nesting stack (the arrow-dotnet flat
 * streaming-writer shape): each append*() targets the current slot - the root,
 * the next array element, or the current object field value (after
 * appendKey()). Object fields are sorted by key on endObject() (canonical
 * order) and their keys accumulate into the metadata dictionary. build()
 * finalizes and returns a Variant.
 *
 * Output is byte-identical to Variant::parseJson() for the equivalent document:
 * parseJson() is itself implemented on top of this builder. Integer widths are
 * the caller's choice here (appendByte .. appendLong), whereas the JSON path
 * selects the smallest width that fits; pick the matching method to reproduce a
 * parsed document exactly.
 *
 * Misuse (an unbalanced startX/endX, appendKey() outside an object, a value
 * append without a preceding appendKey() inside an object, or build() with an
 * open container) throws VariantException.
 */
class VariantBuilder {
  public:
    VariantBuilder();
    ~VariantBuilder();

    // Scalars - append into the current slot.
    void appendNull();
    void appendBoolean(bool b);
    void appendByte(int8_t v);
    void appendShort(int16_t v);
    void appendInt(int32_t v);
    void appendLong(int64_t v);
    void appendFloat(float f);
    void appendDouble(double d);
    /**
     * Append a decimal from its unscaled value (two's-complement big-endian
     * bytes) and scale. The narrowest of DECIMAL4/8/16 that holds the precision
     * is chosen, matching the JSON path.
     */
    void appendDecimal(const std::vector<uint8_t> &unscaledBigEndian, int scale);
    /** Append a string (auto short-string when <= 63 UTF-8 bytes). */
    void appendString(const std::string &s);
    /** Append an opaque binary blob. */
    void appendBinary(const std::vector<uint8_t> &bytes);
    /** Append a UUID from its 16 canonical big-endian bytes. */
    void appendUuid(const std::vector<uint8_t> &uuid16);
    /** Append a DATE (days since the Unix epoch). */
    void appendDate(int32_t daysSinceEpoch);
    /** Append a TIME_NTZ (microseconds since midnight). */
    void appendTime(int64_t microsSinceMidnight);
    /** Append a TIMESTAMP with time zone (microseconds since the Unix epoch). */
    void appendTimestampTz(int64_t micros);
    /** Append a TIMESTAMP without time zone (microseconds since the Unix epoch). */
    void appendTimestampNtz(int64_t micros);
    /** Append a nanosecond TIMESTAMP with time zone (nanoseconds since epoch). */
    void appendTimestampNanosTz(int64_t nanos);
    /** Append a nanosecond TIMESTAMP without time zone (nanoseconds since epoch). */
    void appendTimestampNanosNtz(int64_t nanos);

    // Containers - flat, internal nesting stack.
    void startObject();
    void appendKey(const std::string &key);
    void endObject();
    void startArray();
    void endArray();

    /** Finalize and return the built Variant. */
    Variant build();

  private:
    friend struct VariantJsonSaxHandler;  // JSON bridge (parseJson)

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace schemaregistry::serdes
