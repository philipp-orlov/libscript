// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The engine instance behind `script::Context`: the heap, the string table,
// the shared VM state, and one runtime per engine, created the first time a
// language that needs it is used.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mem/local_heap.hpp"
#include "script/config.hpp"
#include "script/internal/common.hpp"
#include "script/internal/heap.hpp"
#include "script/internal/language.hpp"

namespace script {
namespace internal {

namespace typed {
struct Runtime;
}
namespace dyn {
struct Runtime;
}

/// What an engine's runtime owes the shared heap.
struct EngineRuntime
{
    virtual ~EngineRuntime() = default;
    /// Drop every root the runtime holds into the heap, before the heap is
    /// torn down. Nothing may allocate afterwards.
    virtual void releaseRoots() noexcept = 0;
    /// Called after every cycle collection: sweep engine-private tables,
    /// shrink caches.
    virtual void afterCollect() noexcept = 0;
    /// "   at ..." lines for the frames of this engine that are live.
    virtual std::string captureStack() const = 0;
};

/// A host class as the binding layer declared it, before any engine has seen
/// it. Each engine installs the members it has not installed yet the next
/// time it compiles or wraps something, so registering bindings costs nothing
/// in an engine that is never used.
struct HostClassInfo
{
    struct Member
    {
        HostMember spec;
        NativeFunction fn;
        NativeFunction setter;
    };

    std::string name;
    const void* typeKey = nullptr;  ///< set by `Class<T>`, null for a static class
    std::vector<Member> members;
    /// How many members each engine has installed, indexed by `EngineKind - 1`.
    std::size_t installed[2] = {0, 0};
};

struct ContextImpl
{
    explicit ContextImpl(Context& owner, const Context::Options& options);
    ~ContextImpl();
    ContextImpl(const ContextImpl&) = delete;
    ContextImpl& operator=(const ContextImpl&) = delete;

    Context* owner = nullptr;
    Context::Options options;
    mutable std::recursive_mutex mtx;
    /// Expires with the context, so an `Exception` that outlived it knows not
    /// to touch its value.
    std::shared_ptr<const bool> alive = std::make_shared<const bool>(true);

    // ---- heap ----
    GCObject* heapHead = nullptr;
    std::uint64_t heapBytes = 0;
    std::uint64_t totalAllocated = 0;
    std::uint64_t liveObjects = 0;
    std::uint64_t liveStrings = 0;
    std::uint64_t gcRuns = 0;
    std::uint64_t gcFreed = 0;
    /// Next collection fires when either budget is passed; both are reset from
    /// the post-collection size, so a program that legitimately grows is not
    /// punished with ever more frequent collections.
    std::uint64_t nextGcBytes = 0;
    std::uint64_t nextGcCells = 0;
    bool tearingDown = false;

    mem::LocalHeap heap{payloadHeapOptions()};
    /// Cells whose refcount hit zero while another cell was being destroyed.
    /// Threaded through `gcNext`, so unwinding a deep object graph is a loop
    /// rather than a recursion deep enough to overflow the C++ stack.
    GCObject* pendingFree = nullptr;
    bool destroying = false;

    /// Allocate a cell from the pool and register it with the heap. Budgets
    /// are checked first, so a refused allocation leaves nothing half-made.
    template <class T, class... Args>
    T* newCell(Args&&... args)
    {
        return newCellSized<T>(0, std::forward<Args>(args)...);
    }
    /// The same, with `extra` bytes of trailing storage -- the field slots of
    /// an instance, the elements of an array.
    template <class T, class... Args>
    T* newCellSized(std::size_t extra, Args&&... args)
    {
        maybeCollect();
        const std::size_t bytes = sizeof(T) + extra;
        reserve(bytes);  // a cell that would not fit is refused before any memory is taken
        void* memory = heap.allocate(bytes);
        T* cell;
        try {
            cell = new (memory) T(std::forward<Args>(args)...);
        } catch (...) {
            heap.deallocate(memory, bytes);
            throw;
        }
        cell->cellBytes = std::uint32_t(bytes);
        link(cell);
        return cell;
    }
    void destroyCell(GCObject* cell) noexcept;

    void link(GCObject* cell) noexcept;
    /// `link` for a cell whose size the caller knows and which is not a string: no virtual call.
    /// `head` then `tail`, in a cell with room for `room` characters, for a string that is about to be appended to.
    StringCell* newStringRoom(StringView head, StringView tail, std::size_t room);
    void linkKnown(GCObject* cell, std::uint32_t bytes) noexcept
    {
        cell->rt = this;
        cell->gcNext = heapHead;
        cell->gcPrev = nullptr;
        if (heapHead)
            heapHead->gcPrev = cell;
        heapHead = cell;
        cell->gcBytes = bytes;
        heapBytes += bytes;
        totalAllocated += bytes;
        ++liveObjects;
    }
    void unlink(GCObject* cell) noexcept;
    /// Break unreachable reference cycles. Everything else is freed eagerly.
    void collectCycles();
    /// Collect if either budget has been passed. Throws `OutOfMemory` when the
    /// heap is still over its limit afterwards, so it may only be called where
    /// the interpreter can unwind: cell allocation and the instruction tick.
    void maybeCollect()
    {
        // Nothing here for a program whose garbage reference counting already took care of.
        if (heapBytes < nextGcBytes && liveObjects + liveStrings < nextGcCells &&
            !(options.memoryLimit && heapBytes > options.memoryLimit))
            return;
        collectIfDue();
    }
    void collectIfDue();
    /// Refuse, with `Error(OutOfMemory)`, memory that would take the heap past `Options::memoryLimit`. Call it before
    /// building anything whose size a program chooses -- a cell, an array of a given length, a joined string -- so the
    /// limit holds for what is in flight and not only for what the cells already weigh. Collects cycles once before it
    /// gives up. Free when no limit is set.
    void reserve(std::size_t bytes)
    {
        if (options.memoryLimit && (bytes > options.memoryLimit || heapBytes + bytes > options.memoryLimit))
            reserveSlow(bytes);
    }
    void reserveSlow(std::size_t bytes);
    void charge(std::size_t bytes) noexcept;
    /// A cell whose trailing storage was filled in after it was linked is
    /// bigger than it was charged for. Settle the difference, or the heap's
    /// own total drifts away from what the cells say they weigh.
    void recharge(GCObject* cell) noexcept;
    /// Drop interned strings nothing refers to any more.
    void sweepStrings();

