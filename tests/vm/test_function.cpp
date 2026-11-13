// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// V-8: functions, boxes, closures.
#include "fixture.hpp"

using namespace vmtest;

namespace {

JSValue answer(VM&, JSValue, const JSValue*, std::uint32_t, void*)
{
    return JSValue::int32(42);
}

}  // namespace

TEST(aClosureWithNoUpvaluesIsFortyEightBytes)
{
    Fixture f;
    CodeBlock* code = f.vm.newCodeBlock(f.key("f"), 2, false);
    const std::uint64_t before = f.heapBytes();
    JSFunction* fn = f.vm.newFunction(nullptr, code, nullptr, 0);
    f.vm.releaseCode(code);  // the function holds its own count
    CHECK_EQ(cellBytesOf(reinterpret_cast<CellPtr>(fn)), std::size_t(40));
    CHECK_EQ(f.heapBytes() - before, std::uint64_t(40));
    CHECK_EQ(mem::LocalHeap::usableSize(40), std::size_t(48));
    release(toJSValue(fn));
}

TEST(closuresHoldTheirBoxes)
{
    for (std::uint32_t n : {0u, 1u, 8u}) {
        Fixture f;
        CodeBlock* code = f.vm.newCodeBlock(nullptr, 0, false);
        std::vector<vm::Box*> boxes;
        for (std::uint32_t i = 0; i < n; ++i)
            boxes.push_back(f.vm.newBox(num(std::int32_t(i))));
        JSFunction* fn = f.vm.newFunction(nullptr, code, boxes.data(), n);
        f.vm.releaseCode(code);
        CHECK_EQ(fn->nUpvals, n);
        CHECK_EQ(cellBytesOf(reinterpret_cast<CellPtr>(fn)), sizeof(JSFunction) + 8u * n);
        for (std::uint32_t i = 0; i < n; ++i) {
            CHECK(fn->upvals()[i] == boxes[i]);
            CHECK_EQ(headerOf(reinterpret_cast<CellPtr>(boxes[i]))->rc, std::uint32_t(2));
        }
        for (vm::Box* b : boxes)
            release(JSValue::cell(reinterpret_cast<CellPtr>(b)));
        CHECK_EQ(f.vm.liveCells, std::uint64_t(n + 1));  // the boxes live on in the closure
        release(toJSValue(fn));
        CHECK_EQ(f.vm.liveCells, std::uint64_t(0));
    }
}

TEST(lengthNameAndPrototypeAppearOnDemand)
{
    Fixture f;
    JSObject* objectProto = f.vm.newObject(static_cast<JSObject*>(nullptr));
    f.vm.setIntrinsic(&f.vm.protos.object, objectProto);
    release(toJSValue(objectProto));
    CodeBlock* code = f.vm.newCodeBlock(f.key("add"), 2, true);
    JSFunction* fn = f.vm.newFunction(nullptr, code, nullptr, 0);
    f.vm.releaseCode(code);
    // Nothing yet: a function that is only called costs no property storage.
    CHECK(fn->slots == nullptr);
    CHECK(fn->flags & JSFunction::kLazyProps);
    CHECK_EQ(headerOf(reinterpret_cast<CellPtr>(fn))->rc, std::uint32_t(1));

    JSValue len = getProperty(f.vm, toJSValue(fn), f.vm.atoms.length);
    CHECK(len.isInt32() && len.asInt32() == 2);
    CHECK(!(fn->flags & JSFunction::kLazyProps));
    JSValue name = getProperty(f.vm, toJSValue(fn), f.vm.atoms.name);
    CHECK(isString(name));
    CHECK(asString(name)->text() == StringView(u"add"));
    release(name);
    JSValue proto = getProperty(f.vm, toJSValue(fn), f.vm.atoms.prototype);
    CHECK(isObject(proto));
    JSValue back = getProperty(f.vm, proto, f.vm.atoms.constructor);
    CHECK(back == toJSValue(fn));
    release(back);
    release(proto);
    // The order of `Object.getOwnPropertyNames(fn)`.
    std::vector<std::string> names;
    fn->forEachOwn(f.vm, [&](Key k, std::uint8_t, JSValue) { names.push_back(toUtf8(static_cast<StringCell*>(k)->text())); });
    CHECK((names == std::vector<std::string>{"length", "name", "prototype"}));
    // The prototype made a cycle: the function and its prototype outlive the last handle (until a collection).
    release(toJSValue(fn));
    CHECK(f.vm.liveCells >= 2);
}

TEST(aFunctionThatIsNotAConstructorHasNoPrototype)
{
    Fixture f;
    CodeBlock* code = f.vm.newCodeBlock(nullptr, 0, false);
    JSFunction* fn = f.vm.newFunction(nullptr, code, nullptr, 0);
    f.vm.releaseCode(code);
    CHECK(getProperty(f.vm, toJSValue(fn), f.vm.atoms.prototype).isUndefined());
    JSValue name = getProperty(f.vm, toJSValue(fn), f.vm.atoms.name);
    CHECK(isString(name) && asString(name)->length == 0);
    release(name);
    CHECK_EQ(headerOf(reinterpret_cast<CellPtr>(fn))->rc, std::uint32_t(1));
    release(toJSValue(fn));
    CHECK_EQ(f.vm.liveCells, std::uint64_t(0));
}

TEST(nativeFunctionsKeepTheirPointerAndData)
{
    Fixture f;
    int data = 0;
    JSFunction* fn = f.vm.newNativeFunction(nullptr, &answer, &data, f.key("answer"), 1);
    CHECK(fn->flags & JSFunction::kNative);
    Rare* r = fn->rare();
    CHECK(r && r->native == &answer && r->nativeData == &data);
    CHECK_EQ(r->native(f.vm, JSValue::undefined(), nullptr, 0, nullptr).asInt32(), 42);
    JSValue len = getProperty(f.vm, toJSValue(fn), f.vm.atoms.length);
    CHECK(len.asInt32() == 1);
    release(toJSValue(fn));
}

TEST(aBoxAndAGetterSetterAreSmall)
{
    Fixture f;
    CHECK_EQ(sizeof(vm::Box), std::size_t(24));
    CHECK_EQ(sizeof(GetterSetter), std::size_t(32));
    vm::Box* b = f.vm.newBox(num(1));
    CHECK_EQ(b->value.asInt32(), 1);
    release(JSValue::cell(reinterpret_cast<CellPtr>(b)));
}
