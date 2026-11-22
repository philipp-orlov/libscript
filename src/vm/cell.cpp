// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Allocating, sizing and destroying the cells of the new family. There are no virtual functions: each of these is a
// switch on the cell's kind and class.
#include <algorithm>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "script/internal/vm/census.hpp"
#include "script/internal/vm/bytecode.hpp"
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
    if (!deferredFree_.empty() && !destroying_ && !ctx.tearingDown) [[unlikely]] {
        ++immediate_;
        freeDeferred(ctx.options.freeBudget);
        --immediate_;
    }
    // The zero-count table is full (`deferred`): what no frame holds goes.
    if (deferred && zctCount() >= zctLimit_ && zctCount() && !destroying_ && !immediate_ && !ctx.tearingDown) [[unlikely]]
        scanZct();
    ctx.maybeCollect();
    // The context's budget counts heap bytes, which the buffers of arrays and strings do not add to: a program that makes
    // little garbage of cells with large buffers would hold on to a lot of memory. Cells are counted as well.
    if (liveCells >= cellBudget_ && deferredFree_.empty() && !ctx.tearingDown && !ctx.destroying &&
        !(freeHeldBack() && liveCells < cellBudget_)) [[unlikely]] {
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
    if (bytes >= kLargeCell) [[unlikely]]
        noteLarge(bytes);
    return p;
}

/// A large payload or a large cell in the zero-count table: the scan does not wait for the table to fill.
void VM::noteLarge(std::size_t bytes) noexcept
{
    largeSinceScan_ += bytes;
    if (deferred && largeSinceScan_ >= kLargeScan && zctCount() && !immediate_) {
        zctLimit_ = std::min(zctLimit_, zctCount());
        noteZct();
    }
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
                case ObjClass::RegExpCell: return JSObject::bytesFor(o->h.aux) + JSObject::kRegExpStateBytes;
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
            if (f->code) {
                if (f->code->only == f)
                    f->code->only = nullptr;
                vm.releaseCode(f->code);
            }
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
    // A frame may hold it without a count: it waits in the zero-count table for the next scan.
    if (deferred && !immediate_ && (frameTop != frameBase || parking_ || headerOf(cell)->gc == kInZct)) {
        if (headerOf(cell)->gc != kInZct) {
            zctPush(cell);
            if (zctCount() >= zctLimit_) [[unlikely]]
                noteZct();
        }
        return;
    }
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
    Freed freed;
    destroyFrom(cell, freed);
    countFreed(freed);
    destroying_ = false;
}

void VM::countFreed(const Freed& f) noexcept
{
    ctx.heapBytes -= f.bytes;
    ownedBytes_ -= f.bytes;
    ctx.liveObjects -= f.cells;
    liveCells -= f.cells;
}

inline void VM::destroyFrom(CellPtr cell, Freed& total) noexcept
{
    // A plain object whose properties are all in its cell -- a tree's node, a record -- is freed here, its counters summed
    // and set once at the end, and the first of its values it frees is freed next without going through `pending_`.
    std::size_t freed = 0;
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
            total.bytes += bytes;
            ++total.cells;
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
    restoreCellBudget();
}

void VM::restoreCellBudget() noexcept
{
    if (!deferredFree_.empty() || (deferred && zctCount() >= zctLimit_) || cellBudget_)
        return;
    cellBudget_ = savedCellBudget_ ? savedCellBudget_ : 64 * kMinCellBudget;
    savedCellBudget_ = 0;
}

/// The table has grown to its limit: compiled code allocates through `allocCell` (cell budget 0), which scans.
void VM::noteZct() noexcept
{
    ctx.heldBackDue = true;
    if (zctLimit_ && zctCount() >= zctLimit_ && cellBudget_) [[unlikely]] {
        savedCellBudget_ = cellBudget_;
        cellBudget_ = 0;
    }
}

bool VM::deferOld(detail::GCObject* cell) noexcept
{
    if (!deferred || immediate_)
        return false;
    if (cell->inZct)
        return true;
    if (frameTop == frameBase && !parking_)
        return false;  // no frame to hold it
    try {
        zctOld_.push_back(cell);
    } catch (...) {
        return true;  // out of memory: a frame may hold it, so it stays (a collection or the context's end frees it)
    }
    cell->inZct = 1;
    if (cell->gcBytes >= kLargeCell) [[unlikely]]
        noteLarge(cell->gcBytes);
    if (zctCount() >= zctLimit_) [[unlikely]]
        noteZct();
    return true;
}

