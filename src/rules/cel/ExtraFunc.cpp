// Copyright 2026 Confluent Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "schemaregistry/rules/cel/ExtraFunc.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include "absl/status/status.h"
#include "absl/time/time.h"
#include "eval/public/cel_function.h"
#include "eval/public/cel_value.h"
#include "eval/public/structs/cel_proto_wrapper.h"
#include "google/protobuf/arena.h"
#include "google/protobuf/message.h"
#include "confluent/type/variant.pb.h"
#include "schemaregistry/rules/cel/DecimalUtil.h"
#include "schemaregistry/serdes/Variant.h"

namespace schemaregistry::rules::cel {

namespace cel = google::api::expr::runtime;
using ::google::protobuf::Arena;

namespace {

// ---------------------------------------------------------------------------
// A CelFunction backed by a std::function, so functions can be declared with
// explicit argument-type descriptors (kMessage for Decimal, kAny for the
// runtime-dispatch decimal(dyn), etc.).
// ---------------------------------------------------------------------------
class LambdaFunction : public cel::CelFunction {
   public:
    using Impl = std::function<absl::Status(absl::Span<const cel::CelValue>, cel::CelValue*,
                                            Arena*)>;

    LambdaFunction(absl::string_view name, bool receiver_style,
                   std::vector<cel::CelValue::Type> arg_types, Impl impl)
        : CelFunction(cel::CelFunctionDescriptor(name, receiver_style, std::move(arg_types))),
          impl_(std::move(impl)) {}

    absl::Status Evaluate(absl::Span<const cel::CelValue> args, cel::CelValue* result,
                          Arena* arena) const override {
        // cel-cpp expects functions to signal failure via an error CelValue, not by
        // throwing. Convert any exception a conversion helper raises (e.g. an
        // out-of-range decimal coefficient) into a CEL error so it cannot escape into
        // the interpreter.
        try {
            return impl_(args, result, arena);
        } catch (const std::exception& e) {
            *result = cel::CreateErrorValue(arena, e.what());
            return absl::OkStatus();
        }
    }

