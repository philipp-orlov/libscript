// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// V-6: property access through the chain, accessors, primitives, arrays.
#include "fixture.hpp"

using namespace vmtest;

namespace {

JSObject* make(Fixture& f, JSObject* proto = nullptr)
{
    return f.vm.newObject(proto);
}

JSValue get(Fixture& f, JSObject* o, const char* name)
{
    return getProperty(f.vm, toJSValue(o), f.key(name));
}

std::int32_t getInt(Fixture& f, JSObject* o, const char* name)
{
    const JSValue v = get(f, o, name);
    CHECK(v.isInt32());
    return v.asInt32();
}

// An accessor's functions are native cells; the hook calls them directly.
JSValue hook(VM& vm, JSValue callee, JSValue self, const JSValue* args, std::uint32_t argc, void*)
{
    Rare* r = asObject(callee)->rare();
    return r->native(vm, self, args, argc, r->nativeData);
}

JSValue returnsSeven(VM&, JSValue, const JSValue*, std::uint32_t, void*)
{
    return JSValue::int32(7);
}

JSValue returnsThisX(VM& vm, JSValue self, const JSValue*, std::uint32_t, void* data)
{
    return getProperty(vm, self, static_cast<Key>(data));
}

JSValue throws(VM& vm, JSValue, const JSValue*, std::uint32_t, void*)
{
    return vm.throwError(script::ErrorCode::Exception, "boom");
}

JSValue storesArg(VM& vm, JSValue self, const JSValue* args, std::uint32_t argc, void* data)
{
    if (argc)
        setProperty(vm, self, static_cast<Key>(data), args[0]);
    return JSValue::undefined();
}

}  // namespace

TEST(readsFindThePropertyAtEveryDepthOfTheChain)
{
    for (int depth = 0; depth <= 5; ++depth) {
        Fixture f;
        std::vector<JSObject*> chain;
        JSObject* proto = nullptr;
        for (int i = 0; i <= depth; ++i) {
            proto = make(f, proto);
            chain.push_back(proto);
        }
        // chain.front() is the root, chain.back() the object read from.
        chain.front()->putOwn(f.vm, f.key("deep"), num(depth));
        CHECK_EQ(getInt(f, chain.back(), "deep"), depth);
        CHECK(get(f, chain.back(), "absent").isUndefined());
        CHECK(hasProperty(f.vm, toJSValue(chain.back()), f.key("deep")));
        CHECK(!hasProperty(f.vm, toJSValue(chain.back()), f.key("absent")));
        // A nearer property shadows.
        chain.back()->putOwn(f.vm, f.key("deep"), num(-1));
        CHECK_EQ(getInt(f, chain.back(), "deep"), -1);
        for (JSObject* o : chain)
            release(toJSValue(o));
    }
}

TEST(aWriteCreatesAnOwnPropertyAndLeavesThePrototypeAlone)
{
    Fixture f;
    JSObject* proto = make(f);
    JSObject* o = make(f, proto);
    proto->putOwn(f.vm, f.key("v"), num(1));
    CHECK(setProperty(f.vm, toJSValue(o), f.key("v"), num(2)));
    CHECK_EQ(getInt(f, o, "v"), 2);
    CHECK_EQ(getInt(f, proto, "v"), 1);
    CHECK(setProperty(f.vm, toJSValue(o), f.key("fresh"), num(3)));
    CHECK_EQ(getInt(f, o, "fresh"), 3);
    release(toJSValue(o));
    release(toJSValue(proto));
}

TEST(readOnlyAndNonExtensibleRefuseAStore)
{
    Fixture f;
    JSObject* proto = make(f);
    JSObject* o = make(f, proto);
    proto->putOwn(f.vm, f.key("ro"), num(1), attr::kEnumerable | attr::kConfigurable);
    CHECK(!setProperty(f.vm, toJSValue(o), f.key("ro"), num(2), /*strict=*/false));
    CHECK(f.vm.exception.isUndefined());
    CHECK(!setProperty(f.vm, toJSValue(o), f.key("ro"), num(2), /*strict=*/true));
    CHECK(!f.vm.exception.isUndefined());
    CHECK(f.vm.exceptionCode == script::ErrorCode::TypeError);
    f.vm.clearException();
    CHECK_EQ(getInt(f, o, "ro"), 1);
    o->h.flags |= cellflags::kNotExtensible;
    CHECK(!setProperty(f.vm, toJSValue(o), f.key("new"), num(1), false));
    CHECK(get(f, o, "new").isUndefined());
    release(toJSValue(o));
    release(toJSValue(proto));
}

