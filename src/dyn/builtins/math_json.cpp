// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Number, Boolean, Math, JSON and a small Date.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <random>

#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/numconv.hpp"
#include "script/internal/dyn/vm.hpp"
#include "script/internal/stack_guard.hpp"

namespace script {
namespace internal {
namespace dyn {

namespace {

double thisNumber(CallInfo& ci)
{
    const Value self = ci.thisValue();
    if (self.isNumber())
        return numberOf(self);
    if (DynObject* o = asObject(self); o && o->cls == ObjClass::NumberObject)
        return numberOf(static_cast<DynWrapper*>(o)->primitive);
    ci.fail(ErrorCode::TypeError, "Number.prototype method called on a non-number");
}

double thisDate(CallInfo& ci)
{
    DynObject* o = asObject(ci.thisValue());
    if (!o || o->cls != ObjClass::Date)
        ci.fail(ErrorCode::TypeError, "not a Date");
    return numberOf(static_cast<DynWrapper*>(o)->primitive);
}

std::tm utcOf(double millis)
{
    const std::time_t t = std::time_t(std::floor(millis / 1000));
    std::tm out{};
#if defined(_WIN32)
    gmtime_s(&out, &t);
#else
    gmtime_r(&t, &out);
#endif
    return out;
}

void installNumber(Runtime& rt)
{
    DynObject* proto = rt.newObject(rt.objectProto.get());
    rt.numberProto.reset(proto, true);

    DynFunction* ctor = rt.makeConstructor("Number", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        const double n = ci.size() ? toNumber(rt2, ci.arg(0)) : 0;
        if (!ci.isConstructorCall())
            return Value(n);
        return toObject(rt2, Value(n));
    }, 1, proto);

    rt.constant(ctor, "MAX_SAFE_INTEGER", Value(9007199254740991.0));
    rt.constant(ctor, "MIN_SAFE_INTEGER", Value(-9007199254740991.0));
    rt.constant(ctor, "MAX_VALUE", Value(std::numeric_limits<double>::max()));
    rt.constant(ctor, "MIN_VALUE", Value(std::numeric_limits<double>::denorm_min()));
    rt.constant(ctor, "EPSILON", Value(std::numeric_limits<double>::epsilon()));
    rt.constant(ctor, "POSITIVE_INFINITY", Value(std::numeric_limits<double>::infinity()));
    rt.constant(ctor, "NEGATIVE_INFINITY", Value(-std::numeric_limits<double>::infinity()));
    rt.constant(ctor, "NaN", Value(std::nan("")));

    rt.method(ctor, "isInteger", [](CallInfo& ci) {
        const Value v = ci.arg(0);
        return Value(v.isNumber() && std::isfinite(numberOf(v)) &&
                     numberOf(v) == std::floor(numberOf(v)));
    }, 1);
    rt.method(ctor, "isSafeInteger", [](CallInfo& ci) {
        const Value v = ci.arg(0);
        return Value(v.isNumber() && std::isfinite(numberOf(v)) &&
                     numberOf(v) == std::floor(numberOf(v)) &&
                     std::fabs(numberOf(v)) <= 9007199254740991.0);
    }, 1);
    rt.method(ctor, "isFinite", [](CallInfo& ci) {
        return Value(ci.arg(0).isNumber() && std::isfinite(numberOf(ci.arg(0))));
    }, 1);
    rt.method(ctor, "isNaN", [](CallInfo& ci) {
        return Value(ci.arg(0).isNumber() && std::isnan(numberOf(ci.arg(0))));
    }, 1);
    rt.method(ctor, "parseFloat", [](CallInfo& ci) {
        return Value(parseFloatPrefix(toStringValue(rtOf(ci), ci.arg(0))));
    }, 1);
    rt.method(ctor, "parseInt", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        return Value(parseIntPrefix(toStringValue(rt2, ci.arg(0)),
                                    ci.arg(1).isUndefined() ? 0 : int(toNumber(rt2, ci.arg(1)))));
    }, 2);

    rt.method(proto, "valueOf", [](CallInfo& ci) { return Value(thisNumber(ci)); }, 0);

    rt.method(proto, "toString", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const double n = thisNumber(ci);
        const int radix = ci.arg(0).isUndefined() ? 10 : int(toNumber(rt2, ci.arg(0)));
        if (radix < 2 || radix > 36)
            ci.fail(ErrorCode::RangeError, "toString radix must be between 2 and 36");
        return rt2.str(numberToStringRadix(n, radix));
    }, 1);

