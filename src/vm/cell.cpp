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
    ctx.maybeCollect();
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
    ctx.totalAllocated += bytes;
    return p;
}

void VM::freePayload(void* p, std::size_t bytes) noexcept
{
    PoolAlloc<std::uint8_t>().deallocate(static_cast<std::uint8_t*>(p), bytes);
    ctx.heapBytes -= bytes;
}

// -------------------------------------------------------------------- sizes ---

std::size_t cellBytesOf(const detail::GCObject* cell) noexcept
{
    switch (kindOf(cell)) {
        case Kind::Object: {
            const auto* o = reinterpret_cast<const JSObject*>(cell);
            switch (o->cls()) {
                case ObjClass::Array: return sizeof(JSArray);
                case ObjClass::TypedArray: return sizeof(JSTypedArray);
                case ObjClass::Function: return JSFunction::bytesFor(static_cast<const JSFunction*>(o)->nUpvals);
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
    if (o->cls() == ObjClass::Array)
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

void children(const detail::GCObject* cell, detail::GcList& out)
{
    VM& vm = vmOf(cell);
    switch (kindOf(cell)) {
        case Kind::Object: {
            auto* o = const_cast<JSObject*>(reinterpret_cast<const JSObject*>(cell));
            const std::uint32_t n = usedSlots(vm, o);
            for (std::uint32_t i = 0; i < n; ++i)
                appendIfCell(out, *o->slotAt(i));
            if (o->cls() == ObjClass::Array) {
                auto* a = static_cast<JSArray*>(o);
                for (std::uint32_t i = 0; i < a->length; ++i)
                    appendIfCell(out, a->elems[i]);
            } else if (o->cls() == ObjClass::TypedArray) {
                appendIfCell(out, static_cast<JSTypedArray*>(o)->buffer);
            }
            if (Rare* r = o->rare(); r && r->ext) {
                std::vector<JSValue> held;
                r->ext->values(held);
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
    // At teardown the values are already let go of (`clearReferences`), and the shapes may be gone.
    if (!vm.tearingDown) {
        const std::uint32_t n = usedSlots(vm, o);
        for (std::uint32_t i = 0; i < n; ++i)
            release(*o->slotAt(i));
    }
    switch (o->cls()) {
        case ObjClass::Array: {
            auto* a = static_cast<JSArray*>(o);
            if (!vm.tearingDown)
                for (std::uint32_t i = 0; i < a->length; ++i)
                    release(a->elems[i]);
            if (a->elems)
                vm.freePayload(a->elems, 8u * a->capacity);
            break;
        }
        case ObjClass::TypedArray: {
            if (!vm.tearingDown)
                release(static_cast<JSTypedArray*>(o)->buffer);
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
    destroyNow(cell);
    while (!pending_.empty()) {
        CellPtr next = pending_.back();
        pending_.pop_back();
        destroyNow(next);
    }
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
            if (o->cls() == ObjClass::TypedArray) {
                auto* t = static_cast<JSTypedArray*>(o);
                release(t->buffer);
                t->buffer = JSValue::undefined();
                t->length = 0;
            }
            if (o->cls() == ObjClass::Array) {
                auto* a = static_cast<JSArray*>(o);
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

void VM::releaseRoots() noexcept
{
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
                   &atoms.symSplit}) {
        if (*k)
            detail::decRef(*k);
        *k = nullptr;
    }
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
    for (JSObject*& p : protos.typedArrays) {
        if (p)
            detail::decRef(reinterpret_cast<CellPtr>(p));
        p = nullptr;
    }
    for (JSObject** p : {&protos.object, &protos.function, &protos.array, &protos.number, &protos.string, &protos.boolean,
                         &protos.symbol, &protos.bigint, &protos.generator, &protos.iterator,
                          &protos.nativeIterator, &protos.map, &protos.set, &protos.weakMap, &protos.weakSet, &protos.regexp, &protos.promise, &protos.arrayBuffer}) {
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
