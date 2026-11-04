// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/typed/numconv.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

#include "base/classic_locale.hpp"
#include "base/number.hpp"

namespace script {
namespace internal {
namespace typed {

namespace {

/// C# writes exponents as `E+16`, with at least two digits, and always writes
/// the sign. printf writes `e+16`. Rewrite in place.
String fixExponent(const char* buf)
{
    std::string s(buf);
    const std::size_t e = s.find_first_of("eE");
    if (e == std::string::npos)
        return ascii(s);

    std::string mantissa = s.substr(0, e);
    std::string exponent = s.substr(e + 1);
    char sign = '+';
    if (!exponent.empty() && (exponent[0] == '+' || exponent[0] == '-')) {
        sign = exponent[0];
        exponent.erase(0, 1);
    }
    while (exponent.size() > 2 && exponent[0] == '0')
        exponent.erase(0, 1);
    while (exponent.size() < 2)
        exponent.insert(exponent.begin(), '0');
    return ascii(mantissa + "E" + sign + exponent);
}

/// The shortest form that reads back as the same value, printed the way .NET
/// prints it: fixed point over a wide middle range, scientific outside it.
template <class T>
String shortest(T value)
{
    if (std::isnan(value))
        return ascii("NaN");
    if (std::isinf(value))
        return ascii(value < 0 ? "-Infinity" : "Infinity");
    if (value == 0)
        return ascii(std::signbit(value) ? "-0" : "0");

    const base::ShortestDigits digits = base::shortestDigits(std::fabs(value));
    const int exponent = digits.pointPosition - 1;
    String out;
    if (value < 0)
        out.push_back(u'-');
    if (exponent >= -5 && exponent < 15) {
        appendPlainDecimal(digits.digits, digits.count, digits.pointPosition, out);
        return out;
    }
    out.push_back(Char(digits.digits[0]));
    if (digits.count > 1) {
        out.push_back(u'.');
        for (int i = 1; i < digits.count; ++i)
            out.push_back(Char(digits.digits[i]));
    }
    out.append(exponent < 0 ? u"E-" : u"E+");
    const int magnitude = exponent < 0 ? -exponent : exponent;
    if (magnitude < 10)
        out.push_back(u'0');
    out += ascii(std::to_string(magnitude));
    return out;
}

template <class T>
String integerToString(T value)
{
    char buf[24];
    const auto end = std::to_chars(buf, buf + sizeof buf, value);
    return ascii(std::string_view(buf, std::size_t(end.ptr - buf)));
}

bool isBlank(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/// `text` without surrounding blanks, or empty.
std::string_view trimmedBlanks(std::string_view text) noexcept
{
    while (!text.empty() && isBlank(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isBlank(text.back()))
        text.remove_suffix(1);
    return text;
}

}  // namespace

String doubleToString(double value)
{
    return shortest(value);
}

String floatToString(float value)
{
    return shortest(value);
}

String longToString(std::int64_t value)
{
    return integerToString(value);
}

String ulongToString(std::uint64_t value)
{
    return integerToString(value);
}

bool formatNumber(double value, bool isIntegral, std::int64_t integralValue, StringView format,
                  String& out)
{
    if (format.empty()) {
        out = isIntegral ? longToString(integralValue) : doubleToString(value);
        return true;
    }

    const char specifier = char(format[0]);
    int precision = -1;
    if (format.size() > 1) {
        precision = 0;
        for (std::size_t i = 1; i < format.size(); ++i) {
            if (format[i] < '0' || format[i] > '9')
                return false;
            precision = precision * 10 + int(format[i] - '0');
        }
    }

    base::ClassicLocale classic;
    char buf[512];
    switch (specifier) {
        case 'd':
        case 'D': {
            if (!isIntegral)
                return false;
            String digits = longToString(integralValue < 0 ? -integralValue : integralValue);
            String pad;
            for (int i = int(digits.size()); i < precision; ++i)
                pad.push_back(u'0');
            out = (integralValue < 0 ? ascii("-") : String()) + pad + digits;
            return true;
        }
        case 'f':
        case 'F':
            std::snprintf(buf, sizeof buf, "%.*f", precision < 0 ? 2 : precision, value);
            out = ascii(buf);
            return true;
        case 'e':
        case 'E':
            std::snprintf(buf, sizeof buf, "%.*e", precision < 0 ? 6 : precision, value);
            out = fixExponent(buf);
            return true;
        case 'p':
        case 'P':
            std::snprintf(buf, sizeof buf, "%.*f", precision < 0 ? 2 : precision, value * 100.0);
            out = ascii(buf) + ascii(" %");
            return true;
        case 'x':
        case 'X': {
            if (!isIntegral)
                return false;
            std::snprintf(buf, sizeof buf, specifier == 'x' ? "%llx" : "%llX",
                          static_cast<unsigned long long>(integralValue));
            String digits = ascii(buf);
            String pad;
            for (int i = int(digits.size()); i < precision; ++i)
                pad.push_back(u'0');
            out = pad + digits;
            return true;
        }
        case 'n':
        case 'N': {
            std::snprintf(buf, sizeof buf, "%.*f", precision < 0 ? 2 : precision, value);
            std::string s(buf);
            const std::size_t dot = s.find('.');
            std::size_t start = (!s.empty() && s[0] == '-') ? 1 : 0;
            std::size_t end = dot == std::string::npos ? s.size() : dot;
            for (std::size_t at = end; at > start + 3;) {
                at -= 3;
                s.insert(at, ",");
            }
            out = ascii(s);
            return true;
        }
        case 'g':
        case 'G':
            out = isIntegral ? longToString(integralValue) : doubleToString(value);
            return true;
        default:
            return false;
    }
}

bool parseInteger(StringView text, std::int64_t& out)
{
    const AsciiText ascii8(text);
    std::string_view digits = ascii8.isAscii() ? trimmedBlanks(ascii8.view()) : std::string_view();
    if (!digits.empty() && digits[0] == '+')
        digits.remove_prefix(1);
    if (digits.empty())
        return false;
    long long value = 0;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (parsed.ec != std::errc() || parsed.ptr != digits.data() + digits.size())
        return false;
    out = std::int64_t(value);
    return true;
}

bool parseDouble(StringView text, double& out)
{
    const AsciiText ascii8(text);
    const std::string_view number = ascii8.isAscii() ? trimmedBlanks(ascii8.view()) : std::string_view();
    const base::ParsedDouble parsed = base::parseDouble(number);
    if (parsed.consumed == 0 || parsed.consumed != number.size())
        return false;
    out = parsed.value;
    return true;
}

bool parseIntegerLiteral(const std::string& digits, int radix, std::uint64_t& out)
{
    if (digits.empty())
        return false;
    unsigned long long value = 0;
    const char* end = digits.data() + digits.size();
    const auto parsed = std::from_chars(digits.data(), end, value, radix);
    if (parsed.ec != std::errc() || parsed.ptr != end)
        return false;
    out = value;
    return true;
}

}  // namespace typed
}  // namespace internal
}  // namespace script
