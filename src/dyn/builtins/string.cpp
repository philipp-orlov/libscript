// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// String and its prototype.

#include <algorithm>
#include <cmath>

#include "base/utf16.hpp"
#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/numconv.hpp"
#include "script/internal/dyn/vm.hpp"

namespace script {
namespace internal {
namespace dyn {

namespace {

String thisString(CallInfo& ci)
{
    Runtime& rt = rtOf(ci);
    const Value self = ci.thisValue();
    if (StringCell* s = asString(self))
        return String(s->text());
    if (DynObject* o = asObject(self); o && o->cls == ObjClass::StringObject) {
        if (StringCell* s = asString(static_cast<DynWrapper*>(o)->primitive))
            return String(s->text());
    }
    if (self.isNullish())
        ci.fail(ErrorCode::TypeError, "String.prototype method called on null or undefined");
    return toStringValue(rt, self);
}

std::size_t clampTo(double raw, std::size_t length)
{
    if (std::isnan(raw))
        return 0;
    std::int64_t i = toIntegerOrInfinity(raw);
    if (i < 0)
        i += std::int64_t(length);
    return std::size_t(std::max<std::int64_t>(0, std::min<std::int64_t>(i, std::int64_t(length))));
}

bool isTrimmable(Char c) noexcept
{
    return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r' || c == u'\f' || c == u'\v' ||
           c == 0x00a0 || c == 0xfeff;
}

/// Case mapping over ASCII and Latin-1, which is as far as the engine goes without
/// a Unicode table.
Char upper(Char c) noexcept
{
    if (c >= u'a' && c <= u'z')
        return Char(c - 32);
    if (c >= 0xe0 && c <= 0xfe && c != 0xf7)
        return Char(c - 32);
    return c;
}
Char lower(Char c) noexcept
{
    if (c >= u'A' && c <= u'Z')
        return Char(c + 32);
    if (c >= 0xc0 && c <= 0xde && c != 0xd7)
        return Char(c + 32);
    return c;
}

}  // namespace

void installString(Runtime& rt)
{
    DynObject* proto = rt.newObject(rt.objectProto.get());
    rt.stringProto.reset(proto, true);

    DynFunction* ctor = rt.makeConstructor("String", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        const String text = ci.size() ? toStringValue(rt2, ci.arg(0)) : String();
        if (!ci.isConstructorCall())
            return rt2.str(text);
        return toObject(rt2, rt2.str(text));
    }, 1, proto);

    rt.method(ctor, "fromCharCode", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        String out;
        for (const Value& v : ci.args())
            out.push_back(Char(toUint32(toNumber(rt2, v)) & 0xffff));
        return rt2.str(out);
    }, 1);

    rt.method(ctor, "fromCodePoint", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        String out;
        for (const Value& v : ci.args()) {
            base::appendCodePointUtf16(out, toUint32(toNumber(rt2, v)));
        }
        return rt2.str(out);
    }, 1);

    rt.method(proto, "toString", [](CallInfo& ci) { return rtOf(ci).str(thisString(ci)); }, 0);
    rt.method(proto, "valueOf", [](CallInfo& ci) { return rtOf(ci).str(thisString(ci)); }, 0);

