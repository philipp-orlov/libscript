// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Allocating, sizing and destroying the cells of the new family. There are no virtual functions: each of these is a
// switch on the cell's kind and class.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "script/internal/vm/census.hpp"
#include "script/internal/vm/object.hpp"

namespace script {
namespace internal {
namespace vm {

namespace {

[[noreturn]] void fatalAddress(const void* p)
{
    std::fprintf(stderr,
                 "libscript: the heap returned %p, above 2^47, where the vm core cannot name it (a pointer is a value as "
                 "it stands). Reserve address space below that limit before the first context (LC-1).\n",
                 p);
    std::abort();
}

}  // namespace

// ---------------------------------------------------------------- allocation ---

void* VM::allocCell(std::size_t bytes, Kind kind, ObjClass cls, std::uint8_t aux)
{
    if (bytes > mem::LocalHeap::kMaxSmall) [[unlikely]]
        throw Error(ErrorCode::OutOfMemory, "a cell of the vm core is larger than the heap's small limit");
    // Cells left to free (`Options::freeBudget`): some of them first.
    if (!deferredFree_.empty() && !destroying_ && !ctx.tearingDown) [[unlikely]]
        freeDeferred(ctx.options.freeBudget);
    ctx.maybeCollect();
    // The context's budget counts heap bytes, which the buffers of arrays and strings do not add to: a program that makes
    // little garbage of cells with large buffers would hold on to a lot of memory. Cells are counted as well.
    if (liveCells >= cellBudget_ && deferredFree_.empty() && !ctx.tearingDown && !ctx.destroying) [[unlikely]] {
        // A minor collection frees young cycles only; once the cells have more than doubled since a whole look, take one.
        const bool full = liveCells > cellsAfterFull_ * 2 + kMinCellBudget;
        if (ctx.options.cycles == Context::Options::Cycles::Auto)
            ctx.collectCycles(!full);
        if (liveCells >= cellBudget_)  // the collection did not run its engine's part
            cellBudget_ = liveCells * 2;
    }
    ctx.reserve(bytes);
    void* memory = heap.allocate(bytes);
    if (reinterpret_cast<std::uintptr_t>(memory) + bytes > kAddressLimit) [[unlikely]]
        fatalAddress(memory);
    auto* h = static_cast<CellHeader*>(memory);
    h->shape = kNoShape;
    h->cls = std::uint8_t(cls);
    h->flags = cellflags::kYoung;
    h->aux = aux;
    h->kind = std::uint8_t(0x80 | std::uint8_t(kind));
    h->rc = 1;
    h->gc = 0;
    ctx.heapBytes += bytes;
    ownedBytes_ += bytes;
    ctx.totalAllocated += bytes;
    ++ctx.liveObjects;
    ++liveCells;
    return memory;
}

void* VM::allocPayload(std::size_t bytes)
{
    ctx.reserve(bytes);
    void* p = PoolAlloc<std::uint8_t>().allocate(bytes);
    ctx.heapBytes += bytes;
    ownedBytes_ += bytes;
    ctx.totalAllocated += bytes;
    return p;
}

void VM::freePayload(void* p, std::size_t bytes) noexcept
{
    PoolAlloc<std::uint8_t>().deallocate(static_cast<std::uint8_t*>(p), bytes);
    ctx.heapBytes -= bytes;
    ownedBytes_ -= bytes;
}

// -------------------------------------------------------------------- sizes ---

std::size_t cellBytesOf(const detail::GCObject* cell) noexcept
{
    switch (kindOf(cell)) {
        case Kind::Object: {
            const auto* o = reinterpret_cast<const JSObject*>(cell);
            switch (o->cls()) {
                case ObjClass::Array:
                case ObjClass::Arguments: return sizeof(JSArray);
                case ObjClass::TypedArray: return sizeof(JSTypedArray);
                case ObjClass::Function: return JSFunction::bytesFor(static_cast<const JSFunction*>(o)->nUpvals);
                case ObjClass::Promise: return JSObject::bytesFor(o->h.aux) + JSObject::kPromiseStateBytes;
                default: return JSObject::bytesFor(o->h.aux);
            }
        }
        case Kind::Box: return sizeof(Box);
        case Kind::GetterSetter: return sizeof(GetterSetter);
    }
    return 0;
}

std::size_t heapSizeOf(const detail::GCObject* cell) noexcept
{
    std::size_t bytes = cellBytesOf(cell);
    if (kindOf(cell) != Kind::Object)
        return bytes;
    auto* o = const_cast<JSObject*>(reinterpret_cast<const JSObject*>(cell));
    if (o->slots)
        bytes += sizeof(SlotsHeader) + 8u * o->slotCapacity();
    if (Rare* r = o->rare())
        bytes += sizeof(Rare) + (r->dict ? sizeof(Dictionary) + r->dict->entries.capacity() * sizeof(Entry) : 0);
    if (hasElems(o))
        bytes += 8u * static_cast<JSArray*>(o)->capacity;
    return bytes;
}

namespace {

/// The number of slots of `o` that hold something counted.
std::uint32_t usedSlots(VM& vm, JSObject* o) noexcept
{
    Rare* r = o->rare();
    if (r && r->dict)
        return std::uint32_t(r->dict->entries.size());
    return vm.shapeById(o->h.shape)->count;
}

void appendIfCell(detail::GcList& out, JSValue v)
{
    if (v.isCell())
        out.push_back(v.asCell());
}

}  // namespace

void children(const detail::GCObject* cell, detail::GcList& out, bool tracing)
{
    VM& vm = vmOf(cell);
    switch (kindOf(cell)) {
        case Kind::Object: {
            auto* o = const_cast<JSObject*>(reinterpret_cast<const JSObject*>(cell));
            const std::uint32_t n = usedSlots(vm, o);
            for (std::uint32_t i = 0; i < n; ++i)
                appendIfCell(out, *o->slotAt(i));
            if (hasElems(o)) {
                auto* a = static_cast<JSArray*>(o);
                for (std::uint32_t i = a->packed; i < a->length; ++i)  // no cell among the packed numbers
                    appendIfCell(out, a->elems[i]);
            } else if (o->cls() == ObjClass::TypedArray) {
                appendIfCell(out, static_cast<JSTypedArray*>(o)->buffer);
            }
            if (Rare* r = o->rare(); r && r->ext && !(tracing && r->ext->ephemeron())) {
                Vec<JSValue> held;
                r->ext->values(held);
                for (JSValue v : held)
                    appendIfCell(out, v);
            }
            if (NativeState* st = o->embeddedState()) {
                Vec<JSValue> held;
                st->values(held);
                for (JSValue v : held)
                    appendIfCell(out, v);
            }
            if (o->cls() == ObjClass::Function) {
                auto* f = static_cast<JSFunction*>(o);
                for (std::uint32_t i = 0; i < f->nUpvals; ++i)
                    out.push_back(reinterpret_cast<CellPtr>(f->upvals()[i]));
                if (Rare* r = f->rare()) {
                    appendIfCell(out, r->boundTarget);
                    appendIfCell(out, r->boundThis);
                    appendIfCell(out, r->home);
                    appendIfCell(out, r->fieldInit);
                    if (r->boundArgs)
                        out.push_back(reinterpret_cast<CellPtr>(r->boundArgs));
                }
            }
            break;
        }
        case Kind::Box: appendIfCell(out, reinterpret_cast<const Box*>(cell)->value); break;
        case Kind::GetterSetter: {
            const auto* g = reinterpret_cast<const GetterSetter*>(cell);
            appendIfCell(out, g->getter);
            appendIfCell(out, g->setter);
            break;
        }
    }
}

// ------------------------------------------------------------------- destroy ---

void destroyObjectStorage(VM& vm, JSObject* o) noexcept
{
    if (NativeState* st = o->embeddedState())
        st->~NativeState();  // lets go of what it holds (already done at teardown)
    // At teardown the values are already let go of (`clearReferences`), and the shapes may be gone.
    if (!vm.tearingDown) {
        const std::uint32_t n = usedSlots(vm, o);
        for (std::uint32_t i = 0; i < n; ++i)
            release(*o->slotAt(i));
    }
    switch (o->cls()) {
        case ObjClass::Array:
        case ObjClass::Arguments: {
            auto* a = static_cast<JSArray*>(o);
            if (!vm.tearingDown)
                for (std::uint32_t i = a->packed; i < a->length; ++i)  // no cell among the packed numbers
                    release(a->elems[i]);
            if (a->ownsElems())
                vm.freePayload(a->elems, 8u * a->capacity);
            break;
        }
        case ObjClass::TypedArray: {
            if (!vm.tearingDown) {
                forgetView(static_cast<JSTypedArray*>(o));
                release(static_cast<JSTypedArray*>(o)->buffer);
            }
            break;
        }
        case ObjClass::Function: {
            auto* f = static_cast<JSFunction*>(o);
            for (std::uint32_t i = 0; i < f->nUpvals; ++i)
                if (f->upvals()[i])
                    detail::decRef(reinterpret_cast<CellPtr>(f->upvals()[i]));
            if (f->code)
                vm.releaseCode(f->code);
            break;
        }
        default: break;
    }
    const bool dictionary = o->rare() && o->rare()->dict;
    if (Rare* r = o->rare()) {
        if (r->validity)
            vm.releaseValidity(r->validity);
        for (ValidityCell* c : r->protoUsers)
            vm.releaseValidity(c);
        if (r->fnName)
            detail::decRef(r->fnName);
        // At teardown these were let go of by `clearReferences`.
        release(r->boundTarget);
        release(r->boundThis);
        release(r->home);
        release(r->fieldInit);
        if (r->boundArgs)
            detail::decRef(reinterpret_cast<CellPtr>(r->boundArgs));
        if (r->ext) {
            r->ext->clear(vm);
            delete r->ext;
            r->ext = nullptr;
        }
        if (r->dict) {
            vm.dictionaryGains(*r->dict);
            for (const Entry& e : r->dict->entries)
                if (e.key)
                    detail::decRef(e.key);
            r->dict->~Dictionary();
            PoolAlloc<Dictionary>().deallocate(r->dict, 1);
        }
        r->~Rare();
        vm.freePayload(r, sizeof(Rare));
    }
    if (o->slots)
        vm.freePayload(o->slotsHeader(), sizeof(SlotsHeader) + 8u * o->slotsHeader()->capacity);
    if (dictionary && !vm.tearingDown)
        vm.freeShape(vm.shapeById(o->h.shape));
}

void VM::destroyNow(CellPtr cell) noexcept
{
    CellHeader* const hd = headerOf(cell);
    if (kindOf(cell) == Kind::Object && !(hd->flags & cellflags::kHasRare) && !tearingDown) {
        auto* o = reinterpret_cast<JSObject*>(cell);
        const ObjClass cls = o->cls();
        if (cls == ObjClass::Plain || cls == ObjClass::Array) {
            // An object with nothing on the side (no `Rare`, so no dictionary): its values, its buffers, its memory. What its
            // values free waits in the list `destroy` drains.
            auto drop = [this](JSValue v) {
                if (!v.isCell())
                    return;
                CellPtr c = v.asCell();
                if (--detail::cellCount(c) != 0)
                    return;
                if (destroying_ && isVmCell(c)) {
                    try {
                        pending_.push_back(c);
                        return;
                    } catch (...) {
                    }
                }
                detail::destroyUnreferenced(c);
            };
            const std::uint32_t n = shapeById(hd->shape)->count;
            const std::uint32_t inl = n < hd->aux ? n : hd->aux;
            for (std::uint32_t i = 0; i < inl; ++i)
                drop(o->inl()[i]);
            for (std::uint32_t i = inl; i < n; ++i)
                drop(o->slots[i - hd->aux]);
            std::size_t bytes;
            if (cls == ObjClass::Array) {
                auto* a = static_cast<JSArray*>(o);
                for (std::uint32_t i = a->packed; i < a->length; ++i)  // no cell among the packed numbers
                    drop(a->elems[i]);
                if (a->ownsElems())
                    freePayload(a->elems, 8u * a->capacity);
                bytes = sizeof(JSArray);
            } else {
                bytes = JSObject::bytesFor(hd->aux);
            }
            if (o->slots)
                freePayload(o->slotsHeader(), sizeof(SlotsHeader) + 8u * o->slotsHeader()->capacity);
            heap.deallocate(cell, bytes);
            ctx.heapBytes -= bytes;
            ownedBytes_ -= bytes;
            --ctx.liveObjects;
            --liveCells;
            return;
        }
    }
    const std::size_t bytes = cellBytesOf(cell);
    switch (kindOf(cell)) {
        case Kind::Object: destroyObjectStorage(*this, reinterpret_cast<JSObject*>(cell)); break;
        case Kind::Box: release(reinterpret_cast<Box*>(cell)->value); break;
        case Kind::GetterSetter:
            release(reinterpret_cast<GetterSetter*>(cell)->getter);
            release(reinterpret_cast<GetterSetter*>(cell)->setter);
            break;
    }
    heap.deallocate(cell, bytes);
    ctx.heapBytes -= bytes;
    ownedBytes_ -= bytes;
    --ctx.liveObjects;
    --liveCells;
}

void VM::destroy(CellPtr cell) noexcept
{
    if (tearingDown)
        return;
    // A cell that frees a chain of cells does not recurse: what reaches zero meanwhile waits here.
    if (destroying_) {
        SCRIPT_VM_CENSUS(FreeDeferred);
        try {
            pending_.push_back(cell);
        } catch (...) {
            // Out of memory while freeing: the cell stays, as a leak and not a crash.
        }
        return;
    }
    destroying_ = true;
    // A plain object whose properties are all in its cell -- a tree's node, a record -- is freed here, its counters summed
    // and set once at the end, and the first of its values it frees is freed next without going through `pending_`.
    std::size_t bytesFreed = 0, cellsFreed = 0, freed = 0;
    CellPtr next = cell;
    for (;;) {
        CellPtr follow = nullptr;
        CellHeader* const hd = headerOf(next);
        auto* o = reinterpret_cast<JSObject*>(next);
        if (kindOf(next) == Kind::Object && !(hd->flags & cellflags::kHasRare) && o->cls() == ObjClass::Plain && !o->slots) {
            const std::uint32_t count = shapeById(hd->shape)->count;
            const std::uint32_t n = count < hd->aux ? count : hd->aux;
            const JSValue* values = o->inl();
            for (std::uint32_t i = 0; i < n; ++i) {
                const JSValue v = values[i];
                if (!v.isCell())
                    continue;
                CellPtr c = v.asCell();
                if (--detail::cellCount(c) != 0)
                    continue;
                if (!isVmCell(c)) {
                    detail::destroyUnreferenced(c);
                } else if (!follow) {
                    follow = c;
                } else {
                    try {
                        pending_.push_back(c);
                    } catch (...) {
                        // Out of memory while freeing: the cell stays, as a leak and not a crash.
                    }
                }
            }
            const std::size_t bytes = JSObject::bytesFor(hd->aux);
            heap.deallocate(next, bytes);
            bytesFreed += bytes;
            ++cellsFreed;
        } else {
            destroyNow(next);
        }
        ++freed;
        // A bounded pause (`Options::freeBudget`): what is left waits, freed a part at a time as the program allocates.
        if (const std::size_t budget = ctx.options.freeBudget; budget && freed >= budget) [[unlikely]] {
            try {
                if (follow)
                    deferredFree_.push_back(follow);
                for (CellPtr c : pending_)
                    deferredFree_.push_back(c);
            } catch (...) {
                // Out of memory while freeing: those cells stay, as a leak and not a crash.
            }
            pending_.clear();
            if (cellBudget_) {
                savedCellBudget_ = cellBudget_;
                cellBudget_ = 0;
            }
            break;
        }
        if (follow) {
            next = follow;
            continue;
        }
        if (pending_.empty())
            break;
        next = pending_.back();
        pending_.pop_back();
    }
    ctx.heapBytes -= bytesFreed;
    ownedBytes_ -= bytesFreed;
    ctx.liveObjects -= cellsFreed;
    liveCells -= cellsFreed;
    destroying_ = false;
}

namespace {

/// Let go of everything a cell holds a count of, leaving it holding nothing. Frees nothing.
void clearReferences(VM& vm, CellPtr cell) noexcept
{
    switch (kindOf(cell)) {
        case Kind::Object: {
            auto* o = reinterpret_cast<JSObject*>(cell);
            const std::uint32_t n = usedSlots(vm, o);
            for (std::uint32_t i = 0; i < n; ++i) {
                release(*o->slotAt(i));
                *o->slotAt(i) = JSValue::undefined();
            }
            if (Rare* r = o->rare(); r && r->ext)
                r->ext->clear(vm);
            if (NativeState* st = o->embeddedState())
                st->clear(vm);
            if (o->cls() == ObjClass::TypedArray) {
                auto* t = static_cast<JSTypedArray*>(o);
                forgetView(t);
                release(t->buffer);
                t->buffer = JSValue::undefined();
                t->length = 0;
            }
            if (hasElems(o)) {
                auto* a = static_cast<JSArray*>(o);
                a->packed = 0;
                for (std::uint32_t i = 0; i < a->length; ++i) {
                    release(a->elems[i]);
                    a->elems[i] = JSValue::hole();
                }
            } else if (o->cls() == ObjClass::Function) {
                auto* f = static_cast<JSFunction*>(o);
                for (std::uint32_t i = 0; i < f->nUpvals; ++i) {
                    detail::decRef(reinterpret_cast<CellPtr>(f->upvals()[i]));
                    f->upvals()[i] = nullptr;
                }
                if (Rare* r = f->rare()) {
                    release(r->boundTarget);
                    release(r->boundThis);
                    release(r->home);
                    release(r->fieldInit);
                    r->boundTarget = r->boundThis = r->home = r->fieldInit = JSValue::undefined();
                    if (r->boundArgs)
                        detail::decRef(reinterpret_cast<CellPtr>(r->boundArgs));
                    r->boundArgs = nullptr;
                }
            }
            break;
        }
        case Kind::Box: {
            auto* b = reinterpret_cast<Box*>(cell);
            release(b->value);
            b->value = JSValue::undefined();
            break;
        }
        case Kind::GetterSetter: {
            auto* g = reinterpret_cast<GetterSetter*>(cell);
            release(g->getter);
            release(g->setter);
            g->getter = g->setter = JSValue::undefined();
            break;
        }
    }
}

}  // namespace

namespace {

constexpr std::uint32_t kInSet = 0x80000000u, kReached = 0x40000000u, kCountMask = 0x3FFFFFFFu;

}  // namespace

/// Up to `max` (0: all) of the cells left to free (`Options::freeBudget`); the cell budget back once none are left.
void VM::freeDeferred(std::size_t max) noexcept
{
    std::size_t n = 0;
    while (!deferredFree_.empty() && (!max || n < max)) {
        const CellPtr c = deferredFree_.back();
        deferredFree_.pop_back();
        const std::uint64_t before = liveCells;
        destroy(c);  // frees up to the budget, and leaves the rest here again
        n += before > liveCells ? before - liveCells : 1;
    }
    if (deferredFree_.empty() && !cellBudget_) {
        cellBudget_ = savedCellBudget_ ? savedCellBudget_ : 64 * kMinCellBudget;
        savedCellBudget_ = 0;
    }
}

void VM::collectCycles(bool minor) noexcept
{
    if (destroying_ || tearingDown)
        return;
    // A cell left to free has no count: it is no garbage of a cycle, and goes first.
    if (!deferredFree_.empty())
        freeDeferred(0);
    // The world: the young cells of a minor collection, every cell otherwise. What lies outside it, and a cell that is never
    // freed, counts as a reference from outside.
    Vec<CellPtr> set;
    Vec<CellPtr> work;
    Vec<JSObject*> tables;
    Vec<CellPtr> garbage;
    // Everything up to the garbage list can fail for want of memory, and the collector runs when memory is tight. Until the
    // loop over the garbage, the only thing written to a cell is its `gc` word, so giving up is putting that back.
    try {
    struct Collect
    {
        VM* vm;
        bool minor;
        Vec<CellPtr>* set;
        Vec<CellPtr>* work;
    } collect{this, minor, &set, &work};
    // Each cell is sorted as the heap shows it: there is no list of all of them to make and read again.
    heap.forEachSlot(
        [](void* context, void* slot) {
            auto& c = *static_cast<Collect*>(context);
            const CellPtr cell = static_cast<CellPtr>(slot);
            CellHeader* h = headerOf(cell);
            if (h->flags & cellflags::kImmortal) {
                // Its objects hold their prototypes without a count: what they inherit from stays.
                if (kindOf(cell) == Kind::Object)
                    if (JSObject* p = reinterpret_cast<JSObject*>(cell)->proto(*c.vm))
                        c.work->push_back(reinterpret_cast<CellPtr>(p));
                return;
            }
            if (c.minor && !(h->flags & cellflags::kYoung))
                return;
            c.set->push_back(cell);
            h->gc = (h->rc & kCountMask) | kInSet;
        },
        &collect);

    // A prototype is counted once by the root of the shapes of what inherits from it. That count is not a reference from
    // outside, but a collection that sees the whole heap can tell: whatever inherits from it reaches it below.
    if (!minor) {
        for (CellPtr cell : set) {
            if (kindOf(cell) != Kind::Object || !(headerOf(cell)->flags & cellflags::kIsProto))
                continue;
            Rare* r = reinterpret_cast<JSObject*>(cell)->rare();
            CellHeader* h = headerOf(cell);
            if (r && r->childRoot && r->childRoot->proto == reinterpret_cast<JSObject*>(cell) && (h->gc & kCountMask))
                --h->gc;
        }
    }
    // A plain object with its values all in its cell (no `Rare`): its values are its children, read in place.
    auto plainValues = [&](CellPtr cell, std::uint32_t* count) -> const JSValue* {
        if (kindOf(cell) != Kind::Object)
            return nullptr;
        CellHeader* h = headerOf(cell);
        auto* o = reinterpret_cast<JSObject*>(cell);
        if ((h->flags & cellflags::kHasRare) || o->cls() != ObjClass::Plain || o->slots)
            return nullptr;
        const std::uint32_t n = shapeById(h->shape)->count;
        *count = n < h->aux ? n : h->aux;
        return o->inl();
    };
    detail::GcList kids;
    for (CellPtr cell : set) {
        std::uint32_t n = 0;
        if (const JSValue* values = plainValues(cell, &n)) {
            for (std::uint32_t k = 0; k < n; ++k) {
                if (!values[k].isCell() || !isVmCell(values[k].asCell()))
                    continue;
                CellHeader* kh = headerOf(values[k].asCell());
                if ((kh->gc & kInSet) && (kh->gc & kCountMask))
                    --kh->gc;
            }
            continue;
        }
        kids.clear();
        children(cell, kids);
        for (detail::GCObject* kid : kids) {
            if (!kid || !isVmCell(kid))
                continue;
            CellHeader* kh = headerOf(kid);
            if ((kh->gc & kInSet) && (kh->gc & kCountMask))
                --kh->gc;
        }
    }
    // Whatever still has a reference is reachable from outside the world, and so is what it refers to.
    auto reach = [&](CellPtr cell) {
        CellHeader* h = headerOf(cell);
        if ((h->gc & kInSet) && !(h->gc & kReached)) {
            h->gc |= kReached;
            work.push_back(cell);
        }
    };
    Vec<CellPtr> seeds;
    seeds.swap(work);
    for (CellPtr cell : seeds)
        reach(cell);
    for (CellPtr cell : set) {
        CellHeader* h = headerOf(cell);
        if (h->gc & kCountMask)
            reach(cell);
    }
    while (!work.empty()) {
        CellPtr cell = work.back();
        work.pop_back();
        std::uint32_t n = 0;
        if (const JSValue* values = plainValues(cell, &n)) {
            for (std::uint32_t k = 0; k < n; ++k)
                if (values[k].isCell() && isVmCell(values[k].asCell()))
                    reach(values[k].asCell());
        } else {
            kids.clear();
            children(cell, kids, true);
            for (detail::GCObject* kid : kids)
                if (kid && isVmCell(kid))
                    reach(kid);
        }
        if (kindOf(cell) == Kind::Object) {
            auto* o = reinterpret_cast<JSObject*>(cell);
            if (JSObject* p = o->proto(*this))
                reach(reinterpret_cast<CellPtr>(p));
            if (Rare* r = o->rare(); r && r->ext && r->ext->ephemeron())
                tables.push_back(o);
        }
        if (work.empty() && !tables.empty()) {
            // A weak table keeps a value while its key is reached; a key outside the world counts as reached.
            for (JSObject* t : tables) {
                Vec<JSValue> held;
                t->rare()->ext->values(held);
                for (std::size_t i = 0; i + 1 < held.size(); i += 2) {
                    const JSValue key = held[i], value = held[i + 1];
                    if (!value.isCell() || !isVmCell(value.asCell()))
                        continue;
                    bool alive = true;
                    if (key.isCell() && isVmCell(key.asCell())) {
                        CellHeader* kh = headerOf(key.asCell());
                        alive = !(kh->gc & kInSet) || (kh->gc & kReached);
                    }
                    if (alive)
                        reach(value.asCell());
                }
            }
        }
    }
    garbage.reserve(set.size());
    } catch (...) {
        for (CellPtr cell : set)
            headerOf(cell)->gc = 0;
        return;
    }
    for (CellPtr cell : set) {
        CellHeader* h = headerOf(cell);
        if (!(h->gc & kReached)) {
            ++h->rc;  // held until it has been emptied: nothing frees it on the way
            garbage.push_back(cell);  // keeps its gc word, for the tables below to tell it is dead
        } else {
            h->gc = 0;
            h->flags &= std::uint8_t(~cellflags::kYoung);
        }
    }
    // A table that lives lets go of the entries of keys that do not.
    for (JSObject* t : tables)
        t->rare()->ext->dropEntries(
            [](void*, JSValue key) {
                if (!key.isCell() || !isVmCell(key.asCell()))
                    return false;
                const std::uint32_t gc = headerOf(key.asCell())->gc;
                return (gc & kInSet) && !(gc & kReached);
            },
            nullptr);
    for (CellPtr cell : garbage)
        headerOf(cell)->gc = 0;
    ctx.gcFreed += garbage.size();
    for (CellPtr cell : garbage)
        clearReferences(*this, cell);
    // A dead prototype lets go of its root's count; its shapes are freed once what had them is gone.
    Vec<Shape*> deadRoots;
    for (CellPtr cell : garbage) {
        if (kindOf(cell) != Kind::Object || !(headerOf(cell)->flags & cellflags::kIsProto))
            continue;
        auto* o = reinterpret_cast<JSObject*>(cell);
        invalidateChain(o);
        Rare* r = o->rare();
        if (r && r->childRoot && r->childRoot->proto == o) {
            r->childRoot->proto = nullptr;
            detail::decRef(cell);
            try {
                deadRoots.push_back(r->childRoot);
                r->childRoot = nullptr;
            } catch (...) {
            }
        }
    }
    for (CellPtr cell : garbage)
        detail::decRef(cell);
    for (Shape* root : deadRoots)
        retireShapeTree(root);
    // Re-arm the budget of cells from what is alive now, whoever asked for the collection. One that found little to free
    // is a heap growing with what it keeps: the next look waits until there are four times as many cells.
    if (!minor)
        cellsAfterFull_ = liveCells;
    std::uint64_t room = std::max<std::uint64_t>(kMinCellBudget, cellsAfterFull_);
    if (garbage.size() * 8 < set.size())
        room = std::max<std::uint64_t>(room, liveCells * 3);
    cellBudget_ = liveCells + room;
}

void VM::releaseRoots() noexcept
{
    megaClear();
    for (JSValue& v : smallStrings) {
        release(v);
        v = JSValue::undefined();
    }
    tearingDown = true;
    for (MicroJob& job : jobs) {
        release(job.a);
        release(job.b);
        release(job.c);
    }
    jobs.clear();
    for (auto& entry : symbolRegistry)
        detail::decRef(entry.second);
    symbolRegistry.clear();
    for (Key* k : {&atoms.length, &atoms.prototype, &atoms.constructor, &atoms.name, &atoms.value, &atoms.done, &atoms.next,
                   &atoms.symIterator, &atoms.symAsyncIterator, &atoms.symToPrimitive, &atoms.symToStringTag,
                   &atoms.symHasInstance, &atoms.symSpecies, &atoms.lastIndex, &atoms.index, &atoms.input,
                   &atoms.groups, &atoms.indices, &atoms.symMatch, &atoms.symMatchAll, &atoms.symReplace, &atoms.symSearch,
                   &atoms.symSplit, &atoms.symIsConcatSpreadable, &atoms.symUnscopables, &atoms.then, &atoms.return_,
                   &atoms.join, &atoms.flags, &atoms.exec, &atoms.add, &atoms.set}) {
        if (*k)
            detail::decRef(*k);
        *k = nullptr;
    }
    for (JSValue& name : typeofNames) {
        if (name.bits)
            release(name);
        name = JSValue::raw(0);
    }
    for (const RegExpSlot& s : regexpSlots) {
        release(s.value);
        release(toJSValue(s.owner));
        detail::decRef(s.key);
    }
    regexpSlots.clear();
    for (RegExpLiteral& l : regexpLiterals) {
        if (l.pattern.bits)
            release(l.pattern);
        if (l.flags.bits)
            release(l.flags);
        l = RegExpLiteral{};
    }
    release(regexpCtor);
    release(regexpExec);
    release(promiseCtor);
    release(arrayConstructor);
    release(legacyInput);
    legacyInput = JSValue::undefined();
    for (JSValue& c : typedCtors) {
        release(c);
        c = JSValue::undefined();
    }
    regexpCtor = regexpExec = promiseCtor = arrayConstructor = JSValue::undefined();
    release(exception);
    exception = JSValue::undefined();
    release(returnMarker);
    release(returnValue);
    returnMarker = returnValue = JSValue::undefined();
    for (CodeBlock** slot : codeRoots_)
        if (*slot) {
            releaseCode(*slot);
            *slot = nullptr;
        }
    codeRoots_.clear();
    if (global) {
        detail::decRef(reinterpret_cast<CellPtr>(global));
        global = nullptr;
    }
    for (auto& entry : hostBindings) {
        for (JSObject* o : {entry.second.first, entry.second.second})
            if (o)
                detail::decRef(reinterpret_cast<CellPtr>(o));
    }
    hostBindings.clear();
    for (JSObject*& p : errorProtos) {
        if (p)
            detail::decRef(reinterpret_cast<CellPtr>(p));
        p = nullptr;
    }
    for (Key& k : smallIndexKeys) {
        if (k)
            detail::decRef(k);
        k = nullptr;
    }
    for (JSObject*& p : protos.iteratorFamilies) {
        if (p)
            detail::decRef(reinterpret_cast<CellPtr>(p));
        p = nullptr;
    }
    for (JSObject*& p : protos.typedArrays) {
        if (p)
            detail::decRef(reinterpret_cast<CellPtr>(p));
        p = nullptr;
    }
    for (JSObject** p : {&protos.object, &protos.function, &protos.array, &protos.number, &protos.string, &protos.boolean,
                         &protos.symbol, &protos.bigint, &protos.generator, &protos.iterator,
                          &protos.nativeIterator, &protos.map, &protos.set, &protos.weakMap, &protos.weakSet, &protos.regexp, &protos.promise, &protos.arrayBuffer, &protos.asyncGenerator, &protos.sharedArrayBuffer, &protos.iteratorHelper,
                         &protos.wrapForValidIterator, &protos.arguments, &protos.generatorFunction, &protos.asyncFunction,
                         &protos.asyncGeneratorFunction}) {
        if (*p)
            detail::decRef(reinterpret_cast<CellPtr>(*p));
        *p = nullptr;
    }

    // What is still alive is held by a handle the embedder forgot or sits in a cycle. Pin everything, cut every edge,
    // then give back the memory, so that nothing is touched after it is freed.
    detail::GcList all;
    heap.forEachSlot([](void* list, void* slot) { static_cast<detail::GcList*>(list)->push_back(static_cast<CellPtr>(slot)); },
                     &all);
    for (CellPtr c : all)
        ++headerOf(c)->rc;
    for (CellPtr c : all)
        clearReferences(*this, c);
    for (std::uint32_t id = 1; id < shapeCount_; ++id)
        if (Shape* s = shapeTable_[id]) {
            shapeTable_[id] = nullptr;
            destroyShape(s);
        }
    for (CellPtr c : all)
        destroyNow(c);
    pending_.clear();
}

}  // namespace vm
}  // namespace internal
}  // namespace script