    rt.method(proto, "toFixed", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const int digits = int(toNumber(rt2, ci.arg(0)));
        if (digits < 0 || digits > 100)
            ci.fail(ErrorCode::RangeError, "toFixed digits must be between 0 and 100");
        return rt2.str(numberToFixed(thisNumber(ci), digits));
    }, 1);

    rt.method(proto, "toPrecision", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        if (ci.arg(0).isUndefined())
            return rt2.str(numberToString(thisNumber(ci)));
        const int digits = int(toNumber(rt2, ci.arg(0)));
        if (digits < 1 || digits > 100)
            ci.fail(ErrorCode::RangeError, "toPrecision digits must be between 1 and 100");
        return rt2.str(numberToPrecision(thisNumber(ci), digits));
    }, 1);

    rt.method(proto, "toExponential", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const int digits = ci.arg(0).isUndefined() ? 6 : int(toNumber(rt2, ci.arg(0)));
        return rt2.str(numberToExponential(thisNumber(ci), digits));
    }, 1);

    rt.method(proto, "toLocaleString", [](CallInfo& ci) {
        return rtOf(ci).str(numberToString(thisNumber(ci)));
    }, 0);
}

void installBoolean(Runtime& rt)
{
    DynObject* proto = rt.newObject(rt.objectProto.get());
    rt.booleanProto.reset(proto, true);

    rt.makeConstructor("Boolean", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        const bool b = toBoolean(ci.arg(0));
        if (!ci.isConstructorCall())
            return Value(b);
        return toObject(rt2, Value(b));
    }, 1, proto);

    rt.method(proto, "valueOf", [](CallInfo& ci) -> Value {
        if (ci.thisValue().isBool())
            return ci.thisValue();
        if (DynObject* o = asObject(ci.thisValue()); o && o->cls == ObjClass::BooleanObject)
            return static_cast<DynWrapper*>(o)->primitive;
        ci.fail(ErrorCode::TypeError, "not a boolean");
    }, 0);

    rt.method(proto, "toString", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        return rt2.str(toBoolean(invokeMethod(rt2, ci.thisValue(), rt2.atom.valueOf.get(), {}))
                           ? "true"
                           : "false");
    }, 0);
}