    rt.method(proto, "charAt", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const String s = thisString(ci);
        const std::int64_t i = toIntegerOrInfinity(toNumber(rt2, ci.arg(0)));
        if (i < 0 || i >= std::int64_t(s.size()))
            return rt2.str("");
        return rt2.str(s.substr(std::size_t(i), 1));
    }, 1);

    rt.method(proto, "charCodeAt", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const String s = thisString(ci);
        const std::int64_t i = toIntegerOrInfinity(toNumber(rt2, ci.arg(0)));
        if (i < 0 || i >= std::int64_t(s.size()))
            return Value(std::nan(""));
        return Value(double(s[std::size_t(i)]));
    }, 1);

    rt.method(proto, "codePointAt", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        const String s = thisString(ci);
        const std::int64_t i = toIntegerOrInfinity(toNumber(rt2, ci.arg(0)));
        if (i < 0 || i >= std::int64_t(s.size()))
            return Value::undefined();
        char32_t cp = s[std::size_t(i)];
        if (base::isHighSurrogate(cp) && std::size_t(i) + 1 < s.size())
            cp = base::joinSurrogates(cp, s[std::size_t(i) + 1]);
        return Value(double(cp));
    }, 1);

    rt.method(proto, "at", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        const String s = thisString(ci);
        std::int64_t i = toIntegerOrInfinity(toNumber(rt2, ci.arg(0)));
        if (i < 0)
            i += std::int64_t(s.size());
        if (i < 0 || i >= std::int64_t(s.size()))
            return Value::undefined();
        return rt2.str(s.substr(std::size_t(i), 1));
    }, 1);

    rt.method(proto, "concat", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        String s = thisString(ci);
        for (const Value& v : ci.args())
            s += toStringValue(rt2, v);
        return rt2.str(s);
    }, 1);

    rt.method(proto, "indexOf", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const String s = thisString(ci);
        const String needle = toStringValue(rt2, ci.arg(0));
        const std::size_t from = ci.size() > 1 ? clampTo(toNumber(rt2, ci.arg(1)), s.size()) : 0;
        const std::size_t at = s.find(needle, from);
        return Value(at == String::npos ? -1.0 : double(at));
    }, 1);

    rt.method(proto, "lastIndexOf", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const String s = thisString(ci);
        const std::size_t at = s.rfind(toStringValue(rt2, ci.arg(0)));
        return Value(at == String::npos ? -1.0 : double(at));
    }, 1);

    rt.method(proto, "includes", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        return Value(thisString(ci).find(toStringValue(rt2, ci.arg(0))) != String::npos);
    }, 1);

    rt.method(proto, "startsWith", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const String s = thisString(ci);
        const String needle = toStringValue(rt2, ci.arg(0));
        const std::size_t from = ci.size() > 1 ? clampTo(toNumber(rt2, ci.arg(1)), s.size()) : 0;
        return Value(s.compare(from, needle.size(), needle) == 0);
    }, 1);

    rt.method(proto, "endsWith", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const String s = thisString(ci);
        const String needle = toStringValue(rt2, ci.arg(0));
        const std::size_t end = ci.size() > 1 && !ci.arg(1).isUndefined()
                                    ? clampTo(toNumber(rt2, ci.arg(1)), s.size())
                                    : s.size();
        if (needle.size() > end)
            return Value(false);
        return Value(s.compare(end - needle.size(), needle.size(), needle) == 0);
    }, 1);

    rt.method(proto, "slice", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const String s = thisString(ci);
        const std::size_t from = ci.size() > 0 ? clampTo(toNumber(rt2, ci.arg(0)), s.size()) : 0;
        const std::size_t to = ci.size() > 1 && !ci.arg(1).isUndefined()
                                   ? clampTo(toNumber(rt2, ci.arg(1)), s.size())
                                   : s.size();
        if (from >= to)
            return rt2.str("");
        return rt2.str(s.substr(from, to - from));
    }, 2);

    rt.method(proto, "substring", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const String s = thisString(ci);
        std::size_t a = ci.size() > 0 ? clampTo(std::max(0.0, toNumber(rt2, ci.arg(0))), s.size())
                                      : 0;
        std::size_t b = ci.size() > 1 && !ci.arg(1).isUndefined()
                            ? clampTo(std::max(0.0, toNumber(rt2, ci.arg(1))), s.size())
                            : s.size();
        if (a > b)
            std::swap(a, b);
        return rt2.str(s.substr(a, b - a));
    }, 2);

    rt.method(proto, "substr", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const String s = thisString(ci);
        const std::size_t from = clampTo(toNumber(rt2, ci.arg(0)), s.size());
        const std::size_t count =
            ci.arg(1).isUndefined() ? s.size() - from
                                    : std::size_t(std::max<std::int64_t>(
                                          0, toIntegerOrInfinity(toNumber(rt2, ci.arg(1)))));
        return rt2.str(s.substr(from, count));
    }, 2);

    rt.method(proto, "toUpperCase", [](CallInfo& ci) {
        String s = thisString(ci);
        for (Char& c : s)
            c = upper(c);
        return rtOf(ci).str(s);
    }, 0);

    rt.method(proto, "toLowerCase", [](CallInfo& ci) {
        String s = thisString(ci);
        for (Char& c : s)
            c = lower(c);
        return rtOf(ci).str(s);
    }, 0);

    auto trim = [](CallInfo& ci, bool start, bool end) {
        String s = thisString(ci);
        std::size_t a = 0;
        std::size_t b = s.size();
        if (start) {
            while (a < b && isTrimmable(s[a]))
                ++a;
        }
        if (end) {
            while (b > a && isTrimmable(s[b - 1]))
                --b;
        }
        return rtOf(ci).str(s.substr(a, b - a));
    };
    rt.method(proto, "trim", [trim](CallInfo& ci) { return trim(ci, true, true); }, 0);
    rt.method(proto, "trimStart", [trim](CallInfo& ci) { return trim(ci, true, false); }, 0);
    rt.method(proto, "trimEnd", [trim](CallInfo& ci) { return trim(ci, false, true); }, 0);

    rt.method(proto, "repeat", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const String s = thisString(ci);
        const std::int64_t n = toIntegerOrInfinity(toNumber(rt2, ci.arg(0)));
        if (n < 0)
            ci.fail(ErrorCode::RangeError, "repeat count must not be negative");
        if (s.size() * std::size_t(n) > (1u << 28))
            ci.fail(ErrorCode::RangeError, "repeated string is too long");
        String out;
        out.reserve(s.size() * std::size_t(n));
        for (std::int64_t i = 0; i < n; ++i)
            out += s;
        return rt2.str(out);
    }, 1);

    auto pad = [](CallInfo& ci, bool atStart) {
        Runtime& rt2 = rtOf(ci);
        const String s = thisString(ci);
        const std::size_t target =
            std::size_t(std::max<std::int64_t>(0, toIntegerOrInfinity(toNumber(rt2, ci.arg(0)))));
        if (target <= s.size() || target > (1u << 26))
            return rt2.str(s);
        const String filler = ci.arg(1).isUndefined() ? ascii(" ") : toStringValue(rt2, ci.arg(1));
        if (filler.empty())
            return rt2.str(s);
        String padding;
        while (padding.size() < target - s.size())
            padding += filler;
        padding.resize(target - s.size());
        return rt2.str(atStart ? padding + s : s + padding);
    };
    rt.method(proto, "padStart", [pad](CallInfo& ci) { return pad(ci, true); }, 2);
    rt.method(proto, "padEnd", [pad](CallInfo& ci) { return pad(ci, false); }, 2);

    rt.method(proto, "split", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const String s = thisString(ci);
        Vec<Value> parts;
        if (ci.arg(0).isUndefined()) {
            parts.push_back(rt2.str(s));
            return rt2.makeArray(std::move(parts));
        }
        const String sep = toStringValue(rt2, ci.arg(0));
        if (sep.empty()) {
            for (Char c : s)
                parts.push_back(rt2.str(String(1, c)));
            return rt2.makeArray(std::move(parts));
        }
        std::size_t at = 0;
        for (;;) {
            const std::size_t hit = s.find(sep, at);
            if (hit == String::npos) {
                parts.push_back(rt2.str(s.substr(at)));
                break;
            }
            parts.push_back(rt2.str(s.substr(at, hit - at)));
            at = hit + sep.size();
        }
        return rt2.makeArray(std::move(parts));
    }, 2);

    auto replace = [](CallInfo& ci, bool all) {
        Runtime& rt2 = rtOf(ci);
        const String s = thisString(ci);
        const String needle = toStringValue(rt2, ci.arg(0));
        const Value replacement = ci.arg(1);
        String out;
        std::size_t at = 0;
        for (;;) {
            const std::size_t hit = needle.empty() && at > 0 ? String::npos : s.find(needle, at);
            if (hit == String::npos) {
                out += s.substr(at);
                break;
            }
            out += s.substr(at, hit - at);
            if (asFunction(replacement)) {
                out += toStringValue(
                    rt2, callValue(rt2, replacement, Value::undefined(),
                                   {rt2.str(needle), Value(double(hit)), rt2.str(s)}));
            } else {
                out += toStringValue(rt2, replacement);
            }
            at = hit + std::max<std::size_t>(needle.size(), 1);
            if (at > s.size()) {
                break;
            }
            if (!all) {
                out += s.substr(at);
                break;
            }
        }
        return rt2.str(out);
    };
    rt.method(proto, "replace", [replace](CallInfo& ci) { return replace(ci, false); }, 2);
    rt.method(proto, "replaceAll", [replace](CallInfo& ci) { return replace(ci, true); }, 2);

    rt.method(proto, "localeCompare", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const String a = thisString(ci);
        const String b = toStringValue(rt2, ci.arg(0));
        return Value(a < b ? -1.0 : (a == b ? 0.0 : 1.0));
    }, 1);

    rt.method(proto, "normalize", [](CallInfo& ci) { return rtOf(ci).str(thisString(ci)); }, 0);

    proto->putOwn(rt.sym.iterator.get(),
                  rt.makeFunction(ascii("[Symbol.iterator]"),
                                  [](CallInfo& ci) {
                                      Runtime& rt2 = rtOf(ci);
                                      const String s = thisString(ci);
                                      Vec<Value> chars;
                                      for (std::size_t i = 0; i < s.size(); ++i) {
                                          std::size_t width = 1;
                                          if (base::isHighSurrogate(s[i]) && i + 1 < s.size())
                                              width = 2;
                                          chars.push_back(rt2.str(s.substr(i, width)));
                                          i += width - 1;
                                      }
                                      return makeIterator(rt2, std::move(chars));
                                  },
                                  0),
                  PropWritable | PropConfigurable);
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
