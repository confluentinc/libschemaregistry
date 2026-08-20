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

#include <charconv>
#include <functional>
#include <memory>
#include <regex>
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
#include "schemaregistry/rules/cel/DecimalUtil.h"

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
        return impl_(args, result, arena);
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
        // Shortest round-tripping form, matching BigDecimal.valueOf(double).
        char buf[32];
        auto res = std::to_chars(buf, buf + sizeof(buf), v.DoubleOrDie());
        return decimal::Decimal(std::string(buf, res.ptr));
    }
    if (v.IsBytes()) {
        *error_out = "decimal: raw bytes need a scale; use decimal(bytes, scale)";
        return absl::nullopt;
    }
    *error_out = "decimal: cannot convert value to Decimal";
    return absl::nullopt;
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
                auto d = DecimalUtil::fromUnscaledBytes(std::string(args[0].BytesOrDie().value()),
                                                        static_cast<int32_t>(args[1].Int64OrDie()));
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
    if (s = bin("decimals.add", [](auto a, auto b, std::string*) { return a.add(b, DecimalUtil::context()); });
        !s.ok())
        return s;
    if (s = bin("decimals.sub", [](auto a, auto b, std::string*) { return a.sub(b, DecimalUtil::context()); });
        !s.ok())
        return s;
    if (s = bin("decimals.mul", [](auto a, auto b, std::string*) { return a.mul(b, DecimalUtil::context()); });
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
    if (s = un("decimals.neg", [](auto a, std::string*) { return a.minus(DecimalUtil::context()); }); !s.ok())
        return s;
    if (s = un("decimals.abs",
               [](auto a, std::string*) { return decimalIsZero(a) || a.sign() > 0 ? a : a.minus(DecimalUtil::context()); });
        !s.ok())
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
                                           static_cast<int32_t>(args[1].Int64OrDie()),
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
                                           static_cast<int32_t>(args[1].Int64OrDie())),
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

}  // namespace

absl::Status RegisterExtraFuncs(cel::CelFunctionRegistry& registry, Arena* /*regArena*/) {
    absl::Status s = registerIsFuncs(registry);
    if (!s.ok()) return s;
    s = registerDecimal(registry);
    if (!s.ok()) return s;
    return registerTimestamp(registry);
}

// Test hooks declared in the header.
bool IsIpv4Prefix(std::string_view toValidate, bool /*strict*/) { return validateIpv4(toValidate); }
bool IsIpv6Prefix(std::string_view toValidate, bool /*strict*/) { return validateIpv6(toValidate); }
bool IsIpPrefix(std::string_view toValidate, bool strict) {
    return IsIpv4Prefix(toValidate, strict) || IsIpv6Prefix(toValidate, strict);
}
bool IsHostAndPort(std::string_view toValidate, bool /*portRequired*/) {
    return validateHostname(toValidate);
}

}  // namespace schemaregistry::rules::cel
