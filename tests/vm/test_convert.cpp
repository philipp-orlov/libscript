// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// V-2, V-10, V-12: Value <-> JSValue, symbols, bigints and strings.
#include "fixture.hpp"
#include "script/internal/dyn/object.hpp"

using namespace vmtest;
using script::Type;
using script::Value;

TEST(everyTypeRoundTrips)
{
    Fixture f;
    VM& vm = f.vm;
    auto trip = [&](const Value& v) {
        JSValue j = fromValue(vm, v);
        Value back = toValue(vm, j);
        release(j);
        return back;
    };
    CHECK(trip(Value::undefined()).isUndefined());
    CHECK(trip(Value::null()).isNull());
    CHECK(trip(Value(true)).isBool() && trip(Value(true)).toBool());
    CHECK(!trip(Value(false)).toBool());
    CHECK_EQ(trip(Value(42)).toInt(), 42);
    CHECK_EQ(trip(Value(-7)).toInt(), -7);
    CHECK_EQ(trip(Value(char16_t(65))).toInt(), 65);  // a Char is a number on the way in
    CHECK_EQ(trip(Value(1.5f)).toDouble(), 1.5);
    CHECK_EQ(trip(Value(2.25)).toDouble(), 2.25);
    CHECK(trip(Value(2.25)).type() == Type::Double);
    CHECK_EQ(trip(Value(3.0)).toDouble(), 3.0);  // numbers come back as doubles, as the old core's do
    CHECK(std::signbit(trip(Value(-0.0)).toDouble()));
    CHECK(std::isnan(trip(Value(std::nan(""))).toDouble()));
    CHECK_EQ(trip(Value(std::int64_t(5))).toDouble(), 5.0);
    CHECK_EQ(trip(Value(std::int64_t(1) << 53)).toDouble(), 9007199254740992.0);
    // Past 2^53 a long becomes the nearest double.
    CHECK_EQ(trip(Value((std::int64_t(1) << 53) + 1)).toDouble(), 9007199254740992.0);
    CHECK_EQ(trip(Value(std::int64_t(-9223372036854775807ll - 1))).toDouble(), -9223372036854775808.0);
    // Strings keep their identity.
    Value s = f.ctx.impl()->str("hello");
    Value s2 = trip(s);
    CHECK(s2.type() == Type::String && s2.sameReference(s));
    CHECK_EQ(s2.toString(), std::string("hello"));
}

TEST(aCellOfTheNewFamilyIsAnObject)
{
    Fixture f;
    JSObject* o = f.vm.newObject(static_cast<JSObject*>(nullptr));
    o->putOwn(f.vm, f.key("a"), num(1));
    Value v = toValue(f.vm, toJSValue(o));  // adds a count
    CHECK(v.type() == Type::Object);
    CHECK_EQ(headerOf(reinterpret_cast<CellPtr>(o))->rc, std::uint32_t(2));
    release(toJSValue(o));
    CHECK_EQ(f.cells(), std::uint64_t(1));
    JSValue back = fromValue(f.vm, v);
    CHECK(back == toJSValue(o));
    release(back);
    v = Value();
    CHECK_EQ(f.cells(), std::uint64_t(0));  // the Value's destructor went through the hook
}

TEST(symbolsAndBigIntsOfTheOldFamilyCount)
{
    Fixture f;
    ContextImpl& rt = *f.ctx.impl();
    auto* sym = rt.newCell<dyn::SymbolCell>();
    auto* big = rt.newCell<dyn::BigIntCell>();
    JSValue s = JSValue::cell(sym), b = JSValue::cell(big);
    CHECK(isSymbol(s) && !isBigInt(s) && !isString(s));
    CHECK(isBigInt(b) && !isSymbol(b));
    CHECK(typeOfCell(sym) == Type::Symbol);
    CHECK(typeOfCell(big) == Type::BigInt);
    retain(s);
    CHECK_EQ(sym->rc, std::uint32_t(2));
    release(s);
    Value sv = toValue(f.vm, s);
    CHECK(sv.type() == Type::Symbol);
    Key key = toPropertyKey(f.vm, s);
    CHECK(key == sym);
    release(JSValue::cell(key));
    // As a property key, and held by an object, it counts.
    JSObject* o = f.vm.newObject(static_cast<JSObject*>(nullptr));
    retain(b);
    o->putOwn(f.vm, sym, b);
    CHECK_EQ(big->rc, std::uint32_t(2));
    release(toJSValue(o));
    CHECK_EQ(big->rc, std::uint32_t(1));
    sv = Value();
    release(s);
    release(b);
}

TEST(propertyKeysFromEveryKindOfValue)
{
    Fixture f;
    VM& vm = f.vm;
    auto text = [&](JSValue v) {
        Key k = toPropertyKey(vm, v);
        CHECK(k != nullptr);
        std::string out = toUtf8(static_cast<StringCell*>(k)->text());
        release(JSValue::cell(k));
        return out;
    };
    CHECK_EQ(text(JSValue::int32(0)), std::string("0"));
    CHECK_EQ(text(JSValue::int32(1023)), std::string("1023"));
    CHECK_EQ(text(JSValue::int32(1024)), std::string("1024"));
    CHECK_EQ(text(JSValue::int32(-5)), std::string("-5"));
    CHECK_EQ(text(JSValue::boxDouble(1.5)), std::string("1.5"));
    CHECK_EQ(text(JSValue::boxDouble(-0.0)), std::string("0"));
    CHECK_EQ(text(JSValue::boolean(true)), std::string("true"));
    CHECK_EQ(text(JSValue::null()), std::string("null"));
    CHECK_EQ(text(JSValue::undefined()), std::string("undefined"));
    // An integer key is the same cell as the string of its digits, so a lookup by either finds the same property.
    Key viaInt = toPropertyKey(vm, JSValue::int32(77));
    Key viaString = toPropertyKey(vm, JSValue::cell(f.ctx.impl()->intern(u"77")));
    CHECK(viaInt == viaString);
    // A string that is not interned yet maps to the interned one.
    StringCell* loose = f.ctx.impl()->newString(u"looseName");
    Key k = toPropertyKey(vm, JSValue::cell(loose));
    CHECK(k != loose && k->kind == script::detail::CellKind::String);
    CHECK(static_cast<StringCell*>(k)->interned);
    CHECK(k == toPropertyKey(vm, JSValue::cell(f.ctx.impl()->intern(u"looseName"))));
    // An object is not a key.
    JSObject* o = f.vm.newObject(static_cast<JSObject*>(nullptr));
    CHECK(toPropertyKey(vm, toJSValue(o)) == nullptr);
    CHECK(!vm.exception.isUndefined());
    vm.clearException();
    release(toJSValue(o));
}

TEST(concatenationAndComparison)
{
    Fixture f;
    JSValue a = JSValue::cell(f.ctx.impl()->intern(u"abc"));
    JSValue b = JSValue::cell(f.ctx.impl()->intern(u"abd"));
    CHECK(compareStrings(a, b) < 0);
    CHECK(compareStrings(b, a) > 0);
    CHECK_EQ(compareStrings(a, a), 0);
    CHECK(stringsEqual(a, a) && !stringsEqual(a, b));
    JSValue joined = concatStrings(f.vm, a, b);
    CHECK(isString(joined));
    CHECK(asString(joined)->text() == StringView(u"abcabd"));
    release(joined);
}
