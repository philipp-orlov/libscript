// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The heap: reference-counted cells, instances, arrays, delegates.
//
// Every cell is one allocation. An instance carries its fields in the slots
// that follow its header, an array carries its elements the same way, and
// neither has a hash table in it: the compiler turned every name into an
// index before the program started.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "script/internal/heap.hpp"
#include "script/internal/typed/metadata.hpp"

namespace script {
namespace internal {
namespace typed {

struct Runtime;

// ------------------------------------------------------------- instance ---

/// An object of a user-declared class, or a boxed struct.
///
/// The field slots follow the header in the same allocation. `slots()` is
/// `this + 1`; field `n` of the most derived type sits at the index the
/// compiler baked into the instruction stream, so a field access never
/// searches for anything.
struct CSInstance : GCObject
{
    std::uint32_t count = 0;
    const TypeInfo* type = nullptr;

    CSInstance() { kind = CellKind::Instance; }
    ~CSInstance() override;

    Value* slots() noexcept { return reinterpret_cast<Value*>(this + 1); }
    const Value* slots() const noexcept { return reinterpret_cast<const Value*>(this + 1); }
    Value& at(std::uint32_t i) noexcept { return slots()[i]; }
    const Value& at(std::uint32_t i) const noexcept { return slots()[i]; }

    void children(GcList& out) const override;
    void clearReferences() override;
    std::size_t heapSize() const override { return sizeof(*this) + count * sizeof(Value); }

    static std::size_t bytesFor(std::uint32_t fields) noexcept
    {
        return sizeof(CSInstance) + fields * sizeof(Value);
    }
    /// Construct the slots in place. Called by the allocator, once.
    void initSlots(std::uint32_t n) noexcept;
};

// ---------------------------------------------------------------- array ---

/// How an array stores its elements. A `Value` is 16 bytes; an `int[]` that
/// paid that would cost four times what the language says it does, so arrays
/// of primitives keep raw elements and rebuild the Value on access.
enum class ElemStore : std::uint8_t
{
    Ref,  ///< Value, for strings, objects, arrays, delegates and `object`
    I1,
    I2,
    I4,
    I8,
    R4,
    R8,
};

ElemStore elemStoreOf(const TypeInfo* elementType) noexcept;
std::size_t elemStoreWidth(ElemStore s) noexcept;

struct CSArray : GCObject
{
    ElemStore store = ElemStore::Ref;
    /// Cached from `elemType`, because every element access needs it and the
    /// indirection is the whole cost of a raw array read.
    TypeKind elemKind = TypeKind::Object;
    std::uint32_t length = 0;
    const TypeInfo* elemType = nullptr;

    CSArray() { kind = CellKind::Array; }
    ~CSArray() override;

    void* data() noexcept { return reinterpret_cast<void*>(this + 1); }
    const void* data() const noexcept { return reinterpret_cast<const void*>(this + 1); }
    Value* refs() noexcept { return reinterpret_cast<Value*>(this + 1); }
    const Value* refs() const noexcept { return reinterpret_cast<const Value*>(this + 1); }

    Value get(std::uint32_t i) const noexcept;
    void set(std::uint32_t i, const Value& v) noexcept;

    void children(GcList& out) const override;
    void clearReferences() override;
    std::size_t heapSize() const override
    {
        return sizeof(*this) + length * elemStoreWidth(store);
    }

    static std::size_t bytesFor(ElemStore s, std::uint32_t n) noexcept
    {
        return sizeof(CSArray) + std::size_t(n) * elemStoreWidth(s);
    }
    /// Zero or null every element. Called by the allocator, once.
    void initElements() noexcept;
};

// ------------------------------------------------------------- delegate ---

/// A bound method: `Action`, `Func<...>`, an event handler, a lambda. The
/// captured environment is a flat vector of boxes, not a chain of scopes, so
/// reading a captured variable is one indexed load.
struct CSDelegate : GCObject
{
    const TypeInfo* type = nullptr;  ///< the delegate type
    const MethodInfo* method = nullptr;
    Value target;                    ///< the receiver, null for a static target
    Vec<Ref<Box>> captured;
    /// A multicast chain: `a += b` links them. Invocation runs them in order
    /// and returns the last result.
    Ref<CSDelegate> next;

    CSDelegate() { kind = CellKind::Delegate; }
    void children(GcList& out) const override;
    void clearReferences() override;
    std::size_t heapSize() const override
    {
        return sizeof(*this) + captured.capacity() * sizeof(void*);
    }
};

// ----------------------------------------------------------- collections ---

struct CSList : GCObject
{
    const TypeInfo* type = nullptr;
    Vec<Value> items;

    CSList() { kind = CellKind::List; }
    void children(GcList& out) const override;
    void clearReferences() override;
    std::size_t heapSize() const override
    {
        return sizeof(*this) + items.capacity() * sizeof(Value);
    }
    void add(Value v);
};

/// An open-addressed dictionary whose entries live in one flat vector, so the
/// table is two allocations however many pairs it holds.
struct CSDict : GCObject
{
    struct Entry
    {
        Value key;
        Value value;
        std::size_t hash = 0;
        bool live = false;
    };

    const TypeInfo* type = nullptr;
    Vec<Entry> entries;
    Vec<std::uint32_t> index;  ///< entry index plus one, 0 means empty
    std::uint32_t liveCount = 0;

    CSDict() { kind = CellKind::Dictionary; }
    void children(GcList& out) const override;
    void clearReferences() override;
    std::size_t heapSize() const override
    {
        return sizeof(*this) + entries.capacity() * sizeof(Entry) +
               index.capacity() * sizeof(std::uint32_t);
    }

    Entry* find(const Value& key) noexcept;
    void put(const Value& key, Value value);
    bool erase(const Value& key);
    void rebuild();
    /// Drop tombstones once they outnumber the live entries, so a table that
    /// is used as a queue does not grow without bound.
    void compact();
};

struct CSBuilder : GCObject
{
    const TypeInfo* type = nullptr;
    String buf;

    CSBuilder() { kind = CellKind::Builder; }
    std::size_t heapSize() const override { return sizeof(*this) + buf.capacity() * sizeof(Char); }
};

// ----------------------------------------------------------- utilities ---

/// The declared type of any heap cell, or null for a primitive.
const TypeInfo* typeOfCell(const GCObject* cell) noexcept;

/// The hash `Dictionary<K,V>` uses: value equality for primitives and strings,
/// identity for everything else -- which is what `object.GetHashCode` does.
std::size_t hashValue(const Value& v) noexcept;
bool valueEquals(const Value& a, const Value& b) noexcept;

}  // namespace typed
}  // namespace internal
}  // namespace script
