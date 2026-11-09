// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What every engine allocates: the cell header and the two cell shapes both engines share -- strings and boxes.
//
// Every cell is one allocation with this header in front. Lifetime is
// reference counted, which reclaims the common acyclic case the moment the
// last handle goes away; cycles are found by `ContextImpl::collectCycles`,
// which asks the allocator for the cells it has: no cell carries a list of them.
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

    BigInt,      ///< a JavaScript bigint
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
    union
    {
        internal::ContextImpl* rt = nullptr;
        GCObject* pendingNext;  ///< past `unlink`, on the list of cells waiting to be destroyed
    };
    // Last and small on purpose: a derived class's first members move into the padding after these, which takes 8 bytes
    // off a string, an instance and an array -- a good part of a small object.
    std::uint8_t gcColor : 1;
    /// Holders do not count references to this: something keeps it for as long as the context lasts.
    std::uint8_t pinned : 1;
    /// Made since the last collection: what a minor collection looks at.
    std::uint8_t young : 1;
    /// Last, so that the first thing a derived class keeps here comes right after it: native code reads both at once.
    CellKind kind = CellKind::Object;

    /// From here on it stays. The count this adds is never given back, so it does not matter what was counted before.
    void pin() noexcept
    {
        if (!pinned) {
            pinned = 1;
            ++rc;
        }
    }

    GCObject() : gcColor(0), pinned(0), young(1) {}
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
///
/// A long concatenation makes a rope instead: a cell that holds a count of each
/// of its two halves where the characters would be, so `a + b` copies nothing.
/// The first look at its characters copies them once into a buffer of its own
/// and lets the halves go, which makes building a string piece by piece linear.
/// Strings hold no objects, so the counts alone free a rope.
struct StringCell : GCObject
{
    enum Bits : std::uint8_t
    {
        kInSource = 1,   ///< a name some compiled program spells out; there are only as many of those as the source has
        kRope = 2,       ///< two halves follow the cell, not yet joined
        kFlattened = 4,  ///< a rope whose characters are in a buffer of its own
        kSlice = 8,      ///< part of another string, whose characters it points into and which it holds a count of
        kOutOfLine = kRope | kFlattened | kSlice,  ///< the characters are not after the cell: what native code tests
    };
    bool interned = false;  ///< also present in the context's string table
    std::uint8_t bits = 0;  ///< `Bits`; one byte, so a string still fits the padding of its header
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
    /// A rope of two strings, each of which it holds a count of.
    StringCell(StringCell* head, StringCell* tail) : bits(kRope), length(head->length + tail->length)
    {
        kind = CellKind::String;
        detail::incRef(head);
        detail::incRef(tail);
        halves()[0] = head;
        halves()[1] = tail;
    }
    /// `count` characters of `whole`, a string that is not a slice, from `chars` within it.
    StringCell(StringCell* whole, const Char* chars, std::uint32_t count) : bits(kSlice), length(count)
    {
        kind = CellKind::String;
        detail::incRef(whole);
        buffer() = const_cast<Char*>(chars);
        halves()[1] = whole;
    }
    ~StringCell() override;

    StringView text() const noexcept
    {
        if (!(bits & kOutOfLine)) [[likely]]
            return StringView(reinterpret_cast<const Char*>(this + 1), length);
        return const_cast<StringCell*>(this)->outOfLine();
    }
    std::size_t hash() noexcept
    {
        if (!hash_)
            hash_ = hashString(text());
        return hash_;
    }
    std::size_t heapSize() const override
    {
        if (bits & (kRope | kSlice))
            return cellBytes;
        if (bits & kFlattened)
            return cellBytes + length * sizeof(Char);
        return std::max<std::size_t>(cellBytes, sizeof(*this) + length * sizeof(Char));
    }
    /// How many characters fit in the cell as allocated: more than `length` for one made to be appended to.
    std::size_t capacity() const noexcept
    {
        if (bits & kOutOfLine)
            return length;
        return cellBytes > sizeof(*this) ? (cellBytes - sizeof(*this)) / sizeof(Char) : length;
    }
    Char* mutableChars() noexcept { return reinterpret_cast<Char*>(this + 1); }
    /// The string a slice is part of; any other string is its own.
    StringCell* whole() noexcept { return bits & kSlice ? halves()[1] : this; }
    static constexpr std::size_t kRopeBytes = 2 * sizeof(void*);
    bool inSource() const noexcept { return bits & kInSource; }
    void markInSource() noexcept { bits |= kInSource; }
    /// A rope's halves are let go at teardown, before any cell is destroyed.
    void clearReferences() override;
    /// The buffer of a flattened rope, which its context frees: the cell no longer names it.
    Char* takeBuffer() noexcept
    {
        Char* chars = bits & kFlattened ? buffer() : nullptr;
        bits &= std::uint8_t(~kFlattened);
        return chars;
    }

private:
    StringCell** halves() noexcept { return reinterpret_cast<StringCell**>(this + 1); }
    Char*& buffer() noexcept { return *reinterpret_cast<Char**>(this + 1); }
    /// The characters of a rope, joined on the first call.
    StringView outOfLine() noexcept;
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
