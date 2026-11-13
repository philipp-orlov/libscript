// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Date over the calendar arithmetic the engines share (src/dyn/datecalc.cpp).
#include <cmath>

#include "script/internal/dyn/datecalc.hpp"
#include "builtins_util.hpp"

namespace script {
namespace internal {
namespace vm {
namespace bi {

namespace dc = dyn::datecalc;

namespace {

struct DateState : NativeState
{
    double tv = dc::kNaN;
};

DateState* stateOf(JSValue v)
{
    if (isObject(v) && asObject(v)->cls() == ObjClass::Date)
        if (Rare* r = asObject(v)->rare())
            return static_cast<DateState*>(r->ext);
    return nullptr;
}

DateState* thisDate(VM& vm, JSValue self)
{
    if (DateState* d = stateOf(self))
        return d;
    typeError(vm, "this is not a Date object.");
    return nullptr;
}

JSValue text(VM& vm, const std::string& ascii) { return makeString(vm, std::string_view(ascii)); }

bool numberOf(VM& vm, const JSValue* args, std::uint32_t argc, std::uint32_t i, double* out)
{
    return toNumber(vm, arg(args, argc, i), out);
}

/// The time value of `new Date(y, m, ...)` and `Date.UTC(...)`, before the zone: false with an exception pending.
bool fromComponents(VM& vm, const JSValue* args, std::uint32_t argc, double* out)
{
    double parts[7] = {dc::kNaN, 0, 1, 0, 0, 0, 0};
    for (std::uint32_t i = 0; i < 7; ++i)
        if (i < argc && !toNumber(vm, args[i], &parts[i]))
            return false;
    double year = parts[0];
    if (!std::isnan(year)) {
        const double yi = dc::integerOrInfinity(year);
        if (yi >= 0 && yi <= 99)
            year = 1900 + yi;
    }
    *out = dc::makeDate(dc::makeDay(year, parts[1], parts[2]), dc::makeTime(parts[3], parts[4], parts[5], parts[6]));
    return true;
}

NATIVE(dateCtor)
{
    UNUSED();
    if (vm.newTarget.isUndefined() || !isObject(self))
        return text(vm, dc::toDateText(dc::nowMillis()));
    auto* state = new DateState();
    JSObject* o = asObject(self);
    o->h.cls = std::uint8_t(ObjClass::Date);
    delete o->rareOf(vm).ext;
    o->rareOf(vm).ext = state;
    if (argc == 0) {
        state->tv = dc::timeClip(dc::nowMillis());
    } else if (argc == 1) {
        if (DateState* other = stateOf(args[0])) {
            state->tv = other->tv;
        } else {
            JSValue prim = toPrimitiveHint(vm, args[0], 0);
            if (prim.isException())
                return prim;
            if (isString(prim)) {
                state->tv = dc::parseDate(asString(prim)->text());
            } else {
                double d;
                const bool ok = toNumber(vm, prim, &d);
                if (!ok) {
                    release(prim);
                    return JSValue::exception();
                }
                state->tv = dc::timeClip(d);
            }
            release(prim);
        }
    } else {
        double local;
        if (!fromComponents(vm, args, argc, &local))
            return JSValue::exception();
        state->tv = dc::timeClip(dc::utcOf(local));
    }
    retain(self);
    return self;
}

NATIVE(dateNowFn)
{
    UNUSED();
    return JSValue::boxNumber(dc::nowMillis());
}

NATIVE(dateParse)
{
    UNUSED();
    JSValue s = toStringValue(vm, arg(args, argc, 0));
    if (s.isException())
        return s;
    const double d = dc::parseDate(asString(s)->text());
    release(s);
    return JSValue::boxNumber(d);
}

NATIVE(dateUTC)
{
    UNUSED();
    double tv;
    if (!fromComponents(vm, args, argc, &tv))
        return JSValue::exception();
    return JSValue::boxNumber(dc::timeClip(tv));
}

NATIVE(dateGetTime)
{
    UNUSED();
    DateState* d = thisDate(vm, self);
    if (!d)
        return JSValue::exception();
    return JSValue::boxNumber(d->tv);
}

NATIVE(dateTimezoneOffset)
{
    UNUSED();
    DateState* d = thisDate(vm, self);
    if (!d)
        return JSValue::exception();
    return JSValue::boxNumber(std::isnan(d->tv) ? dc::kNaN : (d->tv - dc::localTime(d->tv)) / dc::kMsPerMinute);
}

using Part = double (*)(double);

NATIVE(dateGetter)
{
    UNUSED();
    DateState* d = thisDate(vm, self);
    if (!d)
        return JSValue::exception();
    if (std::isnan(d->tv))
        return JSValue::boxNumber(dc::kNaN);
    // data: the function, with the low bit set for local time.
    const auto bits = reinterpret_cast<std::uintptr_t>(data);
    const bool local = bits & 1;
    Part part = reinterpret_cast<Part>(bits & ~std::uintptr_t(1));
    return JSValue::boxNumber(part(local ? dc::localTime(d->tv) : d->tv));
}

NATIVE(dateGetYear)
{
    UNUSED();
    DateState* d = thisDate(vm, self);
    if (!d)
        return JSValue::exception();
    return JSValue::boxNumber(std::isnan(d->tv) ? dc::kNaN : dc::yearFromTime(dc::localTime(d->tv)) - 1900);
}

NATIVE(dateSetter)
{
    UNUSED();
    DateState* d = thisDate(vm, self);
    if (!d)
        return JSValue::exception();
    // data: first field (0 ms .. 6 year) | most arguments << 4 | local << 8
    const auto bits = reinterpret_cast<std::uintptr_t>(data);
    const int first = int(bits & 15), most = int((bits >> 4) & 15);
    const bool local = (bits >> 8) & 1;
    double tv = d->tv;
    double given[4] = {0, 0, 0, 0};
    const int count = int(std::min<std::uint32_t>(std::max<std::uint32_t>(argc, 1), std::uint32_t(most)));
    for (int i = 0; i < count; ++i)
        if (!numberOf(vm, args, argc, std::uint32_t(i), &given[i]))
            return JSValue::exception();
    if (std::isnan(tv)) {
        if (first != 6)
            return JSValue::boxNumber(dc::kNaN);
        tv = 0;
    }
    const double t = local ? dc::localTime(tv) : tv;
    double fields[7] = {dc::yearFromTime(t), dc::monthFromTime(t), dc::dateFromTime(t), dc::hourFromTime(t),
                        dc::minFromTime(t),  dc::secFromTime(t),   dc::msFromTime(t)};
    const int start = 6 - first;
    for (int i = 0; i < count; ++i)
        fields[start + i] = given[i];
    double result;
    if (first >= 4)
        result = dc::makeDate(dc::makeDay(fields[0], fields[1], fields[2]), dc::timeWithinDay(t));
    else
        result = dc::makeDate(dc::day(t), dc::makeTime(fields[3], fields[4], fields[5], fields[6]));
    d->tv = dc::timeClip(local ? dc::utcOf(result) : result);
    return JSValue::boxNumber(d->tv);
}

NATIVE(dateSetYear)
{
    UNUSED();
    DateState* d = thisDate(vm, self);
    if (!d)
        return JSValue::exception();
    double y;
    if (!numberOf(vm, args, argc, 0, &y))
        return JSValue::exception();
    if (std::isnan(y)) {
        d->tv = dc::kNaN;
        return JSValue::boxNumber(d->tv);
    }
    const double yi = std::trunc(y);
    const double year = yi >= 0 && yi <= 99 ? yi + 1900 : y;
    const double t = std::isnan(d->tv) ? 0 : dc::localTime(d->tv);
    const double day = dc::makeDay(year, dc::monthFromTime(t), dc::dateFromTime(t));
    d->tv = dc::timeClip(dc::utcOf(dc::makeDate(day, dc::timeWithinDay(t))));
    return JSValue::boxNumber(d->tv);
}

NATIVE(dateSetTime)
{
    UNUSED();
    DateState* d = thisDate(vm, self);
    if (!d)
        return JSValue::exception();
    double v;
    if (!numberOf(vm, args, argc, 0, &v))
        return JSValue::exception();
    d->tv = dc::timeClip(v);
    return JSValue::boxNumber(d->tv);
}

NATIVE(dateFormat)
{
    UNUSED();
    DateState* d = thisDate(vm, self);
    if (!d)
        return JSValue::exception();
    const int which = int(reinterpret_cast<std::intptr_t>(data));
    // 0 toString, 1 toDateString, 2 toTimeString, 3 toUTCString, 4 toISOString, 5 toLocaleString, 6 toLocaleDateString,
    // 7 toLocaleTimeString
    const double tv = d->tv;
    if (which == 4) {
        if (!std::isfinite(tv))
            return vm.throwError(ErrorCode::RangeError, "Invalid time value");
        return text(vm, dc::isoString(tv));
    }
    if (std::isnan(tv))
        return text(vm, "Invalid Date");
    switch (which) {
        case 0: return text(vm, dc::toDateText(tv));
        case 1: return text(vm, dc::dateString(dc::localTime(tv)));
        case 2: return text(vm, dc::timeString(dc::localTime(tv)) + dc::timeZoneString(tv));
        case 3: return text(vm, dc::utcString(tv));
        case 5: return text(vm, dc::localeDate(dc::localTime(tv)) + ", " + dc::localeTime(dc::localTime(tv)));
        case 6: return text(vm, dc::localeDate(dc::localTime(tv)));
        default: return text(vm, dc::localeTime(dc::localTime(tv)));
    }
}

NATIVE(dateToJSON)
{
    UNUSED();
    JSValue tv = toPrimitive(vm, self, false);
    if (tv.isException())
        return tv;
    const bool bad = tv.isNumber() && !std::isfinite(tv.asNumber());
    release(tv);
    if (bad)
        return JSValue::null();
    Key k = vm.internKey(std::string_view("toISOString"));
    JSValue f = getProperty(vm, self, k);
    detail::decRef(k);
    if (f.isException())
        return f;
    Handle keep = Handle::adopt(f);
    return vm.call(f, self, nullptr, 0);
}

NATIVE(dateToPrimitive)
{
    UNUSED();
    if (!isObject(self))
        return typeError(vm, "Date.prototype[Symbol.toPrimitive] called on something not an object");
    const JSValue hint = arg(args, argc, 0);
    bool asNumber;
    if (isString(hint) && asString(hint)->text() == u"number")
        asNumber = true;
    else if (isString(hint) && (asString(hint)->text() == u"string" || asString(hint)->text() == u"default"))
        asNumber = false;
    else
        return typeError(vm, "invalid hint for Date.prototype[Symbol.toPrimitive]");
    for (const char* name : {asNumber ? "valueOf" : "toString", asNumber ? "toString" : "valueOf"}) {
        Key k = vm.internKey(std::string_view(name));
        JSValue f = getProperty(vm, self, k);
        detail::decRef(k);
        if (f.isException())
            return f;
        Handle keep = Handle::adopt(f);
        if (!isFunction(f))
            continue;
        JSValue r = vm.call(f, self, nullptr, 0);
        if (r.isException() || !isObject(r))
            return r;
        release(r);
    }
    return typeError(vm, "cannot convert the date to a primitive value");
}

}  // namespace

void installDate(VM& vm)
{
    JSObject* proto = vm.newObject(vm.protos.object);
    JSFunction* ctor = constructor(vm, "Date", dateCtor, 7, proto);
    method(vm, ctor, "now", dateNowFn, 0);
    method(vm, ctor, "parse", dateParse, 1);
    method(vm, ctor, "UTC", dateUTC, 7);
    method(vm, proto, "getTime", dateGetTime, 0);
    method(vm, proto, "valueOf", dateGetTime, 0);
    method(vm, proto, "getTimezoneOffset", dateTimezoneOffset, 0);
    method(vm, proto, "getYear", dateGetYear, 0);
    method(vm, proto, "setYear", dateSetYear, 1);
    struct Get
    {
        const char* name;
        Part part;
    };
    for (const Get& g : {Get{"FullYear", dc::yearFromTime}, Get{"Month", dc::monthFromTime}, Get{"Date", dc::dateFromTime},
                         Get{"Day", dc::weekDay}, Get{"Hours", dc::hourFromTime}, Get{"Minutes", dc::minFromTime},
                         Get{"Seconds", dc::secFromTime}, Get{"Milliseconds", dc::msFromTime}}) {
        method(vm, proto, (std::string("get") + g.name).c_str(), dateGetter, 0,
               reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(g.part) | 1));
        method(vm, proto, (std::string("getUTC") + g.name).c_str(), dateGetter, 0, reinterpret_cast<void*>(g.part));
    }
    struct Set
    {
        const char* name;
        int first, most;
    };
    for (const Set& s : {Set{"Milliseconds", 0, 1}, Set{"Seconds", 1, 2}, Set{"Minutes", 2, 3}, Set{"Hours", 3, 4},
                         Set{"Date", 4, 1}, Set{"Month", 5, 2}, Set{"FullYear", 6, 3}}) {
        method(vm, proto, (std::string("set") + s.name).c_str(), dateSetter, s.most,
               reinterpret_cast<void*>(std::uintptr_t(s.first | (s.most << 4) | (1 << 8))));
        method(vm, proto, (std::string("setUTC") + s.name).c_str(), dateSetter, s.most,
               reinterpret_cast<void*>(std::uintptr_t(s.first | (s.most << 4))));
    }
    method(vm, proto, "setTime", dateSetTime, 1);
    struct Fmt
    {
        const char* name;
        int which;
    };
    for (const Fmt& f : {Fmt{"toString", 0}, Fmt{"toDateString", 1}, Fmt{"toTimeString", 2}, Fmt{"toUTCString", 3},
                         Fmt{"toISOString", 4}, Fmt{"toLocaleString", 5}, Fmt{"toLocaleDateString", 6},
                         Fmt{"toLocaleTimeString", 7}})
        method(vm, proto, f.name, dateFormat, 0, reinterpret_cast<void*>(std::intptr_t(f.which)));
    {
        Key from = vm.internKey(std::string_view("toUTCString"));
        Key to = vm.internKey(std::string_view("toGMTString"));
        if (JSValue* f = proto->findOwn(vm, from)) {
            retain(*f);
            proto->putOwn(vm, to, *f, attr::kWritable | attr::kConfigurable);
        }
        detail::decRef(from);
        detail::decRef(to);
    }
    method(vm, proto, "toJSON", dateToJSON, 1);
    symbolMethod(vm, proto, vm.atoms.symToPrimitive, "[Symbol.toPrimitive]", dateToPrimitive, 1);
    release(toJSValue(proto));
}

}  // namespace bi
}  // namespace vm
}  // namespace internal
}  // namespace script