void VM::countRoots(int delta) noexcept
{
    // Every register of a live frame, and those up to `stackTop`. Above them the stack holds undefined: what returns or
    // unwinds clears its registers.
    JSValue* hi = stackTop;
    for (Frame* f = frameBase; f < frameTop; ++f)
        if (f->R + f->code->nregs > hi)
            hi = f->R + f->code->nregs;
    if (hi > stackInit)
        hi = stackInit;
    if (delta > 0) {
        // A register above them (an argument of a spread call that is over) may hold what goes now: no later frame finds
        // it there. What this costs is weighed into the table's next limit.
        for (JSValue* r = hi; r < stackInit; ++r)
            *r = JSValue::undefined();
        scanned_ = std::size_t(stackInit - stackBase);
    }
    for (JSValue* r = stackBase; r < hi; ++r) {
        if (!r->isCell())
            continue;
        detail::GCObject* c = r->asCell();
        if (delta > 0) {
            ++detail::cellCount(c);
            continue;
        }
        if (--detail::cellCount(c) != 0)
            continue;
        // Held by a frame only: back in the table. Out of memory, it is left to a collection, as a cell with no count.
        if (isVmCell(c)) {
            if (headerOf(c)->gc != kInZct)
                zctPush(c);
        } else if (!c->inZct) {
            try {
                zctOld_.push_back(c);
                c->inZct = 1;
            } catch (...) {
            }
        }
    }
}

#if defined(SCRIPT_JIT_ENV_OVERRIDE)
/// Testing (`SCRIPT_VM_AUDIT`), with the frames' cells counted: every cell of this family has at least as many counts as
/// cells and registers hold it. A cell with fewer is reported, and the process stops.
void VM::auditCounts() noexcept
{
    static const bool on = std::getenv("SCRIPT_VM_AUDIT") != nullptr;
    if (!on)
        return;
    std::unordered_map<CellPtr, std::uint32_t> refs;
    std::unordered_map<CellPtr, CellPtr> holder;
    std::unordered_map<CellPtr, bool> live;
    heap.forEachSlot([](void* context, void* slot) { (*static_cast<std::unordered_map<CellPtr, bool>*>(context))[static_cast<CellPtr>(slot)] = true; },
                     &live);
    struct State
    {
        std::unordered_map<CellPtr, std::uint32_t>* refs;
        std::unordered_map<CellPtr, CellPtr>* holder;
        detail::GcList kids;
    } st{&refs, &holder, {}};
    heap.forEachSlot(
        [](void* context, void* slot) {
            auto& s = *static_cast<State*>(context);
            s.kids.clear();
            children(static_cast<CellPtr>(slot), s.kids);
            for (detail::GCObject* k : s.kids)
                if (k && isVmCell(k)) {
                    ++(*s.refs)[k];
                    (*s.holder)[k] = static_cast<CellPtr>(slot);
                }
        },
        &st);
    JSValue* hi = stackTop;
    for (Frame* f = frameBase; f < frameTop; ++f)
        if (f->R + f->code->nregs > hi)
            hi = f->R + f->code->nregs;
    if (hi > stackInit)
        hi = stackInit;
    for (JSValue* r = stackBase; r < hi; ++r)
        if (r->isCell() && isVmCell(r->asCell()))
            ++refs[r->asCell()];
    int bad = 0;
    for (JSValue* r = stackBase; r < hi; ++r)
        if (r->isCell() && isVmCell(r->asCell()) && !live.count(r->asCell())) {
            std::fprintf(stderr, "audit: register %ld holds freed cell %p\n", long(r - stackBase), static_cast<void*>(r->asCell()));
            ++bad;
        }
    for (const auto& [c, n] : refs) {
        if (!live.count(c)) {
            std::fprintf(stderr, "audit: cell %p (kind %d) holds freed cell %p\n", static_cast<void*>(holder[c]),
                         int(kindOf(holder[c])), static_cast<void*>(c));
            ++bad;
            continue;
        }
        const CellHeader* h = headerOf(c);
        if ((h->flags & cellflags::kImmortal) || h->rc >= n)
            continue;
        std::fprintf(stderr, "audit: cell %p kind %d class %d count %u, held %u times", static_cast<void*>(c), int(kindOf(c)),
                     int(h->cls), h->rc, n);
        if (holder.count(c))
            std::fprintf(stderr, " (by %p kind %d class %d)", static_cast<void*>(holder[c]), int(kindOf(holder[c])),
                         int(headerOf(holder[c])->cls));
        if (frameTop > frameBase) {
            const Frame* f = frameTop - 1;
            std::fprintf(stderr, " in %s at %ld", f->code->name ? toUtf8(static_cast<StringCell*>(f->code->name)->text()).c_str() : "anon",
                         f->pc ? long(f->pc - f->code->code.data()) : -1L);
        }
        std::fprintf(stderr, "\n");
        ++bad;
    }
    if (bad)
        std::abort();
}
#endif

void VM::scanZct() noexcept
{
    if (immediate_ || tearingDown || ctx.tearingDown)
        return;
    countRoots(1);
    freeZct();
    countRoots(-1);
    setZctLimit();
    restoreCellBudget();
}

