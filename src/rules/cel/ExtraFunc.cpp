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

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
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
#include "eval/public/equality_function_registrar.h"
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

// The CEL/protobuf timestamp range: 0001-01-01T00:00:00Z .. 9999-12-31T23:59:59.999999999Z.
// Same values as Java's TimestampUtils.MIN_EPOCH_SECOND / MAX_EPOCH_SECOND.
constexpr int64_t kMinEpochSecond = -62135596800LL;
constexpr int64_t kMaxEpochSecond = 253402300799LL;

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

absl::Status reg(::cel::FunctionRegistry& registry, absl::string_view name, bool receiver,
                 std::vector<T> types, LambdaFunction::Impl impl) {
    // The modern registry takes the descriptor separately from the implementation. A legacy
    // CelFunction supplies both: CelFunctionDescriptor is an alias of ::cel::FunctionDescriptor
    // and CelFunction derives from ::cel::Function, so no rewrite of the lambdas is needed.
    auto fn =
        std::make_unique<LambdaFunction>(name, receiver, std::move(types), std::move(impl));
    ::cel::FunctionDescriptor descriptor = fn->descriptor();
    return registry.Register(descriptor, std::move(fn));
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

// Rewrite an exact div/sqrt result to the reference's preferred scale: strip trailing zeros
// down to - never below - `preferred_scale`, then pad back up to it when the natural scale is
// smaller. Only ever called on an exact result; padding an inexact one would claim digits it
// does not have.
//
// libmpdec and BigDecimal disagree here, and only on square root: the decimal arithmetic
// spec's ideal exponent for sqrt is floor(exponent / 2), while BigDecimal.sqrt uses
// scale / 2 truncated toward zero. Those are the same for an even scale and one apart for an
// odd one, so `sqrt(9.0)` is "3.0" natively and "3" in the reference. Division needs no such
// step: the spec's ideal exponent for divide is exponent(dividend) - exponent(divisor), which
// *is* the preferred scale.
//
// A zero takes the preferred scale outright, in both directions, because BigDecimal returns
// zeroValueOf(preferredScale) for it. reduce() leaves a zero at exponent 0, so it can never
// reach a negative scale; the zero case has to be separate.
decimal::Decimal applyPreferredScale(const decimal::Decimal& value, int64_t preferred_scale,
                                     const std::string& fn) {
    decimal::Context c = decimal::MaxContext();
    if (value.iszero()) {
        return value.rescale(-preferred_scale, c);
    }
    decimal::Decimal minimal = value.reduce(c);
    const int64_t target = std::max(preferred_scale, -minimal.exponent());
    DecimalUtil::requireSaneWidth(DecimalUtil::rescaledDigits(target, minimal), fn,
                                  "a scale of " + std::to_string(target));
    return minimal.rescale(-target, c);
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
        // Shortest round-tripping form, via std::to_chars. This deliberately does *not*
        // reproduce BigDecimal.valueOf(double), which routes through Double.toString: that
        // always writes at least one fractional digit and uses plain notation only for
        // 1e-3 <= |d| < 1e7, so Java reads 5.0 at scale 1 and 1e7 at scale -6 where
        // to_chars gives "5" (scale 0) and "1e+07" (scale -7). Byte-identical float/double
        // rendering across the clients was designed, implemented in all seven and then
        // deliberately backed out on cost, so each client keeps its native rendering; do not
        // "fix" this toward Java without revisiting that.
        //
        // NaN/Inf have no decimal representation (BigDecimal.valueOf throws on them), and
        // std::to_chars would otherwise emit "nan"/"inf" that Decimal would parse into a
        // non-finite value.
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

absl::Status registerDecimal(::cel::FunctionRegistry& registry) {
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
                // The scale costs nothing here - it only sets the exponent, and the width
                // guard fires later where the digits are needed. The coefficient is the risk
                // on this path, and it is checked from the byte count before the digits are
                // built: one byte carries about 2.41 decimal digits.
                const absl::string_view raw = args[0].BytesOrDie().value();
                DecimalUtil::requireSaneWidth(
                    static_cast<int64_t>(raw.size() * 241 / 100) + 1, "decimal(bytes, scale)",
                    "the coefficient", DecimalUtil::kSaneCoefficient);
                auto d = DecimalUtil::fromUnscaledBytes(
                    std::string(raw),
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
    // add/sub align their operands, so the aligned frame has to be built before a single digit
    // is computed - measured in the sibling client, `add(1e2147483647, 3)` costs 1738 MB and
    // `add(1e2147483647, 1e-2147483647)` 3125 MB, from one expression over two cheaply
    // constructed operands. exactContext() is MaxContext(), so nothing below this bounds it.
    if (s = bin("decimals.add",
                [](auto a, auto b, std::string*) {
                    DecimalUtil::requireAlignable(a, b, "decimals.add");
                    return a.add(b, DecimalUtil::exactContext());
                });
        !s.ok())
        return s;
    if (s = bin("decimals.sub",
                [](auto a, auto b, std::string*) {
                    DecimalUtil::requireAlignable(a, b, "decimals.sub");
                    return a.sub(b, DecimalUtil::exactContext());
                });
        !s.ok())
        return s;
    // mul is unguarded at any width: it adds the exponents and multiplies the coefficients,
    // so the result is as compact as its operands. Measured at 13 MB where add costs 1738 MB.
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
                    // Use the exact (unbounded-precision) context — Java's decimals.mod uses
                    // BigDecimal.remainder(...) with no MathContext (exact). The 38-digit
                    // context() would trap (Division_impossible) when the integer quotient
                    // exceeds 38 digits, e.g. mod(1E40, 3), turning a valid result into a
                    // CEL error. exactContext() (MaxContext) computes the remainder exactly.
                    //
                    // Which is why the width has to be bounded here: the remainder itself is
                    // small, but the *integral quotient* has to be produced to get there.
                    // Not the aligned frame add/sub use - libmpdec short-circuits when the
                    // dividend is the smaller operand or the magnitudes are close, so the
                    // frame would refuse `1e-2147483647 mod 1e2147483647` (which is the
                    // dividend itself) and `1e2147483647 mod 1e2147483000` (647 quotient
                    // digits), both free and both accepted by the JVM.
                    // A zero dividend has a quotient of zero whatever the scales, and its
                    // adjusted exponent says nothing useful - a zero keeps the scale it was
                    // built with, so `0E+2e9 mod 1E-2e9` estimated 4e9 digits for a result
                    // that is just zero. Free on libmpdec; the JDK returns 0 at precision 1.
                    DecimalUtil::requireSaneWidth(
                        a.iszero() ? 1 : std::max<int64_t>(0, a.adjexp() - b.adjexp()) + 1,
                        "decimals.mod", "the integral quotient");
                    return a.rem(b, DecimalUtil::exactContext());
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
                   decimal::Decimal root = a.sqrt(DecimalUtil::context());
                   // An inexact root keeps all 38 digits: a trailing zero there is
                   // significant (1/99 ends in one), so only an exact root is rewritten.
                   if (decimalCompare(root.mul(root, DecimalUtil::exactContext()), a) != 0) {
                       return root;
                   }
                   // C++ integer division truncates toward zero, which is what BigDecimal's
                   // `scale / 2` does - including for the negative scale of a value like
                   // 250E+3, whose root is scale -1 and not the floor's -2.
                   const int64_t scale = -a.exponent();
                   return applyPreferredScale(root, scale / 2, "decimals.sqrt");
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
    // floor/ceil rescale to scale 0 without going through roundTo, so they carry the same
    // bound. Reachable from a rule that names no scale at all: `floor(x)` on a value with a
    // large negative exponent is a multi-GB allocation.
    if (s = un("decimals.floor",
               [](auto a, std::string*) {
                   if (!a.iszero()) {
                       DecimalUtil::requireSaneWidth(DecimalUtil::rescaledDigits(0, a),
                                                     "decimals.floor", "rounding to an integer");
                   }
                   return a.floor(DecimalUtil::context());
               });
        !s.ok())
        return s;
    if (s = un("decimals.ceil",
               [](auto a, std::string*) {
                   if (!a.iszero()) {
                       DecimalUtil::requireSaneWidth(DecimalUtil::rescaledDigits(0, a),
                                                     "decimals.ceil", "rounding to an integer");
                   }
                   return a.ceil(DecimalUtil::context());
               });
        !s.ok())
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
    //
    // No precision cap, because BigDecimal.setScale takes no MathContext: the JVM's
    // round/trunc/floor/ceil rescale and never shorten the coefficient. That matters now that
    // the coefficient is arbitrary-width - `decimals.add` on two operands at CEL's own
    // 38-digit precision already yields 39 digits, and `decimals.mul` 76.
    //
    // The `prec(38)` this used to carry was inert rather than wrong: mpd_qrescale "ignores
    // precision, emax, emin, but uses the rounding mode" (mpdecimal's own comment on
    // _mpd_qrescale), so the rounding mode was the only thing ever read from this context.
    // Built from MaxContext so the code says that, and so a later switch to quantize - which
    // *does* observe prec - cannot start capping silently.
    //
    // Every rescale in the family goes through this one lambda, the one-argument forms
    // included, so the width bound below covers all five call sites. MaxContext() is what
    // makes the bound necessary: libmpdec honours any int32 scale and materialises the whole
    // coefficient, and a 2**31-digit rescale costs 918 MB. Zero is exempt - rescaling it
    // never expands anything and its result stays compact, which BigDecimal agrees with
    // (`new BigDecimal(BigInteger.ZERO, 2147483647)` is precision 1).
    auto roundTo = [](const decimal::Decimal& d, int32_t scale, int round) {
        if (!d.iszero()) {
            DecimalUtil::requireSaneWidth(DecimalUtil::rescaledDigits(scale, d),
                                          "decimals.round",
                                          "a scale of " + std::to_string(scale));
        }
        decimal::Context c = decimal::MaxContext();
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
                // `format("f")` writes every digit of the positional form, and that form can
                // be enormous for a value that was cheap to compute: `div` holds its
                // coefficient to 38 digits while its exponent runs free, so
                // `decimals.div(decimal("1e-2147483647"), decimal("1e2147483647"))` costs
                // nothing and renders as four billion characters. No zero shortcut here,
                // unlike the rescale guard - a zero at an extreme scale renders as that many
                // zeros.
                DecimalUtil::requireSaneWidth(DecimalUtil::plainFormLength(d), "string",
                                              "the plain form");
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
                // std::from_chars, not std::stod or strtod. std::stod throws
                // std::out_of_range on BOTH overflow AND underflow, so a tiny magnitude
                // (e.g. 1e-320) would wrongly become ±Infinity. strtod does not throw, but it
                // reads the radix character from LC_NUMERIC while format("f") always writes
                // '.', so a comma-radix locale in the host process stopped the parse at the
                // dot and double(decimal("100.50")) returned 100. from_chars is defined to
                // ignore the locale.
                //
                // The target behaviour is Java's BigDecimal.doubleValue(): 1e-400 -> 0.0,
                // 1e-320 -> a tiny subnormal, 1e400 -> +Infinity, -1e400 -> -Infinity.
                // from_chars reports an unrepresentable magnitude as result_out_of_range and
                // leaves the value unspecified, so the limit is derived from the text: a
                // magnitude with a nonzero integer part is too large (±Infinity), otherwise
                // it is too small (±0.0). format("f") is plain (non-scientific) notation.
                // Same bound as string(): this arm parses the plain form, so it builds it.
                DecimalUtil::requireSaneWidth(DecimalUtil::plainFormLength(d), "double",
                                              "the plain form");
                const std::string text = d.format("f");
                double val = 0.0;
                const auto parsed =
                    std::from_chars(text.data(), text.data() + text.size(), val);
                if (parsed.ec == std::errc::result_out_of_range) {
                    const bool negative = !text.empty() && text[0] == '-';
                    const std::size_t intStart = negative ? 1 : 0;
                    const std::size_t dot = text.find('.', intStart);
                    const std::size_t intEnd =
                        dot == std::string::npos ? text.size() : dot;
                    const bool tooLarge =
                        text.find_first_not_of('0', intStart) < intEnd;
                    const double magnitude =
                        tooLarge ? std::numeric_limits<double>::infinity() : 0.0;
                    val = negative ? -magnitude : magnitude;
                }
                *out = cel::CelValue::CreateDouble(val);
                return absl::OkStatus();
            });
    return s;
}

// ---------------------------------------------------------------------------
// Numeric equality for Decimal.
//
// A CEL Decimal is a confluent.type.Decimal proto message (unscaled value bytes +
// scale). cel-cpp's default `==` on two messages does field-by-field proto equality
// (google::protobuf::util::MessageDifferencer), which is SCALE-SENSITIVE: decimal("2.0")
// (value=0x14/scale=1) and decimal("2.00") (value=0xC8/scale=2) are different messages
// and would compare unequal. Java/Python/the other clients treat decimal `==` as NUMERIC
// (matching decimals.eq), so 2.0 == 2.00.
//
// Mechanism: cel-cpp's flat_expr_builder installs its built-in heterogeneous-equality
// evaluation step ONLY when the registry has no custom `_==_` overload (it probes with
// FindOverloads(kEqual, {kAny, kAny}), and kAny matches any registered shape). Registering
// `_==_`/`_!=_` here therefore *replaces* the built-in step entirely, so this overload must
// handle EVERY operand type: two Decimals compare numerically; everything else delegates to
// cel-cpp's own CelValueEqualImpl — the very routine the built-in step used — preserving
// standard heterogeneous equality (ints, strings, lists, maps, cross-numeric, messages, …).
// ---------------------------------------------------------------------------

// Whether a value is a Decimal or holds one, at any depth. Only consulted once both operands
// are containers, so it never runs on the scalar comparison path.
bool hasDecimal(const cel::CelValue& v, Arena* arena) {
    if (isDecimalMessage(v)) {
        return true;
    }
    if (v.IsList()) {
        const cel::CelList* list = v.ListOrDie();
        for (int i = 0; i < list->size(); ++i) {
            if (hasDecimal(list->Get(arena, i), arena)) return true;
        }
        return false;
    }
    if (v.IsMap()) {
        const cel::CelMap* map = v.MapOrDie();
        auto keys = map->ListKeys(arena);
        if (!keys.ok()) return false;
        for (int i = 0; i < (*keys)->size(); ++i) {
            auto value = map->Get(arena, (*keys)->Get(arena, i));
            if (value.has_value() && hasDecimal(*value, arena)) return true;
        }
    }
    return false;
}

// CEL == with decimals made numeric, at any depth. Returns nullopt when the comparison is
// undefined, matching cel::CelValueEqualImpl's contract.
//
// A Decimal is a confluent.type.Decimal message here, and comparing two of those structurally -
// field by field over unscaled bytes and scale - calls 12.34 and 12.340 unequal even though they
// are the same number. Containers are handled too, but only when a decimal is actually inside
// one: the general implementation recurses with its own equality, so a Decimal nested in a list
// or map was compared structurally and `[a] == [b]` disagreed with `a == b` on the same values.
// Gating on hasDecimal leaves every decimal-free comparison on the general path exactly as it
// was, and each element pair recurses back through here so a non-decimal element inside a
// decimal-bearing container still gets general semantics.
absl::optional<bool> decimalAwareEqual(const cel::CelValue& a, const cel::CelValue& b,
                                       Arena* arena) {
    if (isDecimalMessage(a) && isDecimalMessage(b)) {
        return decimalCompare(decimalFromMessage(*a.MessageOrDie()),
                              decimalFromMessage(*b.MessageOrDie())) == 0;
    }
    if (isDecimalMessage(a) || isDecimalMessage(b)) {
        // A decimal is never equal to a non-decimal.
        return false;
    }
    if (a.IsList() && b.IsList() && (hasDecimal(a, arena) || hasDecimal(b, arena))) {
        const cel::CelList* al = a.ListOrDie();
        const cel::CelList* bl = b.ListOrDie();
        if (al->size() != bl->size()) return false;
        for (int i = 0; i < al->size(); ++i) {
            absl::optional<bool> eq =
                decimalAwareEqual(al->Get(arena, i), bl->Get(arena, i), arena);
            if (!eq.has_value()) return absl::nullopt;
            if (!*eq) return false;
        }
        return true;
    }
    if (a.IsMap() && b.IsMap() && (hasDecimal(a, arena) || hasDecimal(b, arena))) {
        const cel::CelMap* am = a.MapOrDie();
        const cel::CelMap* bm = b.MapOrDie();
        if (am->size() != bm->size()) return false;
        auto keys = am->ListKeys(arena);
        if (!keys.ok()) return absl::nullopt;
        for (int i = 0; i < (*keys)->size(); ++i) {
            cel::CelValue key = (*keys)->Get(arena, i);
            auto av = am->Get(arena, key);
            auto bv = bm->Get(arena, key);
            if (!av.has_value() || !bv.has_value()) return false;
            absl::optional<bool> eq = decimalAwareEqual(*av, *bv, arena);
            if (!eq.has_value()) return absl::nullopt;
            if (!*eq) return false;
        }
        return true;
    }
    return cel::CelValueEqualImpl(a, b);
}

absl::Status registerEquality(::cel::FunctionRegistry& registry) {
    auto equality = [&registry](const char* name, bool negate) {
        return reg(
            registry, name, false, {T::kAny, T::kAny},
            [name, negate](absl::Span<const cel::CelValue> args, cel::CelValue* out,
                           Arena* arena) {
                absl::optional<bool> eq = decimalAwareEqual(args[0], args[1], arena);
                if (!eq.has_value()) {
                    *out = cel::CreateNoMatchingOverloadError(arena, name);
                    return absl::OkStatus();
                }
                *out = cel::CelValue::CreateBool(negate ? !*eq : *eq);
                return absl::OkStatus();
            });
    };
    absl::Status s;
    if (s = equality("_==_", /*negate=*/false); !s.ok()) return s;
    if (s = equality("_!=_", /*negate=*/true); !s.ok()) return s;

    // `in` is NOT overridden here, and cannot be: it stays structural for a decimal, so
    // `a in [b]` is false for 1.50 against 1.5 while `a == b` is true.
    //
    // The reason is the planner, not the registry. With enable_heterogeneous_equality set,
    // FlatExprVisitor installs its own call handler for @in / in / _in_ and emits a direct
    // interpretable via CreateInStep (eval/compiler/flat_expr_builder.cc,
    // HandleHeterogeneousEqualityIn), so membership never reaches the function registry -
    // registering an overload for it, in any signature, has no effect. Verified identical in
    // cel-cpp 0.11 and 0.16.
    //
    // _==_ escapes that only by accident of a check the same code makes: the planner takes it
    // over *only if* FindOverloads(kEqual, {kAny, kAny}) comes back empty. The (any, any)
    // registration above is what keeps that check non-empty, which is why the equality override
    // works at all - and why removing it would silently hand == back to the planner rather than
    // producing an error. There is no equivalent check for @in.
    //
    // Closing the gap needs one of: an upstream cel-cpp change adding the same detection for
    // @in (~6 lines, mirroring kEqual), or disabling enable_heterogeneous_equality, which would
    // change comparison semantics across every rule and is not worth it for this.
    return absl::OkStatus();
}

// ---------------------------------------------------------------------------
// Timestamp function registration.
// ---------------------------------------------------------------------------

absl::Status registerTimestamp(::cel::FunctionRegistry& registry) {
    // One overload on the *standard* timestamp constructor, rather than a timestamp.of
    // namespace of our own: timestamp(int, int), an epoch value at a Flink-style decimal
    // precision (0 seconds, 3 millis, 6 micros, 9 nanos). Arity 2 collides with nothing in
    // cel-cpp's standard library, so timestamp(string), timestamp(int) (epoch seconds) and
    // timestamp(timestamp) all keep their standard implementations.
    //
    // Nothing is needed for the one-argument non-int cases: fromAvroValue converts a timestamp
    // logical type straight to a CEL timestamp and CelProtoWrapper downcasts the proto WKT to
    // one, so both already satisfy the standard identity overload with no wrapper.
    return reg(
        registry, "timestamp", false, {T::kInt64, T::kInt64},
        [](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
            int64_t value = args[0].Int64OrDie();
            int64_t precision = args[1].Int64OrDie();
            absl::Time t;
            // Precisions outside {0, 3, 6, 9} are rejected rather than generalized to "any p
            // means 10^-p": with the unit a number rather than a name, that check is the only
            // thing between a typo and a silently wrong instant.
            switch (precision) {
                case 0:
                    t = absl::FromUnixSeconds(value);
                    break;
                case 3:
                    t = absl::FromUnixMillis(value);
                    break;
                case 6:
                    t = absl::FromUnixMicros(value);
                    break;
                case 9:
                    t = absl::FromUnixNanos(value);
                    break;
                default:
                    *out = err(arena, "timestamp: unknown precision " +
                                          std::to_string(precision) +
                                          "; expected 0 (seconds), 3 (millis), 6 (micros) or "
                                          "9 (nanos)");
                    return absl::OkStatus();
            }
            // CEL timestamps span 0001-01-01T00:00:00Z..9999-12-31T23:59:59.999999999Z, and
            // Java's instantOfEpoch enforces exactly that ("Timestamp out of range: ...").
            // Without it timestamp(INT64_MAX, 0) produced an out-of-range absl::Time that was
            // handed back as a timestamp, where the one-argument constructor reports overflow.
            const int64_t seconds = absl::ToUnixSeconds(t);
            if (seconds < kMinEpochSecond || seconds > kMaxEpochSecond) {
                *out = err(arena, "timestamp: out of range: " + std::to_string(seconds) +
                                      " seconds since the epoch is outside "
                                      "0001-01-01T00:00:00Z..9999-12-31T23:59:59.999999999Z");
                return absl::OkStatus();
            }
            *out = cel::CelValue::CreateTimestamp(t);
            return absl::OkStatus();
        });
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

absl::Status registerIsFuncs(::cel::FunctionRegistry& registry) {
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
// An absent variant carries no metadata at all: a Protobuf field left unset, or an Avro
// variant record whose byte fields are empty. There is nothing to read, so callers report it as
// CEL null. The check has to precede construction: the SrVariant constructor reads the metadata
// version byte and throws VariantException on an empty buffer.
std::optional<SrVariant> variantFromMessage(const google::protobuf::Message& msg) {
    const auto* desc = msg.GetDescriptor();
    const auto* refl = msg.GetReflection();
    std::string metaScratch;
    std::string valueScratch;
    const std::string& metadata = refl->GetStringReference(
        msg, desc->FindFieldByName("metadata"), &metaScratch);
    const std::string& value = refl->GetStringReference(
        msg, desc->FindFieldByName("value"), &valueScratch);
    if (metadata.empty()) {
        return std::nullopt;
    }
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
        auto variant = variantFromMessage(*value.MessageOrDie());
        if (!variant) {
            // Absent: nothing to read, so it propagates as CEL null like a null receiver.
            *isNull = true;
        }
        return variant;
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
            // Identifier characters. The path is UTF-8; without ICU (not a dependency here)
            // we cannot do full Unicode letter classification like Java's
            // Character.isLetter/isLetterOrDigit, so we approximate: any byte >= 0x80 (a
            // UTF-8 lead/continuation byte of a non-ASCII codepoint) is accepted as an
            // identifier character, in addition to the ASCII letters/digits/'_'. This is
            // broader than Java's Character.isLetter (it also admits e.g. non-ASCII symbols),
            // but it covers all real-world identifier keys such as "café", "über", CJK, etc.
            auto isIdentStart = [](unsigned char b) {
                return std::isalpha(b) || b == '_' || b >= 0x80;
            };
            auto isIdentCont = [](unsigned char b) {
                return std::isalnum(b) || b == '_' || b >= 0x80;
            };
            if (pos >= path.size() ||
                !isIdentStart(static_cast<unsigned char>(path[pos]))) {
                throw std::invalid_argument(
                    "expected identifier (starting with a letter or '_') after '.' in "
                    "variant path: " + path);
            }
            size_t start = pos++;
            while (pos < path.size() &&
                   isIdentCont(static_cast<unsigned char>(path[pos]))) {
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
            bool micros = vt == VariantType::TimestampTz ||
                          vt == VariantType::TimestampNtz;
            // A variant timestamp spans the whole int64 range while a CEL timestamp is
            // 0001-9999, so an out-of-range value is reachable from data. Refused rather
            // than built, and routed through nullOnError like a type mismatch:
            // variants.as errors and names the range, variants.tryAs answers CEL null so a
            // rule can guard. Built regardless, it left an invalid instant in the type
            // system - cel-cpp refuses to render it, so only comparisons could consume it,
            // which is where a wrong answer hides. Mirrors the reference's
            // variantGetTimestamp.
            int64_t perSecond = micros ? 1000000 : 1000000000;
            int64_t seconds = raw / perSecond;
            if (raw % perSecond != 0 && raw < 0) {
                seconds--;  // floor, matching Math.floorDiv
            }
            if (seconds < kMinEpochSecond || seconds > kMaxEpochSecond) {
                if (nullOnError) {
                    *out = cel::CelValue::CreateNull();
                    return absl::OkStatus();
                }
                *out = err(arena, "variants.as: timestamp " + std::to_string(raw) +
                                      " is outside 0001-01-01T00:00:00Z.."
                                      "9999-12-31T23:59:59.999999999Z");
                return absl::OkStatus();
            }
            absl::Time t = micros ? absl::FromUnixMicros(raw)
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

absl::Status registerVariant(::cel::FunctionRegistry& registry) {
    // variant(dyn) - runtime dispatch on the actual value.
    absl::Status s = reg(
        registry, "variant", false, {T::kAny},
        [](absl::Span<const cel::CelValue> args, cel::CelValue* out, Arena* arena) {
            if (isVariantMessage(args[0])) {
                auto variant = variantFromMessage(*args[0].MessageOrDie());
                *out = variant ? wrapVariant(*variant, arena) : cel::CelValue::CreateNull();
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
                if (metadata.empty()) {
                    *out = err(arena,
                               "variant: metadata is empty, so there is no variant to read");
                    return absl::OkStatus();
                }
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
                bool result = false;
                if (isVariantMessage(args[0])) {
                    auto variant = variantFromMessage(*args[0].MessageOrDie());
                    // An absent variant is not a JSON null - there is nothing to read - so
                    // this stays false rather than erroring.
                    result = variant.has_value() &&
                             variant->getType() == VariantType::Null;
                }
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

absl::Status RegisterExtraFuncs(::cel::FunctionRegistry& registry, Arena* /*regArena*/) {
    absl::Status s = registerIsFuncs(registry);
    if (!s.ok()) return s;
    s = registerDecimal(registry);
    if (!s.ok()) return s;
    s = registerEquality(registry);
    if (!s.ok()) return s;
    s = registerTimestamp(registry);
    if (!s.ok()) return s;
    return registerVariant(registry);
}

}  // namespace schemaregistry::rules::cel