void installMath(Runtime& rt)
{
    DynObject* math = rt.newObject(rt.objectProto.get());
    rt.constant(rt.globalObject.get(), "Math", detail::Access::adopt(Type::Object, math));
    math->putOwn(rt.sym.toStringTag.get(), rt.str("Math"), PropConfigurable);

    rt.constant(math, "PI", Value(3.141592653589793));
    rt.constant(math, "E", Value(2.718281828459045));
    rt.constant(math, "LN2", Value(0.6931471805599453));
    rt.constant(math, "LN10", Value(2.302585092994046));
    rt.constant(math, "LOG2E", Value(1.4426950408889634));
    rt.constant(math, "LOG10E", Value(0.4342944819032518));
    rt.constant(math, "SQRT2", Value(1.4142135623730951));
    rt.constant(math, "SQRT1_2", Value(0.7071067811865476));

    auto unary = [&rt, math](const char* name, double (*fn)(double)) {
        rt.method(math, name, [fn](CallInfo& ci) {
            return Value(fn(toNumber(rtOf(ci), ci.arg(0))));
        }, 1);
    };
    unary("abs", [](double d) { return std::fabs(d); });
    unary("floor", [](double d) { return std::floor(d); });
    unary("ceil", [](double d) { return std::ceil(d); });
    unary("trunc", [](double d) { return std::trunc(d); });
    unary("sqrt", [](double d) { return std::sqrt(d); });
    const char* inlined[] = {"sqrt", "abs", "floor", "ceil", "trunc"};
    for (int i = 0; i < 5; ++i)
        rt.intrinsics[Runtime::MathSqrt + i].reset(detail::Access::ptr(math->findOwn(rt.intern(inlined[i]))->value));
    unary("cbrt", [](double d) { return std::cbrt(d); });
    unary("exp", [](double d) { return std::exp(d); });
    unary("expm1", [](double d) { return std::expm1(d); });
    unary("log", [](double d) { return std::log(d); });
    unary("log2", [](double d) { return std::log2(d); });
    unary("log10", [](double d) { return std::log10(d); });
    unary("log1p", [](double d) { return std::log1p(d); });
    unary("sin", [](double d) { return std::sin(d); });
    unary("cos", [](double d) { return std::cos(d); });
    unary("tan", [](double d) { return std::tan(d); });
    unary("asin", [](double d) { return std::asin(d); });
    unary("acos", [](double d) { return std::acos(d); });
    unary("atan", [](double d) { return std::atan(d); });
    unary("sinh", [](double d) { return std::sinh(d); });
    unary("cosh", [](double d) { return std::cosh(d); });
    unary("tanh", [](double d) { return std::tanh(d); });
    unary("sign", [](double d) {
        if (std::isnan(d) || d == 0)
            return d;
        return d < 0 ? -1.0 : 1.0;
    });
    unary("fround", [](double d) { return double(float(d)); });
    unary("round", [](double d) {
        // JavaScript rounds halves towards +Infinity, unlike std::round.
        if (std::isnan(d) || std::isinf(d))
            return d;
        return std::floor(d + 0.5);
    });

    rt.method(math, "pow", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        return Value(std::pow(toNumber(rt2, ci.arg(0)), toNumber(rt2, ci.arg(1))));
    }, 2);
    rt.method(math, "atan2", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        return Value(std::atan2(toNumber(rt2, ci.arg(0)), toNumber(rt2, ci.arg(1))));
    }, 2);
    rt.method(math, "hypot", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        double sum = 0;
        for (const Value& v : ci.args()) {
            const double d = toNumber(rt2, v);
            sum += d * d;
        }
        return Value(std::sqrt(sum));
    }, 2);
    rt.method(math, "max", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        double best = -std::numeric_limits<double>::infinity();
        for (const Value& v : ci.args()) {
            const double d = toNumber(rt2, v);
            if (std::isnan(d))
                return Value(d);
            best = std::max(best, d);
        }
        return Value(best);
    }, 2);
    rt.method(math, "min", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        double best = std::numeric_limits<double>::infinity();
        for (const Value& v : ci.args()) {
            const double d = toNumber(rt2, v);
            if (std::isnan(d))
                return Value(d);
            best = std::min(best, d);
        }
        return Value(best);
    }, 2);
    rt.method(math, "random", [](CallInfo&) {
        static thread_local std::mt19937_64 engine{std::random_device{}()};
        static thread_local std::uniform_real_distribution<double> dist(0.0, 1.0);
        return Value(dist(engine));
    }, 0);
}

void installDate(Runtime& rt)
{
    DynObject* proto = rt.newObject(rt.objectProto.get());
    rt.dateProto.reset(proto, true);

    auto nowMillis = []() {
        using namespace std::chrono;
        return double(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    };

    DynFunction* ctor = rt.makeConstructor("Date", [nowMillis](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        double millis = nowMillis();
        if (ci.size() == 1)
            millis = ci.arg(0).isString() ? std::nan("") : toNumber(rt2, ci.arg(0));
        if (!ci.isConstructorCall())
            return rt2.str(ascii("Date(") + numberToString(millis) + ascii(")"));
        auto* d = rt2.newCell<DynWrapper>();
        d->cls = ObjClass::Date;
        d->setProto(rt2.dateProto.get());
        d->primitive = Value(millis);
        return detail::Access::adopt(Type::Object, d);
    }, 7, proto);

    rt.method(ctor, "now", [nowMillis](CallInfo&) { return Value(nowMillis()); }, 0);

    rt.method(proto, "getTime", [](CallInfo& ci) { return Value(thisDate(ci)); }, 0);
    rt.method(proto, "valueOf", [](CallInfo& ci) { return Value(thisDate(ci)); }, 0);
    rt.method(proto, "getFullYear", [](CallInfo& ci) {
        return Value(double(utcOf(thisDate(ci)).tm_year + 1900));
    }, 0);
    rt.method(proto, "toISOString", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const double millis = thisDate(ci);
        const std::tm out = utcOf(millis);
        char buf[64];
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", out.tm_year + 1900,
                      out.tm_mon + 1, out.tm_mday, out.tm_hour, out.tm_min, out.tm_sec,
                      int(std::fmod(millis, 1000.0)));
        return rt2.str(buf);
    }, 0);
    rt.method(proto, "toString", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        return invokeMethod(rt2, ci.thisValue(), rt2.intern("toISOString"), {});
    }, 0);
    rt.method(proto, "toJSON", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        return invokeMethod(rt2, ci.thisValue(), rt2.intern("toISOString"), {});
    }, 0);
}