TEST(accessorsRunThroughTheHookWithTheReceiverAsThis)
{
    Fixture f;
    f.vm.callHook = &hook;
    JSObject* proto = make(f);
    JSObject* o = make(f, proto);
    Key x = f.key("x");
    o->putOwn(f.vm, x, num(41));
    JSFunction* getter = f.vm.newNativeFunction(nullptr, &returnsThisX, x, f.key("g"), 0);
    JSFunction* setter = f.vm.newNativeFunction(nullptr, &storesArg, x, f.key("s"), 1);
    CHECK(defineAccessor(f.vm, proto, f.key("viaThis"), toJSValue(getter), JSValue::undefined(),
                         attr::kEnumerable | attr::kConfigurable));
    CHECK_EQ(getInt(f, o, "viaThis"), 41);  // `this` is the receiver, not the holder
    CHECK(!setProperty(f.vm, toJSValue(o), f.key("viaThis"), num(1), false));  // no setter
    CHECK(defineAccessor(f.vm, proto, f.key("pair"), toJSValue(getter), toJSValue(setter), attr::kEnumerable));
    CHECK(setProperty(f.vm, toJSValue(o), f.key("pair"), num(99)));
    CHECK_EQ(getInt(f, o, "x"), 99);
    CHECK_EQ(getInt(f, o, "pair"), 99);
    std::uint8_t attrs = 0;
    CHECK(proto->findOwn(f.vm, f.key("pair"), &attrs));
    CHECK(attrs & attr::kAccessor);
    CHECK(!(attrs & attr::kWritable));
    for (JSObject* x2 : {o, proto})
        release(toJSValue(x2));
    release(toJSValue(getter));
    release(toJSValue(setter));
}

TEST(aThrowingGetterGivesTheExceptionSentinel)
{
    Fixture f;
    f.vm.callHook = &hook;
    JSObject* o = make(f);
    JSFunction* bad = f.vm.newNativeFunction(nullptr, &throws, nullptr, f.key("bad"), 0);
    CHECK(defineAccessor(f.vm, o, f.key("p"), toJSValue(bad), JSValue::undefined(), attr::kConfigurable));
    const JSValue v = get(f, o, "p");
    CHECK(v.isException());
    CHECK(!f.vm.exception.isUndefined());
    f.vm.clearException();
    release(toJSValue(o));
    release(toJSValue(bad));
}

TEST(aGetterWithNoHookIsATypeError)
{
    Fixture f;
    JSObject* o = make(f);
    JSFunction* g = f.vm.newNativeFunction(nullptr, &returnsSeven, nullptr, f.key("g"), 0);
    CHECK(defineAccessor(f.vm, o, f.key("p"), toJSValue(g), JSValue::undefined(), attr::kConfigurable));
    CHECK(get(f, o, "p").isException());
    f.vm.clearException();
    release(toJSValue(o));
    release(toJSValue(g));
}

TEST(primitivesReadThroughTheirPrototypes)
{
    Fixture f;
    JSObject* numberProto = make(f);
    JSObject* stringProto = make(f);
    numberProto->putOwn(f.vm, f.key("kind"), num(1));
    stringProto->putOwn(f.vm, f.key("kind"), num(2));
    f.vm.setIntrinsic(&f.vm.protos.number, numberProto);
    f.vm.setIntrinsic(&f.vm.protos.string, stringProto);
    release(toJSValue(numberProto));
    release(toJSValue(stringProto));

    JSValue v = getProperty(f.vm, JSValue::int32(5), f.key("kind"));
    CHECK(v.isInt32() && v.asInt32() == 1);
    v = getProperty(f.vm, JSValue::boxDouble(0.5), f.key("kind"));
    CHECK(v.isInt32() && v.asInt32() == 1);
    JSValue text = JSValue::cell(f.ctx.impl()->intern(u"hello"));
    v = getProperty(f.vm, text, f.key("kind"));
    CHECK(v.isInt32() && v.asInt32() == 2);
    v = getProperty(f.vm, text, f.key("length"));
    CHECK(v.isInt32() && v.asInt32() == 5);
    v = getProperty(f.vm, text, f.key("1"));
    CHECK(isString(v));
    CHECK(asString(v)->text() == StringView(u"e"));
    release(v);
    CHECK(getProperty(f.vm, text, f.key("9")).isUndefined());
    // No prototype installed: nothing found.
    CHECK(getProperty(f.vm, JSValue::boolean(true), f.key("kind")).isUndefined());
    // Nullish bases throw.
    CHECK(getProperty(f.vm, JSValue::null(), f.key("x")).isException());
    f.vm.clearException();
    CHECK(getProperty(f.vm, JSValue::undefined(), f.key("x")).isException());
    f.vm.clearException();
    CHECK(!setProperty(f.vm, JSValue::undefined(), f.key("x"), num(1)));
    f.vm.clearException();
    // A store on a primitive is dropped, and an error only in strict code.
    CHECK(!setProperty(f.vm, JSValue::int32(1), f.key("x"), num(1), false));
    CHECK(f.vm.exception.isUndefined());
}

