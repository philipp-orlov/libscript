// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/vm/shape.hpp"

#include "script/internal/heap.hpp"
#include "script/internal/vm/object.hpp"
#include "sys/vm.hpp"

namespace script {
namespace internal {
namespace vm {

// ---------------------------------------------------------------- dictionary ---

std::int32_t Dictionary::find(Key key) const noexcept
{
    const std::uint32_t slot = table.find(base::hashPointer(key), [&](std::uint32_t i) { return entries[i].key == key; });
    return slot ? std::int32_t(slot - 1) : -1;
}

std::uint32_t Dictionary::add(Key key, std::uint8_t attrs)
{
    const std::uint32_t index = std::uint32_t(entries.size());
    entries.push_back(Entry{key, attrs});
    if (!table.insert(base::hashPointer(key), index))
        rebuild();
    ++live;
    return index;
}

void Dictionary::erase(std::uint32_t index) noexcept
{
    // The table keeps pointing at the hole, which no key matches: a tombstone until the next rebuild.
    entries[index].key = nullptr;
    --live;
}

void Dictionary::rebuild()
{
    table.reset(entries.size() + 1);
    for (std::uint32_t i = 0; i < entries.size(); ++i)
        if (entries[i].key)
            table.insert(base::hashPointer(entries[i].key), i);
}

// --------------------------------------------------------------------- shape ---

void Shape::buildTable()
{
    table.resize(count);
    for (Shape* s = this; s->count; s = s->parent)
        table[s->count - 1] = Entry{s->key, s->attrs};
    lookup.reset(count);
    for (std::uint32_t i = 0; i < count; ++i)
        lookup.insert(base::hashPointer(table[i].key), i);
    flags |= kHasTable;
}

std::int32_t Shape::indexOf(Key k, std::uint8_t* outAttrs)
{
    if (!(keyBits & keyBit(k)))
        return -1;
    if (count <= kScanLimit) {
        for (Shape* s = this; s->count; s = s->parent) {
            if (s->key == k) {
                if (outAttrs)
                    *outAttrs = s->attrs;
                return std::int32_t(s->count - 1);
            }
        }
        return -1;
    }
    if (!(flags & kHasTable))
        buildTable();
    const std::uint32_t slot = lookup.find(base::hashPointer(k), [&](std::uint32_t i) { return table[i].key == k; });
    if (!slot)
        return -1;
    if (outAttrs)
        *outAttrs = table[slot - 1].attrs;
    return std::int32_t(slot - 1);
}

Entry Shape::entryAt(std::uint32_t index)
{
    if (count > kScanLimit) {
        if (!(flags & kHasTable))
            buildTable();
        return table[index];
    }
    Shape* s = this;
    while (s->count - 1 != index)
        s = s->parent;
    return Entry{s->key, s->attrs};
}

Shape* Shape::findTransition(Key k, std::uint8_t a) const noexcept
{
    if (transitions.size() <= kScanLimit) {
        for (const Transition& t : transitions)
            if (t.key == k && t.attrs == a)
                return t.next;
        return nullptr;
    }
    const std::uint32_t slot = transitionIndex.find(base::hashPointer(k) ^ a, [&](std::uint32_t i) {
        return transitions[i].key == k && transitions[i].attrs == a;
    });
    return slot ? transitions[slot - 1].next : nullptr;
}

Shape* Shape::after(VM& vm, Key k, std::uint8_t a)
{
    if (Shape* t = findTransition(k, a))
        return t;
    if (vm.shapesAlive() >= vm.maxShapes)
        return nullptr;
    // A name the program's source spells is as many as the source has; one made at run time can be as many as the
    // program likes, and an object keyed by them is a dictionary, not a tree.
    const bool runtimeName = k->kind == detail::CellKind::String && !(static_cast<StringCell*>(k)->bits & StringCell::kInSource);
    if (runtimeName && runtimeTransitions >= kMaxRuntimeTransitions)
        return nullptr;

    Shape* n = vm.newShape();
    n->count = count + 1;
    n->proto = proto;
    n->parent = this;
    n->root = root;
    n->key = k;
    n->attrs = a;
    n->keyBits = keyBits | keyBit(k);
    detail::incRef(k);

    try {
        transitions.push_back(Transition{k, a, n});
    } catch (...) {
        detail::decRef(k);
        vm.freeShape(n);
        throw;
    }
    if (transitions.size() > kScanLimit) {
        const std::uint32_t last = std::uint32_t(transitions.size() - 1);
        if (transitionIndex.empty() || !transitionIndex.insert(base::hashPointer(k) ^ a, last)) {
            transitionIndex.reset(transitions.size());
            for (std::uint32_t i = 0; i < transitions.size(); ++i)
                transitionIndex.insert(base::hashPointer(transitions[i].key) ^ transitions[i].attrs, i);
        }
    }
    if (runtimeName)
        ++runtimeTransitions;
    return n;
}

Shape* Shape::withLastAttrs(VM& vm, std::uint8_t newAttrs)
{
    return parent->after(vm, key, newAttrs);
}

// ------------------------------------------------------------------------ vm ---

Shape* VM::newShape()
{
    std::uint32_t id;
    if (!freeShapeIds_.empty()) {
        id = freeShapeIds_.back();
        freeShapeIds_.pop_back();
    } else {
        constexpr std::size_t kCommitStep = std::size_t(64) << 10;
        if (shapeCount_ >= shapeRegionBytes_ / sizeof(Shape*))
            throw Error(ErrorCode::OutOfMemory, "too many shapes");
        if (shapeCount_ >= shapeCommitted_) {
            sys::vmCommit(reinterpret_cast<char*>(shapeRegionBase_) + shapeCommitted_ * sizeof(Shape*), kCommitStep);
            shapeCommitted_ += std::uint32_t(kCommitStep / sizeof(Shape*));
        }
        id = shapeCount_++;
    }
    Shape* s;
    try {
        s = ::new (PoolAlloc<Shape>().allocate(1)) Shape();
    } catch (...) {
        freeShapeIds_.push_back(id);
        throw;
    }
    s->id = id;
    shapeTable_[id] = s;
    return s;
}

void VM::destroyShape(Shape* shape) noexcept
{
    if (shape->key)
        detail::decRef(shape->key);
    if ((shape->flags & Shape::kRoot) && shape->proto)
        detail::decRef(reinterpret_cast<CellPtr>(shape->proto));
    shape->~Shape();
    PoolAlloc<Shape>().deallocate(shape, 1);
}

void VM::retireShapeTree(Shape* root) noexcept
{
    std::vector<Shape*> pending{root};
    while (!pending.empty()) {
        Shape* s = pending.back();
        pending.pop_back();
        try {
            for (const Transition& t : s->transitions)
                pending.push_back(t.next);
        } catch (...) {
            // What cannot be listed stays, as it did.
        }
        shapeTable_[s->id] = nullptr;
        ++retiredShapes_;
        s->proto = nullptr;
        destroyShape(s);
    }
}

void VM::freeShape(Shape* shape) noexcept
{
    shapeTable_[shape->id] = nullptr;
    try {
        freeShapeIds_.push_back(shape->id);
    } catch (...) {
        // The id is lost; there are a million of them.
    }
    // A dictionary's shape is private and holds nothing; a tree shape that fails to be made has no key yet.
    shape->~Shape();
    PoolAlloc<Shape>().deallocate(shape, 1);
}

Shape* VM::rootShapeFor(JSObject* proto)
{
    if (!proto) {
        if (!nullRoot_) {
            nullRoot_ = newShape();
            nullRoot_->root = nullRoot_;
            nullRoot_->flags = Shape::kRoot;
        }
        return nullRoot_;
    }
    Rare& r = proto->rareOf(*this);
    if (r.childRoot)
        return r.childRoot;
    Shape* root = newShape();
    root->root = root;
    root->flags = Shape::kRoot;
    root->proto = proto;
    detail::incRef(reinterpret_cast<CellPtr>(proto));
    proto->h.flags |= cellflags::kIsProto;
    r.childRoot = root;
    return root;
}

}  // namespace vm
}  // namespace internal
}  // namespace script