// ------------------------------------------------------------------ JSON ---

/// How deeply JSON text and values may nest. Both the parser and the writer recurse once per level.
constexpr std::size_t kMaxJsonDepth = 1000;

struct JsonParser
{
    Runtime& rt;
    StringView text;
    std::size_t at = 0;
    std::size_t depth = 0;

    [[noreturn]] void fail(const char* what)
    {
        throwError(rt, ErrorCode::SyntaxError,
                   std::string("JSON: ") + what + " at position " + std::to_string(at));
    }
    void skip()
    {
        while (at < text.size() && (text[at] == u' ' || text[at] == u'\t' || text[at] == u'\n' ||
                                    text[at] == u'\r'))
            ++at;
    }
    bool literal(const char* word)
    {
        const std::size_t n = std::char_traits<char>::length(word);
        if (at + n > text.size())
            return false;
        for (std::size_t i = 0; i < n; ++i) {
            if (text[at + i] != Char(word[i]))
                return false;
        }
        at += n;
        return true;
    }
    /// Keys interned so far, by a cheap hash of their text: the same few names repeat in every object of an array.
    StringCell* keys[64] = {};

    /// A string with no escape in it, as the text it is, after the closing quote; false, with nothing read, otherwise.
    bool plainString(StringView& out)
    {
        std::size_t end = at + 1;
        while (end < text.size() && text[end] != u'"' && text[end] != u'\\')
            ++end;
        if (end >= text.size() || text[end] != u'"')
            return false;
        out = text.substr(at + 1, end - at - 1);
        at = end + 1;
        return true;
    }
    StringCell* key()
    {
        StringView plain;
        if (!plainString(plain))
            return rt.intern(string());
        const std::size_t n = plain.size();
        const std::size_t slot = (n * 7 + (n ? std::size_t(plain[0]) * 31 + std::size_t(plain[n - 1]) : 0)) & 63;
        StringCell* known = keys[slot];
        if (known && known->text() == plain)
            return known;
        StringCell* cell = rt.intern(plain);
        keys[slot] = cell;
        return cell;
    }
    String string()
    {
        String out;
        ++at;  // opening quote
        while (at < text.size() && text[at] != u'"') {
            if (text[at] != u'\\') {
                out.push_back(text[at++]);
                continue;
            }
            ++at;
            if (at >= text.size())
                fail("unterminated escape");
            const Char e = text[at++];
            switch (e) {
                case u'n': out.push_back(u'\n'); break;
                case u't': out.push_back(u'\t'); break;
                case u'r': out.push_back(u'\r'); break;
                case u'b': out.push_back(u'\b'); break;
                case u'f': out.push_back(u'\f'); break;
                case u'u': {
                    if (at + 4 > text.size())
                        fail("truncated \\u escape");
                    Char v = 0;
                    for (int i = 0; i < 4; ++i) {
                        const Char c = text[at + std::size_t(i)];
                        int d = 0;
                        if (c >= u'0' && c <= u'9')
                            d = c - u'0';
                        else if (c >= u'a' && c <= u'f')
                            d = c - u'a' + 10;
                        else if (c >= u'A' && c <= u'F')
                            d = c - u'A' + 10;
                        else
                            fail("bad \\u escape");
                        v = Char(v * 16 + d);
                    }
                    at += 4;
                    out.push_back(v);
                    break;
                }
                default: out.push_back(e); break;
            }
        }
        if (at >= text.size())
            fail("unterminated string");
        ++at;
        return out;
    }
    Value value()
    {
        NestingScope nest(depth);
        if (nest.tooDeep(kMaxJsonDepth))
            throwError(rt, ErrorCode::RangeError, "JSON is nested too deeply");
        skip();
        if (at >= text.size())
            fail("unexpected end of input");
        const Char c = text[at];
        if (c == u'"') {
            StringView plain;
            if (plainString(plain))
                return rt.str(plain);
            return rt.str(string());
        }
        if (c == u'{') {
            ++at;
            DynObject* obj = rt.newObject(rt.objectProto.get());
            Value result = detail::Access::adopt(Type::Object, obj);
            skip();
            if (at < text.size() && text[at] == u'}') {
                ++at;
                return result;
            }
            for (;;) {
                skip();
                if (at >= text.size() || text[at] != u'"')
                    fail("expected a property name");
                StringCell* name = key();
                const Ref<StringCell> held(name);
                skip();
                if (at >= text.size() || text[at] != u':')
                    fail("expected ':'");
                ++at;
                obj->putOwn(name, value());
                skip();
                if (at < text.size() && text[at] == u',') {
                    ++at;
                    continue;
                }
                if (at < text.size() && text[at] == u'}') {
                    ++at;
                    return result;
                }
                fail("expected ',' or '}'");
            }
        }
        if (c == u'[') {
            ++at;
            DynArray* arr = rt.newArray();
            Value result = detail::Access::adopt(Type::Object, arr);
            skip();
            if (at < text.size() && text[at] == u']') {
                ++at;
                return result;
            }
            for (;;) {
                arr->append(value());
                skip();
                if (at < text.size() && text[at] == u',') {
                    ++at;
                    continue;
                }
                if (at < text.size() && text[at] == u']') {
                    ++at;
                    return result;
                }
                fail("expected ',' or ']'");
            }
        }
        if (literal("true"))
            return Value(true);
        if (literal("false"))
            return Value(false);
        if (literal("null"))
            return Value::null();

        const std::size_t start = at;
        {
            // A plain integer, the common case, without the general conversion.
            std::size_t i = at;
            const bool negative = i < text.size() && text[i] == u'-';
            if (negative)
                ++i;
            std::uint64_t v = 0;
            const std::size_t first = i;
            while (i < text.size() && text[i] >= u'0' && text[i] <= u'9' && i - first < 15)
                v = v * 10 + std::uint64_t(text[i++] - u'0');
            const bool ends = i == text.size() || (text[i] != u'.' && text[i] != u'e' && text[i] != u'E' &&
                                                   !(text[i] >= u'0' && text[i] <= u'9'));
            if (i > first && ends && !(text[first] == u'0' && i - first > 1) && !(negative && v == 0)) {
                at = i;
                return Value(negative ? -double(v) : double(v));
            }
        }
        // -? (0 | [1-9][0-9]*) (. [0-9]+)? ([eE] [+-]? [0-9]+)?
        const auto digit = [&](std::size_t i) { return i < text.size() && text[i] >= u'0' && text[i] <= u'9'; };
        if (at < text.size() && text[at] == u'-')
            ++at;
        if (!digit(at))
            fail("unexpected character");
        if (text[at] == u'0')
            ++at;
        else
            while (digit(at))
                ++at;
        if (at < text.size() && text[at] == u'.') {
            ++at;
            if (!digit(at))
                fail("invalid number");
            while (digit(at))
                ++at;
        }
        if (at < text.size() && (text[at] == u'e' || text[at] == u'E')) {
            ++at;
            if (at < text.size() && (text[at] == u'+' || text[at] == u'-'))
                ++at;
            if (!digit(at))
                fail("invalid number");
            while (digit(at))
                ++at;
        }
        const double n = stringToNumber(text.substr(start, at - start));
        if (std::isnan(n))
            fail("invalid number");
        return Value(n);
    }
};

