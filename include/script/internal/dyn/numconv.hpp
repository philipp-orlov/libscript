// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Number <-> string conversion following the ECMAScript rules.
#pragma once

#include <cmath>
#include <limits>

#include <string>

#include "script/internal/common.hpp"

namespace script {
namespace internal {
namespace dyn {

/// `String(number)` is `internal::numberToString`.
String numberToStringRadix(double d, int radix);
String numberToFixed(double d, int digits);
String numberToPrecision(double d, int digits);
String numberToExponential(double d, int digits);

/// `Number(string)`: full literal grammar, NaN when the text is not a number.
double stringToNumber(StringView s);
/// `parseInt` / `parseFloat`, which stop at the first character they cannot use.
double parseIntPrefix(StringView s, int radix);
double parseFloatPrefix(StringView s);
/// `parseInt(d)` for a number: what its text would give, usually without making the text.
double parseIntOfNumber(double d) noexcept;

/// What `Math.pow` and `**`, `Math.max`, `Math.min` and `Math.round` answer where C's functions say otherwise.
inline double numberPow(double x, double y) noexcept
{
    if (y == 0)
        return 1;
    if (std::isnan(y) || (std::isinf(y) && std::fabs(x) == 1))
        return std::numeric_limits<double>::quiet_NaN();
    return std::pow(x, y);
}
inline double numberMax(double a, double b) noexcept
{
    if (std::isnan(a) || std::isnan(b))
        return std::numeric_limits<double>::quiet_NaN();
    if (a == b)
        return std::signbit(a) ? b : a;
    return a > b ? a : b;
}
inline double numberMin(double a, double b) noexcept
{
    if (std::isnan(a) || std::isnan(b))
        return std::numeric_limits<double>::quiet_NaN();
    if (a == b)
        return std::signbit(a) ? a : b;
    return a < b ? a : b;
}
inline double numberRound(double d) noexcept
{
    if (!std::isfinite(d) || d == 0)
        return d;
    double r = std::floor(d);
    if (d - r >= 0.5)
        r += 1;
    return std::copysign(r, d);
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
