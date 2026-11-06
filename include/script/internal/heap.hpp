// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What every engine allocates: the cell header and the two cell shapes both engines share -- strings and boxes.
//
// Every cell is one allocation with this header in front. Lifetime is
// reference counted, which reclaims the common acyclic case the moment the
// last handle goes away; cycles are found by `ContextImpl::collectCycles`,
// which walks the `gcNext` chain of all live cells.
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include "base/hash.hpp"
#include "script/internal/common.hpp"

namespace script {

namespace detail {

struct GCObject;
/// A collector's worklist: how many cells it holds follows the heap, so the buffer stays out of the process heap.
using GcList = internal::Vec<GCObject*>;

/// What a cell is. The first group is shared, the second belongs to the typed
/// engine, the third to the dynamic one; a cell's group decides which engine
/// the public API asks about it.
enum class CellKind : std::uint8_t
{
    String,
    Box,  ///< a local a closure captured, a `ref` parameter points at, or a boxed `let`

    Instance,    ///< a C# class or boxed struct: header plus field slots
    Array,       ///< a C# array
    Delegate,
    List,        ///< System.Collections.Generic.List<T>
    Dictionary,
    Builder,     ///< System.Text.StringBuilder
    Native,      ///< a C++ object exposed through `Class<T>`, readable by both engines

    Symbol,      ///< a JavaScript symbol
    Object,      ///< any JavaScript object: plain, array, function, error, ...
};

constexpr bool isTypedCell(CellKind k) noexcept
{
    return k >= CellKind::Instance && k <= CellKind::Builder;
}
constexpr bool isDynamicCell(CellKind k) noexcept
{
    return k == CellKind::Symbol || k == CellKind::Object;
}

/// Header shared by every heap cell.
struct GCObject
{
    std::uint32_t rc = 1;
    std::int32_t gcRef = 0;  ///< cycle collector scratch
    /// Pool slot size in bytes; 0 when the cell came from the general
    /// allocator. Cells with trailing storage can exceed the pool's largest
    /// size class, which is why this is not a 16-bit field.
    std::uint32_t cellBytes = 0;
    std::uint32_t gcBytes = 0;  ///< what this cell was last charged to the heap
    internal::ContextImpl* rt = nullptr;
    GCObject* gcNext = nullptr;
    GCObject* gcPrev = nullptr;
    // Last and small on purpose: a derived class's first members move into the padding after these, which takes 8 bytes
    // off a string, an instance and an array -- a good part of a small object.
    CellKind kind = CellKind::Object;
    std::uint8_t gcColor = 0;

    GCObject() = default;
    GCObject(const GCObject&) = delete;
    GCObject& operator=(const GCObject&) = delete;
    virtual ~GCObject();

    /// Append every cell this one references.
    virtual void children(GcList& out) const { (void)out; }
    /// Drop every outgoing reference. Used to break a proven-dead cycle so
    /// reference counting can finish the job.
    virtual void clearReferences() {}
    virtual std::size_t heapSize() const { return sizeof(GCObject); }

    /// Tell the heap that this cell's variable-length storage grew. Cells are
    /// charged at allocation, so a container that keeps growing afterwards has
    /// to say so or it would never move the collector.
    void chargeGrowth(std::size_t bytes) noexcept;
    /// Refuse, with `Error(OutOfMemory)`, a growth of `bytes` that would take the heap past its limit. Say it before
    /// the container grows; `chargeGrowth` settles the account afterwards.
    void reserveGrowth(std::size_t bytes);
    /// The same for a vector about to hold `wanted` elements, a no-op while its capacity already covers them.
    template <class Vec>
    void reserveFor(const Vec& v, std::size_t wanted)
    {
        if (wanted > v.capacity())
            reserveGrowth((wanted - v.capacity()) * sizeof(typename Vec::value_type));
    }
    /// Charge whatever `v` reallocated since its capacity was `before`.
    template <class Vec>
    void chargeIfGrown(const Vec& v, std::size_t before) noexcept
    {
        if (v.capacity() > before)
            chargeGrowth((v.capacity() - before) * sizeof(typename Vec::value_type));
    }
};

}  // namespace detail

namespace internal {

using detail::CellKind;
using detail::GCObject;
using detail::GcList;

// --------------------------------------------------------- shared cells ---

/// A string: immutable, its characters in the cell itself, so a string is one
/// allocation. Interned strings double as property names in the dynamic engine,
/// where key comparison is then pointer comparison.
struct StringCell : GCObject
{
    bool interned = false;  ///< also present in the context's string table
    std::uint32_t length;
    std::size_t hash_ = 0;  ///< 0 until someone asks

