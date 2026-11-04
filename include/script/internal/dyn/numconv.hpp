// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Number <-> string conversion following the ECMAScript rules.
#pragma once

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

}  // namespace dyn
}  // namespace internal
}  // namespace script
