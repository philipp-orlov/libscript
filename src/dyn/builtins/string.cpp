// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// String and its prototype.

#include <algorithm>
#include <cmath>

#include "base/utf16.hpp"
#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/numconv.hpp"
#include "script/internal/dyn/unicode.hpp"
#include "script/internal/dyn/vm.hpp"

namespace script {
namespace internal {
namespace dyn {

namespace {

/// The receiver's text, without a copy when it is a string already.
struct ThisText
{
    Value keep;
    String owned;
    StringView view;
    explicit ThisText(CallInfo& ci);
    /// `count` characters from `start`: part of the receiver's own string when it is one.
    Value part(Runtime& rt, std::size_t start, std::size_t count) const
    {
        if (StringCell* cell = asString(keep))
            return rt.ctx.substring(cell, start, count);
        return rt.str(view.substr(start, count));
    }
};

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

ThisText::ThisText(CallInfo& ci)
{
    const Value self = ci.thisValue();
    if (StringCell* cell = asString(self)) {
        keep = self;
        view = cell->text();
        return;
    }
    owned = thisString(ci);
    view = owned;
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
/// The whole string in the other case, by code point, with the mappings that make several characters of one and a final
/// sigma that is one.
String caseMapped(const String& s, bool toUpper)
{
    String out;
    out.reserve(s.size());
    const auto put = [&](char32_t c) {
        if (c > 0xffff) {
            out.push_back(Char(0xd800 + ((c - 0x10000) >> 10)));
            out.push_back(Char(0xdc00 + ((c - 0x10000) & 0x3ff)));
        } else {
            out.push_back(Char(c));
        }
    };
    const auto codeAt = [&](std::size_t i, std::size_t& width) -> char32_t {
        width = 1;
        if (base::isHighSurrogate(s[i]) && i + 1 < s.size() && s[i + 1] >= 0xdc00 && s[i + 1] <= 0xdfff) {
            width = 2;
            return 0x10000 + ((char32_t(s[i]) - 0xd800) << 10) + (s[i + 1] - 0xdc00);
        }
        return s[i];
    };
    const auto isCased = [](char32_t c) { return unicode::simpleLower(c) != c || unicode::simpleUpper(c) != c; };
    for (std::size_t i = 0; i < s.size();) {
        std::size_t width;
        const char32_t c = codeAt(i, width);
        if (c < 128) {
            out.push_back(Char(toUpper ? (c >= 'a' && c <= 'z' ? c - 32 : c) : (c >= 'A' && c <= 'Z' ? c + 32 : c)));
        } else if (toUpper) {
            char32_t mapped[3];
            const int n = unicode::fullUpper(c, mapped);
            for (int k = 0; k < n; ++k)
                put(mapped[k]);
        } else if (c == 0x130) {
            put('i');
            put(0x307);
        } else if (c == 0x3a3) {
            // Final: a cased letter before it, and none after it, skipping what case ignores.
            bool before = false;
            for (std::size_t j = i; j-- > 0;) {
                if (isCased(s[j])) {
                    before = true;
                    break;
                }
                if (s[j] != 0x27 && s[j] != 0xad && s[j] != 0x2019 && !(s[j] >= 0x300 && s[j] <= 0x36f))
                    break;
            }
            bool after = false;
            for (std::size_t j = i + 1; j < s.size(); ++j) {
                if (isCased(s[j])) {
                    after = true;
                    break;
                }
                if (s[j] != 0x27 && s[j] != 0xad && s[j] != 0x2019 && !(s[j] >= 0x300 && s[j] <= 0x36f))
                    break;
            }
            put(before && !after ? 0x3c2 : 0x3c3);
        } else {
            put(unicode::simpleLower(c));
        }
        i += width;
    }
    return out;
}

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
        // Asked for a string, a symbol has one to give; only an implicit conversion is an error.
        if (!ci.isConstructorCall() && ci.arg(0).isSymbol()) {
            String shown = ascii("Symbol(");
            shown += static_cast<SymbolCell*>(detail::Access::ptr(ci.arg(0)))->description;
            shown.push_back(u')');
            return rt2.str(shown);
        }
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

    rt.method(ctor, "raw", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        const Value cooked = toObject(rt2, ci.arg(0));
        const Value raw = toObject(rt2, getProperty(rt2, cooked, rt2.intern("raw")));
        const double n = toNumber(rt2, getProperty(rt2, raw, rt2.atom.length.get()));
        String out;
        for (double i = 0; i < n; ++i) {
            out += toStringValue(rt2, getProperty(rt2, raw, toPropertyKey(rt2, Value(i)).get()));
            if (i + 1 < n && std::size_t(i) + 1 < ci.size())
                out += toStringValue(rt2, ci.arg(std::size_t(i) + 1));
        }
        return rt2.str(out);
    }, 1);
    rt.intrinsics[Runtime::StringFromCharCode].reset(detail::Access::ptr(ctor->findOwn(rt.intern("fromCharCode"))->value));

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
        const ThisText receiver(ci);
        const StringView s = receiver.view;
        const std::int64_t i = toIntegerOrInfinity(toNumber(rt2, ci.arg(0)));
        if (i < 0 || i >= std::int64_t(s.size()))
            return rt2.str("");
        return rt2.str(s.substr(std::size_t(i), 1));
    }, 1);

