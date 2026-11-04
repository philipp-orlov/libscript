// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The C library: a static class of native methods the C and C++ front ends
// call as `libc.<name>`. Output goes through the context's print handler like
// `Console.Write` does; there is no file system, no `exit` and no mutable
// string, which is what the signatures below say. The `__` names are the
// front ends' own: `__at` is `s[i]`, `__cout` is a `std::cout` chain.
#include "base/classic_locale.hpp"
#include "script/internal/c/libc.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include "script/internal/typed/builtins.hpp"

namespace script {
namespace internal {
namespace c {

using typed::CSArray;
using typed::Runtime;

namespace {

const LibcFunction kFunctions[] = {
    // stdio
    {"printf", 'i', "sV"},
    {"fprintf", 'i', "isV"},
    {"puts", 'i', "s"},
    {"puts", 'i', "A"},
    {"putchar", 'i', "i"},
    {"__assert", 'v', "bi"},
    {"__at", 'c', "si"},
    {"__cout", 'v', "iV"},
    {"__substr", 's', "si"},
    {"__substr", 's', "sii"},
    {"__repeat", 's', "ic"},
    {"__chr", 's', "i"},
    // string
    {"strlen", 'i', "s"},
    {"strlen", 'i', "A"},
    {"strcmp", 'i', "ss"},
    {"strncmp", 'i', "ssi"},
    {"strstr", 'i', "ss"},
    {"strchr", 'i', "si"},
    {"atoi", 'i', "s"},
    {"atol", 'l', "s"},
    {"atof", 'd', "s"},
    // ctype
    {"isdigit", 'i', "i"},
    {"isalpha", 'i', "i"},
    {"isalnum", 'i', "i"},
    {"isspace", 'i', "i"},
    {"isupper", 'i', "i"},
    {"islower", 'i', "i"},
    {"ispunct", 'i', "i"},
    {"toupper", 'i', "i"},
    {"tolower", 'i', "i"},
    // stdlib
    {"abs", 'i', "i"},
    {"labs", 'l', "l"},
    {"llabs", 'l', "l"},
    {"rand", 'i', ""},
    {"srand", 'v', "u"},
    // math
    {"fabs", 'd', "d"},
    {"sqrt", 'd', "d"},
    {"cbrt", 'd', "d"},
    {"pow", 'd', "dd"},
    {"exp", 'd', "d"},
    {"log", 'd', "d"},
    {"log2", 'd', "d"},
    {"log10", 'd', "d"},
    {"sin", 'd', "d"},
    {"cos", 'd', "d"},
    {"tan", 'd', "d"},
    {"asin", 'd', "d"},
    {"acos", 'd', "d"},
    {"atan", 'd', "d"},
    {"atan2", 'd', "dd"},
    {"sinh", 'd', "d"},
    {"cosh", 'd', "d"},
    {"tanh", 'd', "d"},
    {"floor", 'd', "d"},
    {"ceil", 'd', "d"},
    {"round", 'd', "d"},
    {"trunc", 'd', "d"},
    {"fmod", 'd', "dd"},
    {"fmin", 'd', "dd"},
    {"fmax", 'd', "dd"},
    {"hypot", 'd', "dd"},
    {"isnan", 'i', "d"},
    {"isinf", 'i', "d"},
    {nullptr, 'v', ""},
};

const LibcConstant kConstants[] = {
    {"INT_MAX", 'i', 2147483647LL, 0},
    {"INT_MIN", 'i', -2147483647LL - 1, 0},
    {"UINT_MAX", 'l', 4294967295LL, 0},
    {"LONG_MAX", 'l', std::numeric_limits<std::int64_t>::max(), 0},
    {"LONG_MIN", 'l', std::numeric_limits<std::int64_t>::min(), 0},
    {"LLONG_MAX", 'l', std::numeric_limits<std::int64_t>::max(), 0},
    {"LLONG_MIN", 'l', std::numeric_limits<std::int64_t>::min(), 0},
    {"CHAR_MAX", 'i', 127, 0},
    {"CHAR_MIN", 'i', -128, 0},
    {"SHRT_MAX", 'i', 32767, 0},
    {"SHRT_MIN", 'i', -32768, 0},
    {"UCHAR_MAX", 'i', 255, 0},
    {"RAND_MAX", 'i', 2147483647LL, 0},
    {"EOF", 'i', -1, 0},
    {"EXIT_SUCCESS", 'i', 0, 0},
    {"EXIT_FAILURE", 'i', 1, 0},
    {"stdout", 'i', 1, 0},
    {"stderr", 'i', 2, 0},
    {"stdin", 'i', 0, 0},
    {"DBL_MAX", 'd', 0, std::numeric_limits<double>::max()},
    {"DBL_MIN", 'd', 0, std::numeric_limits<double>::min()},
    {"DBL_EPSILON", 'd', 0, std::numeric_limits<double>::epsilon()},
    {"FLT_MAX", 'd', 0, std::numeric_limits<float>::max()},
    {"FLT_EPSILON", 'd', 0, std::numeric_limits<float>::epsilon()},
    {"M_PI", 'd', 0, 3.14159265358979323846},
    {"M_E", 'd', 0, 2.71828182845904523536},
    {"M_SQRT2", 'd', 0, 1.41421356237309504880},
    {"INFINITY", 'd', 0, std::numeric_limits<double>::infinity()},
    {"NAN", 'd', 0, std::numeric_limits<double>::quiet_NaN()},
    {nullptr, 'i', 0, 0},
};

// ------------------------------------------------------------- helpers ---

/// The characters of a `char*` (a string) or a `char[]` up to its NUL.
String charsOf(const Value& v)
{
    if (v.isString())
        return String(typed::textOf(v));
    if (const CSArray* a = typed::asArray(v)) {
        String out;
        for (std::uint32_t i = 0; i < a->length; ++i) {
            const Value e = a->get(i);
            const char16_t c = char16_t(detail::Access::i4(e));
            if (c == 0)
                break;
            out.push_back(c);
        }
        return out;
    }
    return String();
}

/// One `std::cout <<` operand, spelled the way the default stream does it:
/// integers in decimal, doubles with six significant digits, bools as 1/0.
String coutText(typed::Runtime& rt, const Value& v)
{
    switch (v.type()) {
        case Type::Bool: return ascii(detail::Access::boolean(v) ? "1" : "0");
        case Type::Char: return String(1, char16_t(detail::Access::i4(v)));
        case Type::Double:
        case Type::Float: {
            char buffer[64];
            std::snprintf(buffer, sizeof buffer, "%g",
                          v.type() == Type::Double ? detail::Access::r8(v) : double(detail::Access::r4(v)));
            return ascii(buffer);
        }
        default: return typed::toStringValue(rt, v);
    }
}

std::int64_t integerOf(const Value& v) noexcept
{
    switch (v.type()) {
        case Type::Int:
        case Type::Char: return detail::Access::i4(v);
        case Type::Long: return detail::Access::i8(v);
        case Type::Bool: return detail::Access::boolean(v) ? 1 : 0;
        case Type::Double: return std::int64_t(detail::Access::r8(v));
        case Type::Float: return std::int64_t(detail::Access::r4(v));
        default: return 0;
    }
}

/// printf's engine, over already-evaluated arguments.
String formatC(const String& fmt, const CSArray* args, Runtime& rt)
{
    String out;
    std::uint32_t next = 0;
    auto nextArg = [&]() -> Value {
        if (!args || next >= args->length)
            typed::throwError(rt, ErrorCode::ArgumentException, "printf: not enough arguments for the format");
        return args->get(next++);
    };
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        const char16_t c = fmt[i];
        if (c != u'%') {
            out.push_back(c);
            continue;
        }
        ++i;
        if (i < fmt.size() && fmt[i] == u'%') {
            out.push_back(u'%');
            continue;
        }
        std::string spec = "%";
        while (i < fmt.size() && (fmt[i] == u'-' || fmt[i] == u'+' || fmt[i] == u' ' ||
                                  fmt[i] == u'0' || fmt[i] == u'#'))
            spec.push_back(char(fmt[i++]));
        if (i < fmt.size() && fmt[i] == u'*') {
            spec += std::to_string(integerOf(nextArg()));
            ++i;
        } else {
            while (i < fmt.size() && fmt[i] >= u'0' && fmt[i] <= u'9')
                spec.push_back(char(fmt[i++]));
        }
        int precision = -1;
        if (i < fmt.size() && fmt[i] == u'.') {
            ++i;
            spec.push_back('.');
            if (i < fmt.size() && fmt[i] == u'*') {
                precision = int(integerOf(nextArg()));
                spec += std::to_string(precision);
                ++i;
            } else {
                std::string digits;
                while (i < fmt.size() && fmt[i] >= u'0' && fmt[i] <= u'9')
                    digits.push_back(char(fmt[i++]));
                precision = digits.empty() ? 0 : std::atoi(digits.c_str());
                spec += digits.empty() ? "0" : digits;
            }
        }
        // Length modifiers say nothing here: every integer is 64 bits wide.
        while (i < fmt.size() && (fmt[i] == u'l' || fmt[i] == u'h' || fmt[i] == u'z' ||
                                  fmt[i] == u'j' || fmt[i] == u't' || fmt[i] == u'L'))
            ++i;
        if (i >= fmt.size())
            typed::throwError(rt, ErrorCode::ArgumentException, "printf: incomplete format specifier");
        const char conv = char(fmt[i]);
        char buffer[512];
        switch (conv) {
            case 'd':
            case 'i': {
                spec += "lld";
                std::snprintf(buffer, sizeof buffer, spec.c_str(), (long long)integerOf(nextArg()));
                out += ascii(buffer);
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            case 'o': {
                spec += "ll";
                spec.push_back(conv);
                const Value v = nextArg();
                unsigned long long u = (unsigned long long)integerOf(v);
                if (v.type() == Type::Int)
                    u = (unsigned long long)detail::Access::u4(v);
                std::snprintf(buffer, sizeof buffer, spec.c_str(), u);
                out += ascii(buffer);
                break;
            }
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G': {
                spec.push_back(conv);
                std::snprintf(buffer, sizeof buffer, spec.c_str(), numberOf(nextArg()));
                out += ascii(buffer);
                break;
            }
            case 'c': {
                spec.push_back('s');
                const String one(1, char16_t(integerOf(nextArg())));
                const std::string utf8 = toUtf8(one);
                std::snprintf(buffer, sizeof buffer, spec.c_str(), utf8.c_str());
                out += fromUtf8(buffer);
                break;
            }
            case 's': {
                const Value v = nextArg();
                String text = v.isNullish() ? ascii("(null)") : charsOf(v);
                if (precision >= 0 && std::size_t(precision) < text.size())
                    text.resize(std::size_t(precision));
                // Width applies to characters, so pad here rather than in snprintf.
                std::string widthDigits;
                bool leftAlign = false;
                for (std::size_t k = 1; k < spec.size(); ++k) {
                    if (spec[k] == '-')
                        leftAlign = true;
                    else if (spec[k] >= '1' && spec[k] <= '9')
                        widthDigits = spec.substr(k, spec.find('.', k) == std::string::npos
                                                         ? std::string::npos
                                                         : spec.find('.', k) - k);
                    if (spec[k] == '.')
                        break;
                }
                const std::size_t width = widthDigits.empty() ? 0 : std::size_t(std::atoi(widthDigits.c_str()));
                if (text.size() < width) {
                    const String pad(width - text.size(), u' ');
                    text = leftAlign ? text + pad : pad + text;
                }
                out += text;
                break;
            }
            case 'p': {
                const Value v = nextArg();
                out += ascii(v.isNullish() ? "(nil)" : "0x1");
                break;
            }
            default:
                typed::throwError(rt, ErrorCode::ArgumentException,
                                  std::string("printf: unsupported conversion '%") + conv + "'");
        }
    }
    return out;
}

int classify(std::int64_t ch, int (*test)(int)) noexcept
{
    return ch >= 0 && ch <= 127 && test(int(ch)) ? 1 : 0;
}

}  // namespace

const LibcFunction* libcFunctions() noexcept
{
    return kFunctions;
}

const LibcConstant* libcConstants() noexcept
{
    return kConstants;
}

void installLibc(Runtime& rt)
{
    typed::TypeTable& table = *rt.types;
    if (table.find("libc"))
        return;
    const typed::CoreTypes& core = table.core();
    typed::TypeInfo* libc = table.create(typed::TypeKind::Class, "libc");
    libc->base = core.objectType;
    libc->isStaticClass = true;
    libc->isSealed = true;

    auto typeOf = [&](char letter) -> const typed::TypeInfo* {
        switch (letter) {
            case 'i': return core.intType;
            case 'u': return core.uintType;
            case 'l': return core.longType;
            case 'd': return core.doubleType;
            case 'c': return core.charType;
            case 's': return core.stringType;
            case 'A': return table.arrayOf(core.charType);
            case 'b': return core.boolType;
            case 'V': return table.arrayOf(core.objectType);
            default: return core.voidType;
        }
    };

    auto native = [](const std::string& name) -> NativeFunction {
        using typed::runtimeOf;
        if (name == "printf")
            return [](CallInfo& ci) {
                Runtime& rt2 = runtimeOf(ci);
                const String text = formatC(String(typed::textOf(ci.arg(0))), typed::asArray(ci.arg(1)), rt2);
                rt2.write(text, false);
                return Value(std::int32_t(text.size()));
            };
        if (name == "fprintf")
            return [](CallInfo& ci) {
                Runtime& rt2 = runtimeOf(ci);
                const String text = formatC(String(typed::textOf(ci.arg(1))), typed::asArray(ci.arg(2)), rt2);
                rt2.write(text, ci.arg(0).toInt() == 2);
                return Value(std::int32_t(text.size()));
            };
        if (name == "puts")
            return [](CallInfo& ci) {
                Runtime& rt2 = runtimeOf(ci);
                rt2.write(charsOf(ci.arg(0)) + u"\n", false);
                return Value(std::int32_t(1));
            };
        if (name == "putchar")
            return [](CallInfo& ci) {
                Runtime& rt2 = runtimeOf(ci);
                const std::int64_t c = integerOf(ci.arg(0));
                rt2.write(String(1, char16_t(c)), false);
                return Value(std::int32_t(c));
            };
        if (name == "__assert")
            return [](CallInfo& ci) {
                if (!ci.arg(0).toBool())
                    typed::fail(ci, ErrorCode::InvalidOperation,
                                "assertion failed at line " + std::to_string(ci.arg(1).toInt()));
                return Value();
            };
        if (name == "__cout")
            return [](CallInfo& ci) {
                Runtime& rt2 = runtimeOf(ci);
                String text;
                if (const CSArray* items = typed::asArray(ci.arg(1))) {
                    for (std::uint32_t i = 0; i < items->length; ++i)
                        text += coutText(rt2, items->get(i));
                }
                rt2.write(text, ci.arg(0).toInt() == 2);
                return Value();
            };
        if (name == "__substr")
            return [](CallInfo& ci) {
                // std::string::substr: `pos` past the end throws, `count` is clamped.
                const StringView s = typed::textOf(ci.arg(0));
                const std::int64_t pos = integerOf(ci.arg(1));
                if (pos < 0 || pos > std::int64_t(s.size()))
                    typed::fail(ci, ErrorCode::ArgumentOutOfRange, "substr: position out of range");
                std::int64_t count = ci.size() > 2 ? integerOf(ci.arg(2)) : std::int64_t(s.size());
                if (count < 0 || pos + count > std::int64_t(s.size()))
                    count = std::int64_t(s.size()) - pos;
                return runtimeOf(ci).str(String(s.substr(std::size_t(pos), std::size_t(count))));
            };
        if (name == "__chr")
            return [](CallInfo& ci) { return runtimeOf(ci).str(String(1, char16_t(integerOf(ci.arg(0))))); };
        if (name == "__repeat")
            return [](CallInfo& ci) {
                const std::int64_t n = std::max<std::int64_t>(0, integerOf(ci.arg(0)));
                // `std::string s(2000000000, 'a')` built the whole string before the memory limit saw it.
                runtimeOf(ci).ctx.reserve(std::size_t(n) * sizeof(char16_t));
                return runtimeOf(ci).str(String(std::size_t(n), char16_t(integerOf(ci.arg(1)))));
            };
        if (name == "__at")
            return [](CallInfo& ci) {
                const StringView s = typed::textOf(ci.arg(0));
                const std::int64_t i = integerOf(ci.arg(1));
                if (i == std::int64_t(s.size()))
                    return Value(char16_t(0));
                if (i < 0 || i > std::int64_t(s.size()))
                    typed::fail(ci, ErrorCode::ArgumentOutOfRange, "string index out of range");
                return Value(s[std::size_t(i)]);
            };
        if (name == "strlen")
            return [](CallInfo& ci) { return Value(std::int32_t(charsOf(ci.arg(0)).size())); };
        if (name == "strcmp")
            return [](CallInfo& ci) {
                const int r = charsOf(ci.arg(0)).compare(charsOf(ci.arg(1)));
                return Value(std::int32_t(r < 0 ? -1 : r > 0 ? 1 : 0));
            };
        if (name == "strncmp")
            return [](CallInfo& ci) {
                const std::size_t n = std::size_t(std::max<std::int64_t>(0, integerOf(ci.arg(2))));
                const int r = charsOf(ci.arg(0)).substr(0, n).compare(charsOf(ci.arg(1)).substr(0, n));
                return Value(std::int32_t(r < 0 ? -1 : r > 0 ? 1 : 0));
            };
        if (name == "strstr")
            return [](CallInfo& ci) {
                const std::size_t at = charsOf(ci.arg(0)).find(charsOf(ci.arg(1)));
                return Value(std::int32_t(at == String::npos ? -1 : std::int32_t(at)));
            };
        if (name == "strchr")
            return [](CallInfo& ci) {
                const std::size_t at = charsOf(ci.arg(0)).find(char16_t(integerOf(ci.arg(1))));
                return Value(std::int32_t(at == String::npos ? -1 : std::int32_t(at)));
            };
        if (name == "atoi" || name == "atol")
            return [name](CallInfo& ci) {
                const std::string s = toUtf8(charsOf(ci.arg(0)));
                const long long v = std::strtoll(s.c_str(), nullptr, 10);
                return name == "atoi" ? Value(std::int32_t(v)) : Value(std::int64_t(v));
            };
        if (name == "atof")
            return [](CallInfo& ci) {
                base::ClassicLocale classic;
                return Value(std::strtod(toUtf8(charsOf(ci.arg(0))).c_str(), nullptr));
            };
        if (name == "isdigit") return [](CallInfo& ci) { return Value(std::int32_t(classify(integerOf(ci.arg(0)), std::isdigit))); };
        if (name == "isalpha") return [](CallInfo& ci) { return Value(std::int32_t(classify(integerOf(ci.arg(0)), std::isalpha))); };
        if (name == "isalnum") return [](CallInfo& ci) { return Value(std::int32_t(classify(integerOf(ci.arg(0)), std::isalnum))); };
        if (name == "isspace") return [](CallInfo& ci) { return Value(std::int32_t(classify(integerOf(ci.arg(0)), std::isspace))); };
        if (name == "isupper") return [](CallInfo& ci) { return Value(std::int32_t(classify(integerOf(ci.arg(0)), std::isupper))); };
        if (name == "islower") return [](CallInfo& ci) { return Value(std::int32_t(classify(integerOf(ci.arg(0)), std::islower))); };
        if (name == "ispunct") return [](CallInfo& ci) { return Value(std::int32_t(classify(integerOf(ci.arg(0)), std::ispunct))); };
        if (name == "toupper")
            return [](CallInfo& ci) {
                const std::int64_t c = integerOf(ci.arg(0));
                return Value(std::int32_t(c >= 'a' && c <= 'z' ? c - 32 : c));
            };
        if (name == "tolower")
            return [](CallInfo& ci) {
                const std::int64_t c = integerOf(ci.arg(0));
                return Value(std::int32_t(c >= 'A' && c <= 'Z' ? c + 32 : c));
            };
        if (name == "abs") return [](CallInfo& ci) { return Value(std::int32_t(std::abs(ci.arg(0).toInt()))); };
        if (name == "labs" || name == "llabs")
            return [](CallInfo& ci) { return Value(std::int64_t(std::llabs(integerOf(ci.arg(0))))); };
        if (name == "rand") return [](CallInfo&) { return Value(std::int32_t(std::rand())); };
        if (name == "srand")
            return [](CallInfo& ci) {
                std::srand(unsigned(integerOf(ci.arg(0))));
                return Value();
            };
        if (name == "isnan") return [](CallInfo& ci) { return Value(std::int32_t(std::isnan(ci.arg(0).toDouble()) ? 1 : 0)); };
        if (name == "isinf") return [](CallInfo& ci) { return Value(std::int32_t(std::isinf(ci.arg(0).toDouble()) ? 1 : 0)); };
        struct Unary
        {
            const char* name;
            double (*fn)(double);
        };
        static const Unary unaries[] = {
            {"fabs", [](double x) { return std::fabs(x); }},   {"sqrt", [](double x) { return std::sqrt(x); }},
            {"cbrt", [](double x) { return std::cbrt(x); }},   {"exp", [](double x) { return std::exp(x); }},
            {"log", [](double x) { return std::log(x); }},     {"log2", [](double x) { return std::log2(x); }},
            {"log10", [](double x) { return std::log10(x); }}, {"sin", [](double x) { return std::sin(x); }},
            {"cos", [](double x) { return std::cos(x); }},     {"tan", [](double x) { return std::tan(x); }},
            {"asin", [](double x) { return std::asin(x); }},   {"acos", [](double x) { return std::acos(x); }},
            {"atan", [](double x) { return std::atan(x); }},   {"sinh", [](double x) { return std::sinh(x); }},
            {"cosh", [](double x) { return std::cosh(x); }},   {"tanh", [](double x) { return std::tanh(x); }},
            {"floor", [](double x) { return std::floor(x); }}, {"ceil", [](double x) { return std::ceil(x); }},
            {"round", [](double x) { return std::round(x); }}, {"trunc", [](double x) { return std::trunc(x); }},
        };
        for (const Unary& u : unaries) {
            if (name == u.name) {
                double (*fn)(double) = u.fn;
                return [fn](CallInfo& ci) { return Value(fn(ci.arg(0).toDouble())); };
            }
        }
        struct Binary
        {
            const char* name;
            double (*fn)(double, double);
        };
        static const Binary binaries[] = {
            {"pow", [](double a, double b) { return std::pow(a, b); }},
            {"atan2", [](double a, double b) { return std::atan2(a, b); }},
            {"fmod", [](double a, double b) { return std::fmod(a, b); }},
            {"fmin", [](double a, double b) { return std::fmin(a, b); }},
            {"fmax", [](double a, double b) { return std::fmax(a, b); }},
            {"hypot", [](double a, double b) { return std::hypot(a, b); }},
        };
        for (const Binary& b : binaries) {
            if (name == b.name) {
                double (*fn)(double, double) = b.fn;
                return [fn](CallInfo& ci) { return Value(fn(ci.arg(0).toDouble(), ci.arg(1).toDouble())); };
            }
        }
        return nullptr;
    };

    for (const LibcFunction* f = kFunctions; f->name; ++f) {
        std::vector<typed::ParamInfo> params;
        for (const char* p = f->params; *p; ++p)
            params.push_back(table.param(*p == 'V' ? "args" : "a", typeOf(*p)));
        NativeFunction fn = native(f->name);
        if (!fn)
            continue;
        table.addMethod(libc, f->name, typeOf(f->result), std::move(params), typed::MethodStatic,
                        std::move(fn));
    }
}

}  // namespace c
}  // namespace internal
}  // namespace script
