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
        d->proto.reset(rt2.dateProto.get());
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

struct JsonParser
{
    Runtime& rt;
    StringView text;
    std::size_t at = 0;

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
        skip();
        if (at >= text.size())
            fail("unexpected end of input");
        const Char c = text[at];
        if (c == u'"')
            return rt.str(string());
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
                const String key = string();
                skip();
                if (at >= text.size() || text[at] != u':')
                    fail("expected ':'");
                ++at;
                obj->putOwn(rt.intern(key), value());
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
        if (at < text.size() && text[at] == u'-')
            ++at;
        while (at < text.size() && ((text[at] >= u'0' && text[at] <= u'9') || text[at] == u'.' ||
                                    text[at] == u'e' || text[at] == u'E' || text[at] == u'+' ||
                                    text[at] == u'-'))
            ++at;
        if (at == start)
            fail("unexpected character");
        const double n = stringToNumber(text.substr(start, at - start));
        if (std::isnan(n))
            fail("invalid number");
        return Value(n);
    }
};

bool stringifyValue(Runtime& rt, const Value& v, const Value& replacer, const String& indent,
                    const String& current, String& out);

bool stringifyEntry(Runtime& rt, const Value& holder, const Value& key, const Value& replacer,
                    const String& indent, const String& current, String& out)
{
    Value item = holder.isUndefined() ? key : getProperty(rt, holder, toPropertyKey(rt, key).get());
    if (item.isObject()) {
        const Value toJson = getProperty(rt, item, rt.intern("toJSON"));
        if (asFunction(toJson))
            item = callValue(rt, toJson, item, {key});
    }
    if (asFunction(replacer))
        item = callValue(rt, replacer, holder, {key, item});
    return stringifyValue(rt, item, replacer, indent, current, out);
}

bool stringifyValue(Runtime& rt, const Value& v, const Value& replacer, const String& indent,
                    const String& current, String& out)
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
        case Type::Double: {
            const double d = numberOf(v);
            out += std::isfinite(d) ? numberToString(d) : ascii("null");
            return true;
        }
        case Type::String:
            appendJsonQuoted(asString(v)->text(), out);
            return true;
        default:
            break;
    }
    if (asFunction(v))
        return false;

    const String inner = current + indent;
    const String open = indent.empty() ? String() : ascii("\n") + inner;
    const String close = indent.empty() ? String() : ascii("\n") + current;

    if (DynArray* arr = asArray(v)) {
        out.push_back(u'[');
        for (std::size_t i = 0; i < arr->elements.size(); ++i) {
            if (i)
                out.push_back(u',');
            out += open;
            if (!stringifyEntry(rt, v, Value(double(i)), replacer, indent, inner, out))
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
    for (const Value& key : ownKeys(rt, obj, true, false)) {
        String piece;
        if (!stringifyEntry(rt, v, key, replacer, indent, inner, piece))
            continue;
        if (!first)
            out.push_back(u',');
        first = false;
        out += open;
        appendJsonQuoted(asString(key)->text(), out);
        out.push_back(u':');
        if (!indent.empty())
            out.push_back(u' ');
        out += piece;
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
    return stringifyEntry(rt, Value::undefined(), value, replacer, String(indent), String(), out);
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