    explicit StringCell(StringView a, StringView b = StringView()) : length(std::uint32_t(a.size() + b.size()))
    {
        kind = CellKind::String;
        auto* chars = reinterpret_cast<Char*>(this + 1);
        if (!a.empty())
            std::memcpy(chars, a.data(), a.size() * sizeof(Char));
        if (!b.empty())
            std::memcpy(chars + a.size(), b.data(), b.size() * sizeof(Char));
    }
    ~StringCell() override;

    StringView text() const noexcept { return StringView(reinterpret_cast<const Char*>(this + 1), length); }
    std::size_t hash() noexcept
    {
        if (!hash_)
            hash_ = hashString(text());
        return hash_;
    }
    std::size_t heapSize() const override { return sizeof(*this) + length * sizeof(Char); }
};
static_assert(alignof(StringCell) >= alignof(Char));

/// The interned strings: open addressing over a flat table of cell pointers,
/// looked up by content without building a key, and with no node per entry.
class StringTable
{
public:
    StringCell* find(StringView text, std::size_t hash) const noexcept;
    /// `cell` is not in the table, and its hash is computed.
    void insert(StringCell* cell);
    void erase(StringCell* cell) noexcept;
    std::size_t size() const noexcept { return count_; }
    /// Remove every cell for which `dead` holds, and hand it to `release`; the
    /// table is sized down to what is left.
    template <class Dead, class Release>
    void sweep(Dead dead, Release release)
    {
        std::size_t kept = 0;
        for (StringCell*& slot : slots_) {
            if (!slot)
                continue;
            if (dead(slot)) {
                release(slot);
                slot = nullptr;
            } else {
                ++kept;
            }
        }
        if (kept != count_)
            rebuild(kept);
    }
    void clear() noexcept
    {
        Vec<StringCell*>().swap(slots_);
        count_ = 0;
    }

private:
    void rebuild(std::size_t liveCells);
    Vec<StringCell*> slots_;
    std::size_t count_ = 0;
};

/// A variable that outlived its frame: a local a lambda or closure captured,
/// or the target of a `ref`/`out` parameter. One indirection, one cell, no
/// scope chain.
struct Box : GCObject
{
    Value value;

    Box() { kind = CellKind::Box; }
    void children(GcList& out) const override;
    void clearReferences() override { value = Value(); }
    std::size_t heapSize() const override { return sizeof(*this); }
};

/// Set on a cell that wraps a C++ instance registered through `Class<T>`.
struct NativeSlot
{
    const void* typeKey = nullptr;
    void* data = nullptr;
    void* owner = nullptr;
    Context::Finalizer finalizer = nullptr;

    ~NativeSlot()
    {
        if (finalizer)
            finalizer(owner ? owner : data);
    }
};

/// A C++ instance handed to script by the host, or made by a C# `new` of a
/// bound class. Both engines read it: the typed engine through the class it
/// was bound to, the dynamic engine through that class's prototype. A
/// wrapper made by a JavaScript `new` is an ordinary JavaScript object
/// carrying the same `NativeSlot`, because a constructor's `this` has to be
/// one.
struct NativeCell : GCObject
{
    /// The typed engine's `TypeInfo` for the bound class, filled in when
    /// that engine first looks; null until then.
    const void* typedType = nullptr;
    NativeSlot slot;

    NativeCell() { kind = CellKind::Native; }
    std::size_t heapSize() const override { return sizeof(*this); }
};

using base::hashPointer;
using base::tableSizeFor;

}  // namespace internal
}  // namespace script