/// The containers being written, outermost first. A container that is already on the path is a cycle.
using JsonPath = Vec<const void*>;  // pooled, so writing JSON still makes no call to the general allocator

bool stringifyValue(Runtime& rt, const Value& v, const Value& replacer, const String& indent,
                    const String& current, String& out, JsonPath& path);

bool isIndexKey(StringView text) noexcept
{
    return !text.empty() && text[0] >= u'0' && text[0] <= u'9';
}

/// `holder[key]`: an element or a data property read where it is, anything else the long way.
Value entryOf(Runtime& rt, const Value& holder, const Value& key)
{
    if (DynArray* arr = asArray(holder)) {
        if (key.isNumber()) {
            const double d = numberOf(key);
            if (d >= 0 && d < double(arr->elements.size()) && double(std::size_t(d)) == d)
                return arr->elements[std::size_t(d)];
        }
    } else if (key.isString() && detail::Access::ptr(holder)->kind == CellKind::Object) {
        if (const Prop* p = static_cast<DynObject*>(detail::Access::ptr(holder))->findOwn(detail::Access::ptr(key))) {
            if (!p->accessor())
                return p->value;
        }
    }
    return getProperty(rt, holder, toPropertyKey(rt, key).get());
}

bool stringifyItem(Runtime& rt, const Value& holder, const Value& key, Value item, const Value& replacer,
                   const String& indent, const String& current, String& out, JsonPath& path)
{
    // What a program sees of an element's index is a string, as with any other key.
    const auto named = [&]() { return key.isNumber() ? rt.str(numberToString(numberOf(key))) : key; };
    if (item.isObject()) {
        const Value toJson = getProperty(rt, item, rt.atom.toJSON.get());
        if (asFunction(toJson))
            item = callValue(rt, toJson, item, {named()});
    }
    if (asFunction(replacer))
        item = callValue(rt, replacer, holder, {named(), item});
    return stringifyValue(rt, item, replacer, indent, current, out, path);
}

