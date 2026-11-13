// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// V-11: validity cells.
#include "fixture.hpp"

using namespace vmtest;

namespace {

struct Chain
{
    Fixture f;
    JSObject *a, *b, *c;  // a <- b <- c: a is the root
    Chain()
    {
        a = f.vm.newObject(static_cast<JSObject*>(nullptr));
        b = f.vm.newObject(a);
        c = f.vm.newObject(b);
        a->putOwn(f.vm, f.key("m"), num(1));
    }
    ~Chain()
    {
        for (JSObject* o : {a, b, c})
            release(toJSValue(o));
    }
};

}  // namespace

TEST(aNewCellIsValidAndStaysTheSameCell)
{
    Chain t;
    ValidityCell* cell = t.f.vm.validityOf(t.c);
    CHECK(cell->valid);
    CHECK(t.f.vm.validityOf(t.c) == cell);
    CHECK(t.f.vm.validityOf(t.b) != cell);
    CHECK(t.c->isProto());
    CHECK(t.a->isProto());
}

TEST(addingAPropertyToAnAncestorInvalidatesTheChain)
{
    Chain t;
    ValidityCell* forC = t.f.vm.validityOf(t.c);
    ValidityCell* forB = t.f.vm.validityOf(t.b);
    ValidityCell* forA = t.f.vm.validityOf(t.a);
    // A cache's own counts: without them a cell is gone as soon as it is invalid.
    t.f.vm.retainValidity(forC);
    t.f.vm.retainValidity(forB);
    t.f.vm.retainValidity(forA);
    t.a->putOwn(t.f.vm, t.f.key("added"), num(2));
    CHECK(!forC->valid);
    CHECK(!forB->valid);
    CHECK(!forA->valid);
    ValidityCell* again = t.f.vm.validityOf(t.c);
    CHECK(again->valid);
    CHECK(again != forC);
    t.f.vm.releaseValidity(forC);
    t.f.vm.releaseValidity(forB);
    t.f.vm.releaseValidity(forA);
}

TEST(changingAValueDoesNotInvalidate)
{
    Chain t;
    ValidityCell* cell = t.f.vm.validityOf(t.c);
    t.a->putOwn(t.f.vm, t.f.key("m"), num(5));
    CHECK(cell->valid);
    CHECK(setProperty(t.f.vm, toJSValue(t.a), t.f.key("m"), num(6)));
    CHECK(cell->valid);
}

TEST(deletingChangingAttributesAndMovingThePrototypeInvalidate)
{
    {
        Chain t;
        ValidityCell* cell = t.f.vm.validityOf(t.c);
        t.f.vm.retainValidity(cell);
        CHECK(t.b->deleteOwn(t.f.vm, t.f.key("none")));  // nothing there: nothing changes
        CHECK(cell->valid);
        CHECK(t.a->deleteOwn(t.f.vm, t.f.key("m")));
        CHECK(!cell->valid);
        t.f.vm.releaseValidity(cell);
    }
    {
        Chain t;
        ValidityCell* cell = t.f.vm.validityOf(t.c);
        t.f.vm.retainValidity(cell);
        CHECK(t.a->setAttrs(t.f.vm, t.f.key("m"), attr::kEnumerable));
        CHECK(!cell->valid);
        t.f.vm.releaseValidity(cell);
    }
    {
        Chain t;
        ValidityCell* cell = t.f.vm.validityOf(t.c);
        t.f.vm.retainValidity(cell);
        JSObject* other = t.f.vm.newObject(static_cast<JSObject*>(nullptr));
        t.b->setProto(t.f.vm, other);
        CHECK(!cell->valid);
        t.f.vm.releaseValidity(cell);
        ValidityCell* fresh = t.f.vm.validityOf(t.c);
        CHECK(fresh->valid);
        t.a->putOwn(t.f.vm, t.f.key("late"), num(1));  // a is no longer on the chain
        CHECK(fresh->valid);
        other->putOwn(t.f.vm, t.f.key("late"), num(1));
        CHECK(!fresh->valid);
        release(toJSValue(other));
    }
}

TEST(anObjectThatIsNobodysPrototypeInvalidatesNothing)
{
    Fixture f;
    JSObject* lone = f.vm.newObject(static_cast<JSObject*>(nullptr));
    CHECK(!lone->isProto());
    lone->putOwn(f.vm, f.key("x"), num(1));
    CHECK(lone->rare() == nullptr || lone->rare()->protoUsers.empty());
    release(toJSValue(lone));
}

TEST(cellsNobodyHoldsAreReclaimedWhenAListGrows)
{
    Chain t;
    // Many short-lived chains under one root: the root's list must not grow without bound.
    for (int i = 0; i < 5000; ++i) {
        JSObject* leaf = t.f.vm.newObject(t.a);
        t.f.vm.validityOf(leaf);
        release(toJSValue(leaf));
    }
    CHECK(t.a->rare()->protoUsers.size() < 100);
}

TEST(aCellOutlivesItsObjectWhileSomethingHoldsIt)
{
    Fixture f;
    JSObject* a = f.vm.newObject(static_cast<JSObject*>(nullptr));
    JSObject* b = f.vm.newObject(a);
    ValidityCell* cell = f.vm.validityOf(b);
    f.vm.retainValidity(cell);
    release(toJSValue(b));
    CHECK(cell->valid);
    CHECK_EQ(cell->refs, std::uint32_t(2));  // ours, and the root's list
    a->putOwn(f.vm, f.key("x"), num(1));
    CHECK(!cell->valid);
    f.vm.releaseValidity(cell);
    release(toJSValue(a));
}
