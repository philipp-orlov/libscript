// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Number formatting and parsing, following ECMA-262 section 7.1.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include "base/ascii.hpp"
#include "base/classic_locale.hpp"
#include "base/number.hpp"
#include "script/internal/dyn/numconv.hpp"

namespace script {
namespace internal {
namespace dyn {

namespace {

bool isSpace(Char c) noexcept
{
    return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r' || c == u'\f' || c == u'\v' ||
           c == 0x00a0 || c == 0xfeff || c == 0x2028 || c == 0x2029;
}

/// The grammar of a decimal literal has digits or a point up front; the words a
/// C library would also read as numbers (`inf`, `nan`) are not numbers here.
bool startsLikeANumber(std::string_view text) noexcept
{
    const std::size_t at = !text.empty() && (text[0] == '+' || text[0] == '-') ? 1 : 0;
    return at < text.size() && (text[at] == '.' || (text[at] >= '0' && text[at] <= '9'));
}

}  // namespace

String numberToStringRadix(double d, int radix)
{
    if (radix == 10)
        return numberToString(d);
    if (std::isnan(d))
        return ascii("NaN");
    if (std::isinf(d))
        return ascii(d > 0 ? "Infinity" : "-Infinity");

    const bool negative = d < 0;
    d = std::fabs(d);
    static const char* kDigits = "0123456789abcdefghijklmnopqrstuvwxyz";

    double integral = std::floor(d);
    double fraction = d - integral;

    std::string whole;
    if (integral == 0) {
        whole = "0";
    } else {
        while (integral >= 1) {
            const double q = std::floor(integral / radix);
            whole.push_back(kDigits[int(integral - q * radix)]);
            integral = q;
        }
        std::reverse(whole.begin(), whole.end());
    }

    // 52 digits is past the point where any binary double carries information
    // in this radix; stop early once the remainder vanishes.
    if (fraction > 0) {
        whole.push_back('.');
        for (int i = 0; i < 52 && fraction > 0; ++i) {
            fraction *= radix;
            const int digit = int(std::floor(fraction));
            whole.push_back(kDigits[digit]);
            fraction -= digit;
        }
    }
    return ascii(negative ? "-" + whole : whole);
}

namespace {
String formatted(const char* spec, int digits, double d)
{
    base::ClassicLocale classic;
    char buf[512];
    std::snprintf(buf, sizeof buf, spec, digits, d);
    return ascii(buf);
}
}  // namespace

String numberToFixed(double d, int digits)
{
    if (std::isnan(d))
        return ascii("NaN");
    if (std::fabs(d) >= 1e21 || std::isinf(d))
        return numberToString(d);
    return formatted("%.*f", digits, d);
}

String numberToExponential(double d, int digits)
{
    if (std::isnan(d))
        return ascii("NaN");
    if (std::isinf(d))
        return numberToString(d);
    String s = formatted("%.*e", digits, d);
    // C prints at least two exponent digits; JavaScript prints the minimum.
    const std::size_t e = s.find(u'e');
    if (e != String::npos) {
        std::size_t p = e + 2;
        std::size_t z = p;
        while (z + 1 < s.size() && s[z] == u'0')
            ++z;
        s.erase(p, z - p);
    }
    return s;
}

String numberToPrecision(double d, int digits)
{
    if (std::isnan(d))
        return ascii("NaN");
    if (std::isinf(d) || d == 0)
        return d == 0 ? numberToFixed(d, digits - 1) : numberToString(d);
    const int e = int(std::floor(std::log10(std::fabs(d))));
    if (e < -6 || e >= digits)
        return numberToExponential(d, digits - 1);
    return formatted("%.*f", digits - 1 - e, d);
}

double stringToNumber(StringView s)
{
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && isSpace(s[begin]))
        ++begin;
    while (end > begin && isSpace(s[end - 1]))
        --end;
    if (begin == end)
        return 0;

    StringView body = s.substr(begin, end - begin);
    if (body.size() > 2 && body[0] == u'0') {
        int radix = 0;
        if (body[1] == u'x' || body[1] == u'X')
            radix = 16;
        else if (body[1] == u'o' || body[1] == u'O')
            radix = 8;
        else if (body[1] == u'b' || body[1] == u'B')
            radix = 2;
        if (radix) {
            double acc = 0;
            for (std::size_t i = 2; i < body.size(); ++i) {
                const int v = base::digitValue(body[i]);
                if (v < 0 || v >= radix)
                    return std::nan("");
                acc = acc * radix + v;
            }
            return acc;
        }
    }

    const AsciiText text(body);
    if (!text.isAscii())
        return std::nan("");
    const std::string_view narrow = text.view();
    if (narrow == "Infinity" || narrow == "+Infinity")
        return std::numeric_limits<double>::infinity();
    if (narrow == "-Infinity")
        return -std::numeric_limits<double>::infinity();
    if (!startsLikeANumber(narrow))
        return std::nan("");
    const base::ParsedDouble parsed = base::parseDouble(narrow);
    return parsed.consumed == narrow.size() ? parsed.value : std::nan("");
}

double parseIntOfNumber(double d)
{
    // A number that prints without an exponent has its whole part for an answer, and no text need be made to say so.
    const double size = std::fabs(d);
    if (size >= 1e-6 && size < 9007199254740992.0)
        return std::trunc(d);
    if (d == 0)
        return 0;
    return parseIntPrefix(numberToString(d), 0);
}

double parseIntPrefix(StringView s, int radix)
{
    std::size_t i = 0;
    while (i < s.size() && isSpace(s[i]))
        ++i;
    int sign = 1;
    if (i < s.size() && (s[i] == u'+' || s[i] == u'-')) {
        sign = s[i] == u'-' ? -1 : 1;
        ++i;
    }
    if (radix == 0) {
        if (i + 1 < s.size() && s[i] == u'0' && (s[i + 1] == u'x' || s[i + 1] == u'X')) {
            radix = 16;
            i += 2;
        } else {
            radix = 10;
        }
    } else if (radix == 16 && i + 1 < s.size() && s[i] == u'0' &&
               (s[i + 1] == u'x' || s[i + 1] == u'X')) {
        i += 2;
    }
    if (radix < 2 || radix > 36)
        return std::nan("");

    double acc = 0;
    bool any = false;
    for (; i < s.size(); ++i) {
        const int v = base::digitValue(s[i]);
        if (v < 0 || v >= radix)
            break;
        acc = acc * radix + v;
        any = true;
    }
    return any ? sign * acc : std::nan("");
}

double parseFloatPrefix(StringView s)
{
    std::size_t i = 0;
    while (i < s.size() && isSpace(s[i]))
        ++i;
    std::size_t end = i;
    while (end < s.size() && s[end] < 0x80)
        ++end;
    const AsciiText text(s.substr(i, end - i));
    const std::string_view narrow = text.view();
    if (narrow.substr(0, 8) == "Infinity" || narrow.substr(0, 9) == "+Infinity")
        return std::numeric_limits<double>::infinity();
    if (narrow.substr(0, 9) == "-Infinity")
        return -std::numeric_limits<double>::infinity();
    if (!startsLikeANumber(narrow))
        return std::nan("");
    const base::ParsedDouble parsed = base::parseDouble(narrow);
    return parsed.consumed ? parsed.value : std::nan("");
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