TEST(arraysHaveALengthAndElementsAddressedByIndexNames)
{
    Fixture f;
    JSArray* a = f.vm.newArray(nullptr);
    JSValue arr = toJSValue(a);
    CHECK(setProperty(f.vm, arr, f.key("0"), num(10)));
    CHECK(setProperty(f.vm, arr, f.key("2"), num(30)));
    CHECK_EQ(a->length, std::uint32_t(3));
    JSValue len = getProperty(f.vm, arr, f.key("length"));
    CHECK(len.isInt32() && len.asInt32() == 3);
    CHECK(getProperty(f.vm, arr, f.key("2")).asInt32() == 30);
    CHECK(getProperty(f.vm, arr, f.key("1")).isUndefined());  // a hole
    CHECK(!hasProperty(f.vm, arr, f.key("1")));
    CHECK(hasProperty(f.vm, arr, f.key("0")));
    CHECK(setProperty(f.vm, arr, f.key("length"), num(1)));
    CHECK_EQ(a->length, std::uint32_t(1));
    CHECK(!setProperty(f.vm, arr, f.key("length"), JSValue::boxDouble(-1.5), false));
    CHECK(!setProperty(f.vm, arr, f.key("length"), JSValue::boxDouble(0.5), true));
    CHECK(f.vm.exceptionCode == script::ErrorCode::RangeError);
    f.vm.clearException();
    // Named properties live beside the elements.
    CHECK(setProperty(f.vm, arr, f.key("name"), num(5)));
    CHECK_EQ(getProperty(f.vm, arr, f.key("name")).asInt32(), 5);
    CHECK(deleteProperty(f.vm, arr, f.key("0")));
    CHECK(a->at(0).isHole());
    CHECK(!deleteProperty(f.vm, arr, f.key("length"), false));
    // "01" and "-1" are names, not indexes.
    std::uint32_t index;
    CHECK(arrayIndexOf(f.key("0"), &index) && index == 0);
    CHECK(arrayIndexOf(f.key("4294967294"), &index) && index == 4294967294u);
    CHECK(!arrayIndexOf(f.key("4294967295"), &index));
    CHECK(!arrayIndexOf(f.key("01"), &index));
    CHECK(!arrayIndexOf(f.key("-1"), &index));
    CHECK(!arrayIndexOf(f.key("a"), &index));
    CHECK(!arrayIndexOf(f.key(""), &index));
    release(arr);
}

TEST(deleteAndDefine)
{
    Fixture f;
    JSObject* o = make(f);
    JSValue ov = toJSValue(o);
    CHECK(defineOwn(f.vm, o, f.key("a"), num(1), attr::kWritable | attr::kEnumerable));
    CHECK(!deleteProperty(f.vm, ov, f.key("a"), false));  // not configurable
    CHECK(!deleteProperty(f.vm, ov, f.key("a"), true));
    CHECK(f.vm.exceptionCode == script::ErrorCode::TypeError);
    f.vm.clearException();
    CHECK(!defineOwn(f.vm, o, f.key("a"), num(2), attr::kDefault));  // cannot become configurable
    CHECK(defineOwn(f.vm, o, f.key("b"), num(1), attr::kDefault));
    CHECK(deleteProperty(f.vm, ov, f.key("b")));
    CHECK(deleteProperty(f.vm, ov, f.key("never")));
    CHECK(!hasProperty(f.vm, ov, f.key("b")));
    release(ov);
}
