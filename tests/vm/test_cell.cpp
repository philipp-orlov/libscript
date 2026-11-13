// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// V-3: cell header, allocation, destruction.
#include "fixture.hpp"

using namespace vmtest;

TEST(headerIsSixteenBytesWithTheCountAtEight)
{
    CHECK_EQ(sizeof(CellHeader), std::size_t(16));
    CHECK_EQ(offsetof(CellHeader, rc), script::detail::kCellCountOffset);
}

TEST(aNewCellKnowsItsFamilyKindAndOwner)
{
    Fixture f;
    vm::Box* box = f.vm.newBox(num(1));
    CellPtr cell = reinterpret_cast<CellPtr>(box);
    CHECK(isVmCell(cell));
    CHECK(kindOf(cell) == Kind::Box);
    CHECK_EQ(int(headerOf(cell)->kind), 0x80 | int(Kind::Box));
    CHECK_EQ(headerOf(cell)->rc, std::uint32_t(1));
    CHECK(&vmOf(cell) == &f.vm);
    CHECK_EQ(cellBytesOf(cell), sizeof(vm::Box));
    CHECK(reinterpret_cast<std::uintptr_t>(cell) % 16 == 0);
    CHECK(reinterpret_cast<std::uintptr_t>(cell) + sizeof(vm::Box) <= kAddressLimit);
    // An old cell is not mistaken for one.
    script::Value s = f.ctx.impl()->str("hello");
    CHECK(!isVmCell(script::detail::Access::ptr(s)));
    release(JSValue::cell(cell));
    CHECK_EQ(f.liveObjects(), std::uint64_t(0));
}

TEST(countsReturnToTheStartAfterAMillionBoxes)
{
    Fixture f;
    const std::uint64_t before = f.liveObjects();
    const std::uint64_t bytes = f.heapBytes();
    for (int i = 0; i < 1000000; ++i)
        release(JSValue::cell(reinterpret_cast<CellPtr>(f.vm.newBox(num(i)))));
    CHECK_EQ(f.liveObjects(), before);
    CHECK_EQ(f.heapBytes(), bytes);
    CHECK_EQ(f.cells(), std::uint64_t(0));
    std::vector<JSValue> held;
    for (int i = 0; i < 100000; ++i)
        held.push_back(JSValue::cell(reinterpret_cast<CellPtr>(f.vm.newBox(num(i)))));
    CHECK_EQ(f.cells(), std::uint64_t(100000));
    CHECK_EQ(f.liveObjects(), before + 100000);
    for (JSValue v : held)
        release(v);
    CHECK_EQ(f.liveObjects(), before);
}

TEST(aLongChainOfBoxesFreesWithoutRecursing)
{
    Fixture f;
    const std::uint64_t before = f.liveObjects();
    JSValue head = JSValue::undefined();
    for (int i = 0; i < 300000; ++i)
        head = JSValue::cell(reinterpret_cast<CellPtr>(f.vm.newBox(head)));
    CHECK_EQ(f.cells(), std::uint64_t(300000));
    release(head);
    CHECK_EQ(f.cells(), std::uint64_t(0));
    CHECK_EQ(f.liveObjects(), before);
}

TEST(aLongChainOfObjectsFreesWithoutRecursing)
{
    Fixture f;
    Key next = f.key("next");
    JSValue head = JSValue::null();
    for (int i = 0; i < 200000; ++i) {
        JSObject* o = f.vm.newObject(static_cast<JSObject*>(nullptr));
        o->putOwn(f.vm, next, head);
        head = toJSValue(o);
    }
    release(head);
    CHECK_EQ(f.cells(), std::uint64_t(0));
}

TEST(aCellHoldsItsOldFamilyChildrenAndLetsThemGo)
{
    Fixture f;
    ContextImpl& rt = *f.ctx.impl();
    const std::uint64_t strings = rt.liveStrings;
    StringCell* text = rt.newString(u"a string only the box knows");
    CHECK_EQ(rt.liveStrings, strings + 1);
    vm::Box* box = f.vm.newBox(JSValue::cell(text));
    release(JSValue::cell(reinterpret_cast<CellPtr>(box)));
    CHECK_EQ(rt.liveStrings, strings);
}

TEST(childrenListsTheCellsAnObjectHolds)
{
    Fixture f;
    JSObject* o = f.vm.newObject(static_cast<JSObject*>(nullptr));
    vm::Box* a = f.vm.newBox(num(1));
    vm::Box* b = f.vm.newBox(num(2));
    o->putOwn(f.vm, f.key("a"), JSValue::cell(reinterpret_cast<CellPtr>(a)));
    o->putOwn(f.vm, f.key("b"), JSValue::cell(reinterpret_cast<CellPtr>(b)));
    o->putOwn(f.vm, f.key("n"), num(7));
    GcList out;
    children(reinterpret_cast<CellPtr>(o), out);
    CHECK_EQ(out.size(), std::size_t(2));
    CHECK(out[0] == reinterpret_cast<CellPtr>(a));
    CHECK(out[1] == reinterpret_cast<CellPtr>(b));
    CHECK(heapSizeOf(reinterpret_cast<CellPtr>(o)) >= cellBytesOf(reinterpret_cast<CellPtr>(o)));
    release(toJSValue(o));
}

TEST(cellsStillAliveWhenTheContextEndsAreFreedWithIt)
{
    // A handle the embedder forgot, and a cycle: the teardown frees both, which ASAN checks.
    script::Context::Options options = Fixture::options();
    auto ctx = std::make_unique<script::Context>(options);
    VM& vm = ctx->impl()->vm();
    Key self = vm.internKey("self");
    for (int i = 0; i < 1000; ++i) {
        JSObject* a = vm.newObject(static_cast<JSObject*>(nullptr));
        JSObject* b = vm.newObject(static_cast<JSObject*>(nullptr));
        retain(toJSValue(b));
        a->putOwn(vm, self, toJSValue(b));
        retain(toJSValue(a));
        b->putOwn(vm, self, toJSValue(a));
        JSArray* arr = vm.newArray(nullptr);
        arr->append(vm, toJSValue(a));
        arr->append(vm, JSValue::int32(5));
        // `a`, `b` and `arr` keep their creation counts: forgotten.
    }
    ctx.reset();
}
