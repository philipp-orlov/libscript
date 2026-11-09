// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The heap: the cell pool, reference counting, cycle collection, the string
// table, and the engine runtimes the context creates on demand.
#include "script/internal/context.hpp"

#include <algorithm>

#if SCRIPT_ENGINE_TYPED
#include "script/internal/typed/runtime.hpp"
#endif
#if SCRIPT_ENGINE_DYNAMIC
#include "script/internal/dyn/runtime.hpp"
#endif

namespace script {

#if SCRIPT_ENGINE_TYPED && SCRIPT_ENGINE_DYNAMIC
// Native code tells an object of the dynamic engine by the shape it expects at this offset, with no look at what kind of
// cell it is, so whatever else a value tagged as an object can be must keep something there that is never a shape.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
static_assert(offsetof(internal::dyn::DynObject, shape) == offsetof(internal::typed::CSInstance, type));
static_assert(offsetof(internal::dyn::DynObject, shape) == offsetof(internal::typed::CSList, type));
static_assert(offsetof(internal::dyn::DynObject, shape) == offsetof(internal::typed::CSDict, type));
static_assert(offsetof(internal::dyn::DynObject, shape) == offsetof(internal::typed::CSBuilder, type));
static_assert(offsetof(internal::dyn::DynObject, shape) == offsetof(internal::NativeCell, typedType));
#pragma GCC diagnostic pop
#endif

// ---------------------------------------------------------- refcounting ---

namespace detail {

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
static_assert(offsetof(GCObject, rc) == kCellCountOffset, "value.hpp counts at this offset");
#pragma GCC diagnostic pop

GCObject::~GCObject() = default;

void GCObject::chargeGrowth(std::size_t bytes) noexcept
{
    if (!rt || bytes == 0)
        return;
    gcBytes += std::uint32_t(bytes);
    rt->charge(bytes);
}

void GCObject::reserveGrowth(std::size_t bytes)
{
    if (rt && bytes)
        rt->reserve(bytes);
}

/// Freeing one cell releases its children, which may free them too. Doing that
/// recursively overflows the stack on a long list or a deep tree, so only the
/// outermost call actually destroys: nested releases park their cell on a
/// worklist threaded through `pendingNext` and the outer loop drains it.
void destroyUnreferenced(GCObject* cell) noexcept
{
    // A pinned cell stays until its context ends, which frees it with everything else. Not every holder of one keeps a
    // count of it -- native code writes it into the objects it makes without one -- so its count says nothing.
    if (cell->pinned) {
        cellCount(cell) = 1;
        return;
    }
    internal::ContextImpl* rt = cell->rt;
    if (!rt) {
        cell->~GCObject();
        ::operator delete(cell);
        return;
    }
    if (rt->tearingDown)
        return;  // the teardown loop owns every remaining cell
    if (rt->destroying) {
        rt->unlink(cell);
        cell->pendingNext = rt->pendingFree;
        rt->pendingFree = cell;
        return;
    }

    rt->destroying = true;
    rt->unlink(cell);
    rt->destroyCell(cell);
    while (rt->pendingFree) {
        GCObject* next = rt->pendingFree;
        rt->pendingFree = next->pendingNext;
        next->rt = nullptr;
        rt->destroyCell(next);
    }
    rt->destroying = false;
}

}  // namespace detail

namespace internal {

// --------------------------------------------------------- shared cells ---

StringCell::~StringCell()
{
    if (interned && rt)
        rt->stringTable.erase(this);
    if (bits & kRope) {
        // Onto the context's worklist when they die too, so a long chain of halves frees without recursing.
        detail::decRef(halves()[0]);
        detail::decRef(halves()[1]);
    }
}

void StringCell::clearReferences()
{
    if (!(bits & kRope))
        return;
    StringCell* head = halves()[0];
    StringCell* tail = halves()[1];
    halves()[0] = halves()[1] = nullptr;
    detail::decRef(head);
    detail::decRef(tail);
}

StringView StringCell::outOfLine() noexcept
{
    if (bits & kRope) {
        // From the payload pool, not the cell heap, whose slots are all taken for cells.
        const std::size_t bytes = std::size_t(length) * sizeof(Char);
        auto* chars = static_cast<Char*>(bytes <= payload_cache::kMaxBytes ? payload_cache::take(bytes)
                                                                            : payload_cache::takeLarge(bytes));
        // Left to right with a stack of what is still to copy: a rope built by appending is as deep as it is long.
        std::vector<const StringCell*> pending{halves()[1], halves()[0]};
        std::size_t at = 0;
        while (!pending.empty()) {
            const StringCell* piece = pending.back();
            pending.pop_back();
            if (piece->bits & kRope) {
                auto* const* parts = reinterpret_cast<StringCell* const*>(piece + 1);
                pending.push_back(parts[1]);
                pending.push_back(parts[0]);
                continue;
            }
            const StringView text = piece->text();
            if (!text.empty())
                std::memcpy(chars + at, text.data(), text.size() * sizeof(Char));
            at += text.size();
        }
        StringCell* head = halves()[0];
        StringCell* tail = halves()[1];
        buffer() = chars;
        halves()[1] = nullptr;
        bits = std::uint8_t((bits & ~kRope) | kFlattened);
        const std::uint32_t was = gcBytes;
        gcBytes = std::uint32_t(heapSize());
        rt->heapBytes += gcBytes - was;
        detail::decRef(head);
        detail::decRef(tail);
    }
    return StringView(buffer(), length);
}

StringCell* StringTable::find(StringView text, std::size_t hash) const noexcept
{
    if (slots_.empty())
        return nullptr;
    const std::size_t mask = slots_.size() - 1;
    for (std::size_t i = hash & mask;; i = (i + 1) & mask) {
        StringCell* cell = slots_[i];
        if (!cell)
            return nullptr;
        if (cell->hash_ == hash && cell->text() == text)
            return cell;
    }
}

void StringTable::insert(StringCell* cell)
{
    if ((count_ + 1) * 2 > slots_.size())
        rebuild((count_ + 1) * 2);
    const std::size_t mask = slots_.size() - 1;
    std::size_t i = cell->hash_ & mask;
    while (slots_[i])
        i = (i + 1) & mask;
    slots_[i] = cell;
    ++count_;
}

// Backward-shift deletion: no tombstones, so a table used as a name churn stays as
// short to probe as a fresh one.
void StringTable::erase(StringCell* cell) noexcept
{
    if (slots_.empty())
        return;
    const std::size_t mask = slots_.size() - 1;
    std::size_t i = cell->hash_ & mask;
    while (slots_[i] != cell) {
        if (!slots_[i])
            return;
        i = (i + 1) & mask;
    }
    for (std::size_t j = (i + 1) & mask; slots_[j]; j = (j + 1) & mask) {
        const std::size_t home = slots_[j]->hash_ & mask;
        if (((j - home) & mask) >= ((j - i) & mask)) {
            slots_[i] = slots_[j];
            i = j;
        }
    }
    slots_[i] = nullptr;
    --count_;
}

void StringTable::rebuild(std::size_t liveCells)
{
    Vec<StringCell*> next(tableSizeFor(liveCells * 2, 64), nullptr);
    const std::size_t mask = next.size() - 1;
    std::size_t count = 0;
    for (StringCell* cell : slots_) {
        if (!cell)
            continue;
        std::size_t i = cell->hash_ & mask;
        while (next[i])
            i = (i + 1) & mask;
        next[i] = cell;
        ++count;
    }
    slots_.swap(next);
    count_ = count;
}

void Box::children(GcList& out) const
{
    if (GCObject* p = detail::Access::ptr(value))
        out.push_back(p);
}

// -------------------------------------------------------------- context ---

namespace {

/// Live cells before a collection is worth having: reference counting has freed everything but cycles by then, and a
/// collection walks every live cell whether it finds anything or not.
constexpr std::uint64_t kMinCellBudget = 1u << 17;

}  // namespace

ContextImpl::ContextImpl(Context& ownerContext, const Context::Options& opts)
    : owner(&ownerContext), options(opts)
{
    nextGcBytes = std::max<std::uint64_t>(options.gcThreshold, 1u << 20);
    nextGcCells = kMinCellBudget;
}

ContextImpl::~ContextImpl()
{
    tearingDown = true;
    for (std::size_t i = 0; i < runtimeCount_; ++i)
        runtimes_[i]->releaseRoots();

    // Everything still linked is either garbage in a cycle or held by a Value
    // the embedder forgot. Pin it all first: cutting one cell's edges can free
    // another, and that would invalidate the list we are walking.
    GcList all;
    allCells(all);
    for (GCObject* cell : all)
        ++cell->rc;
    for (GCObject* cell : all)
        cell->clearReferences();
    pendingFree = nullptr;
    largeCells.clear();
    for (GCObject* cell : all)
        destroyCell(cell);
    stringTable.clear();

    typed_.reset();
    dyn_.reset();
}

/// Let go of what `count` values refer to, as a dying cell does: nothing is destroyed from here, so a long chain of them
/// takes no stack.
void ContextImpl::releaseAll(const Value* values, std::size_t count) noexcept
{
    // A dying tree is far from the cache: ask for every child before the first one is needed.
    for (std::size_t i = 0; i < count; ++i)
        if (GCObject* child = detail::Access::ptr(values[i]))
            __builtin_prefetch(child, 1);
    const bool inTeardown = tearingDown;
    for (std::size_t i = 0; i < count; ++i) {
        GCObject* child = detail::Access::ptr(values[i]);
        if (!child || --child->rc != 0 || inTeardown)
            continue;
        if (!child->rt || !destroying) {
            detail::destroyUnreferenced(child);
            continue;
        }
        unlink(child);
        child->pendingNext = pendingFree;
        pendingFree = child;
    }
}

void ContextImpl::destroyCell(GCObject* cell) noexcept
{
    const std::uint32_t bytes = cell->cellBytes;
    // The kinds a program makes by the million get out of the virtual call.
    switch (cell->kind) {
#if SCRIPT_ENGINE_TYPED
        case CellKind::Instance: {
            // By hand, not through `~Value` and `decRef`, because a dying tree spends its time here.
            auto* instance = static_cast<typed::CSInstance*>(cell);
            releaseAll(instance->slots(), instance->count);
            break;
        }
        case CellKind::Array: {
            // An array of numbers holds nothing to release, and `~GCObject` does nothing.
            auto* array = static_cast<typed::CSArray*>(cell);
            if (array->store == typed::ElemStore::Ref)
                array->typed::CSArray::~CSArray();
            break;
        }
#endif
#if SCRIPT_ENGINE_DYNAMIC
        case CellKind::Object: {
            // A plain object with nothing unusual about it: its values and its prototype are all it holds.
            auto* object = static_cast<dyn::DynObject*>(cell);
            // What sites and native code learned about where a property is may name this one: it no longer holds.
            if (object->isProto)
                ++protoEpoch;
            if (object->rare || object->props.heapCapacity() != 0 ||
                (object->cls != dyn::ObjClass::Plain && (object->cls != dyn::ObjClass::Array || !object->props.empty()))) {
                cell->~GCObject();
                break;
            }
            static_assert(sizeof(dyn::Prop) == sizeof(Value));
            if (object->cls == dyn::ObjClass::Array) {
                // Its elements, in place of properties.
                auto* array = static_cast<dyn::DynArray*>(object);
                if (!array->numbersOnly)
                    releaseAll(array->store.begin(), array->size());
                array->freeStorage();
            } else {
                releaseAll(reinterpret_cast<const Value*>(object->props.data()), object->props.size());
            }
            if (dyn::DynObject* proto = object->proto.get(); proto && !proto->pinned && --proto->rc == 0) {
                if (tearingDown)
                    break;
                unlink(proto);
                proto->pendingNext = pendingFree;
                pendingFree = proto;
            }
            break;
        }
#endif
        case CellKind::String: {
            auto* text = static_cast<StringCell*>(cell);
            if (Char* chars = text->takeBuffer()) {
                const std::size_t bytes = std::size_t(text->length) * sizeof(Char);
                if (bytes <= payload_cache::kMaxBytes)
                    payload_cache::give(chars, bytes);
                else
                    payload_cache::giveLarge(chars, bytes);
            }
            cell->~GCObject();
            break;
        }
        default: cell->~GCObject(); break;
    }
    if (bytes)
        heap.deallocate(cell, bytes);
    else
        ::operator delete(cell);
}

void ContextImpl::link(GCObject* cell) noexcept
{
    cell->rt = this;
    cell->gcBytes = std::uint32_t(cell->heapSize());
    heapBytes += cell->gcBytes;
    totalAllocated += cell->gcBytes;
    if (cell->cellBytes > mem::LocalHeap::kMaxSmall)
        noteLarge(cell);
    if (cell->kind == CellKind::String)
        ++liveStrings;
    else
        ++liveObjects;
}

void ContextImpl::unlink(GCObject* cell) noexcept
{
    if (cell->cellBytes > mem::LocalHeap::kMaxSmall)
        forgetLarge(cell);
    heapBytes -= std::min<std::uint64_t>(heapBytes, cell->gcBytes);
    if (cell->kind == CellKind::String)
        --liveStrings;
    else
        --liveObjects;
    cell->rt = nullptr;
}

void ContextImpl::noteLarge(GCObject* cell)
{
    cell->gcRef = std::int32_t(largeCells.size());
    largeCells.push_back(cell);
}

void ContextImpl::forgetLarge(GCObject* cell) noexcept
{
    GCObject* last = largeCells.back();
    largeCells[std::size_t(cell->gcRef)] = last;
    last->gcRef = cell->gcRef;
    largeCells.pop_back();
}

void ContextImpl::allCells(GcList& out) const
{
    out.reserve(liveObjects + liveStrings);
    heap.forEachSlot(
        [](void* list, void* slot) {
            auto* cell = static_cast<GCObject*>(slot);
            static_cast<GcList*>(list)->push_back(cell);
        },
        &out);
    out.insert(out.end(), largeCells.begin(), largeCells.end());
}

void ContextImpl::charge(std::size_t bytes) noexcept
{
    heapBytes += bytes;
    totalAllocated += bytes;
}

void ContextImpl::recharge(GCObject* cell) noexcept
{
    const std::size_t now = cell->heapSize();
    if (now <= cell->gcBytes)
        return;
    charge(now - cell->gcBytes);
    cell->gcBytes = std::uint32_t(now);
}

void ContextImpl::reserveSlow(std::size_t bytes)
{
    // Garbage in unreachable cycles may be what stands in the way.
    if (!tearingDown && !destroying && bytes <= options.memoryLimit)
        collectCycles();
    if (bytes > options.memoryLimit || heapBytes + bytes > options.memoryLimit)
        throw Error(ErrorCode::OutOfMemory, "the context exceeded its memory limit");
}

void ContextImpl::collectIfDue()
{
    if (tearingDown || destroying)
        return;
    const std::uint64_t cells = liveObjects + liveStrings;
    const bool overBudget = options.memoryLimit && heapBytes > options.memoryLimit;
    if (!overBudget && heapBytes < nextGcBytes && cells < nextGcCells)
        return;

    // Mostly the cells made since the last time: cycles of a closure and its own cell die young. All of it when the heap
    // has doubled since it last was looked at whole, or is over its limit.
    const bool full = overBudget || heapBytes > std::max<std::uint64_t>(bytesAfterFull * 2, 4u << 20);
    collectCycles(!full);

    // Both budgets are re-armed from the size that survived, so a program that
    // legitimately grows is not punished with ever more frequent collections.
    const std::uint64_t baseBytes = options.gcThreshold ? options.gcThreshold : (1u << 20);
    nextGcBytes = std::max<std::uint64_t>(baseBytes, heapBytes * 2);
    nextGcCells = std::max<std::uint64_t>(kMinCellBudget, (liveObjects + liveStrings) * 2);
    if (options.memoryLimit && heapBytes > options.memoryLimit)
        throw Error(ErrorCode::OutOfMemory, "the context exceeded its memory limit");
}

void ContextImpl::collectCycles(bool minor)
{
    ++gcRuns;
    GcList kids;

    GcList all;
    allCells(all);
    // A minor collection's world is the young cells. What an old one refers to counts as a reference from outside, which
    // keeps it -- sound, since what is found is still only what nothing outside the set refers to.
    if (minor) {
        std::size_t kept = 0;
        for (GCObject* cell : all) {
            if (cell->young)
                all[kept++] = cell;
        }
        all.resize(kept);
    } else {
        for (GCObject* cell : all)
            cell->young = 1;
    }

    // 1. Assume every reference is internal, then subtract the ones we can see.
    for (GCObject* cell : all) {
        cell->gcRef = std::int32_t(cell->rc);
        cell->gcColor = 0;
    }
    for (GCObject* cell : all) {
        kids.clear();
        cell->children(kids);
        for (GCObject* kid : kids) {
            if (kid && kid->rt == this && kid->young)
                --kid->gcRef;
        }
    }

    // 2. Whatever still has a reference is reachable from outside the heap.
    GcList work;
    for (GCObject* cell : all) {
        if (cell->gcRef > 0 && !cell->gcColor) {
            cell->gcColor = 1;
            work.push_back(cell);
        }
    }
    while (!work.empty()) {
        GCObject* cell = work.back();
        work.pop_back();
        kids.clear();
        cell->children(kids);
        for (GCObject* kid : kids) {
            if (kid && kid->young && !kid->gcColor) {
                kid->gcColor = 1;
                work.push_back(kid);
            }
        }
    }

    // `gcRef` was scratch until here; freeing a big cell below needs its place back.
    for (std::size_t i = 0; i < largeCells.size(); ++i)
        largeCells[i]->gcRef = std::int32_t(i);

    // 3. The rest is a cycle nothing can reach. Pin it, cut its edges, release.
    GcList garbage;
    std::uint64_t bytes = 0;
    for (GCObject* cell : all) {
        const std::uint32_t was = cell->gcBytes;
        cell->gcBytes = std::uint32_t(cell->heapSize());
        bytes += cell->gcBytes;
        // Survivors are old from here on.
        cell->young = 0;
        if (!cell->gcColor) {
            ++cell->rc;
            garbage.push_back(cell);
        } else if (minor) {
            // A minor collection sees part of the heap: what it learns of a cell's size it adds as a change.
            heapBytes += cell->gcBytes;
            heapBytes -= std::min<std::uint64_t>(heapBytes, was);
        }
    }
    if (!minor)
        heapBytes = bytes;
    gcFreed += garbage.size();
    GcList().swap(all);
    for (GCObject* cell : garbage)
        cell->clearReferences();
    for (GCObject* cell : garbage)
        detail::decRef(cell);
    if (!minor)
        bytesAfterFull = heapBytes;

    // In this order: an engine dropping what it held may leave strings nothing names.
    for (std::size_t i = 0; i < runtimeCount_; ++i)
        runtimes_[i]->afterCollect();
    sweepStrings();
    trimBuffers();
    heap.trim();
    payloadPool().trim();
}

/// An interned string whose only reference is the table itself is not spelled
/// anywhere in any live object, so it can go. This is what stops a program
/// that builds strings or property names at run time from growing the table
/// for as long as the process lives.
void ContextImpl::sweepStrings()
{
    stringTable.sweep([](StringCell* text) { return text->rc <= 1; },
                      [](StringCell* text) {
                          // Clear the flag first: the destructor would otherwise look
                          // for this entry during the sweep.
                          text->interned = false;
                          detail::decRef(text);
                      });
}

// -------------------------------------------------------------- strings ---

StringCell* ContextImpl::newString(StringView text)
{
    return newCellSized<StringCell>(text.size() * sizeof(Char), text);
}

StringCell* ContextImpl::newString(StringView head, StringView tail)
{
    return newCellSized<StringCell>((head.size() + tail.size()) * sizeof(Char), head, tail);
}

StringCell* ContextImpl::newStringRoom(StringView head, StringView tail, std::size_t room)
{
    room = std::max(room, head.size() + tail.size());
    return newCellSized<StringCell>(room * sizeof(Char), head, tail);
}

StringCell* ContextImpl::intern(StringView text)
{
    const std::size_t hash = hashString(text);
    if (StringCell* existing = stringTable.find(text, hash))
        return existing;
    StringCell* cell = newString(text);
    cell->hash_ = hash;
    cell->interned = true;
    stringTable.insert(cell);
    return cell;
}

Value ContextImpl::str(StringView text)
{
    if (text.size() <= kInternedLength)
        return detail::Access::make(Type::String, intern(text));
    return detail::Access::adopt(Type::String, newString(text));
}

Value ContextImpl::concat(StringCell* head, StringCell* tail)
{
    // A rope only past a length where copying starts to cost more than the cell; short ones are interned as before.
    const std::size_t total = std::size_t(head->length) + tail->length;
    if (total < kRopeLength || head->length == 0 || tail->length == 0)
        return concat(head->text(), tail->text());
    if (total > 0xFFFFFFF0u)
        throw Error(ErrorCode::OutOfMemory, "string too long");
    return detail::Access::adopt(Type::String, newCellSized<StringCell>(StringCell::kRopeBytes, head, tail));
}

Value ContextImpl::concat(StringView head, StringView tail)
{
    const std::size_t total = head.size() + tail.size();
    if (total > kInternedLength)
        return detail::Access::adopt(Type::String, newString(head, tail));
    Char joined[kInternedLength];
    if (!head.empty())
        std::memcpy(joined, head.data(), head.size() * sizeof(Char));
    if (!tail.empty())
        std::memcpy(joined + head.size(), tail.data(), tail.size() * sizeof(Char));
    return str(StringView(joined, total));
}

Box* ContextImpl::newBox(Value v)
{
    auto* cell = newCell<Box>();
    cell->value = std::move(v);
    return cell;
}

void ContextImpl::write(StringView text, bool isError)
{
    const std::string utf8 = toUtf8(text);
    if (print) {
        print(utf8, isError);
        return;
    }
    std::fwrite(utf8.data(), 1, utf8.size(), isError ? stderr : stdout);
}

std::string ContextImpl::captureStack() const
{
    std::string out;
    for (std::size_t i = 0; i < runtimeCount_; ++i)
        out += runtimes_[i]->captureStack();
    return out;
}

void ContextImpl::trimBuffers() noexcept
{
    // What a nesting spike or one huge spread call left behind; the levels in use are not touched.
    constexpr std::size_t kKeepLevels = 64;
    constexpr std::size_t kKeepCapacity = 256;
    if (argBuffers_.size() > argDepth_ + kKeepLevels)
        argBuffers_.resize(argDepth_ + kKeepLevels);
    for (std::size_t i = argDepth_; i < argBuffers_.size(); ++i) {
        if (argBuffers_[i]->capacity() > kKeepCapacity)
            std::vector<Value>().swap(*argBuffers_[i]);
    }
}

std::vector<Value>& ContextImpl::argBufferAt(std::size_t depth)
{
    // Outer buffers are in use while inner ones are added, hence one allocation per buffer.
    if (depth >= argBuffers_.size())
        argBuffers_.emplace_back(std::make_unique<std::vector<Value>>());
    return *argBuffers_[depth];
}

ContextImpl::ArgBuffer::ArgBuffer(ContextImpl& rt) : args(rt.argBufferAt(rt.argDepth_)), rt_(rt)
{
    ++rt_.argDepth_;
}

// -------------------------------------------------------------- engines ---

typed::Runtime& ContextImpl::typed()
{
#if SCRIPT_ENGINE_TYPED
    if (!typed_) {
        typed_ = std::make_unique<typed::Runtime>(*this);
        runtimes_[runtimeCount_++] = typed_.get();
    }
    return *typedIfAny();
#else
    throw Error(ErrorCode::NotSupported, "this build of libscript has no C# engine");
#endif
}

dyn::Runtime& ContextImpl::dyn()
{
#if SCRIPT_ENGINE_DYNAMIC
    if (!dyn_) {
        dyn_ = std::make_unique<dyn::Runtime>(*this);
        runtimes_[runtimeCount_++] = dyn_.get();
    }
    return *dynIfAny();
#else
    throw Error(ErrorCode::NotSupported,
                "this build of libscript has no TypeScript or JavaScript engine");
#endif
}

typed::Runtime* ContextImpl::typedIfAny() const noexcept
{
#if SCRIPT_ENGINE_TYPED
    return static_cast<typed::Runtime*>(typed_.get());
#else
    return nullptr;
#endif
}

dyn::Runtime* ContextImpl::dynIfAny() const noexcept
{
#if SCRIPT_ENGINE_DYNAMIC
    return static_cast<dyn::Runtime*>(dyn_.get());
#else
    return nullptr;
#endif
}

EngineKind ContextImpl::engineFor(Language language) const noexcept
{
    if (activeEngine != EngineKind::None)
        return activeEngine;
    if (language != Language::Auto) {
        if (const LanguageInfo* info = languageInfo(language))
            return info->engine;
    }
    if (lastEngine != EngineKind::None)
        return lastEngine;
    if (typed_ && !dyn_)
        return EngineKind::Typed;
    if (dyn_ && !typed_)
        return EngineKind::Dynamic;
    const auto& table = languageTable();
    return table.empty() ? EngineKind::None : table.front()->engine;
}

}  // namespace internal
}  // namespace script

namespace script {
namespace internal {

HostClassInfo* ContextImpl::findHostClass(std::string_view name) noexcept
{
    for (auto& info : hostClasses) {
        if (info->name == name)
            return info.get();
    }
    return nullptr;
}

HostClassInfo* ContextImpl::hostClassForKey(const void* typeKey) noexcept
{
    for (auto& info : hostClasses) {
        if (info->typeKey == typeKey)
            return info.get();
    }
    return nullptr;
}

}  // namespace internal
}  // namespace script