   private:
    Impl impl_;
};

using T = cel::CelValue::Type;

absl::Status reg(cel::CelFunctionRegistry& registry, absl::string_view name, bool receiver,
                 std::vector<T> types, LambdaFunction::Impl impl) {
    return registry.Register(
        std::make_unique<LambdaFunction>(name, receiver, std::move(types), std::move(impl)));
}

cel::CelValue err(Arena* arena, const std::string& msg) {
    return cel::CreateErrorValue(arena, msg);
}

// ---------------------------------------------------------------------------
// Decimal helpers. A CEL Decimal is a confluent.type.Decimal proto message.
// ---------------------------------------------------------------------------

bool isDecimalMessage(const cel::CelValue& v) {
    if (!v.IsMessage()) {
        return false;
    }
    const google::protobuf::Message* m = v.MessageOrDie();
    return m != nullptr && m->GetDescriptor()->full_name() == "confluent.type.Decimal";
}

// Read a confluent.type.Decimal message via reflection (works for both the generated
// class and a runtime DynamicMessage).
decimal::Decimal decimalFromMessage(const google::protobuf::Message& m) {
    const auto* desc = m.GetDescriptor();
    const auto* refl = m.GetReflection();
    const auto* value_field = desc->FindFieldByName("value");
    const auto* scale_field = desc->FindFieldByName("scale");
    std::string scratch;
    const std::string& value = refl->GetStringReference(m, value_field, &scratch);
    int32_t scale = refl->GetInt32(m, scale_field);
    return DecimalUtil::fromUnscaledBytes(value, scale);
}

// Wrap a Decimal into a CEL value (a confluent.type.Decimal message on the arena).
cel::CelValue wrapDecimal(const decimal::Decimal& d, Arena* arena) {
    auto* msg = Arena::Create<confluent::type::Decimal>(arena);
    *msg = DecimalUtil::toProto(d);
    return cel::CelProtoWrapper::CreateMessage(msg, arena);
}

bool decimalIsZero(const decimal::Decimal& d) {
    mpd_uint128_triple_t t = d.as_uint128_triple();
    return t.tag == MPD_TRIPLE_NORMAL && t.hi == 0 && t.lo == 0;
}

// -1 / 0 / 1 comparison via the shared context.
int decimalCompare(const decimal::Decimal& a, const decimal::Decimal& b) {
    mpd_uint128_triple_t t = a.compare(b, DecimalUtil::context()).as_uint128_triple();
    if (t.hi == 0 && t.lo == 0) {
        return 0;
    }
    return t.sign ? -1 : 1;
}

// Convert an arbitrary CEL value to a Decimal for decimal(dyn).
absl::optional<decimal::Decimal> toDecimalDyn(const cel::CelValue& v, std::string* error_out) {
    if (isDecimalMessage(v)) {
        return decimalFromMessage(*v.MessageOrDie());
    }
    if (v.IsString()) {
        try {
            return decimal::Decimal(std::string(v.StringOrDie().value()));
        } catch (const std::exception& e) {
            *error_out = std::string("decimal: invalid string: ") + e.what();
            return absl::nullopt;
        }
    }
    if (v.IsInt64()) {
        return decimal::Decimal(v.Int64OrDie());
    }
    if (v.IsUint64()) {
        return decimal::Decimal(std::to_string(v.Uint64OrDie()));
    }
    if (v.IsDouble()) {
        // Shortest round-tripping form, matching BigDecimal.valueOf(double). NaN/Inf have
        // no decimal representation (BigDecimal.valueOf throws on them), and std::to_chars
        // would otherwise emit "nan"/"inf" that Decimal would parse into a non-finite value.
        double d = v.DoubleOrDie();
        if (!std::isfinite(d)) {
            *error_out = "decimal: cannot convert non-finite double to Decimal";
            return absl::nullopt;
        }
        char buf[32];
        auto res = std::to_chars(buf, buf + sizeof(buf), d);
        if (res.ec != std::errc()) {
            *error_out = "decimal: cannot convert double to Decimal";
            return absl::nullopt;
        }
        return decimal::Decimal(std::string(buf, res.ptr));
    }
    if (v.IsBytes()) {
        *error_out = "decimal: raw bytes need a scale; use decimal(bytes, scale)";
        return absl::nullopt;
    }
    *error_out = "decimal: cannot convert value to Decimal";
    return absl::nullopt;
}

// Narrow a CEL int (i64) scale argument down to the i32 that BigDecimal-style scale requires,
// matching Java's requireIntScale (Math.toIntExact). Throws on out-of-range so the
// LambdaFunction wrapper reports a CEL error, rather than silently keeping the low 32 bits
// (e.g. 2^32 -> 0) and producing a wildly wrong Decimal.
int32_t requireIntScale(int64_t scale, const char* functionName) {
    if (scale < INT32_MIN || scale > INT32_MAX) {
        throw std::invalid_argument(std::string(functionName) +
                                    ": scale out of int range: " + std::to_string(scale));
    }
    return static_cast<int32_t>(scale);
}

// ---------------------------------------------------------------------------
// Decimal function registration.
// ---------------------------------------------------------------------------

absl::Status registerDecimal(cel::CelFunctionRegistry& registry) {
    // decimal(dyn)
    absl::Status s = reg(registry, "decimal", false, {T::kAny},
                         [](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                             std::string e;
                             auto d = toDecimalDyn(args[0], &e);
                             if (!d) {
                                 *out = err(arena, e);
                             } else {
                                 *out = wrapDecimal(*d, arena);
                             }
                             return absl::OkStatus();
                         });
    if (!s.ok()) return s;

    // decimal(bytes, int)
    s = reg(registry, "decimal", false, {T::kBytes, T::kInt64},
            [](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                auto d = DecimalUtil::fromUnscaledBytes(
                    std::string(args[0].BytesOrDie().value()),
                    requireIntScale(args[1].Int64OrDie(), "decimal(bytes, scale)"));
                *out = wrapDecimal(d, arena);
                return absl::OkStatus();
            });
    if (!s.ok()) return s;

    // Binary comparison: decimals.eq/lt/le/gt/ge -> bool
    auto cmp = [&registry](const char* name, std::function<bool(int)> pred) {
        return reg(registry, name, false, {T::kMessage, T::kMessage},
                   [pred](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                       if (!isDecimalMessage(args[0]) || !isDecimalMessage(args[1])) {
                           *out = err(arena, "decimals: expected Decimal operands");
                           return absl::OkStatus();
                       }
                       int c = decimalCompare(decimalFromMessage(*args[0].MessageOrDie()),
                                              decimalFromMessage(*args[1].MessageOrDie()));
                       *out = cel::CelValue::CreateBool(pred(c));
                       return absl::OkStatus();
                   });
    };
    if (s = cmp("decimals.eq", [](int c) { return c == 0; }); !s.ok()) return s;
    if (s = cmp("decimals.lt", [](int c) { return c < 0; }); !s.ok()) return s;
    if (s = cmp("decimals.le", [](int c) { return c <= 0; }); !s.ok()) return s;
    if (s = cmp("decimals.gt", [](int c) { return c > 0; }); !s.ok()) return s;
    if (s = cmp("decimals.ge", [](int c) { return c >= 0; }); !s.ok()) return s;

    // Binary arithmetic/selection: decimals.add/sub/mul/div/mod/greatest/least -> Decimal
    auto bin =
        [&registry](const char* name,
                    std::function<absl::optional<decimal::Decimal>(const decimal::Decimal&,
                                                                   const decimal::Decimal&,
                                                                   std::string*)> op) {
            return reg(
                registry, name, false, {T::kMessage, T::kMessage},
                [op, name](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                    if (!isDecimalMessage(args[0]) || !isDecimalMessage(args[1])) {
                        *out = err(arena, std::string(name) + ": expected Decimal operands");
                        return absl::OkStatus();
                    }
                    std::string e;
                    auto r = op(decimalFromMessage(*args[0].MessageOrDie()),
                                decimalFromMessage(*args[1].MessageOrDie()), &e);
                    *out = r ? wrapDecimal(*r, arena) : err(arena, e);
                    return absl::OkStatus();
                });
        };
    // add/sub/mul use the exact (unbounded-precision) context so results are not capped at
    // 38 digits — matching Java's exact BigDecimal add/subtract/multiply. div/sqrt below
    // deliberately keep the 38-digit context() (the shared cross-client contract).
    if (s = bin("decimals.add", [](auto a, auto b, std::string*) { return a.add(b, DecimalUtil::exactContext()); });
        !s.ok())
        return s;
    if (s = bin("decimals.sub", [](auto a, auto b, std::string*) { return a.sub(b, DecimalUtil::exactContext()); });
        !s.ok())
        return s;
    if (s = bin("decimals.mul", [](auto a, auto b, std::string*) { return a.mul(b, DecimalUtil::exactContext()); });
        !s.ok())
        return s;
    if (s = bin("decimals.div",
                [](auto a, auto b, std::string* e) -> absl::optional<decimal::Decimal> {
                    if (decimalIsZero(b)) {
                        *e = "decimals.div: division by zero";
                        return absl::nullopt;
                    }
                    return a.div(b, DecimalUtil::context());
                });
        !s.ok())
        return s;
    if (s = bin("decimals.mod",
                [](auto a, auto b, std::string* e) -> absl::optional<decimal::Decimal> {
                    if (decimalIsZero(b)) {
                        *e = "decimals.mod: division by zero";
                        return absl::nullopt;
                    }
                    return a.rem(b, DecimalUtil::context());
                });
        !s.ok())
        return s;
    if (s = bin("decimals.greatest",
                [](auto a, auto b, std::string*) { return decimalCompare(a, b) >= 0 ? a : b; });
        !s.ok())
        return s;
    if (s = bin("decimals.least",
                [](auto a, auto b, std::string*) { return decimalCompare(a, b) <= 0 ? a : b; });
        !s.ok())
        return s;

    // Unary -> Decimal: decimals.sqrt/neg/abs/floor/ceil
    auto un =
        [&registry](const char* name,
                    std::function<absl::optional<decimal::Decimal>(const decimal::Decimal&,
                                                                   std::string*)> op) {
            return reg(
                registry, name, false, {T::kMessage},
                [op, name](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                    if (!isDecimalMessage(args[0])) {
                        *out = err(arena, std::string(name) + ": expected a Decimal");
                        return absl::OkStatus();
                    }
                    std::string e;
                    auto r = op(decimalFromMessage(*args[0].MessageOrDie()), &e);
                    *out = r ? wrapDecimal(*r, arena) : err(arena, e);
                    return absl::OkStatus();
                });
        };
    if (s = un("decimals.sqrt",
               [](auto a, std::string* e) -> absl::optional<decimal::Decimal> {
                   if (a.sign() < 0 && !decimalIsZero(a)) {
                       *e = "decimals.sqrt: square root of negative number";
                       return absl::nullopt;
                   }
                   return a.sqrt(DecimalUtil::context());
               });
        !s.ok())
        return s;
    // neg/abs use copy_negate/copy_abs — exact sign flips with no rounding/precision cap
    // (matching Java BigDecimal.negate/abs and Python's copy_negate/copy_abs). Applying the
    // 38-digit context() here would round a >38-digit operand produced by an earlier exact
    // add/sub/mul.
    if (s = un("decimals.neg", [](auto a, std::string*) { return a.copy_negate(); }); !s.ok())
        return s;
    if (s = un("decimals.abs", [](auto a, std::string*) { return a.copy_abs(); }); !s.ok())
        return s;
    if (s = un("decimals.floor", [](auto a, std::string*) { return a.floor(DecimalUtil::context()); }); !s.ok())
        return s;
    if (s = un("decimals.ceil", [](auto a, std::string*) { return a.ceil(DecimalUtil::context()); }); !s.ok())
        return s;

    // decimals.sign -> int (-1/0/1; sign() returns 1 for zero, so special-case)
    s = reg(registry, "decimals.sign", false, {T::kMessage},
            [](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                if (!isDecimalMessage(args[0])) {
                    *out = err(arena, "decimals.sign: expected a Decimal");
                    return absl::OkStatus();
                }
                auto d = decimalFromMessage(*args[0].MessageOrDie());
                int sign = decimalIsZero(d) ? 0 : d.sign();
                *out = cel::CelValue::CreateInt64(sign);
                return absl::OkStatus();
            });
    if (!s.ok()) return s;

    // Rounding: decimals.round/trunc (unary + scale), floor/ceil already done.
    auto roundTo = [](const decimal::Decimal& d, int32_t scale, int round) {
        decimal::Context c;
        c.prec(38);
        c.round(round);
        return d.rescale(-static_cast<int64_t>(scale), c);
    };
    // decimals.round(Decimal) / (Decimal, int) — HALF_UP
    s = reg(registry, "decimals.round", false, {T::kMessage},
            [roundTo](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                if (!isDecimalMessage(args[0])) {
                    *out = err(arena, "decimals.round: expected a Decimal");
                    return absl::OkStatus();
                }
                *out = wrapDecimal(roundTo(decimalFromMessage(*args[0].MessageOrDie()), 0,
                                           MPD_ROUND_HALF_UP),
                                   arena);
                return absl::OkStatus();
            });
    if (!s.ok()) return s;
    s = reg(registry, "decimals.round", false, {T::kMessage, T::kInt64},
            [roundTo](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                if (!isDecimalMessage(args[0])) {
                    *out = err(arena, "decimals.round: expected a Decimal");
                    return absl::OkStatus();
                }
                *out = wrapDecimal(roundTo(decimalFromMessage(*args[0].MessageOrDie()),
                                           requireIntScale(args[1].Int64OrDie(), "decimals.round"),
                                           MPD_ROUND_HALF_UP),
                                   arena);
                return absl::OkStatus();
            });
    if (!s.ok()) return s;
    // decimals.trunc — ROUND_DOWN, with Flink's no-op when target scale >= current scale.
    auto truncTo = [roundTo](const decimal::Decimal& d, int32_t scale) {
        int64_t current = -d.exponent();
        if (scale >= current) {
            return d;
        }
        return roundTo(d, scale, MPD_ROUND_DOWN);
    };
    s = reg(registry, "decimals.trunc", false, {T::kMessage},
            [truncTo](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                if (!isDecimalMessage(args[0])) {
                    *out = err(arena, "decimals.trunc: expected a Decimal");
                    return absl::OkStatus();
                }
                *out = wrapDecimal(truncTo(decimalFromMessage(*args[0].MessageOrDie()), 0), arena);
                return absl::OkStatus();
            });
    if (!s.ok()) return s;
    s = reg(registry, "decimals.trunc", false, {T::kMessage, T::kInt64},
            [truncTo](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                if (!isDecimalMessage(args[0])) {
                    *out = err(arena, "decimals.trunc: expected a Decimal");
                    return absl::OkStatus();
                }
                *out = wrapDecimal(truncTo(decimalFromMessage(*args[0].MessageOrDie()),
                                           requireIntScale(args[1].Int64OrDie(), "decimals.trunc")),
                                   arena);
                return absl::OkStatus();
            });
    if (!s.ok()) return s;

    // string(Decimal) -> plain string; double(Decimal) -> double. kMessage arg avoids
    // overlap with the stdlib string()/double() conversions.
    s = reg(registry, "string", false, {T::kMessage},
            [](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                if (!isDecimalMessage(args[0])) {
                    *out = err(arena, "string: expected a Decimal");
                    return absl::OkStatus();
                }
                auto d = decimalFromMessage(*args[0].MessageOrDie());
                auto* str = Arena::Create<std::string>(arena, d.format("f"));
                *out = cel::CelValue::CreateString(str);
                return absl::OkStatus();
            });
    if (!s.ok()) return s;
    s = reg(registry, "double", false, {T::kMessage},
            [](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                if (!isDecimalMessage(args[0])) {
                    *out = err(arena, "double: expected a Decimal");
                    return absl::OkStatus();
                }
                auto d = decimalFromMessage(*args[0].MessageOrDie());
                double val = 0.0;
                try {
                    val = std::stod(d.format("f"));
                } catch (const std::out_of_range&) {
                    val = d.sign() < 0 ? -HUGE_VAL : HUGE_VAL;
                }
                *out = cel::CelValue::CreateDouble(val);
                return absl::OkStatus();
            });
    return s;
}

// ---------------------------------------------------------------------------
// Timestamp function registration.
// ---------------------------------------------------------------------------

absl::Status registerTimestamp(cel::CelFunctionRegistry& registry) {
    // timestamp.of(dyn)
    absl::Status s =
        reg(registry, "timestamp.of", false, {T::kAny},
            [](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                const cel::CelValue& v = args[0];
                if (v.IsTimestamp()) {
                    *out = v;  // already a CEL timestamp (incl. pre-converted Avro / proto WKT)
                } else if (v.IsString()) {
                    absl::Time t;
                    std::string parse_err;
                    if (absl::ParseTime(absl::RFC3339_full, std::string(v.StringOrDie().value()),
                                        &t, &parse_err)) {
                        *out = cel::CelValue::CreateTimestamp(t);
                    } else {
                        *out = err(arena, "timestamp.of: cannot parse RFC 3339: " + parse_err);
                    }
                } else if (v.IsInt64() || v.IsUint64()) {
                    *out = err(arena,
                               "timestamp.of: raw int needs a unit; use "
                               "timestamp.of(value, \"millis\"|\"micros\"|\"nanos\"|\"seconds\")");
                } else {
                    *out = err(arena, "timestamp.of: cannot convert value to Timestamp");
                }
                return absl::OkStatus();
            });
    if (!s.ok()) return s;

    // timestamp.of(int, string)
    s = reg(registry, "timestamp.of", false, {T::kInt64, T::kString},
            [](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                int64_t value = args[0].Int64OrDie();
                std::string unit(args[1].StringOrDie().value());
                absl::Time t;
                if (unit == "millis") {
                    t = absl::FromUnixMillis(value);
                } else if (unit == "micros") {
                    t = absl::FromUnixMicros(value);
                } else if (unit == "nanos") {
                    t = absl::FromUnixNanos(value);
                } else if (unit == "seconds") {
                    t = absl::FromUnixSeconds(value);
                } else {
                    *out = err(arena, "timestamp.of: unknown unit '" + unit +
                                          "'; expected millis, micros, nanos, seconds");
                    return absl::OkStatus();
                }
                *out = cel::CelValue::CreateTimestamp(t);
                return absl::OkStatus();
            });
    return s;
}

// ---------------------------------------------------------------------------
// is* validators (member-style: "foo@bar.com".isEmail()). Self-contained.
// ---------------------------------------------------------------------------

bool validateHostname(std::string_view s) {
    static const std::regex re(
        R"(^(?=.{1,253}$)([a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)(\.[a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)*$)");
    return std::regex_match(s.begin(), s.end(), re);
}

bool validateEmail(std::string_view s) {
    static const std::regex re(
        R"(^[a-zA-Z0-9.!#$%&'*+/=?^_`{|}~-]+@[a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?(\.[a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)*$)");
    return s.size() <= 254 && std::regex_match(s.begin(), s.end(), re);
}

bool validateIpv4(std::string_view s) {
    std::string str(s);
    unsigned char buf[sizeof(struct in_addr)];
    return inet_pton(AF_INET, str.c_str(), buf) == 1;
}

bool validateIpv6(std::string_view s) {
    std::string str(s);
    unsigned char buf[sizeof(struct in6_addr)];
    return inet_pton(AF_INET6, str.c_str(), buf) == 1;
}

bool validateUri(std::string_view s) {
    // Absolute URI: scheme ":" hier-part, no spaces.
    static const std::regex re(R"(^[a-zA-Z][a-zA-Z0-9+.-]*:[^\s]*$)");
    return std::regex_match(s.begin(), s.end(), re);
}

bool validateUriRef(std::string_view s) {
    // URI or relative reference: no whitespace.
    if (s.empty()) return false;
    return s.find_first_of(" \t\r\n") == std::string_view::npos;
}

bool validateUuid(std::string_view s) {
    static const std::regex re(
        R"(^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$)");
    return std::regex_match(s.begin(), s.end(), re);
}

absl::Status registerIsFuncs(cel::CelFunctionRegistry& registry) {
    auto member = [&registry](const char* name, std::function<bool(std::string_view)> fn) {
        return reg(registry, name, true, {T::kString},
                   [fn](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena*) {
                       std::string_view s = args[0].StringOrDie().value();
                       *out = cel::CelValue::CreateBool(!s.empty() && fn(s));
                       return absl::OkStatus();
                   });
    };
    absl::Status s;
    if (s = member("isEmail", validateEmail); !s.ok()) return s;
    if (s = member("isHostname", validateHostname); !s.ok()) return s;
    if (s = member("isIpv4", validateIpv4); !s.ok()) return s;
    if (s = member("isIpv6", validateIpv6); !s.ok()) return s;
    if (s = member("isUri", validateUri); !s.ok()) return s;
    if (s = member("isUriRef", validateUriRef); !s.ok()) return s;
    if (s = member("isUuid", validateUuid); !s.ok()) return s;
    return absl::OkStatus();
}

// ---------------------------------------------------------------------------
// Variant helpers. A CEL Variant is a confluent.type.Variant proto message; the
// variants.* functions wrap/unwrap it through the schemaregistry::serdes::Variant
// codec. Mirrors the Decimal helpers above. Null model: CEL null = absent (a miss,
// out-of-bounds, or non-variant); a Variant whose top type is NULL = present-but-null.
// ---------------------------------------------------------------------------

using SrVariant = schemaregistry::serdes::Variant;
using schemaregistry::serdes::VariantType;

cel::CelValue makeCelString(Arena* arena, const std::string& value) {
    return cel::CelValue::CreateString(Arena::Create<std::string>(arena, value));
}

bool isVariantMessage(const cel::CelValue& value) {
    if (!value.IsMessage()) {
        return false;
    }
    const google::protobuf::Message* msg = value.MessageOrDie();
    return msg != nullptr &&
           msg->GetDescriptor()->full_name() == "confluent.type.Variant";
}

// Read a confluent.type.Variant message via reflection (works for the generated
// class and a runtime DynamicMessage).
SrVariant variantFromMessage(const google::protobuf::Message& msg) {
    const auto* desc = msg.GetDescriptor();
    const auto* refl = msg.GetReflection();
    std::string metaScratch;
    std::string valueScratch;
    const std::string& metadata = refl->GetStringReference(
        msg, desc->FindFieldByName("metadata"), &metaScratch);
    const std::string& value = refl->GetStringReference(
        msg, desc->FindFieldByName("value"), &valueScratch);
    return SrVariant(std::vector<uint8_t>(value.begin(), value.end()),
                     std::vector<uint8_t>(metadata.begin(), metadata.end()));
}

// Wrap a Variant into a CEL value. Uses standaloneValueBytes() so a navigated
// sub-variant re-encodes as its own self-contained (metadata, value) message.
cel::CelValue wrapVariant(const SrVariant& variant, Arena* arena) {
    auto* msg = Arena::Create<confluent::type::Variant>(arena);
    const std::vector<uint8_t>& metadata = variant.metadataBytes();
    std::vector<uint8_t> value = variant.standaloneValueBytes();
    msg->set_metadata(std::string(metadata.begin(), metadata.end()));
    msg->set_value(std::string(value.begin(), value.end()));
    return cel::CelProtoWrapper::CreateMessage(msg, arena);
}

// The coarse label variants.type returns (integer widths -> "int", float/double ->
// "double", decimal widths -> "decimal", all timestamp variants -> "timestamp").
const char* variantTypeLabel(VariantType type) {
    switch (type) {
        case VariantType::Object: return "object";
        case VariantType::Array: return "array";
        case VariantType::Null: return "null";
        case VariantType::Boolean: return "boolean";
        case VariantType::Byte:
        case VariantType::Short:
        case VariantType::Int:
        case VariantType::Long: return "int";
        case VariantType::Float:
        case VariantType::Double: return "double";
        case VariantType::Decimal4:
        case VariantType::Decimal8:
        case VariantType::Decimal16: return "decimal";
        case VariantType::Date: return "date";
        case VariantType::Time: return "time";
        case VariantType::TimestampTz:
        case VariantType::TimestampNtz:
        case VariantType::TimestampNanosTz:
        case VariantType::TimestampNanosNtz: return "timestamp";
        case VariantType::String: return "string";
        case VariantType::Binary: return "bytes";
        case VariantType::Uuid: return "uuid";
    }
    throw std::invalid_argument("unsupported variant type");
}

// A variants.* navigation receiver: sets *isNull when the arg is CEL null (a
// passthrough), else returns the Variant if it is a variant message, else nullopt
// (the caller reports a hard error).
std::optional<SrVariant> receiverVariant(const cel::CelValue& value, bool* isNull) {
    *isNull = value.IsNull();
    if (value.IsNull()) {
        return std::nullopt;
    }
    if (isVariantMessage(value)) {
        return variantFromMessage(*value.MessageOrDie());
    }
    return std::nullopt;
}

// JSONPath subset (port of the Java/Python VariantPath). Throws std::invalid_argument
// on a malformed path (the LambdaFunction turns it into a CEL error); a resolution
// miss returns nullopt. Quoted-key escapes: only "\\" and backslash+quote (option B).
struct VariantPathSeg {
    bool isIndex;
    std::string key;
    int index;
};

std::vector<VariantPathSeg> parseVariantPath(const std::string& path) {
    if (path.empty()) {
        throw std::invalid_argument("variant path must start with '$'");
    }
    if (path[0] != '$') {
        throw std::invalid_argument("variant path must start with '$', got: " + path);
    }
    std::vector<VariantPathSeg> out;
    size_t pos = 1;
    while (pos < path.size()) {
        char ch = path[pos];
        if (ch == '.') {
            pos++;
            if (pos >= path.size() ||
                !(std::isalpha(static_cast<unsigned char>(path[pos])) || path[pos] == '_')) {
                throw std::invalid_argument(
                    "expected identifier (starting with a letter or '_') after '.' in "
                    "variant path: " + path);
            }
            size_t start = pos++;
            while (pos < path.size() &&
                   (std::isalnum(static_cast<unsigned char>(path[pos])) || path[pos] == '_')) {
                pos++;
            }
            out.push_back({false, path.substr(start, pos - start), 0});
        } else if (ch == '[') {
            pos++;
            if (pos >= path.size()) {
                throw std::invalid_argument(
                    "unexpected end of input after '[' in variant path: " + path);
            }
            if (path[pos] == '"' || path[pos] == '\'') {
                char quote = path[pos++];
                std::string key;
                bool closed = false;
                while (pos < path.size()) {
                    char c = path[pos++];
                    if (c == '\\') {
                        if (pos >= path.size()) {
                            throw std::invalid_argument(
                                "unterminated escape at end of quoted key in variant path: " +
                                path);
                        }
                        char esc = path[pos++];
                        if (esc == '\\' || esc == quote) {
                            key.push_back(esc);
                        } else {
                            throw std::invalid_argument(
                                "unsupported escape in quoted key of variant path (only '\\\\' "
                                "and backslash+quote are allowed): " + path);
                        }
                    } else if (c == quote) {
                        closed = true;
                        break;
                    } else {
                        key.push_back(c);
                    }
                }
                if (!closed) {
                    throw std::invalid_argument(
                        "unterminated quoted key in variant path: " + path);
                }
                out.push_back({false, key, 0});
            } else {
                if (path[pos] == '-') {
                    throw std::invalid_argument(
                        "negative indices are not supported in variant path: " + path);
                }
                size_t start = pos;
                while (pos < path.size() &&
                       std::isdigit(static_cast<unsigned char>(path[pos]))) {
                    pos++;
                }
                if (pos == start) {
                    throw std::invalid_argument(
                        "expected integer index in variant path: " + path);
                }
                try {
                    long long val = std::stoll(path.substr(start, pos - start));
                    if (val < 0 || val > INT32_MAX) {
                        throw std::out_of_range("range");
                    }
                    out.push_back({true, "", static_cast<int>(val)});
                } catch (const std::exception&) {
                    throw std::invalid_argument(
                        "index out of int range in variant path: " + path);
                }
            }
            if (pos >= path.size() || path[pos] != ']') {
                throw std::invalid_argument("expected ']' in variant path: " + path);
            }
            pos++;
        } else {
            throw std::invalid_argument(
                std::string("unexpected character '") + ch + "' in variant path: " + path);
        }
    }
    return out;
}

std::optional<SrVariant> walkVariantPath(const SrVariant& root, const std::string& path) {
    std::optional<SrVariant> current = root;
    for (const auto& seg : parseVariantPath(path)) {
        if (!current) {
            return std::nullopt;
        }
        if (seg.isIndex) {
            current = current->getType() == VariantType::Array
                          ? current->getElementAtIndex(seg.index)
                          : std::nullopt;
        } else {
            current = current->getType() == VariantType::Object
                          ? current->getFieldByKey(seg.key)
                          : std::nullopt;
        }
    }
    return current;
}

// Backing for variants.as (strict) and variants.tryAs (soft). Extracts a typed value;
// on a type mismatch the strict form errors and the soft form returns CEL null. Types
// with no CEL scalar extraction (object/array/null/date/time/uuid) always error.
absl::Status variantAsImpl(absl::Span<const cel::CelValue> args, cel::CelValue* out,
                           Arena* arena, bool nullOnError) {
    bool isNull = false;
    auto variant = receiverVariant(args[0], &isNull);
    if (isNull) {
        *out = cel::CelValue::CreateNull();
        return absl::OkStatus();
    }
    if (!variant) {
        *out = err(arena, "variants.as: expected a Variant");
        return absl::OkStatus();
    }
    std::string type(args[1].StringOrDie().value());
    VariantType vt = variant->getType();
    bool recognized = true;
    if (type == "string") {
        if (vt == VariantType::String) {
            *out = makeCelString(arena, variant->getString());
            return absl::OkStatus();
        }
    } else if (type == "int") {
        if (vt == VariantType::Byte || vt == VariantType::Short ||
            vt == VariantType::Int || vt == VariantType::Long) {
            *out = cel::CelValue::CreateInt64(variant->getLong());
            return absl::OkStatus();
        }
    } else if (type == "double") {
        if (vt == VariantType::Float) {
            *out = cel::CelValue::CreateDouble(
                static_cast<double>(variant->getFloat()));
            return absl::OkStatus();
        }
        if (vt == VariantType::Double) {
            *out = cel::CelValue::CreateDouble(variant->getDouble());
            return absl::OkStatus();
        }
    } else if (type == "boolean") {
        if (vt == VariantType::Boolean) {
            *out = cel::CelValue::CreateBool(variant->getBoolean());
            return absl::OkStatus();
        }
    } else if (type == "decimal") {
        if (vt == VariantType::Decimal4 || vt == VariantType::Decimal8 ||
            vt == VariantType::Decimal16) {
            std::vector<uint8_t> unscaled;
            int scale = 0;
            variant->getDecimalParts(unscaled, scale);
            *out = wrapDecimal(
                DecimalUtil::fromUnscaledBytes(
                    std::string(unscaled.begin(), unscaled.end()), scale),
                arena);
            return absl::OkStatus();
        }
    } else if (type == "timestamp") {
        if (vt == VariantType::TimestampTz || vt == VariantType::TimestampNtz ||
            vt == VariantType::TimestampNanosTz || vt == VariantType::TimestampNanosNtz) {
            int64_t raw = variant->getLong();
            // Micros variants carry epoch micros; nanos variants carry epoch nanos.
            // absl::Time keeps sub-microsecond resolution, so preserve nanos in full and
            // let absl floor-divide for negatives — matching Java's fromEpochMicros /
            // fromEpochNanos, which use Math.floorDiv into a seconds+nanos proto Timestamp.
            // A raw/1000 truncation would drop the sub-micro nanos and mis-round negative
            // timestamps toward zero.
            absl::Time t = (vt == VariantType::TimestampTz ||
                            vt == VariantType::TimestampNtz)
                               ? absl::FromUnixMicros(raw)
                               : absl::FromUnixNanos(raw);
            *out = cel::CelValue::CreateTimestamp(t);
            return absl::OkStatus();
        }
    } else if (type == "bytes") {
        if (vt == VariantType::Binary) {
            std::vector<uint8_t> bin = variant->getBinary();
            auto* arenaBytes = Arena::Create<std::string>(arena);
            arenaBytes->assign(bin.begin(), bin.end());
            *out = cel::CelValue::CreateBytes(arenaBytes);
            return absl::OkStatus();
        }
    } else if (type == "object" || type == "array" || type == "null" ||
               type == "date" || type == "time" || type == "uuid") {
        // Not extractable as a CEL scalar - always an error, even in the soft form.
        *out = err(arena, "variants.as: type '" + type +
                              "' is not supported for extraction (use variants.type/"
                              "variants.path/variants.field/variants.index instead)");
        return absl::OkStatus();
    } else {
        recognized = false;
    }

    if (!recognized) {
        if (nullOnError) {
            *out = cel::CelValue::CreateNull();
        } else {
            *out = err(arena, "variants.as: unknown type '" + type +
                                  "' (expected one of: string, int, double, boolean, "
                                  "decimal, timestamp, bytes)");
        }
        return absl::OkStatus();
    }

    // Recognized type string, but the variant's actual type does not match.
    if (nullOnError) {
        *out = cel::CelValue::CreateNull();
    } else {
        *out = err(arena, "variants.as: variant is not " + type + "-typed");
    }
    return absl::OkStatus();
}

absl::Status registerVariant(cel::CelFunctionRegistry& registry) {
    // variant(dyn) - runtime dispatch on the actual value.
    absl::Status s = reg(
        registry, "variant", false, {T::kAny},
        [](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
            if (isVariantMessage(args[0])) {
                *out = wrapVariant(variantFromMessage(*args[0].MessageOrDie()), arena);
            } else if (args[0].IsNull()) {
                // CEL null passes through as CEL null (matching the navigation accessors
                // and the Java reference), rather than erroring.
                *out = cel::CelValue::CreateNull();
            } else if (args[0].IsString()) {
                *out = err(arena,
                           "variant: cannot convert string to Variant; use "
                           "variants.parseJson(s) or variants.tryParseJson(s)");
            } else {
                *out = err(arena, "variant: cannot convert value to Variant");
            }
            return absl::OkStatus();
        });
    if (!s.ok()) return s;

    // variant(bytes, bytes) = (value, metadata).
    s = reg(registry, "variant", false, {T::kBytes, T::kBytes},
            [](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                auto value = args[0].BytesOrDie().value();
                auto metadata = args[1].BytesOrDie().value();
                SrVariant variant(std::vector<uint8_t>(value.begin(), value.end()),
                                  std::vector<uint8_t>(metadata.begin(), metadata.end()));
                *out = wrapVariant(variant, arena);
                return absl::OkStatus();
            });
    if (!s.ok()) return s;

    // variants.parseJson(string) - strict (a parse failure surfaces as a CEL error).
    s = reg(registry, "variants.parseJson", false, {T::kString},
            [](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                *out = wrapVariant(
                    SrVariant::parseJson(std::string(args[0].StringOrDie().value())), arena);
                return absl::OkStatus();
            });
    if (!s.ok()) return s;

    // variants.tryParseJson(string) - soft (CEL null on any failure).
    s = reg(registry, "variants.tryParseJson", false, {T::kString},
            [](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                try {
                    *out = wrapVariant(
                        SrVariant::parseJson(std::string(args[0].StringOrDie().value())),
                        arena);
                } catch (const std::exception&) {
                    *out = cel::CelValue::CreateNull();
                }
                return absl::OkStatus();
            });
    if (!s.ok()) return s;

    // variants.type(dyn) -> string label (CEL null passes through).
    s = reg(registry, "variants.type", false, {T::kAny},
            [](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                bool isNull = false;
                auto variant = receiverVariant(args[0], &isNull);
                if (isNull) {
                    *out = cel::CelValue::CreateNull();
                } else if (!variant) {
                    *out = err(arena, "variants.type: expected a Variant");
                } else {
                    *out = makeCelString(arena, variantTypeLabel(variant->getType()));
                }
                return absl::OkStatus();
            });
    if (!s.ok()) return s;

    // variants.isNull(dyn) -> true iff a Variant whose top type is NULL (never errors).
    s = reg(registry, "variants.isNull", false, {T::kAny},
            [](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* /*arena*/) {
                bool result =
                    isVariantMessage(args[0]) &&
                    variantFromMessage(*args[0].MessageOrDie()).getType() ==
                        VariantType::Null;
                *out = cel::CelValue::CreateBool(result);
                return absl::OkStatus();
            });
    if (!s.ok()) return s;

    // variants.path(dyn, string) -> Variant or CEL null on a miss.
    s = reg(registry, "variants.path", false, {T::kAny, T::kString},
            [](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                bool isNull = false;
                auto variant = receiverVariant(args[0], &isNull);
                if (isNull) {
                    *out = cel::CelValue::CreateNull();
                    return absl::OkStatus();
                }
                if (!variant) {
                    *out = err(arena, "variants.path: expected a Variant");
                    return absl::OkStatus();
                }
                auto result =
                    walkVariantPath(*variant, std::string(args[1].StringOrDie().value()));
                *out = result ? wrapVariant(*result, arena) : cel::CelValue::CreateNull();
                return absl::OkStatus();
            });
    if (!s.ok()) return s;

    // variants.field(dyn, string) -> Variant or CEL null on a miss / non-object.
    s = reg(registry, "variants.field", false, {T::kAny, T::kString},
            [](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                bool isNull = false;
                auto variant = receiverVariant(args[0], &isNull);
                if (isNull) {
                    *out = cel::CelValue::CreateNull();
                    return absl::OkStatus();
                }
                if (!variant) {
                    *out = err(arena, "variants.field: expected a Variant");
                    return absl::OkStatus();
                }
                if (variant->getType() != VariantType::Object) {
                    *out = cel::CelValue::CreateNull();
                    return absl::OkStatus();
                }
                auto result =
                    variant->getFieldByKey(std::string(args[1].StringOrDie().value()));
                *out = result ? wrapVariant(*result, arena) : cel::CelValue::CreateNull();
                return absl::OkStatus();
            });
    if (!s.ok()) return s;

    // variants.index(dyn, int) -> Variant or CEL null on out-of-bounds / non-array.
    s = reg(registry, "variants.index", false, {T::kAny, T::kInt64},
            [](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                bool isNull = false;
                auto variant = receiverVariant(args[0], &isNull);
                if (isNull) {
                    *out = cel::CelValue::CreateNull();
                    return absl::OkStatus();
                }
                if (!variant) {
                    *out = err(arena, "variants.index: expected a Variant");
                    return absl::OkStatus();
                }
                if (variant->getType() != VariantType::Array) {
                    *out = cel::CelValue::CreateNull();
                    return absl::OkStatus();
                }
                int64_t idx = args[1].Int64OrDie();
                if (idx < 0 || idx > INT32_MAX) {
                    *out = cel::CelValue::CreateNull();
                    return absl::OkStatus();
                }
                auto result = variant->getElementAtIndex(static_cast<int>(idx));
                *out = result ? wrapVariant(*result, arena) : cel::CelValue::CreateNull();
                return absl::OkStatus();
            });
    if (!s.ok()) return s;

    // variants.as(dyn, string) - strict typed extraction.
    s = reg(registry, "variants.as", false, {T::kAny, T::kString},
            [](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                return variantAsImpl(args, out, arena, /*nullOnError=*/false);
            });
    if (!s.ok()) return s;

    // variants.tryAs(dyn, string) - soft typed extraction (CEL null on mismatch).
    s = reg(registry, "variants.tryAs", false, {T::kAny, T::kString},
            [](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                return variantAsImpl(args, out, arena, /*nullOnError=*/true);
            });
    if (!s.ok()) return s;

    // variants.toJson(dyn) -> JSON string (CEL null passes through).
    s = reg(registry, "variants.toJson", false, {T::kAny},
            [](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
                bool isNull = false;
                auto variant = receiverVariant(args[0], &isNull);
                if (isNull) {
                    *out = cel::CelValue::CreateNull();
                } else if (!variant) {
                    *out = err(arena, "variants.toJson: expected a Variant");
                } else {
                    *out = makeCelString(arena, variant->toJson());
                }
                return absl::OkStatus();
            });
    return s;
}

}  // namespace

absl::Status RegisterExtraFuncs(cel::CelFunctionRegistry& registry, Arena* /*regArena*/) {
    absl::Status s = registerIsFuncs(registry);
    if (!s.ok()) return s;
    s = registerDecimal(registry);
    if (!s.ok()) return s;
    s = registerTimestamp(registry);
    if (!s.ok()) return s;
    return registerVariant(registry);
}

}  // namespace schemaregistry::rules::cel
