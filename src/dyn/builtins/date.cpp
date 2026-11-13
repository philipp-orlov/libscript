// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Date: a time value, milliseconds since 1970 in UTC, and the calendar arithmetic of ECMAScript's section 21.4 over it.
// Local time is the host's time zone, asked of the C library for each instant, so daylight saving time is right for
// the dates it applies to. Without Intl the `toLocale...` forms are those of en-US.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <string>

#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/datecalc.hpp"
#include "script/internal/dyn/numconv.hpp"
#include "script/internal/dyn/vm.hpp"

namespace script {
namespace internal {
namespace dyn {

using namespace datecalc;

namespace {

// ---- the methods ----

DynWrapper* thisDateObject(CallInfo& ci)
{
    DynObject* o = asObject(ci.thisValue());
    if (!o || o->cls != ObjClass::Date)
        ci.fail(ErrorCode::TypeError, "this is not a Date");
    return static_cast<DynWrapper*>(o);
}

double thisTime(CallInfo& ci) { return numberOf(thisDateObject(ci)->primitive); }

double argNumber(CallInfo& ci, std::size_t i) { return toNumber(rtOf(ci), ci.arg(i)); }

Value text(Runtime& rt, const std::string& ascii)
{
    return rt.str(fromUtf8(ascii));
}

Value setTimeValue(DynWrapper* d, double tv)
{
    d->primitive = Value(tv);
    return Value(tv);
}

Value newDate(Runtime& rt, double tv)
{
    auto* d = rt.newCell<DynWrapper>();
    d->cls = ObjClass::Date;
    d->setProto(rt.dateProto.get());
    d->primitive = Value(tv);
    return detail::Access::adopt(Type::Object, d);
}

/// The time value of `y, m[, d[, h[, min[, s[, ms]]]]]` in local time or UTC, each converted in order.
double fromComponents(CallInfo& ci)
{
    double parts[7] = {kNaN, 0, 1, 0, 0, 0, 0};
    for (std::size_t i = 0; i < 7; ++i) {
        if (i < ci.size())
            parts[i] = argNumber(ci, i);
    }
    double year = parts[0];
    if (!std::isnan(year)) {
        const double yi = integerOrInfinity(year);
        if (yi >= 0 && yi <= 99)
            year = 1900 + yi;
    }
    return makeDate(makeDay(year, parts[1], parts[2]), makeTime(parts[3], parts[4], parts[5], parts[6]));
}

using Getter = double (*)(double);

void getter(Runtime& rt, DynObject* proto, const char* name, Getter part, bool local)
{
    rt.method(proto, name, [part, local](CallInfo& ci) {
        const double tv = thisTime(ci);
        if (std::isnan(tv))
            return Value(kNaN);
        return Value(part(local ? localTime(tv) : tv));
    }, 0);
}

/// The setters: `first` is the first field the arguments set (0 ms, 1 seconds, 2 minutes, 3 hours, 4 date, 5 month,
/// 6 year), `most` how many arguments they take. Each argument is converted before the time value is looked at.
void setter(Runtime& rt, DynObject* proto, const char* name, int first, int most, bool local)
{
    rt.method(proto, name, [first, most, local](CallInfo& ci) {
        DynWrapper* d = thisDateObject(ci);
        double tv = numberOf(d->primitive);
        double given[4];
        const int count = int(std::min<std::size_t>(std::max<std::size_t>(ci.size(), 1), std::size_t(most)));
        for (int i = 0; i < count; ++i)
            given[i] = argNumber(ci, std::size_t(i));
        // Only a year can be set on an invalid date: it starts from +0 then.
        if (std::isnan(tv)) {
            if (first != 6)
                return Value(kNaN);
            tv = 0;
        }
        const double t = local ? localTime(tv) : tv;
        // Fields from the largest down: year, month, date, hours, minutes, seconds, milliseconds.
        double fields[7] = {yearFromTime(t), monthFromTime(t), dateFromTime(t), hourFromTime(t), minFromTime(t), secFromTime(t), msFromTime(t)};
        const int start = 6 - first;  // index in fields of the first one set
        for (int i = 0; i < count; ++i)
            fields[start + i] = given[i];
        double result;
        if (first >= 4) {
            result = makeDate(makeDay(fields[0], fields[1], fields[2]), timeWithinDay(t));
        } else {
            result = makeDate(day(t), makeTime(fields[3], fields[4], fields[5], fields[6]));
        }
        return setTimeValue(d, timeClip(local ? utcOf(result) : result));
    }, most);
}

}  // namespace

void installDate(Runtime& rt)
{
    DynObject* proto = rt.newObject(rt.objectProto.get());
    rt.dateProto.reset(proto, true);

    DynFunction* ctor = rt.makeConstructor("Date", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        if (!ci.isConstructorCall())
            return text(rt2, toDateText(nowMillis()));
        if (ci.size() == 0)
            return newDate(rt2, timeClip(nowMillis()));
        if (ci.size() == 1) {
            DynObject* other = asObject(ci.arg(0));
            if (other && other->cls == ObjClass::Date)
                return newDate(rt2, numberOf(static_cast<DynWrapper*>(other)->primitive));
            const Value v = toPrimitive(rt2, ci.arg(0), 0);
            if (v.isString())
                return newDate(rt2, parseDate(asString(v)->text()));
            return newDate(rt2, timeClip(toNumber(rt2, v)));
        }
        return newDate(rt2, timeClip(utcOf(fromComponents(ci))));
    }, 7, proto);

