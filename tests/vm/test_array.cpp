// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// V-7: arrays.
#include "fixture.hpp"

using namespace vmtest;

TEST(anArrayIsEightyBytesWithFourElementsInPlaceAndHoldsEightByteElements)
{
    Fixture f;
    JSArray* a = f.vm.newArray(nullptr);
    CHECK_EQ(cellBytesOf(reinterpret_cast<CellPtr>(a)), sizeof(JSArray));
    CHECK_EQ(sizeof(JSArray), std::size_t(80));  // what the heap's slot for 72 bytes was already
    CHECK_EQ(a->packed, 0u);
    CHECK(a->elems == a->small && !a->ownsElems() && a->capacity == JSArray::kSmall);
    CHECK(a->allNumbers());
    for (int i = 0; i < 100; ++i)
        a->append(f.vm, num(i));
    CHECK_EQ(a->packed, 100u);  // numbers only, from the start
    CHECK(a->allNumbers());
    CHECK_EQ(a->length, std::uint32_t(100));
    CHECK(a->capacity >= 100);
    for (int i = 0; i < 100; ++i)
        CHECK_EQ(a->at(i).asInt32(), i);
    CHECK(a->at(100).isHole());
    a->append(f.vm, JSValue::null());
    CHECK(!a->allNumbers());
    release(toJSValue(a));
}

TEST(elementsAreCountedAndReleased)
{
    Fixture f;
    JSArray* a = f.vm.newArray(nullptr);
    std::vector<CellPtr> boxes;
    for (int i = 0; i < 10; ++i) {
        vm::Box* b = f.vm.newBox(num(i));
        boxes.push_back(reinterpret_cast<CellPtr>(b));
        retain(JSValue::cell(boxes.back()));
        a->append(f.vm, JSValue::cell(boxes.back()));
    }
    for (CellPtr c : boxes)
        CHECK_EQ(headerOf(c)->rc, std::uint32_t(2));
    a->removeRange(f.vm, 2, 3);
    CHECK_EQ(a->length, std::uint32_t(7));
    for (int i = 2; i < 5; ++i)
        CHECK_EQ(headerOf(boxes[i])->rc, std::uint32_t(1));
    a->resize(f.vm, 5);
    CHECK_EQ(headerOf(boxes[9])->rc, std::uint32_t(1));
    release(toJSValue(a));
    CHECK_EQ(headerOf(boxes[0])->rc, std::uint32_t(1));
    CHECK_EQ(headerOf(boxes[5])->rc, std::uint32_t(1));
    for (CellPtr c : boxes)
        release(JSValue::cell(c));
    CHECK_EQ(f.cells(), std::uint64_t(0));
}

TEST(setAtGrowsWithHolesAndRefusesAHugeGap)
{
    Fixture f;
    JSArray* a = f.vm.newArray(nullptr);
    CHECK(a->setAt(f.vm, 5, num(1)));
    CHECK_EQ(a->length, std::uint32_t(6));
    for (int i = 0; i < 5; ++i)
        CHECK(a->at(i).isHole());
    CHECK(a->setAt(f.vm, 0, num(2)));
    CHECK_EQ(a->at(0).asInt32(), 2);
    vm::Box* b = f.vm.newBox(num(0));
    retain(JSValue::cell(reinterpret_cast<CellPtr>(b)));
    CHECK(!a->setAt(f.vm, 0x7FFFFFF0u, JSValue::cell(reinterpret_cast<CellPtr>(b))));  // value released
    CHECK_EQ(headerOf(reinterpret_cast<CellPtr>(b))->rc, std::uint32_t(1));
    release(JSValue::cell(reinterpret_cast<CellPtr>(b)));
    release(toJSValue(a));
}

TEST(resizeInsertGapAppendAllAndPop)
{
    Fixture f;
    JSArray* a = f.vm.newArray(nullptr, 3);
    CHECK_EQ(a->length, std::uint32_t(3));
    CHECK(a->at(1).isHole());
    CHECK(!a->allNumbers());
    a->resize(f.vm, 0);
    const JSValue src[] = {num(1), num(2), num(3), num(4)};
    a->appendAll(f.vm, src, 4);
    a->insertGap(f.vm, 1, 2);
    CHECK_EQ(a->length, std::uint32_t(6));
    CHECK(a->at(1).isHole() && a->at(2).isHole());
    CHECK_EQ(a->at(3).asInt32(), 2);
    CHECK_EQ(a->at(5).asInt32(), 4);
    CHECK_EQ(a->pop(f.vm).asInt32(), 4);
    CHECK_EQ(a->length, std::uint32_t(5));
    a->removeRange(f.vm, 0, 100);
    CHECK_EQ(a->length, std::uint32_t(0));
    CHECK(a->pop(f.vm).isUndefined());
    release(toJSValue(a));
}

TEST(tenMillionNumbersTakeEightBytesEach)
{
    Fixture f;
    JSArray* a = f.vm.newArray(nullptr);
    const std::uint64_t before = f.heapBytes();
    a->reserve(f.vm, 1000000);
    CHECK(f.heapBytes() - before <= 8u * 1000000 + 4096);
    for (int i = 0; i < 1000000; ++i)
        a->append(f.vm, JSValue::int32(i));
    CHECK(f.heapBytes() - before <= 8u * 1000000 + 4096);
    release(toJSValue(a));
}

TEST(anArrayHasItsPrototypeInItsShape)
{
    Fixture f;
    JSObject* proto = f.vm.newObject(static_cast<JSObject*>(nullptr));
    JSArray* a = f.vm.newArray(proto);
    CHECK(a->proto(f.vm) == proto);
    a->putOwn(f.vm, f.key("extra"), num(1));
    CHECK_EQ(a->findOwn(f.vm, f.key("extra"))->asInt32(), 1);
    release(toJSValue(a));
    release(toJSValue(proto));
}
