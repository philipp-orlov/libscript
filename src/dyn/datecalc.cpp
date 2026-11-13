// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The calendar arithmetic and the date parser of ECMAScript's section 21.4, over time values: what the dynamic engine's
// Date and the vm core's share.
#include "script/internal/dyn/datecalc.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <string>

#include "script/internal/dyn/numconv.hpp"

namespace script {
namespace internal {
namespace dyn {
namespace datecalc {

namespace {

const char* const kWeekDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

}  // namespace




double positiveMod(double a, double b)
{
    const double r = std::fmod(a, b);
    return r < 0 ? r + b : r + 0.0;  // never -0, which prints as "-0"
}

double integerOrInfinity(double v)
{
    if (std::isnan(v))
        return 0;
    return std::trunc(v) + 0.0;
}

double day(double t) { return std::floor(t / kMsPerDay); }
double timeWithinDay(double t) { return positiveMod(t, kMsPerDay); }

/// Days from 1970-01-01 to the first day of `month` (0-based) of `year`, proleptic Gregorian: Howard Hinnant's
/// days_from_civil, in doubles, since a time value's years go well beyond what an int day count holds exactly.
double daysFromCivil(double year, double month, double dayOfMonth)
{
    const double y = month <= 1 ? year - 1 : year;
    const double era = std::floor(y / 400);
    const double yoe = y - era * 400;
    const double m = month + 1;  // 1..12
    const double doy = std::floor((153 * (m > 2 ? m - 3 : m + 9) + 2) / 5) + dayOfMonth - 1;
    const double doe = yoe * 365 + std::floor(yoe / 4) - std::floor(yoe / 100) + doy;
    return era * 146097 + doe - 719468;
}

/// The days in `month` (0-based) of `year`.
double daysInMonth(double year, double month)
{
    return daysFromCivil(month >= 11 ? year + 1 : year, month >= 11 ? 0 : month + 1, 1) - daysFromCivil(year, month, 1);
}

struct Civil
{
    double year, month, date;  // month 0-based, date 1-based
};

Civil civilFromDays(double z)
{
    z += 719468;
    const double era = std::floor(z / 146097);
    const double doe = z - era * 146097;
    const double yoe = std::floor((doe - std::floor(doe / 1460) + std::floor(doe / 36524) - std::floor(doe / 146096)) / 365);
    const double y = yoe + era * 400;
    const double doy = doe - (365 * yoe + std::floor(yoe / 4) - std::floor(yoe / 100));
    const double mp = std::floor((5 * doy + 2) / 153);
    const double d = doy - std::floor((153 * mp + 2) / 5) + 1;
    const double m = mp < 10 ? mp + 3 : mp - 9;  // 1..12
    return {m <= 2 ? y + 1 : y, m - 1, d};
}

double yearFromTime(double t) { return civilFromDays(day(t)).year; }
double monthFromTime(double t) { return civilFromDays(day(t)).month; }
double dateFromTime(double t) { return civilFromDays(day(t)).date; }
double weekDay(double t) { return positiveMod(day(t) + 4, 7); }
double hourFromTime(double t) { return positiveMod(std::floor(t / kMsPerHour), 24); }
double minFromTime(double t) { return positiveMod(std::floor(t / kMsPerMinute), 60); }
double secFromTime(double t) { return positiveMod(std::floor(t / kMsPerSecond), 60); }
double msFromTime(double t) { return positiveMod(t, kMsPerSecond); }

double makeTime(double hour, double min, double sec, double ms)
{
    if (!std::isfinite(hour) || !std::isfinite(min) || !std::isfinite(sec) || !std::isfinite(ms))
        return kNaN;
    return integerOrInfinity(hour) * kMsPerHour + integerOrInfinity(min) * kMsPerMinute + integerOrInfinity(sec) * kMsPerSecond +
           integerOrInfinity(ms);
}

double makeDay(double year, double month, double date)
{
    if (!std::isfinite(year) || !std::isfinite(month) || !std::isfinite(date))
        return kNaN;
    const double y = integerOrInfinity(year);
    const double m = integerOrInfinity(month);
    const double dt = integerOrInfinity(date);
    const double ym = y + std::floor(m / 12);
    if (std::fabs(ym) > 400000)  // far past any time value TimeClip lets through
        return kNaN;
    const double mn = positiveMod(m, 12);
    return daysFromCivil(ym, mn, 1) + dt - 1;
}

double makeDate(double dayCount, double time)
{
    if (!std::isfinite(dayCount) || !std::isfinite(time))
        return kNaN;
    const double tv = dayCount * kMsPerDay + time;
    return std::isfinite(tv) ? tv : kNaN;
}

double timeClip(double time)
{
    if (!std::isfinite(time) || std::fabs(time) > 8.64e15)
        return kNaN;
    return integerOrInfinity(time);
}

/// The host's offset from UTC at the instant `utc`, in milliseconds.
double offsetAt(double utc)
{
    if (!std::isfinite(utc))
        return 0;
    // time_t covers every time value with room to spare; the C library may not, for years far from now, and then the
    // offset of the same day of a year near 1970 with the same calendar stands in.
    double probe = utc;
    const double year = yearFromTime(utc);
    if (year < 1900 || year > 9999) {
        const double near = year < 1900 ? 1972 + positiveMod(year, 28) : 2000 + positiveMod(year, 28);
        probe = makeDate(makeDay(near, monthFromTime(utc), dateFromTime(utc)), timeWithinDay(utc));
    }
    const std::time_t seconds = std::time_t(std::floor(probe / 1000));
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &seconds) != 0)
        return 0;
    long bias = 0;
    _get_timezone(&bias);
    return double(-bias + (local.tm_isdst > 0 ? 3600 : 0)) * 1000;
#else
    if (!localtime_r(&seconds, &local))
        return 0;
    return double(local.tm_gmtoff) * 1000;
#endif
}

double localTime(double t) { return t + offsetAt(t); }

/// UTC(t) for a local time `t`: the offset of the instant it most likely names.
double utcOf(double local)
{
    if (!std::isfinite(local))
        return kNaN;
    const double guess = local - offsetAt(local);
    return local - offsetAt(guess);
}

std::string zoneName(double utc)
{
#if defined(_WIN32)
    (void)utc;
    return std::string();
#else
    const std::time_t seconds = std::time_t(std::floor(utc / 1000));
    std::tm local{};
    if (!localtime_r(&seconds, &local) || !local.tm_zone)
        return std::string();
    return local.tm_zone;
#endif
}

double nowMillis()
{
    using namespace std::chrono;
    return double(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

// ---- formatting ----

std::string yearString(double year, bool expanded)
{
    char buf[32];
    if (expanded && (year < 0 || year > 9999))
        std::snprintf(buf, sizeof buf, "%c%06.0f", year < 0 ? '-' : '+', std::fabs(year));
    else if (year < 0)
        std::snprintf(buf, sizeof buf, "-%04.0f", -year);
    else
        std::snprintf(buf, sizeof buf, "%04.0f", year);
    return buf;
}

std::string dateString(double tv)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s %s %02.0f ", kWeekDays[int(weekDay(tv))], kMonths[int(monthFromTime(tv))], dateFromTime(tv));
    return buf + yearString(yearFromTime(tv), false);
}

std::string timeString(double tv)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02.0f:%02.0f:%02.0f GMT", hourFromTime(tv), minFromTime(tv), secFromTime(tv));
    return buf;
}

std::string timeZoneString(double utc)
{
    const double offset = offsetAt(utc);
    const double minutes = std::fabs(offset) / kMsPerMinute;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%c%02.0f%02.0f", offset >= 0 ? '+' : '-', std::floor(minutes / 60), positiveMod(minutes, 60));
    std::string out = buf;
    const std::string name = zoneName(utc);
    if (!name.empty())
        out += " (" + name + ")";
    return out;
}

std::string toDateText(double tv)
{
    if (std::isnan(tv))
        return "Invalid Date";
    const double t = localTime(tv);
    return dateString(t) + " " + timeString(t) + timeZoneString(tv);
}

std::string isoString(double tv)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "-%02.0f-%02.0fT%02.0f:%02.0f:%02.0f.%03.0fZ", monthFromTime(tv) + 1, dateFromTime(tv), hourFromTime(tv),
                  minFromTime(tv), secFromTime(tv), msFromTime(tv));
    return yearString(yearFromTime(tv), true) + buf;
}