    rt.method(ctor, "now", [](CallInfo&) { return Value(nowMillis()); }, 0);
    rt.method(ctor, "parse", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        return Value(parseDate(toStringValue(rt2, ci.arg(0))));
    }, 1);
    rt.method(ctor, "UTC", [](CallInfo& ci) { return Value(timeClip(fromComponents(ci))); }, 7);

    rt.method(proto, "getTime", [](CallInfo& ci) { return Value(thisTime(ci)); }, 0);
    rt.method(proto, "valueOf", [](CallInfo& ci) { return Value(thisTime(ci)); }, 0);
    rt.method(proto, "getTimezoneOffset", [](CallInfo& ci) {
        const double tv = thisTime(ci);
        return Value(std::isnan(tv) ? kNaN : (tv - localTime(tv)) / kMsPerMinute);
    }, 0);

    getter(rt, proto, "getFullYear", yearFromTime, true);
    getter(rt, proto, "getMonth", monthFromTime, true);
    getter(rt, proto, "getDate", dateFromTime, true);
    getter(rt, proto, "getDay", weekDay, true);
    getter(rt, proto, "getHours", hourFromTime, true);
    getter(rt, proto, "getMinutes", minFromTime, true);
    getter(rt, proto, "getSeconds", secFromTime, true);
    getter(rt, proto, "getMilliseconds", msFromTime, true);
    getter(rt, proto, "getUTCFullYear", yearFromTime, false);
    getter(rt, proto, "getUTCMonth", monthFromTime, false);
    getter(rt, proto, "getUTCDate", dateFromTime, false);
    getter(rt, proto, "getUTCDay", weekDay, false);
    getter(rt, proto, "getUTCHours", hourFromTime, false);
    getter(rt, proto, "getUTCMinutes", minFromTime, false);
    getter(rt, proto, "getUTCSeconds", secFromTime, false);
    getter(rt, proto, "getUTCMilliseconds", msFromTime, false);
    rt.method(proto, "getYear", [](CallInfo& ci) {
        const double tv = thisTime(ci);
        return Value(std::isnan(tv) ? kNaN : yearFromTime(localTime(tv)) - 1900);
    }, 0);

    setter(rt, proto, "setMilliseconds", 0, 1, true);
    setter(rt, proto, "setSeconds", 1, 2, true);
    setter(rt, proto, "setMinutes", 2, 3, true);
    setter(rt, proto, "setHours", 3, 4, true);
    setter(rt, proto, "setDate", 4, 1, true);
    setter(rt, proto, "setMonth", 5, 2, true);
    setter(rt, proto, "setFullYear", 6, 3, true);
    setter(rt, proto, "setUTCMilliseconds", 0, 1, false);
    setter(rt, proto, "setUTCSeconds", 1, 2, false);
    setter(rt, proto, "setUTCMinutes", 2, 3, false);
    setter(rt, proto, "setUTCHours", 3, 4, false);
    setter(rt, proto, "setUTCDate", 4, 1, false);
    setter(rt, proto, "setUTCMonth", 5, 2, false);
    setter(rt, proto, "setUTCFullYear", 6, 3, false);
    rt.method(proto, "setTime", [](CallInfo& ci) {
        DynWrapper* d = thisDateObject(ci);
        return setTimeValue(d, timeClip(argNumber(ci, 0)));
    }, 1);
    rt.method(proto, "setYear", [](CallInfo& ci) {
        DynWrapper* d = thisDateObject(ci);
        const double y = argNumber(ci, 0);
        if (std::isnan(y))
            return setTimeValue(d, kNaN);
        double year = integerOrInfinity(y);
        if (year >= 0 && year <= 99)
            year += 1900;
        const double tv = numberOf(d->primitive);
        const double t = std::isnan(tv) ? 0 : localTime(tv);
        const double date = makeDate(makeDay(year, monthFromTime(t), dateFromTime(t)), timeWithinDay(t));
        return setTimeValue(d, timeClip(utcOf(date)));
    }, 1);