    rt.method(proto, "charCodeAt", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const ThisText receiver(ci);
        const StringView s = receiver.view;
        const std::int64_t i = toIntegerOrInfinity(toNumber(rt2, ci.arg(0)));
        if (i < 0 || i >= std::int64_t(s.size()))
            return Value(std::nan(""));
        return Value(double(s[std::size_t(i)]));
    }, 1);

    rt.intrinsics[Runtime::StringCharCodeAt].reset(detail::Access::ptr(proto->findOwn(rt.intern("charCodeAt"))->value));

    rt.method(proto, "codePointAt", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        const ThisText receiver(ci);
        const StringView s = receiver.view;
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
        const ThisText receiver(ci);
        const StringView s = receiver.view;
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
        const ThisText receiver(ci);
        const StringView s = receiver.view;
        const String needle = toStringValue(rt2, ci.arg(0));
        const std::size_t from = ci.size() > 1 ? clampTo(toNumber(rt2, ci.arg(1)), s.size()) : 0;
        const std::size_t at = s.find(needle, from);
        return Value(at == String::npos ? -1.0 : double(at));
    }, 1);

    rt.method(proto, "lastIndexOf", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const ThisText receiver(ci);
        const StringView s = receiver.view;
        const std::size_t at = s.rfind(toStringValue(rt2, ci.arg(0)));
        return Value(at == String::npos ? -1.0 : double(at));
    }, 1);

    rt.method(proto, "includes", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        return Value(thisString(ci).find(toStringValue(rt2, ci.arg(0))) != String::npos);
    }, 1);

    rt.method(proto, "startsWith", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const ThisText receiver(ci);
        const StringView s = receiver.view;
        const String needle = toStringValue(rt2, ci.arg(0));
        const std::size_t from = ci.size() > 1 ? clampTo(toNumber(rt2, ci.arg(1)), s.size()) : 0;
        return Value(s.compare(from, needle.size(), needle) == 0);
    }, 1);

    rt.method(proto, "endsWith", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const ThisText receiver(ci);
        const StringView s = receiver.view;
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
        const ThisText receiver(ci);
        const StringView s = receiver.view;
        const std::size_t from = ci.size() > 0 ? clampTo(toNumber(rt2, ci.arg(0)), s.size()) : 0;
        const std::size_t to = ci.size() > 1 && !ci.arg(1).isUndefined()
                                   ? clampTo(toNumber(rt2, ci.arg(1)), s.size())
                                   : s.size();
        if (from >= to)
            return rt2.str("");
        return receiver.part(rt2, from, to - from);
    }, 2);

    rt.method(proto, "substring", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const ThisText receiver(ci);
        const StringView s = receiver.view;
        std::size_t a = ci.size() > 0 ? clampTo(std::max(0.0, toNumber(rt2, ci.arg(0))), s.size())
                                      : 0;
        std::size_t b = ci.size() > 1 && !ci.arg(1).isUndefined()
                            ? clampTo(std::max(0.0, toNumber(rt2, ci.arg(1))), s.size())
                            : s.size();
        if (a > b)
            std::swap(a, b);
        return receiver.part(rt2, a, b - a);
    }, 2);

    rt.method(proto, "substr", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const ThisText receiver(ci);
        const StringView s = receiver.view;
        const std::size_t from = clampTo(toNumber(rt2, ci.arg(0)), s.size());
        const std::size_t count =
            ci.arg(1).isUndefined() ? s.size() - from
                                    : std::size_t(std::max<std::int64_t>(
                                          0, toIntegerOrInfinity(toNumber(rt2, ci.arg(1)))));
        return receiver.part(rt2, from, std::min(count, s.size() - from));
    }, 2);

    const auto toUpper = [](CallInfo& ci) { return rtOf(ci).str(caseMapped(thisString(ci), true)); };
    const auto toLower = [](CallInfo& ci) { return rtOf(ci).str(caseMapped(thisString(ci), false)); };
    rt.method(proto, "toUpperCase", toUpper, 0);
    rt.method(proto, "toLowerCase", toLower, 0);
    rt.method(proto, "toLocaleUpperCase", toUpper, 0);
    rt.method(proto, "toLocaleLowerCase", toLower, 0);

    rt.method(proto, "isWellFormed", [](CallInfo& ci) {
        const String s = thisString(ci);
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (base::isHighSurrogate(s[i]) && i + 1 < s.size() && s[i + 1] >= 0xdc00 && s[i + 1] <= 0xdfff)
                ++i;
            else if (s[i] >= 0xd800 && s[i] <= 0xdfff)
                return Value(false);
        }
        return Value(true);
    }, 0);
    rt.method(proto, "toWellFormed", [](CallInfo& ci) {
        String s = thisString(ci);
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (base::isHighSurrogate(s[i]) && i + 1 < s.size() && s[i + 1] >= 0xdc00 && s[i + 1] <= 0xdfff)
                ++i;
            else if (s[i] >= 0xd800 && s[i] <= 0xdfff)
                s[i] = 0xfffd;
        }
        return rtOf(ci).str(s);
    }, 0);

    // The old HTML wrappers of Annex B.
    static const struct { const char* name; const char* tag; const char* attribute; } kHtml[] = {
        {"anchor", "a", "name"}, {"big", "big", nullptr}, {"blink", "blink", nullptr}, {"bold", "b", nullptr}, {"fixed", "tt", nullptr},
        {"fontcolor", "font", "color"}, {"fontsize", "font", "size"}, {"italics", "i", nullptr}, {"link", "a", "href"}, {"small", "small", nullptr},
        {"strike", "strike", nullptr}, {"sub", "sub", nullptr}, {"sup", "sup", nullptr},
    };
    for (const auto& html : kHtml) {
        const char* tag = html.tag;
        const char* attribute = html.attribute;
        rt.method(proto, html.name, [tag, attribute](CallInfo& ci) {
            Runtime& rt2 = rtOf(ci);
            const String s = thisString(ci);
            String out = ascii("<") + ascii(tag);
            if (attribute) {
                String value = toStringValue(rt2, ci.arg(0));
                String escaped;
                for (Char c : value) {
                    if (c == u'"')
                        escaped += ascii("&quot;");
                    else
                        escaped.push_back(c);
                }
                out += ascii(" ") + ascii(attribute) + ascii("=\"") + escaped + ascii("\"");
            }
            out += ascii(">") + s + ascii("</") + ascii(tag) + ascii(">");
            return rt2.str(out);
        }, attribute ? 1 : 0);
    }

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
    proto->putOwn(rt.intern("trimLeft"), proto->findOwn(rt.intern("trimStart"))->value, PropWritable | PropConfigurable);
    proto->putOwn(rt.intern("trimRight"), proto->findOwn(rt.intern("trimEnd"))->value, PropWritable | PropConfigurable);

    rt.method(proto, "repeat", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const ThisText receiver(ci);
        const StringView s = receiver.view;
        const std::int64_t n = toIntegerOrInfinity(toNumber(rt2, ci.arg(0)));
        if (n < 0)
            ci.fail(ErrorCode::RangeError, "repeat count must not be negative");
        if (s.empty() || n == 0)
            return rt2.str(String());  // also keeps `''.repeat(2 ** 62)` from looping 2 ** 62 times doing nothing
        // Divide rather than multiply: s.size() * n wraps around for a large n and would pass the check.
        if (std::uint64_t(n) > (1u << 28) / s.size())
            ci.fail(ErrorCode::RangeError, "repeated string is too long");
        rt2.ctx.reserve(s.size() * std::size_t(n) * sizeof(Char));
        String out;
        out.reserve(s.size() * std::size_t(n));
        for (std::int64_t i = 0; i < n; ++i)
            out += s;
        return rt2.str(out);
    }, 1);

    auto pad = [](CallInfo& ci, bool atStart) {
        Runtime& rt2 = rtOf(ci);
        const ThisText receiver(ci);
        const StringView s = receiver.view;
        const std::size_t target =
            std::size_t(std::max<std::int64_t>(0, toIntegerOrInfinity(toNumber(rt2, ci.arg(0)))));
        if (target <= s.size())
            return rt2.str(s);
        const String filler = ci.arg(1).isUndefined() ? ascii(" ") : toStringValue(rt2, ci.arg(1));
        if (filler.empty())
            return rt2.str(s);
        // Handing back the unpadded string would let a program believe it got what it asked for.
        if (target > (1u << 26))
            ci.fail(ErrorCode::RangeError, "invalid string length");
        rt2.ctx.reserve(target * sizeof(Char) * 2);
        String padding;
        while (padding.size() < target - s.size())
            padding += filler;
        padding.resize(target - s.size());
        if (atStart)
            return rt2.str(padding.append(s));
        return rt2.str(String(s).append(padding));
    };
    rt.method(proto, "padStart", [pad](CallInfo& ci) { return pad(ci, true); }, 2);
    rt.method(proto, "padEnd", [pad](CallInfo& ci) { return pad(ci, false); }, 2);

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
                                      const ThisText receiver(ci);
        const StringView s = receiver.view;
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
