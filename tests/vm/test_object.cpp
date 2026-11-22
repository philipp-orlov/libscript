// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// V-5: objects.
#include "fixture.hpp"

using namespace vmtest;

namespace {

std::vector<std::string> keysOf(Fixture& f, JSObject* o)
{
    std::vector<std::string> out;
    o->forEachOwn(f.vm, [&](Key k, std::uint8_t, JSValue) { out.push_back(script::internal::toUtf8(static_cast<StringCell*>(k)->text())); });
    return out;
}

JSObject* plain(Fixture& f, JSObject* proto = nullptr, std::uint8_t aux = 3)
{
    return f.vm.newObject(proto, aux);
}

}  // namespace

TEST(anObjectWithTwoPropertiesIsFortyEightBytes)
{
    Fixture f;
    Key x = f.key("x"), y = f.key("y");  // names are strings, which are charged too
    f.vm.rootShapeFor(nullptr, 3);
    const std::uint64_t before = f.heapBytes();
    JSObject* o = plain(f);
    o->putOwn(f.vm, x, num(1));
    o->putOwn(f.vm, y, num(2));
    CHECK_EQ(f.heapBytes() - before, std::uint64_t(48));
    CHECK_EQ(cellBytesOf(reinterpret_cast<CellPtr>(o)), std::size_t(48));
    CHECK(o->slots == nullptr);
    CHECK_EQ(o->slotAt(0)->asInt32(), 1);
    CHECK_EQ(o->slotAt(1)->asInt32(), 2);
    release(toJSValue(o));
    CHECK_EQ(f.heapBytes(), before);
}

TEST(addOverwriteAndReadBack)
{
    Fixture f;
    JSObject* o = plain(f);
    Key a = f.key("a");
    CHECK(o->findOwn(f.vm, a) == nullptr);
    o->putOwn(f.vm, a, num(1));
    CHECK_EQ(o->findOwn(f.vm, a)->asInt32(), 1);
    o->putOwn(f.vm, a, num(2));
    CHECK_EQ(o->findOwn(f.vm, a)->asInt32(), 2);
    CHECK_EQ(o->propertyCount(f.vm), std::uint32_t(1));
    std::uint8_t attrs = 0;
    o->putOwn(f.vm, a, num(3), attr::kEnumerable);
    CHECK_EQ(o->findOwn(f.vm, a, &attrs)->asInt32(), 3);
    CHECK_EQ(int(attrs), int(attr::kEnumerable));
    release(toJSValue(o));
}

TEST(propertiesBeyondTheInlineOnesGoOutOfLine)
{
    Fixture f;
    JSObject* o = plain(f, nullptr, 1);
    std::vector<Key> keys;
    for (int i = 0; i < 40; ++i) {
        keys.push_back(f.key("p" + std::to_string(i)));
        o->putOwn(f.vm, keys.back(), num(i * 10));
    }
    CHECK(o->slots != nullptr);
    CHECK(o->slotCapacity() >= 39);
    for (int i = 0; i < 40; ++i)
        CHECK_EQ(o->findOwn(f.vm, keys[i])->asInt32(), i * 10);
    CHECK(!o->isDictionary(f.vm));
    release(toJSValue(o));
}

TEST(deleteTheLastAddedGoesBackToTheParentShape)
{
    Fixture f;
    JSObject* o = plain(f);
    Key a = f.key("a"), b = f.key("b");
    o->putOwn(f.vm, a, num(1));
    Shape* afterA = o->shapeOf(f.vm);
    o->putOwn(f.vm, b, num(2));
    CHECK(o->deleteOwn(f.vm, b));
    CHECK(o->shapeOf(f.vm) == afterA);
    CHECK(!o->isDictionary(f.vm));
    CHECK(o->findOwn(f.vm, b) == nullptr);
    CHECK(o->deleteOwn(f.vm, b));  // absent: nothing to do
    release(toJSValue(o));
}

TEST(deleteAnyOtherMakesADictionaryAndKeepsTheOrder)
{
    Fixture f;
    JSObject* o = plain(f);
    for (const char* n : {"a", "b", "c", "d"})
        o->putOwn(f.vm, f.key(n), num(1));
    CHECK(o->deleteOwn(f.vm, f.key("b")));
    CHECK(o->isDictionary(f.vm));
    CHECK((keysOf(f, o) == std::vector<std::string>{"a", "c", "d"}));
    o->putOwn(f.vm, f.key("b"), num(9));
    CHECK((keysOf(f, o) == std::vector<std::string>{"a", "c", "d", "b"}));
    CHECK_EQ(o->findOwn(f.vm, f.key("b"))->asInt32(), 9);
    CHECK_EQ(o->propertyCount(f.vm), std::uint32_t(4));
    release(toJSValue(o));
}

TEST(aThousandPropertiesKeepOrderAndValues)
{
    Fixture f;
    JSObject* o = plain(f);
    std::vector<std::string> names;
    for (int i = 0; i < 1000; ++i) {
        names.push_back("prop" + std::to_string(i));
        o->putOwn(f.vm, f.key(names.back()), num(i));
    }
    CHECK(o->isDictionary(f.vm));
    CHECK(keysOf(f, o) == names);
    for (int i = 0; i < 1000; ++i)
        CHECK_EQ(o->findOwn(f.vm, f.key(names[i]))->asInt32(), i);
    // Churn in a dictionary compacts instead of growing for ever.
    for (int round = 0; round < 20000; ++round) {
        Key k = f.key("churn" + std::to_string(round % 50));
        o->putOwn(f.vm, k, num(round));
        o->deleteOwn(f.vm, k);
    }
    CHECK_EQ(o->propertyCount(f.vm), std::uint32_t(1000));
    CHECK(o->rare()->dict->entries.size() < 1600);
    CHECK(keysOf(f, o) == names);
    for (int i = 0; i < 1000; i += 7)
        CHECK_EQ(o->findOwn(f.vm, f.key(names[i]))->asInt32(), i);
    release(toJSValue(o));
}

