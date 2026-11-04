// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Number <-> text, in the forms C# specifies.
#pragma once

#include <cstdint>
#include <string>

#include "script/internal/common.hpp"

namespace script {
namespace internal {
namespace typed {

/// Shortest representation that round-trips, which is what `double.ToString()`
/// produces on .NET Core 3.0 and later.
String doubleToString(double value);
String floatToString(float value);
String longToString(std::int64_t value);
String ulongToString(std::uint64_t value);

/// The standard numeric format strings the engine supports: `D`, `F`, `N`,
/// `X`, `E`, `P` and `G`, each with an optional precision. An unsupported
/// specifier returns false and the caller raises a `FormatException`.
bool formatNumber(double value, bool isIntegral, std::int64_t integralValue, StringView format,
                  String& out);

/// `int.Parse` / `TryParse`. Leading and trailing whitespace is allowed, a
/// leading sign is allowed, and the result must fit.
bool parseInteger(StringView text, std::int64_t& out);
bool parseDouble(StringView text, double& out);

/// Digits of a literal, after the lexer stripped separators. Returns false on
/// overflow of the widest type.
bool parseIntegerLiteral(const std::string& digits, int radix, std::uint64_t& out);

}  // namespace typed
}  // namespace internal
}  // namespace script
