// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The heap: reference-counted cells, objects, property tables.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "script/internal/heap.hpp"

namespace script {
namespace internal {
namespace dyn {

struct FunctionCode;
struct Runtime;

struct SymbolCell : GCObject
{
    String description;
    bool registered = false;  ///< created through Symbol.for, held by the registry

    SymbolCell() { kind = CellKind::Symbol; }
    ~SymbolCell() override;
    std::size_t heapSize() const override { return sizeof(*this) + description.size() * 2; }
};

// ------------------------------------------------------------ properties ---

enum PropAttr : std::uint8_t
{
    PropWritable = 1u << 0,
    PropEnumerable = 1u << 1,
    PropConfigurable = 1u << 2,
    PropAccessor = 1u << 3,  ///< `value` holds the getter, `setter` the setter
    PropDefault = PropWritable | PropEnumerable | PropConfigurable,
};

/// One own property. `key` is either an interned StringCell or a SymbolCell, so
/// key comparison is pointer comparison.
struct Prop
{
    Ref<GCObject> key;
    Value value = Value::undefined();
    Value setter = Value::undefined();
    std::uint8_t attrs = PropDefault;

    bool accessor() const noexcept { return (attrs & PropAccessor) != 0; }
};

enum class ObjClass : std::uint8_t
{
    Plain,
    Array,
    Function,
    Error,
    BooleanObject,
    NumberObject,
    StringObject,
    SymbolObject,
    Date,
    Map,
    Set,
    Promise,
};

struct DynObject : GCObject
{
    ObjClass cls = ObjClass::Plain;
    bool extensible = true;
    bool frozen = false;
    Ref<DynObject> proto;
    Vec<Prop> props;
    /// Open-addressed map from key pointer to `props` index plus one, sized to
    /// a power of two. Built lazily -- small objects are faster to scan -- and
    /// flat, so a property table is one allocation rather than one per entry.
    Vec<std::uint32_t> index;
    std::unique_ptr<NativeSlot> nativeSlot;

    DynObject() { kind = CellKind::Object; }
    void children(GcList& out) const override;
    void clearReferences() override;
    std::size_t heapSize() const override;

    Prop* findOwn(GCObject* key) noexcept;
    const Prop* findOwn(GCObject* key) const noexcept
    {
        return const_cast<DynObject*>(this)->findOwn(key);
    }
    /// Insert or overwrite, preserving insertion order for existing keys.
    Prop& putOwn(GCObject* key, Value value, std::uint8_t attrs = PropDefault);
    bool deleteOwn(GCObject* key);
    void reindex();
    /// Hand back capacity a shrunken property table no longer needs.
    void trim();

    static constexpr std::size_t kIndexThreshold = 8;

private:
    void indexInsert(std::uint32_t at) noexcept;
};

struct DynArray : DynObject
{
    Vec<Value> elements;

    DynArray() { cls = ObjClass::Array; }
    void children(GcList& out) const override;
    void clearReferences() override;
    std::size_t heapSize() const override;

    /// Append, charging any reallocation to the heap so that a loop which only
    /// grows one array still reaches the collector and the memory limit.
    void append(Value v);
    void appendAll(const Value* items, std::size_t count);
    /// Set the length, filling with `undefined` or dropping the tail.
    void resize(std::size_t n);
    /// Release element capacity once the array has shrunk well below it.
    void trimElements();
};

/// Wraps a primitive: `new Number(1)`, `Object("s")`, and `Date` (whose
/// primitive is its epoch milliseconds).
struct DynWrapper : DynObject
{
    Value primitive = Value::undefined();

    void children(GcList& out) const override;
    void clearReferences() override { primitive = Value::undefined(); }
};

enum class FnKind : std::uint8_t
{
    Bytecode,
    Native,
    Bound,
};

struct DynFunction : DynObject
{
    FnKind fnKind = FnKind::Native;
    bool isClassConstructor = false;
    bool isDerivedConstructor = false;
    bool isArrow = false;
    int arity = 0;
    String name;

    std::shared_ptr<const FunctionCode> code;  ///< Bytecode
    Vec<Ref<Box>> upvalues;           ///< Bytecode
    Ref<DynObject> homeObject;                  ///< `super` lookups start here

    NativeFunction native;  ///< Native

    Ref<DynFunction> boundTarget;  ///< Bound
    Value boundThis = Value::undefined();
    Vec<Value> boundArgs;

    DynFunction() { cls = ObjClass::Function; }
    void children(GcList& out) const override;
    void clearReferences() override;
    std::size_t heapSize() const override;
};

/// Backs both `Map` and `Set`; a Set simply ignores `value`.
struct DynMap : DynObject
{
    struct Entry
    {
        Value key = Value::undefined();
        Value value = Value::undefined();
        bool live = true;
    };
    Vec<Entry> entries;
    /// Open-addressed map from key hash to `entries` index plus one. Deleting
    /// leaves a tombstone in `entries`, which `compact` sweeps once the dead
    /// outnumber the living -- that is what keeps a churning Map bounded.
    Vec<std::uint32_t> lookup;
    std::size_t liveCount = 0;

    void children(GcList& out) const override;
    void clearReferences() override;
    std::size_t heapSize() const override;

    std::int64_t find(const Value& key) const;
    void insert(const Value& key, const Value& value);
    bool erase(const Value& key);
    void clear();
    void compact();

private:
    void rehash(std::size_t wanted);
    void place(std::uint32_t at);
};

struct DynPromise : DynObject
{
    enum class State : std::uint8_t
    {
        Pending,
        Fulfilled,
        Rejected,
    };
    struct Reaction
    {
        Value onFulfilled = Value::undefined();
        Value onRejected = Value::undefined();
        Ref<DynPromise> next;
    };

    State state = State::Pending;
    bool handled = false;
    Value result = Value::undefined();
    std::vector<Reaction> reactions;

    DynPromise() { cls = ObjClass::Promise; }
    void children(GcList& out) const override;
    void clearReferences() override;
};

// ------------------------------------------------------------- helpers ---

/// The JavaScript object behind a value, or null. A C# instance carries the
/// same `Type::Object` tag, so the cell kind decides, not the tag.
inline DynObject* asObject(const Value& v) noexcept
{
    GCObject* cell = detail::Access::ptr(v);
    return cell && cell->kind == CellKind::Object ? static_cast<DynObject*>(cell) : nullptr;
}
inline StringCell* asString(const Value& v) noexcept
{
    return v.isString() ? static_cast<StringCell*>(detail::Access::ptr(v)) : nullptr;
}
inline SymbolCell* asSymbol(const Value& v) noexcept
{
    return v.isSymbol() ? static_cast<SymbolCell*>(detail::Access::ptr(v)) : nullptr;
}
DynFunction* asFunction(const Value& v) noexcept;
DynArray* asArray(const Value& v) noexcept;

/// SameValueZero, used by Map/Set keys and `Array.prototype.includes`.
bool sameValueZero(const Value& a, const Value& b) noexcept;
bool strictEquals(const Value& a, const Value& b) noexcept;
std::size_t hashValue(const Value& v) noexcept;

}  // namespace dyn
}  // namespace internal
}  // namespace script