    // ---- strings ----
    StringTable stringTable;
    /// Strings up to this long are interned by `str`.
    static constexpr std::size_t kInternedLength = 32;
    /// The interned cell for `text`. Its initial reference is the table's own,
    /// so a string nothing else names has a count of exactly one and
    /// `sweepStrings` can recognise it.
    StringCell* intern(StringView text);
    StringCell* intern(const char* asciiText) { return intern(ascii(asciiText)); }
    /// A fresh string cell that is not interned.
    StringCell* newString(StringView text);
    /// The same with `head` and `tail` joined, without building the joined text first.
    StringCell* newString(StringView head, StringView tail);
    /// A string value. Short strings are interned: a program makes them by the
    /// million and they are usually names, so one lookup saves the cell.
    Value str(StringView text);
    Value str(const char* asciiText) { return str(ascii(asciiText)); }
    /// `str` of `head` and `tail` joined.
    Value concat(StringView head, StringView tail);
    Box* newBox(Value v);

    // ---- shared vm state ----
    /// Moves whenever an object something inherits from changes what it has: what a site learned about a property found
    /// on a prototype holds for as long as this does not move.
    std::uint64_t protoEpoch = 0;
    /// Call depth across both engines, so `stackLimit` holds however the
    /// languages nest.
    std::size_t callDepth = 0;
    std::uint32_t interruptCountdown = 0;
    bool interrupted = false;
    /// Which engine is executing host code right now, so a wrapper made by
    /// the binding layer takes the shape its caller expects. `None` outside
    /// any script call.
    EngineKind activeEngine = EngineKind::None;
    /// The engine used most recently, which is what `Auto` falls back on.
    EngineKind lastEngine = EngineKind::None;

    std::function<bool()> interrupt;
    std::function<void(std::string_view, bool)> print;
    std::function<bool(std::string&)> readLine;
    std::function<void(const Value&)> unhandledRejection;

    void write(StringView text, bool isError);
    /// The live frames of every engine, innermost first.
    std::string captureStack() const;

    // ---- host classes ----
    std::vector<std::unique_ptr<HostClassInfo>> hostClasses;
    HostClassInfo* findHostClass(std::string_view name) noexcept;
    HostClassInfo* hostClassForKey(const void* typeKey) noexcept;

    // ---- engines ----
    /// Created on first use, so a context that only ever runs one language
    /// pays for one engine.
    typed::Runtime& typed();
    dyn::Runtime& dyn();
    typed::Runtime* typedIfAny() const noexcept;
    dyn::Runtime* dynIfAny() const noexcept;
    /// The engine that should shape a new host wrapper: the one running, else
    /// the one asked for, else the one used last, else the only one present.
    EngineKind engineFor(Language language) const noexcept;

    /// An argument vector for one call, for the length of that call. A native
    /// function reads its arguments as a `std::vector`; making one per call is
    /// the hottest source of heap traffic there could be, so there is one buffer
    /// per nesting level, and a call in steady state allocates nothing.
    class ArgBuffer
    {
    public:
        explicit ArgBuffer(ContextImpl& rt);
        ~ArgBuffer()
        {
            args.clear();
            --rt_.argDepth_;
        }
        ArgBuffer(const ArgBuffer&) = delete;
        ArgBuffer& operator=(const ArgBuffer&) = delete;
        std::vector<Value>& args;

    private:
        ContextImpl& rt_;
    };

    /// Sets `activeEngine` for the duration of a host call and restores it.
    struct ActiveEngine
    {
        ActiveEngine(ContextImpl& rt, EngineKind kind) noexcept
            : rt_(rt), saved_(rt.activeEngine)
        {
            rt_.activeEngine = kind;
        }
        ~ActiveEngine() { rt_.activeEngine = saved_; }
        ActiveEngine(const ActiveEngine&) = delete;
        ActiveEngine& operator=(const ActiveEngine&) = delete;

    private:
        ContextImpl& rt_;
        EngineKind saved_;
    };

private:
    std::vector<std::unique_ptr<std::vector<Value>>> argBuffers_;
    std::size_t argDepth_ = 0;
    std::vector<Value>& argBufferAt(std::size_t depth);
    void trimBuffers() noexcept;
    std::unique_ptr<EngineRuntime> typed_;
    std::unique_ptr<EngineRuntime> dyn_;
    EngineRuntime* runtimes_[2] = {nullptr, nullptr};
    std::size_t runtimeCount_ = 0;
};

}  // namespace internal
}  // namespace script