    rt.method(proto, "toString", [](CallInfo& ci) { return text(rtOf(ci), toDateText(thisTime(ci))); }, 0);
    rt.method(proto, "toDateString", [](CallInfo& ci) {
        const double tv = thisTime(ci);
        return text(rtOf(ci), std::isnan(tv) ? std::string("Invalid Date") : dateString(localTime(tv)));
    }, 0);
    rt.method(proto, "toTimeString", [](CallInfo& ci) {
        const double tv = thisTime(ci);
        return text(rtOf(ci), std::isnan(tv) ? std::string("Invalid Date") : timeString(localTime(tv)) + timeZoneString(tv));
    }, 0);
    rt.method(proto, "toUTCString", [](CallInfo& ci) {
        const double tv = thisTime(ci);
        return text(rtOf(ci), std::isnan(tv) ? std::string("Invalid Date") : utcString(tv));
    }, 0);
    // Annex B: the same function under its old name.
    proto->putOwn(rt.intern("toGMTString"), proto->findOwn(rt.intern("toUTCString"))->value, PropWritable | PropConfigurable);
    rt.method(proto, "toISOString", [](CallInfo& ci) {
        const double tv = thisTime(ci);
        if (!std::isfinite(tv))
            ci.fail(ErrorCode::RangeError, "Invalid time value");
        return text(rtOf(ci), isoString(tv));
    }, 0);
    rt.method(proto, "toLocaleString", [](CallInfo& ci) {
        const double tv = thisTime(ci);
        return text(rtOf(ci), std::isnan(tv) ? std::string("Invalid Date") : localeDate(localTime(tv)) + ", " + localeTime(localTime(tv)));
    }, 0);
    rt.method(proto, "toLocaleDateString", [](CallInfo& ci) {
        const double tv = thisTime(ci);
        return text(rtOf(ci), std::isnan(tv) ? std::string("Invalid Date") : localeDate(localTime(tv)));
    }, 0);
    rt.method(proto, "toLocaleTimeString", [](CallInfo& ci) {
        const double tv = thisTime(ci);
        return text(rtOf(ci), std::isnan(tv) ? std::string("Invalid Date") : localeTime(localTime(tv)));
    }, 0);
    rt.method(proto, "toJSON", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        const Value object = toObject(rt2, ci.thisValue());
        const Value tv = toPrimitive(rt2, object, 1);
        if (tv.isNumber() && !std::isfinite(numberOf(tv)))
            return Value::null();
        return invokeMethod(rt2, object, rt2.intern("toISOString"), {});
    }, 1);
    // A date converts to a string unless asked for a number: `date + ""` is its text.
    proto->putOwn(rt.sym.toPrimitive.get(), rt.makeFunction(ascii("[Symbol.toPrimitive]"), [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynObject* self = asObject(ci.thisValue());
        if (!self)
            ci.fail(ErrorCode::TypeError, "Date.prototype[Symbol.toPrimitive] called on something not an object");
        const Value hint = ci.arg(0);
        const String name = hint.isString() ? String(asString(hint)->text()) : String();
        bool asNumber;
        if (name == u"number")
            asNumber = true;
        else if (name == u"string" || name == u"default")
            asNumber = false;
        else
            ci.fail(ErrorCode::TypeError, "invalid hint for Date.prototype[Symbol.toPrimitive]");
        GCObject* first = asNumber ? rt2.atom.valueOf.get() : rt2.atom.toString.get();
        GCObject* second = asNumber ? rt2.atom.toString.get() : rt2.atom.valueOf.get();
        for (GCObject* method : {first, second}) {
            const Value fn = getProperty(rt2, ci.thisValue(), method);
            if (!asFunction(fn))
                continue;
            const Value r = callValue(rt2, fn, ci.thisValue(), {});
            if (!r.isObject())
                return r;
        }
        ci.fail(ErrorCode::TypeError, "cannot convert the date to a primitive value");
    }, 1), PropConfigurable);
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