void VM::zctPush(CellPtr cell) noexcept
{
    if (zctTop_ == zctCap_) {
        const std::size_t used = std::size_t(zctTop_ - zctBase_);
        const std::size_t cap = std::max<std::size_t>(2 * used, std::max<std::size_t>(zctLimit_, 1024));
        CellPtr* grown;
        try {
            grown = static_cast<CellPtr*>(allocPayload(cap * sizeof(CellPtr)));
        } catch (...) {
            return;
        }
        if (used)
            std::memcpy(grown, zctBase_, used * sizeof(CellPtr));
        if (zctBase_)
            freePayload(zctBase_, std::size_t(zctCap_ - zctBase_) * sizeof(CellPtr));
        zctBase_ = grown;
        zctTop_ = grown + used;
        zctCap_ = grown + cap;
        zctEnd_ = zctBase_ + std::min(zctLimit_, cap);
    }
    *zctTop_++ = cell;
    headerOf(cell)->gc = kInZct;
}

void VM::setZctLimit() noexcept
{
    largeSinceScan_ = 0;
    zctLimit_ = std::max<std::size_t>({zctMin_, 2 * zctCount(), scanned_ / 4});
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (!zctMin_)
        zctLimit_ = 0;  // testing: every allocation scans
#endif
    if (std::size_t(zctCap_ - zctBase_) < zctLimit_) {
        // Room up to the limit, so that compiled code finds its end where the limit is.
        CellPtr* grown;
        try {
            grown = static_cast<CellPtr*>(allocPayload(zctLimit_ * sizeof(CellPtr)));
        } catch (...) {
            zctEnd_ = zctCap_;
            return;
        }
        const std::size_t used = std::size_t(zctTop_ - zctBase_);
        if (used)
            std::memcpy(grown, zctBase_, used * sizeof(CellPtr));
        if (zctBase_)
            freePayload(zctBase_, std::size_t(zctCap_ - zctBase_) * sizeof(CellPtr));
        zctBase_ = grown;
        zctTop_ = grown + used;
        zctCap_ = grown + zctLimit_;
    }
    zctEnd_ = zctBase_ + zctLimit_;
}

void VM::freeZct() noexcept
{
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    auditCounts();
#endif
    ++immediate_;
    // Every entry leaves the table. A cell at zero now is held by nothing: freeing it frees only what it alone held, none
    // of which is in the table at zero (it had this cell's count). Nothing is added while this runs (`immediate_`).
    std::size_t kept = 0;
    for (CellPtr* p = zctBase_; p < zctTop_; ++p) {
        CellHeader* h = headerOf(*p);
        h->gc = 0;
        if (h->rc == 0)
            zctBase_[kept++] = *p;
    }
    zctTop_ = zctBase_;
    // The old-family part keeps its room, for `zeroCount` to add to in line.
    std::size_t keptOld = 0;
    for (detail::GCObject* c : zctOld_) {
        c->inZct = 0;
        if (detail::cellCount(c) == 0)
            zctOld_[keptOld++] = c;
    }
    if (kept && !destroying_ && !tearingDown) {
        destroying_ = true;
        Freed freed;
        for (std::size_t k = 0; k < kept; ++k)
            destroyFrom(zctBase_[k], freed);
        countFreed(freed);
        destroying_ = false;
    } else {
        for (std::size_t k = 0; k < kept; ++k)
            destroy(zctBase_[k]);
    }
    for (std::size_t k = 0; k < keptOld; ++k)
        detail::destroyUnreferenced(zctOld_[k]);
    zctOld_.clear();
    --immediate_;
}

bool VM::freeHeldBack() noexcept
{
    if (!deferred || !zctCount() || immediate_ || destroying_ || tearingDown)
        return false;
    scanZct();
    return true;
}

void VM::rootsBegin() noexcept
{
    if (!deferred || immediate_ || tearingDown)
        return;
    // The table is empty for the whole collection, which writes the `gc` word of every cell it looks at.
    countRoots(1);
    freeZct();
    ++immediate_;
    rootsCounted_ = true;
}

void VM::rootsEnd() noexcept
{
    if (!rootsCounted_)
        return;
    rootsCounted_ = false;
    --immediate_;
    countRoots(-1);
    setZctLimit();
    restoreCellBudget();
}

void VM::collectCycles(bool minor) noexcept
{
    if (destroying_ || tearingDown)
        return;
    // A cell left to free has no count: it is no garbage of a cycle, and goes first.
    if (!deferredFree_.empty()) {
        ++immediate_;
        freeDeferred(0);
        --immediate_;
    }
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
            for (const Shape* s = r ? r->childRoot : nullptr; s; s = s->nextAux)
                if (s->proto == reinterpret_cast<JSObject*>(cell) && (h->gc & kCountMask))
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
            for (Shape* s = r->childRoot; s; s = s->nextAux) {
                s->proto = nullptr;
                detail::decRef(cell);
                try {
                    deadRoots.push_back(s);
                } catch (...) {
                }
            }
            r->childRoot = nullptr;
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