bool stringifyEntry(Runtime& rt, const Value& holder, const Value& key, const Value& replacer,
                    const String& indent, const String& current, String& out, JsonPath& path)
{
    return stringifyItem(rt, holder, key, entryOf(rt, holder, key), replacer, indent, current, out, path);
}

/// A number as JSON writes it: an integer that fits is written straight, digit by digit.
void appendNumber(double d, String& out)
{
    if (!std::isfinite(d)) {
        out += ascii("null");
        return;
    }
    if (d == std::trunc(d) && std::fabs(d) < 1e15 && !(d == 0 && std::signbit(d))) {
        char buf[24];
        int n = 0;
        std::uint64_t v = std::uint64_t(std::fabs(d));
        do {
            buf[n++] = char('0' + v % 10);
            v /= 10;
        } while (v);
        if (d < 0)
            out.push_back(u'-');
        while (n)
            out.push_back(Char(buf[--n]));
        return;
    }
    out += numberToString(d);
}

bool stringifyValue(Runtime& rt, const Value& v, const Value& replacer, const String& indent,
                    const String& current, String& out, JsonPath& path)
{
    switch (v.type()) {
        case Type::Undefined:
        case Type::Symbol:
            return false;
        case Type::Null:
            out += ascii("null");
            return true;
        case Type::Bool:
            out += ascii(boolOf(v) ? "true" : "false");
            return true;
        case Type::Int:
        case Type::Long:
        case Type::Float:
        case Type::Char:
        case Type::Double:
            appendNumber(numberOf(v), out);
            return true;
        case Type::String:
            appendJsonQuoted(asString(v)->text(), out);
            return true;
        default:
            break;
    }
    if (asFunction(v))
        return false;

    // A container inside itself would recurse until the stack ran out; say so the way JavaScript does. The path is
    // also what bounds the depth of a legitimate, very deep value.
    const void* const self = asArray(v) ? static_cast<const void*>(asArray(v)) : static_cast<const void*>(asObject(v));
    if (std::find(path.begin(), path.end(), self) != path.end())
        throwError(rt, ErrorCode::TypeError, "Converting circular structure to JSON");
    if (path.size() >= kMaxJsonDepth || nativeStackLow())
        throwError(rt, ErrorCode::RangeError, "JSON value is nested too deeply");
    struct Open
    {
        JsonPath& path;
        Open(JsonPath& p, const void* item) : path(p) { path.push_back(item); }
        ~Open() { path.pop_back(); }
    } open_(path, self);

    const String inner = current + indent;
    const String open = indent.empty() ? String() : ascii("\n") + inner;
    const String close = indent.empty() ? String() : ascii("\n") + current;

    if (DynArray* arr = asArray(v)) {
        out.push_back(u'[');
        for (std::size_t i = 0; i < arr->elements.size(); ++i) {
            if (i)
                out.push_back(u',');
            out += open;
            const Value key{double(i)};
            const Value item = i < arr->elements.size() ? arr->elements[i] : entryOf(rt, v, key);
            if (!stringifyItem(rt, v, key, item, replacer, indent, inner, out, path))
                out += ascii("null");
        }
        if (!arr->elements.empty())
            out += close;
        out.push_back(u']');
        return true;
    }

    DynObject* obj = asObject(v);
    out.push_back(u'{');
    bool first = true;
    // A small plain object of plain properties: its keys as they are now, on the stack, which is all the order takes.
    constexpr std::size_t kFewKeys = 32;
    bool plain = obj->cls == ObjClass::Plain && obj->props.size() <= kFewKeys;
    if (plain) {
        for (const Prop& p : obj->props)
            plain = plain && !p.accessor() && p.key->kind == CellKind::String && (p.attrs & PropEnumerable) != 0 &&
                    !isIndexKey(static_cast<StringCell*>(p.key.get())->text());
    }
    if (plain) {
        Value keys[kFewKeys];
        const std::size_t count = obj->props.size();
        for (std::size_t i = 0; i < count; ++i)
            keys[i] = detail::Access::make(Type::String, obj->props[i].key.get());
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t mark = out.size();
            if (!first)
                out.push_back(u',');
            out += open;
            appendJsonQuoted(asString(keys[i])->text(), out);
            out.push_back(u':');
            if (!indent.empty())
                out.push_back(u' ');
            // Where it was, unless something run since has moved it.
            const Value item = i < obj->props.size() && obj->props[i].key.get() == detail::Access::ptr(keys[i]) && !obj->props[i].accessor()
                                   ? obj->props[i].value
                                   : entryOf(rt, v, keys[i]);
            if (!stringifyItem(rt, v, keys[i], item, replacer, indent, inner, out, path)) {
                out.resize(mark);
                continue;
            }
            first = false;
        }
        if (!first)
            out += close;
        out.push_back(u'}');
        return true;
    }
    for (const Value& key : ownKeys(rt, obj, true, false)) {
        // Written in place, and taken back when the value turns out to be one JSON leaves out.
        const std::size_t mark = out.size();
        if (!first)
            out.push_back(u',');
        out += open;
        appendJsonQuoted(asString(key)->text(), out);
        out.push_back(u':');
        if (!indent.empty())
            out.push_back(u' ');
        if (!stringifyEntry(rt, v, key, replacer, indent, inner, out, path)) {
            out.resize(mark);
            continue;
        }
        first = false;
    }
    if (!first)
        out += close;
    out.push_back(u'}');
    return true;
}

}  // namespace

