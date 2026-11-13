// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Time values (milliseconds since 1970 in UTC) and the calendar over them, shared by the engines' Date.
#pragma once

#include <limits>
#include <string>

#include "script/internal/common.hpp"

namespace script {
namespace internal {
namespace dyn {
namespace datecalc {

inline constexpr double kMsPerDay = 86400000.0;
inline constexpr double kMsPerHour = 3600000.0;
inline constexpr double kMsPerMinute = 60000.0;
inline constexpr double kMsPerSecond = 1000.0;
inline const double kNaN = std::numeric_limits<double>::quiet_NaN();

double positiveMod(double a, double b);
double integerOrInfinity(double v);
double day(double t);
double timeWithinDay(double t);
double daysFromCivil(double year, double month, double dayOfMonth);
double daysInMonth(double year, double month);
double yearFromTime(double t);
double monthFromTime(double t);
double dateFromTime(double t);
double weekDay(double t);
double hourFromTime(double t);
double minFromTime(double t);
double secFromTime(double t);
double msFromTime(double t);
double makeTime(double hour, double min, double sec, double ms);
double makeDay(double year, double month, double date);
double makeDate(double dayCount, double time);
double timeClip(double time);
double offsetAt(double utc);
double localTime(double t);
double utcOf(double local);
std::string zoneName(double utc);
double nowMillis();
std::string dateString(double tv);
std::string timeString(double tv);
std::string timeZoneString(double utc);
std::string toDateText(double tv);
std::string isoString(double tv);
std::string utcString(double tv);
std::string localeDate(double t);
std::string localeTime(double t);
double parseDate(StringView text);

}  // namespace datecalc
}  // namespace dyn
}  // namespace internal
}  // namespace script