TEST(attributesChangeAsATransitionOrByDictionary)
{
    Fixture f;
    JSObject* o = plain(f);
    Key a = f.key("a"), b = f.key("b");
    o->putOwn(f.vm, a, num(1));
    o->putOwn(f.vm, b, num(2));
    std::uint8_t attrs = 0;
    CHECK(o->setAttrs(f.vm, b, attr::kEnumerable));
    CHECK(!o->isDictionary(f.vm));  // the last one added: a transition
    CHECK(o->findOwn(f.vm, b, &attrs) && attrs == attr::kEnumerable);
    CHECK(o->setAttrs(f.vm, a, attr::kEnumerable));
    CHECK(o->isDictionary(f.vm));
    CHECK(o->findOwn(f.vm, a, &attrs) && attrs == attr::kEnumerable);
    CHECK(!o->setAttrs(f.vm, f.key("nope"), 0));
    // A property that is not configurable stays.
    CHECK(!o->deleteOwn(f.vm, a));
    CHECK(o->findOwn(f.vm, a) != nullptr);
    release(toJSValue(o));
}

TEST(thePrototypeIsInTheShape)
{
    Fixture f;
    JSObject* p1 = plain(f);
    JSObject* p2 = plain(f);
    JSObject* a = plain(f, p1);
    JSObject* b = plain(f, p1);
    JSObject* c = plain(f, p2);
    for (JSObject* o : {a, b, c})
        o->putOwn(f.vm, f.key("x"), num(1));
    CHECK_EQ(a->h.shape, b->h.shape);
    CHECK(a->h.shape != c->h.shape);
    CHECK(a->proto(f.vm) == p1);
    CHECK(c->proto(f.vm) == p2);
    for (JSObject* o : {a, b, c, p1, p2})
        release(toJSValue(o));
}

TEST(changingThePrototypeMovesBetweenShapeTrees)
{
    Fixture f;
    JSObject* p1 = plain(f);
    JSObject* p2 = plain(f);
    JSObject* o = plain(f, p1);
    o->putOwn(f.vm, f.key("x"), num(1));
    o->putOwn(f.vm, f.key("y"), num(2));
    JSObject* twin = plain(f, p2);
    twin->putOwn(f.vm, f.key("x"), num(5));
    twin->putOwn(f.vm, f.key("y"), num(6));
    const std::uint32_t before = o->h.shape;
    o->setProto(f.vm, p2);
    CHECK(o->proto(f.vm) == p2);
    CHECK(o->h.shape != before);
    CHECK_EQ(o->h.shape, twin->h.shape);  // the same tree, so the same shape
    CHECK_EQ(o->findOwn(f.vm, f.key("x"))->asInt32(), 1);
    CHECK_EQ(o->findOwn(f.vm, f.key("y"))->asInt32(), 2);
    o->setProto(f.vm, nullptr);
    CHECK(o->proto(f.vm) == nullptr);
    CHECK_EQ(o->findOwn(f.vm, f.key("y"))->asInt32(), 2);
    CHECK(o->wouldCycle(f.vm, o));
    p2->setProto(f.vm, o);
    CHECK(o->wouldCycle(f.vm, p2));
    CHECK(!o->wouldCycle(f.vm, p1));
    for (JSObject* x : {p1, p2, o, twin})
        release(toJSValue(x));
}

TEST(aBigObjectMovingPrototypeBecomesADictionary)
{
    Fixture f;
    JSObject* p = plain(f);
    JSObject* o = plain(f);
    for (int i = 0; i < 100; ++i)
        o->putOwn(f.vm, f.key("q" + std::to_string(i)), num(i));
    CHECK(!o->isDictionary(f.vm));
    o->setProto(f.vm, p);
    CHECK(o->isDictionary(f.vm));
    CHECK(o->proto(f.vm) == p);
    for (int i = 0; i < 100; ++i)
        CHECK_EQ(o->findOwn(f.vm, f.key("q" + std::to_string(i)))->asInt32(), i);
    release(toJSValue(p));
    release(toJSValue(o));
}

TEST(valuesAreCountedAndReleasedWhenReplacedOrDeleted)
{
    Fixture f;
    JSObject* o = plain(f);
    vm::Box* box = f.vm.newBox(num(1));
    CellPtr cell = reinterpret_cast<CellPtr>(box);
    retain(JSValue::cell(cell));  // ours; the object gets the creation count
    o->putOwn(f.vm, f.key("b"), JSValue::cell(cell));
    CHECK_EQ(headerOf(cell)->rc, std::uint32_t(2));
    o->putOwn(f.vm, f.key("b"), num(0));
    CHECK_EQ(headerOf(cell)->rc, std::uint32_t(1));
    release(JSValue::cell(cell));
    CHECK_EQ(f.cells(), std::uint64_t(1));
    release(toJSValue(o));
    CHECK_EQ(f.cells(), std::uint64_t(0));
}