Value jsonParse(Runtime& rt, StringView text)
{
    JsonParser parser{rt, text, 0};
    Value v = parser.value();
    parser.skip();
    if (parser.at != text.size())
        parser.fail("trailing characters");
    return v;
}

bool jsonStringify(Runtime& rt, const Value& value, const Value& replacer, StringView indent,
                   String& out)
{
    JsonPath path;
    // The next one is likely to be the size of the last.
    out.reserve(rt.jsonSizeSeen);
    struct Remember
    {
        Runtime& rt;
        String& out;
        ~Remember() { rt.jsonSizeSeen = std::min<std::size_t>(out.size(), std::size_t(1) << 24); }
    } remember{rt, out};
    // The value is the one property of an object that only a replacer gets to see.
    Value holder = Value::undefined();
    const Value key = rt.str(String());
    if (asFunction(replacer)) {
        DynObject* wrapper = rt.newObject(rt.objectProto.get());
        holder = detail::Access::adopt(Type::Object, wrapper);
        wrapper->putOwn(rt.intern(String()), value);
    }
    return stringifyItem(rt, holder, key, value, replacer, String(indent), String(), out, path);
}

void installMathJson(Runtime& rt)
{
    installNumber(rt);
    installBoolean(rt);
    installMath(rt);
    installDate(rt);

    DynObject* json = rt.newObject(rt.objectProto.get());
    rt.constant(rt.globalObject.get(), "JSON", detail::Access::adopt(Type::Object, json));
    json->putOwn(rt.sym.toStringTag.get(), rt.str("JSON"), PropConfigurable);

    rt.method(json, "parse", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        if (ci.arg(0).isString()) {
            // Held by the arguments for as long as this runs: nothing the parser does runs any of the program's code.
            const Value text = ci.arg(0);
            return jsonParse(rt2, asString(text)->text());
        }
        return jsonParse(rt2, toStringValue(rt2, ci.arg(0)));
    }, 2);

    rt.method(json, "stringify", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        String indent;
        const Value spacer = ci.arg(2);
        if (spacer.isNumber())
            indent = String(std::size_t(std::min(10.0, std::max(0.0, numberOf(spacer)))), u' ');
        else if (spacer.isString())
            indent = asString(spacer)->text().substr(0, 10);
        String out;
        if (!jsonStringify(rt2, ci.arg(0), ci.arg(1), indent, out))
            return Value::undefined();
        return rt2.str(out);
    }, 3);
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