std::string utcString(double tv)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s, %02.0f %s ", kWeekDays[int(weekDay(tv))], dateFromTime(tv), kMonths[int(monthFromTime(tv))]);
    return buf + yearString(yearFromTime(tv), false) + " " + timeString(tv);
}

/// en-US, as Intl would give with no locale data: `2/1/2022, 12:00:00 AM`.
std::string localeDate(double t)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.0f/%.0f/%.0f", monthFromTime(t) + 1, dateFromTime(t), yearFromTime(t));
    return buf;
}

std::string localeTime(double t)
{
    const double h = hourFromTime(t);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.0f:%02.0f:%02.0f %s", h == 0 ? 12 : h > 12 ? h - 12 : h, minFromTime(t), secFromTime(t), h < 12 ? "AM" : "PM");
    return buf;
}

// ---- parsing ----

/// What `Date.parse` accepts: the date-time string format of 21.4.1.32 and its date-only forms, and what toString and
/// toUTCString write, which have to round-trip; and, beyond what the specification asks, the common shapes those lead
/// to: a month name with a day and a year in either order, an optional time with AM or PM, a zone as GMT/UTC/Z or ±hhmm.
struct DateParser
{
    StringView s;
    std::size_t at = 0;

    bool end() const { return at >= s.size(); }
    Char peek(std::size_t k = 0) const { return at + k < s.size() ? s[at + k] : Char(0); }
    static bool digit(Char c) { return c >= u'0' && c <= u'9'; }
    static bool alpha(Char c) { return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z'); }

    bool digits(int count, double& out)
    {
        double v = 0;
        for (int i = 0; i < count; ++i) {
            if (!digit(peek()))
                return false;
            v = v * 10 + double(peek() - u'0');
            ++at;
        }
        out = v;
        return true;
    }

    double iso()
    {
        at = 0;
        double year = 0, month = 1, dayOfMonth = 1, hour = 0, minute = 0, second = 0, ms = 0;
        if (peek() == u'+' || peek() == u'-') {
            const bool negative = peek() == u'-';
            ++at;
            if (!digits(6, year))
                return kNaN;
            if (negative && year == 0)
                return kNaN;  // -000000 is not a year
            if (negative)
                year = -year;
        } else if (!digits(4, year)) {
            return kNaN;
        }
        bool dateOnly = true;
        if (peek() == u'-') {
            ++at;
            if (!digits(2, month) || month < 1 || month > 12)
                return kNaN;
            if (peek() == u'-') {
                ++at;
                if (!digits(2, dayOfMonth) || dayOfMonth < 1 || dayOfMonth > 31)
                    return kNaN;
            }
        }
        bool hasZone = false;
        double offset = 0;
        if (peek() == u'T' || peek() == u't') {
            dateOnly = false;
            ++at;
            if (!digits(2, hour) || peek() != u':')
                return kNaN;
            ++at;
            if (!digits(2, minute))
                return kNaN;
            if (peek() == u':') {
                ++at;
                if (!digits(2, second))
                    return kNaN;
                if (peek() == u'.' || peek() == u',') {
                    ++at;
                    if (!digit(peek()))
                        return kNaN;
                    double scale = 100;
                    while (digit(peek())) {
                        ms += double(peek() - u'0') * scale;
                        scale /= 10;
                        ++at;
                    }
                    ms = std::floor(ms);
                }
            }
            if (hour > 24 || minute > 59 || second > 59 || (hour == 24 && (minute != 0 || second != 0 || ms != 0)))
                return kNaN;
            if (peek() == u'Z' || peek() == u'z') {
                ++at;
                hasZone = true;
            } else if (peek() == u'+' || peek() == u'-') {
                const double sign = peek() == u'-' ? -1 : 1;
                ++at;
                double oh = 0, om = 0;
                if (!digits(2, oh) || peek() != u':')
                    return kNaN;
                ++at;
                if (!digits(2, om) || oh > 23 || om > 59)
                    return kNaN;
                offset = sign * (oh * kMsPerHour + om * kMsPerMinute);
                hasZone = true;
            }
        }
        if (!end())
            return kNaN;
        if (dayOfMonth > daysInMonth(year, month - 1))
            return kNaN;
        const double tv = makeDate(makeDay(year, month - 1, dayOfMonth), makeTime(hour, minute, second, ms));
        // A date alone is UTC; a date and a time with no zone are local.
        if (dateOnly || hasZone)
            return timeClip(tv - offset);
        return timeClip(utcOf(tv));
    }

    double loose()
    {
        at = 0;
        double year = kNaN, month = kNaN, dayOfMonth = kNaN, hour = 0, minute = 0, second = 0, ms = 0;
        double offset = 0;
        bool hasZone = false;
        bool pm = false, am = false;
        std::vector<double> numbers;
        while (!end()) {
            const Char c = peek();
            if (c == u' ' || c == u',' || c == u'\t' || c == u'/' || c == u'-' || c == u'.') {
                // A sign directly after GMT/UTC or a time belongs to a zone; elsewhere these only separate.
                if ((c == u'-') && hasZone && digit(peek(1)) && offset == 0) {
                    // handled below with '+'
                } else {
                    ++at;
                    continue;
                }
            }
            if (c == u'(') {  // a zone's name, which says nothing the offset does not
                while (!end() && peek() != u')')
                    ++at;
                ++at;
                continue;
            }
            if ((c == u'+' || c == u'-') && digit(peek(1))) {
                const double sign = c == u'-' ? -1 : 1;
                ++at;
                double v = 0;
                int n = 0;
                while (digit(peek())) {
                    v = v * 10 + double(peek() - u'0');
                    ++at;
                    ++n;
                }
                if (peek() == u':') {
                    ++at;
                    double m = 0;
                    if (!digits(2, m))
                        return kNaN;
                    offset = sign * (v * kMsPerHour + m * kMsPerMinute);
                } else {
                    offset = n <= 2 ? sign * v * kMsPerHour : sign * (std::floor(v / 100) * kMsPerHour + positiveMod(v, 100) * kMsPerMinute);
                }
                hasZone = true;
                continue;
            }
            if (digit(c)) {
                double v = 0;
                while (digit(peek())) {
                    v = v * 10 + double(peek() - u'0');
                    ++at;
                }
                if (peek() == u':') {
                    hour = v;
                    ++at;
                    if (!digits(2, minute))
                        return kNaN;
                    if (peek() == u':') {
                        ++at;
                        if (!digits(2, second))
                            return kNaN;
                        if (peek() == u'.') {
                            ++at;
                            double scale = 100;
                            while (digit(peek())) {
                                ms += double(peek() - u'0') * scale;
                                scale /= 10;
                                ++at;
                            }
                            ms = std::floor(ms);
                        }
                    }
                    continue;
                }
                numbers.push_back(v);
                continue;
            }
            if (alpha(c)) {
                std::string word;
                while (alpha(peek())) {
                    word += char(peek() | 0x20);
                    ++at;
                }
                if (word == "am" || word == "pm") {
                    (word == "am" ? am : pm) = true;
                    continue;
                }
                if (word == "z" || word == "gmt" || word == "utc" || word == "ut") {
                    hasZone = true;
                    continue;
                }
                bool known = false;
                for (int m = 0; m < 12; ++m) {
                    std::string name = kMonths[m];
                    name[0] = char(name[0] | 0x20);
                    if (word.size() >= 3 && word.compare(0, 3, name) == 0) {
                        month = m;
                        known = true;
                    }
                }
                for (const char* wd : kWeekDays) {
                    std::string name = wd;
                    name[0] = char(name[0] | 0x20);
                    if (word.size() >= 3 && word.compare(0, 3, name) == 0)
                        known = true;
                }
                if (!known)
                    return kNaN;
                continue;
            }
            return kNaN;
        }
        if (!std::isnan(month)) {
            // `Feb 01 2022` or `01 Feb 2022`: the day and the year, a year being what does not fit a day.
            if (numbers.size() < 2)
                return kNaN;
            if (numbers[0] > 31) {
                year = numbers[0];
                dayOfMonth = numbers[1];
            } else {
                dayOfMonth = numbers[0];
                year = numbers[1];
            }
        } else if (numbers.size() >= 3) {
            if (numbers[0] > 31) {  // 2022/02/01
                year = numbers[0];
                month = numbers[1] - 1;
                dayOfMonth = numbers[2];
            } else {  // 02/01/2022, as en-US writes it
                month = numbers[0] - 1;
                dayOfMonth = numbers[1];
                year = numbers[2];
            }
        } else {
            return kNaN;
        }
        if (year < 50 && numbers.size() && year == numbers.back())
            year += 2000;
        else if (year < 100)
            year += 1900;
        if (pm && hour < 12)
            hour += 12;
        if (am && hour == 12)
            hour = 0;
        if (month < 0 || month > 11 || dayOfMonth < 1 || dayOfMonth > daysInMonth(year, month) || hour > 24 || minute > 59 ||
            second > 59)
            return kNaN;
        const double tv = makeDate(makeDay(year, month, dayOfMonth), makeTime(hour, minute, second, ms));
        return timeClip(hasZone ? tv - offset : utcOf(tv));
    }
};

double parseDate(StringView text)
{
    DateParser p{text};
    const double iso = p.iso();
    if (!std::isnan(iso))
        return iso;
    return p.loose();
}


}  // namespace datecalc
}  // namespace dyn
}  // namespace internal
}  // namespace script
