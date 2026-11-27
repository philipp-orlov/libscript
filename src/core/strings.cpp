// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// UTF-8 <-> UTF-16, the string hash, and the language-neutral number spelling.
#include <charconv>
#include <cmath>
#include <cstdlib>

#include "base/hash.hpp"
#include "base/number.hpp"
#include "base/utf16.hpp"
#include "json/escape.hpp"
#include "script/internal/common.hpp"

namespace script {
namespace internal {

std::string toUtf8(StringView s)
{
    std::string out;
    base::appendUtf8FromUtf16(s, out);
    return out;
}

String fromUtf8(std::string_view s)
{
    String out;
    base::appendUtf16FromUtf8(s, out);
    return out;
}

String ascii(std::string_view s)
{
    String out;
    out.reserve(s.size());
    for (char c : s)
        out.push_back(Char(std::uint8_t(c)));
    return out;
}

std::size_t hashString(StringView s) noexcept
{
    return std::size_t(base::hashBytes64(s.data(), s.size() * sizeof(Char)));
}

AsciiText::AsciiText(StringView s)
{
    char* out = small_;
    if (s.size() > sizeof small_) {
        large_.resize(s.size());
        out = large_.data();
    }
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] > 0x7f) {
            ascii_ = false;
            return;
        }
        out[i] = char(s[i]);
    }
    view_ = std::string_view(out, s.size());
}

void appendJsonQuoted(StringView s, String& out)
{
    struct Sink
    {
        String& out;
        void put(const Char* p, std::size_t n) { out.append(p, n); }
        void put(const char* p, std::size_t n)
        {
            for (std::size_t i = 0; i < n; ++i)
                out.push_back(Char(std::uint8_t(p[i])));
        }
    } sink{out};
    out.push_back(u'"');
    json::escapeUnitsTo<Sink, Char>(sink, s);
    out.push_back(u'"');
}

void appendPlainDecimal(const char* digits, int count, int pointPosition, String& out)
{
    const auto put = [&](const char* text, int n) {
        for (int i = 0; i < n; ++i)
            out.push_back(Char(std::uint8_t(text[i])));
    };
    if (pointPosition <= 0) {
        out.append(u"0.");
        out.append(std::size_t(-pointPosition), u'0');
        put(digits, count);
    } else if (count <= pointPosition) {
        put(digits, count);
        out.append(std::size_t(pointPosition - count), u'0');
    } else {
        put(digits, pointPosition);
        out.push_back(u'.');
        put(digits + pointPosition, count - pointPosition);
    }
}

std::size_t formatNumber(double d, Char* out) noexcept
{
    Char* p = out;
    const auto text = [&](const char* s) {
        while (*s)
            *p++ = Char(std::uint8_t(*s++));
        return std::size_t(p - out);
    };
    if (std::isnan(d))
        return text("NaN");
    if (d == 0)
        return text("0");
    if (std::isinf(d))
        return text(d < 0 ? "-Infinity" : "Infinity");
    if (d < 0)
        *p++ = u'-';
    const double a = std::fabs(d);

    // A whole number, or one of up to four decimals, below 2^40 once scaled: k decimals that divide back to it exactly are
    // its shortest form, since at that size decimals of k places lie thousands of doubles apart (only one rounds to it).
    {
        static constexpr double kTens[] = {1, 10, 100, 1000, 10000};
        for (int k = 0; k <= 4; ++k) {
            const double scaled = a * kTens[k];
            if (scaled >= 1099511627776.0)
                break;
            const double m = std::nearbyint(scaled);
            if (m == 0 || m / kTens[k] != a)
                continue;
            std::uint64_t whole = std::uint64_t(m);
            const std::uint64_t unit = std::uint64_t(kTens[k]);
            const std::uint64_t fraction = whole % unit;
            whole /= unit;
            Char digits[24];
            Char* q = digits + 24;
            do {
                *--q = Char(u'0' + whole % 10);
                whole /= 10;
            } while (whole);
            while (q < digits + 24)
                *p++ = *q++;
            if (k > 0) {
                *p++ = u'.';
                for (std::uint64_t place = unit / 10; place > 0; place /= 10)
                    *p++ = Char(u'0' + (fraction / place) % 10);
            }
            return std::size_t(p - out);
        }
    }

    const base::ShortestDigits shortest = base::shortestDigits(a);
    const int n = shortest.pointPosition;
    const int k = shortest.count;
    const auto put = [&](const char* s, int count) {
        for (int i = 0; i < count; ++i)
            *p++ = Char(std::uint8_t(s[i]));
    };
    if (-6 < n && n <= 21) {
        // (As `appendPlainDecimal`.)
        if (n <= 0) {
            *p++ = u'0';
            *p++ = u'.';
            for (int i = 0; i < -n; ++i)
                *p++ = u'0';
            put(shortest.digits, k);
        } else if (k <= n) {
            put(shortest.digits, k);
            for (int i = 0; i < n - k; ++i)
                *p++ = u'0';
        } else {
            put(shortest.digits, n);
            *p++ = u'.';
            put(shortest.digits + n, k - n);
        }
        return std::size_t(p - out);
    }
    *p++ = Char(std::uint8_t(shortest.digits[0]));
    if (k > 1) {
        *p++ = u'.';
        put(shortest.digits + 1, k - 1);
    }
    int e = n - 1;
    *p++ = u'e';
    *p++ = e >= 0 ? u'+' : u'-';
    e = std::abs(e);
    if (e >= 100)
        *p++ = Char(u'0' + e / 100);
    if (e >= 10)
        *p++ = Char(u'0' + e / 10 % 10);
    *p++ = Char(u'0' + e % 10);
    return std::size_t(p - out);
}

String numberToString(double d)
{
    Char text[kMaxNumberText];
    return String(text, formatNumber(d, text));
}

}  // namespace internal
}  // namespace script
