// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The heap: reference-counted cells, objects, property tables.
#pragma once

#include <cstdint>
#include <cstddef>
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
    PropAccessor = 1u << 3,  ///< `value` holds the getter; the setter is the object's `setterOf`
    PropDefault = PropWritable | PropEnumerable | PropConfigurable,
};

/// One own property. `key` is either an interned StringCell or a SymbolCell, so
/// key comparison is pointer comparison.
/// A reference that counts only while what it points at is not pinned: the names of properties and the prototypes of many
/// objects, which every object made would otherwise add to and take from the same few counts.
template <class T>
class PinRef
{
public:
    PinRef() noexcept = default;
    PinRef(T* p) noexcept : p_(p)
    {
        if (p_ && !p_->pinned)
            detail::incRef(p_);
    }
    PinRef(const PinRef& o) noexcept : PinRef(o.p_) {}
    PinRef(PinRef&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~PinRef()
    {
        if (p_ && !p_->pinned)
            detail::decRef(p_);
    }
    PinRef& operator=(const PinRef& o) noexcept
    {
        PinRef tmp(o);
        std::swap(p_, tmp.p_);
        return *this;
    }
    PinRef& operator=(PinRef&& o) noexcept
    {
        std::swap(p_, o.p_);
        return *this;
    }
    void reset(T* p = nullptr) noexcept
    {
        PinRef tmp(p);
        std::swap(p_, tmp.p_);
    }
    T* get() const noexcept { return p_; }
    T* operator->() const noexcept { return p_; }
    T& operator*() const noexcept { return *p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }
    bool operator==(const PinRef& o) const noexcept { return p_ == o.p_; }
    bool operator!=(const PinRef& o) const noexcept { return p_ != o.p_; }

private:
    T* p_ = nullptr;
};

struct Prop
{
    PinRef<GCObject> key;
    Value value = Value::undefined();
    std::uint8_t attrs = PropDefault;

    bool accessor() const noexcept { return (attrs & PropAccessor) != 0; }
};

/// A property table: a vector's three pointers, which native code reads as one, over storage that may start out at the
/// end of the object itself, so that a small object is a single allocation.
class PropVec
{
public:
    using value_type = Prop;
    using iterator = Prop*;
    using const_iterator = const Prop*;

    PropVec() = default;
    PropVec(const PropVec&) = delete;
    PropVec& operator=(const PropVec&) = delete;
    ~PropVec()
    {
        clear();
        release();
    }

    /// Start out in `count` slots at `room`, which belong to the owner and are never freed from here.
    void adopt(Prop* room, std::size_t count) noexcept
    {
        begin_ = end_ = room;
        cap_ = room + count;
    }

    Prop* begin() noexcept { return begin_; }
    Prop* end() noexcept { return end_; }
    const Prop* begin() const noexcept { return begin_; }
    const Prop* end() const noexcept { return end_; }
    Prop* data() noexcept { return begin_; }
    const Prop* data() const noexcept { return begin_; }
    std::size_t size() const noexcept { return std::size_t(end_ - begin_); }
    std::size_t capacity() const noexcept { return std::size_t(cap_ - begin_); }
    bool empty() const noexcept { return end_ == begin_; }
    Prop& operator[](std::size_t i) noexcept { return begin_[i]; }
    const Prop& operator[](std::size_t i) const noexcept { return begin_[i]; }
    Prop& back() noexcept { return end_[-1]; }
    const Prop& back() const noexcept { return end_[-1]; }
    /// Slots allocated apart from the owner.
    std::size_t heapCapacity() const noexcept { return inCell() ? 0 : capacity(); }

    Prop& emplace_back()
    {
        if (end_ == cap_)
            reallocate(size() < 2 ? 4 : size() * 2);
        return *new (end_++) Prop();
    }
    void push_back(Prop&& p)
    {
        if (end_ == cap_)
            reallocate(size() < 2 ? 4 : size() * 2);
        new (end_++) Prop(std::move(p));
    }
    void push_back(const Prop& p)
    {
        Prop copy{p.key, p.value, p.attrs};
        push_back(std::move(copy));
    }
    void reserve(std::size_t n)
    {
        if (n > capacity())
            reallocate(n);
    }
    void shrink_to_fit()
    {
        if (inCell() || end_ == cap_)
            return;
        if (empty()) {
            release();
            begin_ = end_ = cap_ = nullptr;
            return;
        }
        reallocate(size());
    }
    Prop* erase(Prop* at)
    {
        for (Prop* p = at; p + 1 < end_; ++p)
            *p = std::move(p[1]);
        (--end_)->~Prop();
        return at;
    }
    void clear() noexcept
    {
        while (end_ != begin_)
            (--end_)->~Prop();
    }

private:
    void reallocate(std::size_t n)
    {
        Prop* fresh = PoolAlloc<Prop>().allocate(n);
        Prop* out = fresh;
        for (Prop* p = begin_; p != end_; ++p, ++out) {
            new (out) Prop(std::move(*p));
            p->~Prop();
        }
        release();
        begin_ = fresh;
        end_ = out;
        cap_ = fresh + n;
    }
    void release() noexcept
    {
        if (begin_ && !inCell())
            PoolAlloc<Prop>().deallocate(begin_, capacity());
    }

