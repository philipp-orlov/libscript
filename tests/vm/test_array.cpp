// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// V-7: arrays.
#include "fixture.hpp"

using namespace vmtest;

TEST(anArrayIsNinetySixBytesWithSixElementsInPlaceAndHoldsEightByteElements)
{
    Fixture f;
    JSArray* a = f.vm.newArray(nullptr);
    CHECK_EQ(cellBytesOf(reinterpret_cast<CellPtr>(a)), sizeof(JSArray));
    CHECK_EQ(sizeof(JSArray), std::size_t(96));  // six elements in place: a literal of up to six is made in compiled code
    CHECK_EQ(a->packed, 0u);
    CHECK(a->store == a->small && !a->ownsElems() && a->capacity == JSArray::kSmall);
    for (int i = 0; i < 100; ++i)
        a->append(f.vm, num(i));
    CHECK_EQ(a->packed, 100u);  // numbers only, from the start
    CHECK_EQ(a->length, std::uint32_t(100));
    CHECK(a->capacity >= 100);
    for (int i = 0; i < 100; ++i)
        CHECK_EQ(a->at(i).asInt32(), i);
    CHECK(a->at(100).isHole());
    a->append(f.vm, JSValue::null());
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

TEST(anArrayOfInt32sTurnsIntoDoublesAndIntoValuesByWhatIsStored)
{
    Fixture f;
    f.vm.arrayKinds = true;
    JSArray* a = f.vm.newFreshArray(nullptr, 0, 3);
    CHECK(a->ints() && !a->boxed() && a->length == 3u);
    CHECK(a->holeAt(0) && a->at(2).isHole());
    a->put(f.vm, 0, num(7));
    a->put(f.vm, 1, JSValue::boxDouble(4.0));  // a whole double is the int32 it is
    CHECK(a->ints() && a->get(1).isInt32() && a->get(1).asInt32() == 4);
    a->append(f.vm, JSValue::boxDouble(0.5));
    CHECK(a->nums() && a->length == 4u);
    CHECK(a->get(0).isInt32() && a->get(0).asInt32() == 7);  // read back as the int32 it was
    CHECK(a->get(3).isDouble() && a->get(3).asDouble() == 0.5);
    CHECK(a->holeAt(2) && a->get(2).isHole());
    a->put(f.vm, 2, JSValue::boxDouble(std::nan("")));
    CHECK(!a->holeAt(2) && a->get(2).isDouble() && a->get(2).asDouble() != a->get(2).asDouble());
    a->put(f.vm, 0, JSValue::boxDouble(-0.0));
    CHECK(a->get(0).isDouble() && std::signbit(a->get(0).asDouble()));
    a->resize(f.vm, 6);
    CHECK(a->holeAt(5));
    CHECK(a->pop(f.vm).isHole());
    a->insertGap(f.vm, 1, 1);
    CHECK(a->holeAt(1) && a->get(2).asInt32() == 4 && a->length == 6u);
    a->removeRange(f.vm, 1, 1);
    CHECK(a->nums() && a->get(1).asInt32() == 4 && a->length == 5u);
    // Anything else makes values of them, and values is what it stays.
    vm::Box* b = f.vm.newBox(num(1));
    a->put(f.vm, 1, JSValue::cell(reinterpret_cast<CellPtr>(b)));
    CHECK(a->boxed() && a->cls() == ObjClass::Array && a->get(1).isCell());
    CHECK(a->get(0).isDouble() && a->get(3).asDouble() == 0.5 && a->get(4).isHole());
    CHECK_EQ(a->packed, 1u);
    a->put(f.vm, 1, JSValue::boxDouble(1.5));
    CHECK(a->boxed());
    release(toJSValue(a));
    CHECK_EQ(f.cells(), std::uint64_t(0));
}

TEST(aWholeNumberPastInt32MakesValuesOfInt32sAndCodeThatKnowsValuesOnlyGetsThem)
{
    Fixture f;
    f.vm.arrayKinds = true;
    JSArray* a = f.vm.newFreshArray(nullptr, 0);
    const JSValue two[] = {num(1), num(2)};
    a->appendAll(f.vm, two, 2);
    CHECK(a->ints() && a->cls() == ObjClass::Array);  // a program sees an Array, whatever the kind
    a->append(f.vm, JSValue::boxDouble(3000000000.0));
    CHECK(a->boxed() && a->get(2).asDouble() == 3000000000.0 && a->packed == 3u);
    release(toJSValue(a));
    JSArray* d = f.vm.newFreshArray(nullptr, 0);
    d->append(f.vm, JSValue::boxDouble(0.25));
    d->append(f.vm, num(3));
    CHECK(d->nums() && d->firstCell() == 2u);
    JSValue* v = d->vals();
    CHECK(d->boxed() && v[0].isDouble() && v[0].asDouble() == 0.25 && v[1].isInt32() && v[1].asInt32() == 3);
    release(toJSValue(d));
}
