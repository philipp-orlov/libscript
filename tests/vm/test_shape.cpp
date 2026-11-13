// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// V-4: shapes.
#include <random>

#include "fixture.hpp"

using namespace vmtest;

namespace {

std::int32_t bruteForce(Shape* s, Key k)
{
    for (std::uint32_t i = 0; i < s->count; ++i)
        if (s->entryAt(i).key == k)
            return std::int32_t(i);
    return -1;
}

}  // namespace

TEST(sameKeysInTheSameOrderShareAShape)
{
    Fixture f;
    Shape* root = f.vm.rootShapeFor(nullptr);
    Key a = f.key("a"), b = f.key("b");
    Shape* ab1 = root->after(f.vm, a, attr::kDefault)->after(f.vm, b, attr::kDefault);
    Shape* ab2 = root->after(f.vm, a, attr::kDefault)->after(f.vm, b, attr::kDefault);
    Shape* ba = root->after(f.vm, b, attr::kDefault)->after(f.vm, a, attr::kDefault);
    CHECK(ab1 == ab2);
    CHECK(ab1 != ba);
    CHECK_EQ(ab1->count, std::uint32_t(2));
    CHECK(ab1->root == root);
    CHECK_EQ(ab1->indexOf(a), 0);
    CHECK_EQ(ab1->indexOf(b), 1);
    CHECK_EQ(ba->indexOf(a), 1);
    CHECK_EQ(ba->indexOf(b), 0);
    CHECK_EQ(ab1->indexOf(f.key("c")), -1);
    // The attributes are part of the transition.
    Shape* ro = root->after(f.vm, a, attr::kEnumerable);
    CHECK(ro != root->after(f.vm, a, attr::kDefault));
    std::uint8_t attrs = 0;
    CHECK_EQ(ro->indexOf(a, &attrs), 0);
    CHECK_EQ(int(attrs), int(attr::kEnumerable));
}

TEST(shapesAreFoundByIdAndHoldThePrototype)
{
    Fixture f;
    JSObject* proto = f.vm.newObject(static_cast<JSObject*>(nullptr));
    Shape* root = f.vm.rootShapeFor(proto);
    CHECK(root->proto == proto);
    CHECK(f.vm.rootShapeFor(proto) == root);
    CHECK(f.vm.rootShapeFor(nullptr) != root);
    CHECK(f.vm.shapeById(root->id) == root);
    CHECK(proto->isProto());
    Shape* child = root->after(f.vm, f.key("x"), attr::kDefault);
    CHECK(child->proto == proto);
    CHECK(f.vm.shapeById(child->id) == child);
    // The root holds a count of its prototype.
    CHECK_EQ(headerOf(reinterpret_cast<CellPtr>(proto))->rc, std::uint32_t(2));
    release(toJSValue(proto));
    CHECK(f.vm.rootShapeFor(proto) == root);
}

TEST(indexOfAgreesWithABruteForceScanForOneTo300Properties)
{
    Fixture f;
    std::vector<Key> keys;
    for (int i = 0; i < 400; ++i)
        keys.push_back(f.key("k" + std::to_string(i)));
    Shape* s = f.vm.rootShapeFor(nullptr);
    std::mt19937 rng(5);
    for (std::uint32_t n = 1; n <= 300; ++n) {
        s = s->after(f.vm, keys[n - 1], std::uint8_t(1 + rng() % 7));
        if (!s) {
            // The run-time-name limit applies to one shape's transitions, never to a chain of single ones.
            CHECK(false);
            return;
        }
        CHECK_EQ(s->count, n);
        if (n <= 12 || n % 17 == 0) {
            for (Key k : keys) {
                const std::int32_t want = bruteForce(s, k);
                std::uint8_t attrs = 0;
                CHECK_EQ(s->indexOf(k, &attrs), want);
                if (want >= 0)
                    CHECK_EQ(int(attrs), int(s->entryAt(std::uint32_t(want)).attrs));
            }
        }
    }
}

TEST(aNameMadeAtRunTimeIsLimitedToThirtyTwoTransitionsPerShape)
{
    Fixture f;
    JSObject* proto = f.vm.newObject(static_cast<JSObject*>(nullptr));
    Shape* root = f.vm.rootShapeFor(proto);  // a root of its own: the built-ins used up the shared one's names
    std::uint32_t made = 0;
    for (int i = 0; i < 40; ++i) {
        StringCell* name = f.ctx.impl()->intern(script::internal::fromUtf8("dyn" + std::to_string(i)));
        if (root->after(f.vm, name, attr::kDefault))
            ++made;
    }
    CHECK_EQ(made, kMaxRuntimeTransitions);
    // A name the source spells has no such limit.
    for (int i = 0; i < 40; ++i) {
        StringCell* name = f.ctx.impl()->intern(script::internal::fromUtf8("src" + std::to_string(i)));
        name->bits |= StringCell::kInSource;
        CHECK(root->after(f.vm, name, attr::kDefault) != nullptr);
    }
}

TEST(manyTransitionsFromOneShapeAreAllFound)
{
    Fixture f;
    Shape* root = f.vm.rootShapeFor(nullptr);
    std::vector<std::pair<Key, Shape*>> made;
    for (int i = 0; i < 200; ++i) {
        StringCell* name = f.ctx.impl()->intern(script::internal::fromUtf8("t" + std::to_string(i)));
        name->bits |= StringCell::kInSource;
        made.push_back({name, root->after(f.vm, name, attr::kDefault)});
    }
    for (auto& [k, s] : made)
        CHECK(root->after(f.vm, k, attr::kDefault) == s);
}

TEST(theShapeTableIsCappedAndObjectsBecomeDictionaries)
{
    Fixture f;
    f.vm.maxShapes = f.vm.shapesAlive() + 5;
    JSObject* o = f.vm.newObject(static_cast<JSObject*>(nullptr));
    for (int i = 0; i < 20; ++i)
        o->putOwn(f.vm, f.key("m" + std::to_string(i)), num(i));
    CHECK(o->isDictionary(f.vm));
    for (int i = 0; i < 20; ++i) {
        JSValue* slot = o->findOwn(f.vm, f.key("m" + std::to_string(i)));
        CHECK(slot && slot->asInt32() == i);
    }
    release(toJSValue(o));
}

TEST(shapeIdsOfDictionariesAreReused)
{
    Fixture f;
    const std::uint32_t before = f.vm.shapesAlive();
    for (int round = 0; round < 50; ++round) {
        JSObject* o = f.vm.newObject(static_cast<JSObject*>(nullptr));
        o->putOwn(f.vm, f.key("x"), num(1));
        o->toDictionary(f.vm);
        CHECK(o->isDictionary(f.vm));
        release(toJSValue(o));
    }
    CHECK(f.vm.shapesAlive() <= before + 3);
}