    Prop* begin_ = nullptr;
    Prop* end_ = nullptr;
    Prop* cap_ = nullptr;
    /// Whether the slots are still the ones in the owner's cell.
    bool inCell() const noexcept;
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

/// What objects with the same properties, added in the same order, on the same prototype, have in common: a hidden class.
/// A site that has seen one shape knows where a property is on every object of that shape without looking.
///
/// A shape says everything about an object's layout or nothing: an object that does something unusual -- deletes a
/// property, changes attributes, gets a new prototype, grows past `kMaxProperties` -- drops to no shape at all and is simply
/// looked up by name from then on. Shapes live as long as their runtime, and `Runtime::newShape` caps how many there are.
struct Shape
{
    static constexpr std::uint32_t kMaxProperties = 128;
    static constexpr std::size_t kMaxTransitions = 32;

    Ref<GCObject> key;  ///< the property this one added, null for a root
    std::uint8_t attrs = 0;
    std::uint32_t count = 0;
    /// Where this tree of shapes starts, and there, how many properties its objects have been seen to reach: a new one
    /// takes its table at that size and grows no more.
    Shape* root = nullptr;
    std::uint32_t expect = 0;
    /// There, too, how many objects have started from it.
    std::uint32_t births = 0;
    /// The shape this one adds a property to; null for a root.
    Shape* parent = nullptr;
    /// Whether an object of this shape has `k`, following the chain back to the root.
    bool has(const GCObject* k) const noexcept
    {
        for (const Shape* s = this; s; s = s->parent) {
            if (s->key.get() == k)
                return true;
        }
        return false;
    }
    struct Transition
    {
        GCObject* key;
        std::uint8_t attrs;
        Shape* to;
    };
    Vec<Transition> transitions;
};

struct DynObject : GCObject
{
    ObjClass cls = ObjClass::Plain;
    bool extensible = true;
    bool frozen = false;
    /// Something's prototype: changing which properties this has invalidates what sites know about objects beyond it.
    bool isProto = false;
    /// The properties started out in the cell, past the end of this.
    bool propsInCell = false;
    Shape* shape = nullptr;
    PinRef<DynObject> proto;
    /// Sets `proto`, keeping what shapes and sites know right. Every change of prototype goes through here.
    void setProto(DynObject* next);
    /// The layout is about to change in a way no shape describes.
    void dropShape() noexcept;
    /// The table moved or changed shape: the global object says so to native code that reads globals in place.
    void tableChanged() noexcept;
    /// Append a property known to be missing, moving to `next`, the shape that says so.
    void appendKnown(GCObject* key, Value value, Shape* next);
    PropVec props;
    /// What few objects need, kept apart so that the many do not carry it.
    struct Rare
    {
        /// Open-addressed map from key pointer to `props` index plus one, sized to
        /// a power of two. Built lazily -- small objects are faster to scan -- and
        /// flat, so a property table is one allocation rather than one per entry.
        Vec<std::uint32_t> index;
        std::unique_ptr<NativeSlot> nativeSlot;
        /// The setters of accessor properties that have one, by key.
        std::unique_ptr<Vec<std::pair<Ref<GCObject>, Value>>> setters;
        /// The shape an object with this one as its prototype starts from.
        Shape* childRoot = nullptr;
    };
    std::unique_ptr<Rare> rare;
    Rare& rareOf()
    {
        if (!rare)
            rare = std::make_unique<Rare>();
        return *rare;
    }
    Value setterOf(const Prop& p) const noexcept
    {
        if (rare && rare->setters) {
            for (const auto& [key, fn] : *rare->setters) {
                if (key.get() == p.key.get())
                    return fn;
            }
        }
        return Value::undefined();
    }
    /// Undefined removes it.
    void setSetter(const Prop& p, Value fn);

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

/// Arrays are dense, so a length is memory. Past this a request is refused with a `RangeError` rather than left to fail
/// in the allocator, which would end the process. A context with a memory limit has that limit to answer instead.
inline constexpr std::size_t kMaxDenseArrayLength = std::size_t(1) << 28;

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
inline bool PropVec::inCell() const noexcept
{
    const auto* owner = reinterpret_cast<const DynObject*>(reinterpret_cast<const char*>(this) - offsetof(DynObject, props));
    return owner->propsInCell && begin_ == reinterpret_cast<const Prop*>(owner + 1);
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

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
