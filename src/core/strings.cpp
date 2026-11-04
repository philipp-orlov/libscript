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

String numberToString(double d)
{
    if (std::isnan(d))
        return ascii("NaN");
    if (d == 0)
        return ascii("0");
    if (std::isinf(d))
        return ascii(d < 0 ? "-Infinity" : "Infinity");

    const base::ShortestDigits shortest = base::shortestDigits(std::fabs(d));
    const int n = shortest.pointPosition;
    const int k = shortest.count;

    String out;
    if (d < 0)
        out.push_back(u'-');
    if (-6 < n && n <= 21) {
        appendPlainDecimal(shortest.digits, k, n, out);
        return out;
    }
    out.push_back(Char(shortest.digits[0]));
    if (k > 1) {
        out.push_back(u'.');
        for (int i = 1; i < k; ++i)
            out.push_back(Char(shortest.digits[i]));
    }
    const int e = n - 1;
    out.append(e >= 0 ? u"e+" : u"e-");
    char exponent[8];
    const auto end = std::to_chars(exponent, exponent + sizeof exponent, std::abs(e));
    for (const char* p = exponent; p < end.ptr; ++p)
        out.push_back(Char(*p));
    return out;
}

}  // namespace internal
}  // namespace script
