// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Validity cells: one load and compare says whether a prototype chain is as it was when something learned from it.
//
// `validityOf(P)` is registered with every object above `P` (and `P` itself), so a change to any of them reaches it
// directly: no walk down from the changed object, and no global epoch that one freed prototype anywhere would move.
#include "script/internal/vm/census.hpp"
#include "script/internal/vm/bytecode.hpp"
#include "script/internal/vm/object.hpp"

namespace script {
namespace internal {
namespace vm {

ValidityCell* VM::allocValidity()
{
    if (!validityFree_) {
        validityChunks_.reserve(validityChunks_.size() + 1);
        void* chunk = allocPayload(kValidityChunkBytes);
        validityChunks_.push_back(chunk);
        auto* cells = static_cast<ValidityCell*>(chunk);
        const std::size_t n = kValidityChunkBytes / sizeof(ValidityCell);
        for (std::size_t i = 0; i < n; ++i) {
            // A free cell links to the next through its two words.
            cells[i].refs = 0;
            cells[i].valid = 0;
        }
        for (std::size_t i = n; i-- > 0;) {
            *reinterpret_cast<ValidityCell**>(&cells[i]) = validityFree_;
            validityFree_ = &cells[i];
        }
    }
    ValidityCell* cell = validityFree_;
    validityFree_ = *reinterpret_cast<ValidityCell**>(cell);
    cell->refs = 1;
    cell->valid = 1;
    cell->holes = 0;
    return cell;
}

void VM::freeValidity(ValidityCell* cell) noexcept
{
    *reinterpret_cast<ValidityCell**>(cell) = validityFree_;
    validityFree_ = cell;
}

void VM::releaseValidity(ValidityCell* cell) noexcept
{
    if (--cell->refs == 0)
        freeValidity(cell);
}

ValidityCell* VM::absenceOf(JSObject* o)
{
    Dictionary& d = *o->rare()->dict;
    if (!d.absent) {
        d.absent = allocValidity();
        d.absent->refs = 1;
        d.absent->valid = 1;
    }
    return d.absent;
}

void VM::dictionaryGains(Dictionary& d) noexcept
{
    if (!d.absent)
        return;
    d.absent->valid = 0;
    releaseValidity(d.absent);
    d.absent = nullptr;
}

ValidityCell* VM::validityOf(JSObject* proto)
{
    Rare& own = proto->rareOf(*this);
    if (own.validity) {
        if (own.validity->valid)
            return own.validity;
        releaseValidity(own.validity);  // the cell is gone from here; whatever holds it goes on holding it
        own.validity = nullptr;
    }
    // Make room in every list first, so that a failure leaves nothing half registered.
    for (JSObject* a = proto; a; a = a->proto(*this)) {
        Rare& r = a->rareOf(*this);
        if (r.protoUsers.size() == r.protoUsers.capacity()) {
            // Cells nobody but this list refers to are dead weight: drop them before growing.
            std::size_t kept = 0;
            for (ValidityCell* c : r.protoUsers) {
                if (c->refs == 1)
                    releaseValidity(c);
                else
                    r.protoUsers[kept++] = c;
            }
            r.protoUsers.resize(kept);
            r.protoUsers.reserve(kept < 4 ? 4 : kept * 2);
        } else if (r.protoUsers.capacity() == 0) {
            r.protoUsers.reserve(4);
        }
        a->h.flags |= cellflags::kIsProto;
    }
    ValidityCell* cell = allocValidity();
    own.validity = cell;
    for (JSObject* a = proto; a; a = a->proto(*this)) {
        ++cell->refs;
        a->rare()->protoUsers.push_back(cell);
    }
    return cell;
}

ValidityCell* VM::holeStoreCell()
{
    JSObject* const ap = protos.array;
    if (!ap || ap->proto(*this) != protos.object)
        return nullptr;
    if (!holeCell_ || !holeCell_->valid) {
        if (holeCell_)
            releaseValidity(holeCell_);
        holeCell_ = validityOf(ap);
        retainValidity(holeCell_);
        bool plain = true;
        for (JSObject* o = ap; o && plain; o = o->proto(*this))
            o->forEachOwn(*this, [&](Key key, std::uint8_t, JSValue) {
                std::uint32_t index;
                if (arrayIndexOf(key, &index))
                    plain = false;
            });
        holePlain_ = plain;
    }
    return holePlain_ ? holeCell_ : nullptr;
}

bool VM::plainHoleStores()
{
    if (!holeStoreCell())
        return false;
    JSObject* const ap = protos.array;
    return ap->cls() != ObjClass::Array || static_cast<JSArray*>(ap)->length == 0;
}

bool VM::plainHoles(JSArray* a)
{
    const Shape* s = a->shapeOf(*this);
    if ((s->flags & (Shape::kDictionary | Shape::kIndexNames)) || !s->proto)
        return false;
    if (s->proto == protos.array)
        return plainHoleStores();
    const Rare* r = s->proto->rare();
    ValidityCell* c = r && r->validity && r->validity->valid ? r->validity : validityOf(s->proto);
    if (!c->holes) {
        bool plain = true;
        for (JSObject* o = s->proto; o && plain; o = o->proto(*this)) {
            plain = o == protos.array || o->cls() == ObjClass::Plain;
            o->forEachOwn(*this, [&](Key key, std::uint8_t, JSValue) {
                std::uint32_t index;
                if (arrayIndexOf(key, &index))
                    plain = false;
            });
        }
        c->holes = plain ? 1 : 2;
    }
    JSObject* const ap = protos.array;
    return c->holes == 1 && (ap->cls() != ObjClass::Array || static_cast<JSArray*>(ap)->length == 0);
}

HoleRec* VM::holeRecAt(CodeBlock* code, std::uint32_t index)
{
    if (code->holeRecs.size() < code->code.size())  // (merged inlining makes the code longer)
        code->holeRecs.resize(code->code.size(), nullptr);
    HoleRec*& rec = code->holeRecs[index];
    if (!rec)
        rec = new HoleRec();
    return rec;
}

void VM::noteHoleShape(CodeBlock* code, std::uint32_t index, JSArray* a)
{
    const Shape* s = a->shapeOf(*this);
    if (!s->proto || s->proto == protos.array)
        return;  // a plain array's: compiled code knows it without a record
    const Rare* r = s->proto->rare();
    ValidityCell* c = r ? r->validity : nullptr;
    if (!c || !c->valid || c->holes != 1)
        return;
    HoleRec* rec = holeRecAt(code, index);
    if (rec->shape == a->h.shape && rec->cell == c)
        return;
    retainValidity(c);
    if (rec->cell)
        releaseValidity(rec->cell);
    rec->shape = a->h.shape;
    rec->cell = c;
}

void VM::invalidateChain(JSObject* proto)
{
    if (!(proto->h.flags & cellflags::kIsProto))
        return;
    Rare* r = proto->rare();
    if (!r)
        return;
    SCRIPT_VM_CENSUS(ChainInvalidated);
    if (r->watched)
        watchedChanged(*r);
    if (r->validity) {
        r->validity->valid = 0;
        releaseValidity(r->validity);
        r->validity = nullptr;
    }
    // Each of these cells is registered with every object of its chain, so this list reaches all of them: no walk
    // down to the objects that inherit from this one.
    for (ValidityCell* c : r->protoUsers) {
        c->valid = 0;
        releaseValidity(c);
    }
    r->protoUsers.clear();
}

}  // namespace vm
}  // namespace internal
}  // namespace script
