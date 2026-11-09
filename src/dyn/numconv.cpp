// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Number formatting and parsing, following ECMA-262 section 7.1.

#include <charconv>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

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

/// Adds one to a string of decimal digits; a carry out of the first digit
/// grows the string by a leading 1.
void incrementDigits(std::string& digits)
{
    for (std::size_t i = digits.size(); i-- > 0;) {
        if (digits[i] != '9') {
            ++digits[i];
            return;
        }
        digits[i] = '0';
    }
    digits.insert(digits.begin(), '1');
}

/// Whether the exact digits `rest` that follow a cut round the kept digits up.
/// glibc prints the exact binary value, so `rest` is an exact tie only when it
/// is a 5 followed by zeros.
bool roundsUp(std::string_view rest, char lastKept, RoundTies ties)
{
    if (rest.empty() || rest[0] < '5')
        return false;
    if (rest[0] > '5' || ties == RoundTies::AwayFromZero)
        return true;
    for (std::size_t i = 1; i < rest.size(); ++i)
        if (rest[i] != '0')
            return true;
    return (lastKept - '0') % 2 != 0;
}

/// The decimal digits of |d| rounded to `count` significant digits. `exp10` is
/// the exponent of the first digit.
std::string significantDigits(double d, int count, int& exp10, RoundTies ties)
{
    base::ClassicLocale classic;
    char buf[1024];
    std::snprintf(buf, sizeof buf, "%.800e", std::fabs(d));
    std::string digits(1, buf[0]);
    const char* p = buf + 2;
    for (; *p != 'e'; ++p)
        digits += *p;
    exp10 = std::atoi(p + 1);
    const std::size_t n = std::size_t(count);
    const bool up = n < digits.size() && roundsUp(std::string_view(digits).substr(n), digits[n - 1], ties);
    digits.resize(n, '0');
    if (up) {
        incrementDigits(digits);
        if (digits.size() > n) {
            digits.pop_back();
            ++exp10;
        }
    }
    return digits;
}

String exponentialForm(const std::string& digits, int exp10, bool negative)
{
    std::string out = negative ? "-" : "";
    out += digits[0];
    if (digits.size() > 1) {
        out += '.';
        out.append(digits, 1, std::string::npos);
    }
    out += exp10 < 0 ? "e-" : "e+";
    out += std::to_string(std::abs(exp10));
    return ascii(out.c_str());
}

/// |d| with `fraction` digits after the point.
std::string fixedDigits(double d, int fraction, RoundTies ties)
{
    base::ClassicLocale classic;
    char buf[1500];
    std::snprintf(buf, sizeof buf, "%.1100f", std::fabs(d));
    const char* point = std::strchr(buf, '.');
    std::string digits(buf, std::size_t(point - buf));
    digits.append(point + 1, std::size_t(fraction));
    if (roundsUp(point + 1 + fraction, digits.back(), ties))
        incrementDigits(digits);
    const std::size_t intLength = digits.size() - std::size_t(fraction);
    if (fraction == 0)
        return digits;
    return digits.substr(0, intLength) + '.' + digits.substr(intLength);
}

}  // namespace

String numberToFixed(double d, int digits, RoundTies ties)
{
    if (std::isnan(d))
        return ascii("NaN");
    if (std::fabs(d) >= 1e21 || std::isinf(d))
        return numberToString(d);
    std::string out = fixedDigits(d, digits, ties);
    if (d < 0)
        out.insert(out.begin(), '-');
    return ascii(out.c_str());
}

String numberToExponential(double d, int digits, RoundTies ties)
{
    if (std::isnan(d))
        return ascii("NaN");
    if (std::isinf(d))
        return numberToString(d);
    if (d == 0)
        return exponentialForm(std::string(std::size_t(digits) + 1, '0'), 0, false);
    int exp10 = 0;
    const std::string sig = significantDigits(d, digits + 1, exp10, ties);
    return exponentialForm(sig, exp10, d < 0);
}

String numberToPrecision(double d, int digits, RoundTies ties)
{
    if (std::isnan(d))
        return ascii("NaN");
    if (std::isinf(d))
        return numberToString(d);
    if (d == 0)
        return numberToFixed(0.0, digits - 1);
    int exp10 = 0;
    const std::string sig = significantDigits(d, digits, exp10, ties);
    if (exp10 < -6 || exp10 >= digits)
        return exponentialForm(sig, exp10, d < 0);
    std::string out = d < 0 ? "-" : "";
    if (exp10 >= 0) {
        out.append(sig, 0, std::size_t(exp10) + 1);
        if (std::size_t(exp10) + 1 < sig.size()) {
            out += '.';
            out.append(sig, std::size_t(exp10) + 1, std::string::npos);
        }
    } else {
        out += "0.";
        out.append(std::size_t(-exp10 - 1), '0');
        out += sig;
    }
    return ascii(out.c_str());
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

double parseIntOfNumber(double d) noexcept
{
    if (std::isnan(d) || std::isinf(d))
        return std::numeric_limits<double>::quiet_NaN();  // "NaN", "Infinity"
    if (d == 0)
        return 0;
    // Below 2^53 a number prints its whole part exactly, and that is the answer.
    const double size = std::fabs(d);
    if (size >= 1e-6 && size < 9007199254740992.0)
        return std::trunc(d);
    char text[32];
    if (size >= 1e-6 && size < 1e21) {
        // Up to 1e21 it prints its shortest digits padded with zeros, which need not read back as the number.
        const auto printed = std::to_chars(text, text + sizeof text, d, std::chars_format::fixed);
        double back = d;
        if (printed.ec == std::errc())
            std::from_chars(text, printed.ptr, back);
        return back;
    }
    // One that prints with an exponent stops at its first digit, which is the first of its shortest digits.
    const auto printed = std::to_chars(text, text + sizeof text, size, std::chars_format::scientific);
    const double digit = printed.ec == std::errc() ? double(text[0] - '0') : 0;
    return d < 0 ? -digit : digit;
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

    if (radix == 10) {
        // Correctly rounded, as the standard asks up to 20 significant digits: adding digit by digit in a double is not.
        std::string digits;
        for (; i < s.size() && s[i] >= u'0' && s[i] <= u'9'; ++i)
            digits.push_back(char(s[i]));
        if (digits.empty())
            return std::nan("");
        double value = 0;
        // A whole number can only be too large, never too small.
        if (std::from_chars(digits.data(), digits.data() + digits.size(), value).ec == std::errc::result_out_of_range)
            value = std::numeric_limits<double>::infinity();
        return sign * value;
    }
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
