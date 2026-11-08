// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The heap: reference-counted cells, objects, property tables.
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
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
    bool isPrivate = false;   ///< a class's `#x`: no reflection sees it, and only its own object has it

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

struct BigIntCell : GCObject
{
    bool negative = false;
    Vec<std::uint32_t> digits;  ///< little end first, with no zeros at the top: zero has none

    BigIntCell() { kind = CellKind::BigInt; }
    std::size_t heapSize() const override { return sizeof(*this) + digits.capacity() * sizeof(std::uint32_t); }
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

/// One property's value. Its name and attributes are the object's shape's to say, or, for an object without one, in the
/// table of its `Rare`: `DynObject::keyAt` and `attrsAt` ask. Objects of a shape share them, so a property takes no more
/// than what is different in each object.
struct Prop
{
    Value value = Value::undefined();
};

/// A property table: a pointer and two counts, over storage that may start out at the end of the object itself, so that a
/// small object is a single allocation.
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
        begin_ = room;
        size_ = 0;
        cap_ = std::uint32_t(count);
    }

    Prop* begin() noexcept { return begin_; }
    Prop* end() noexcept { return begin_ + size_; }
    const Prop* begin() const noexcept { return begin_; }
    const Prop* end() const noexcept { return begin_ + size_; }
    Prop* data() noexcept { return begin_; }
    const Prop* data() const noexcept { return begin_; }
    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return cap_; }
    bool empty() const noexcept { return size_ == 0; }
    Prop& operator[](std::size_t i) noexcept { return begin_[i]; }
    const Prop& operator[](std::size_t i) const noexcept { return begin_[i]; }
    Prop& back() noexcept { return begin_[size_ - 1]; }
    const Prop& back() const noexcept { return begin_[size_ - 1]; }
    /// Slots allocated apart from the owner.
    std::size_t heapCapacity() const noexcept { return inCell() ? 0 : capacity(); }

    Prop& emplace_back()
    {
        if (size_ == cap_)
            reallocate(size_ < 2 ? 4 : std::size_t(size_) * 2);
        return *new (begin_ + size_++) Prop();
    }
    void reserve(std::size_t n)
    {
        if (n > capacity())
            reallocate(n);
    }
    void shrink_to_fit()
    {
        if (inCell() || size_ == cap_)
            return;
        if (empty()) {
            release();
            begin_ = nullptr;
            cap_ = 0;
            return;
        }
        reallocate(size_);
    }
    Prop* erase(Prop* at)
    {
        for (Prop* p = at; p + 1 < end(); ++p)
            *p = std::move(p[1]);
        begin_[--size_].~Prop();
        return at;
    }
    void clear() noexcept
    {
        while (size_)
            begin_[--size_].~Prop();
    }

private:
    void reallocate(std::size_t n)
    {
        Prop* fresh = PoolAlloc<Prop>().allocate(n);
        for (std::uint32_t i = 0; i < size_; ++i) {
            new (fresh + i) Prop(std::move(begin_[i]));
            begin_[i].~Prop();
        }
        release();
        begin_ = fresh;
        cap_ = std::uint32_t(n);
    }
    void release() noexcept
    {
        if (begin_ && !inCell())
            PoolAlloc<Prop>().deallocate(begin_, capacity());
    }

    Prop* begin_ = nullptr;
    std::uint32_t size_ = 0;
    std::uint32_t cap_ = 0;
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
    Generator,
    RegExp,
    RegExpStringIterator,
    BigIntObject,
    ArrayIterator,
    ArrayBuffer,
    SharedArrayBuffer,
    TypedArray,
    DataView,
    Proxy,  ///< a DynProxy over something that cannot be called
    IteratorHelper,
};

/// What objects with the same properties, added in the same order, on the same prototype, have in common: a hidden class.
/// A site that has seen one shape knows where a property is on every object of that shape without looking.
///
/// A shape says everything about an object's layout or nothing: an object that does something unusual -- deletes a
/// property, changes attributes, gets a new prototype, grows past `kMaxProperties` -- drops to no shape at all and is simply
/// looked up by name from then on. Shapes live as long as their runtime, and `Runtime::newShape` caps how many there are.
struct Shape
{
    static constexpr std::uint32_t kMaxProperties = 1024;
    /// A shape pins its name for the runtime's life: a name made at run time, `o['f' + i]`, gets that only this far, so
    /// such names cannot pile up without bound.
    static constexpr std::uint32_t kMaxMadeNames = 128;
    /// How many different properties may follow a shape when they are names worked out at run time. An object used as a
    /// dictionary stops there, and has no shape. Names in the source have no such limit: a program has only so many,
    /// and a large one has more than this many classes with a static member, or literals starting differently.
    static constexpr std::size_t kMaxTransitions = 32;

    Ref<GCObject> key;  ///< the property this one added, null for a root
    std::uint8_t attrs = 0;
    /// Every property of this shape has the attributes of a plain assignment: nothing to look up for most objects.
    bool plainAttrs = true;
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
    /// A property, by its place in the table.
    struct Entry
    {
        GCObject* key;
        std::uint8_t attrs;
    };
    /// All of them in order, made the first time something asks.
    const Entry* entries() const;
    /// Where `k` is in the table, or -1.
    std::int32_t indexOf(const GCObject* k) const;
    struct Transition
    {
        GCObject* key;
        std::uint8_t attrs;
        Shape* to;
    };
    Vec<Transition> transitions;
    /// The shape that adding `k` with `attrs` leads to, if one was made already.
    Shape* after(const GCObject* k, std::uint8_t attrs) const noexcept;
    void addTransition(GCObject* k, std::uint8_t attrs, Shape* to);

private:
    /// Past a few transitions, key to place plus one, as `lookup_` is for properties.
    Vec<std::uint32_t> transitionIndex_;
    /// What `entries` and `indexOf` keep, for the few shapes they are asked about. The lookup is the same table as an
    /// object's own index: key to place plus one.
    mutable Vec<Entry> table_;
    mutable Vec<std::uint32_t> lookup_;
    mutable std::uint32_t asked_ = 0;
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
    /// An array whose elements are bare numbers; see `DynArray`. Here because it is the one byte left before `shape`.
    bool numbersOnly = false;
    Shape* shape = nullptr;
    PinRef<DynObject> proto;
    /// A proxy, whose handler answers for it: its class says so, or it is a function of the proxy kind.
    bool fnKindIsProxy() const noexcept;
    /// Sets `proto`, keeping what shapes and sites know right. Every change of prototype goes through here.
    void setProto(DynObject* next);
    /// The layout is about to change in a way no shape describes. The names and attributes move to the object's own table.
    void dropShape() noexcept;
    /// Its properties, in table order.
    GCObject* keyAt(std::size_t i) const noexcept { return shape ? shape->entries()[i].key : rare->keys[i].key.get(); }
    std::uint8_t attrsAt(std::size_t i) const noexcept
    {
        if (!shape)
            return rare->keys[i].attrs;
        return shape->plainAttrs ? std::uint8_t(PropDefault) : shape->entries()[i].attrs;
    }
    /// Those of a property of this object, which `p` must be.
    GCObject* keyOf(const Prop& p) const noexcept { return keyAt(std::size_t(&p - props.data())); }
    std::uint8_t attrsOf(const Prop& p) const noexcept { return attrsAt(std::size_t(&p - props.data())); }
    bool accessorOf(const Prop& p) const noexcept { return (attrsOf(p) & PropAccessor) != 0; }
    /// Change one; only an object without a shape can have this differ from its neighbours.
    void setAttrsAt(std::size_t i, std::uint8_t attrs) noexcept { rare->keys[i].attrs = attrs; }
    /// The table moved or changed shape: the global object says so to native code that reads globals in place.
    void tableChanged() noexcept;
    /// Append a property known to be missing, moving to `next`, the shape that says so.
    void appendKnown(Value value, Shape* next);
    PropVec props;
    /// What few objects need, kept apart so that the many do not carry it.
    struct Rare
    {
        /// The names and attributes of the properties of an object without a shape, one for each in `props`.
        struct Key
        {
            PinRef<GCObject> key;
            std::uint8_t attrs = PropDefault;
        };
        Vec<Key> keys;
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
                if (key.get() == keyOf(p))
                    return fn;
            }
        }
        return Value::undefined();
    }
    /// Undefined removes it.
    void setSetter(const Prop& p, Value fn) { setSetterOf(keyOf(p), std::move(fn)); }
    void setSetterOf(GCObject* key, Value fn);

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
    void moveKeysHere() noexcept;
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

/// What stands for `undefined` among the elements of an array of numbers. A signaling NaN with a payload of its own: no
/// arithmetic produces one, and it is never loaded as a number without being checked for.
inline constexpr std::uint64_t kUndefinedElement = 0x7FF4DEAD00000001ull;
/// The payload of an `undefined` that marks a hole in an array of values: no element there at all. It reads as undefined
/// wherever it goes; only the array's own questions -- `in`, its keys, the methods that skip holes -- tell it apart. In an
/// array of numbers a hole is `kUndefinedElement`, and an `undefined` put there makes it an array of values.
inline constexpr std::uint64_t kHolePayload = 0x484f4c45ull;
inline Value holeValue() noexcept
{
    Value v(kHolePayload);
    detail::Access::retag(v, Type::Undefined);
    return v;
}
inline bool isHole(const Value& v) noexcept { return v.isUndefined() && detail::Access::u8(v) == kHolePayload; }

/// The elements as values, with the part of a vector's interface the engine uses. The storage is a pointer and two counts,
/// and may start out in the array's own cell. Only `DynArray::values` hands one out, having made sure that is what they are.
class ElemVec
{
public:
    using value_type = Value;
    using iterator = Value*;
    using const_iterator = const Value*;

    Value* begin() noexcept { return static_cast<Value*>(data_); }
    Value* end() noexcept { return begin() + size_; }
    const Value* begin() const noexcept { return static_cast<const Value*>(data_); }
    const Value* end() const noexcept { return begin() + size_; }
    Value* data() noexcept { return begin(); }
    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return cap_; }
    bool empty() const noexcept { return size_ == 0; }
    Value& operator[](std::size_t i) noexcept { return begin()[i]; }
    const Value& operator[](std::size_t i) const noexcept { return begin()[i]; }
    Value& back() noexcept { return begin()[size_ - 1]; }
    Value& front() noexcept { return begin()[0]; }
    void pop_back() noexcept { begin()[--size_].~Value(); }

private:
    friend struct DynArray;
    void* data_ = nullptr;
    std::uint32_t size_ = 0;
    std::uint32_t cap_ = 0;  ///< in elements of what it holds now
};

/// Dense, and in one of two forms, which `numbersOnly` tells apart. Bare doubles at 8 bytes each while every element is a
/// number or `undefined`, which is what most large arrays hold; or values at 16. The first element that needs a value
/// turns it into the second for good. Small arrays have their elements in the cell.
struct DynArray : DynObject
{
    ElemVec store;

    DynArray()
    {
        cls = ObjClass::Array;
        numbersOnly = true;
    }
    ~DynArray() override;
    void children(GcList& out) const override;
    void clearReferences() override;
    std::size_t heapSize() const override;

    std::size_t size() const noexcept { return store.size_; }
    bool empty() const noexcept { return store.size_ == 0; }
    double* numbers() noexcept { return static_cast<double*>(store.data_); }
    const double* numbers() const noexcept { return static_cast<const double*>(store.data_); }
    /// One element, which must exist.
    Value at(std::size_t i) const noexcept
    {
        if (!numbersOnly) {
            const Value& v = store[i];
            return v.isUndefined() ? Value::undefined() : v;
        }
        std::uint64_t bits;
        std::memcpy(&bits, numbers() + i, 8);
        return bits == kUndefinedElement ? Value::undefined() : Value(numbers()[i]);
    }
    /// Set one that exists.
    void put(std::size_t i, const Value& v)
    {
        if (numbersOnly) {
            if (v.type() == Type::Double) {
                numbers()[i] = detail::Access::r8(v);
                return;
            }
            box();
        }
        store[i] = v;
    }
    /// The elements as values, which for an array of numbers changes what it is from here on. For what is not worth doing
    /// twice; the rest asks `numbersOnly`.
    ElemVec& values()
    {
        if (numbersOnly)
            box();
        return store;
    }
    void box();
    /// Whether element `i`, which is within the length, is there at all.
    bool hasElement(std::size_t i) const noexcept
    {
        if (!numbersOnly)
            return !isHole(store[i]);
        std::uint64_t bits;
        std::memcpy(&bits, numbers() + i, 8);
        return bits != kUndefinedElement;
    }
    void makeHole(std::size_t i);
    void appendHole();
    /// No holes at all, as the quick ways through an array assume.
    bool isPacked() const noexcept;

    /// Append, charging any reallocation to the heap so that a loop which only
    /// grows one array still reaches the collector and the memory limit.
    void append(Value v);
    void appendAll(const Value* items, std::size_t count);
    /// Set the length, filling with `undefined` or dropping the tail.
    void resize(std::size_t n);
    /// Take out `[from, to)`, or put `count` values in at `at`.
    void removeRange(std::size_t from, std::size_t to);
    void insertRange(std::size_t at, const Value* items, std::size_t count);
    /// Replace the lot, in whichever form they fit.
    void assignAll(const Value* items, std::size_t count);
    void assignNumbers(const double* items, std::size_t count);
    void reverse();
    /// Release element capacity once the array has shrunk well below it.
    void trimElements();
    /// Room for `n` elements of the present form.
    void reserveElements(std::size_t n);
    /// Give the elements' storage back, whose values have already been dealt with.
    void freeStorage() noexcept;
    /// How many bytes of the cell are for elements.
    std::size_t roomInCell() const noexcept { return cellBytes > sizeof(DynArray) ? cellBytes - sizeof(DynArray) : 0; }
    /// A new cell with this room is laid out here; see `Runtime::newArray`.
    void adoptRoom(std::size_t bytes) noexcept;

private:
    bool inCell() const noexcept { return store.data_ == static_cast<const void*>(this + 1); }
    std::size_t elementBytes() const noexcept { return numbersOnly ? sizeof(double) : sizeof(Value); }
    void moveTo(std::size_t capacity);
    void growFor(std::size_t n);
};
static_assert(sizeof(Value) == 16, "an element is two of what a number takes");

/// Wraps a primitive: `new Number(1)`, `Object("s")`, and `Date` (whose
/// primitive is its epoch milliseconds).
struct DynWrapper : DynObject
{
    Value primitive = Value::undefined();

    void children(GcList& out) const override;
    void clearReferences() override
    {
        DynObject::clearReferences();
        primitive = Value::undefined();
    }
};

namespace regex {
struct Program;
}

/// A regular expression: what it was made from, and what that compiled to.
struct DynRegExp : DynObject
{
    Value source = Value::undefined();
    Value flagText = Value::undefined();
    std::shared_ptr<const regex::Program> program;
    unsigned flags = 0;

    void children(GcList& out) const override;
    void clearReferences() override
    {
        DynObject::clearReferences();
        source = Value::undefined();
        flagText = Value::undefined();
    }
};

/// What `values`, `keys` and `entries` of an array or a typed array hand back.
struct DynArrayIterator : DynObject
{
    Value target = Value::undefined();  ///< undefined once it is done
    double index = 0;
    int mode = 0;  ///< 0 values, 1 keys, 2 entries

    void children(GcList& out) const override;
    void clearReferences() override
    {
        DynObject::clearReferences();
        target = Value::undefined();
    }
};

/// What `matchAll` hands back.
struct DynRegExpIterator : DynObject
{
    Value regexp = Value::undefined();
    Value input = Value::undefined();
    bool global = false;
    bool pairs = false;
    bool done = false;

    void children(GcList& out) const override;
    void clearReferences() override
    {
        DynObject::clearReferences();
        regexp = Value::undefined();
        input = Value::undefined();
    }
};

enum class FnKind : std::uint8_t
{
    Bytecode,
    Native,
    Bound,
    Proxy,  ///< a DynProxy over something callable
};

struct DynFunction : DynObject
{
    FnKind fnKind = FnKind::Native;
    bool isClassConstructor = false;
    bool isNativeConstructor = false;  ///< a built-in `new` means something for
    bool isDerivedConstructor = false;
    bool isArrow = false;
    int arity = 0;
    String name;

    std::shared_ptr<const FunctionCode> code;  ///< Bytecode
    Vec<Ref<Box>> upvalues;           ///< Bytecode
    Ref<DynObject> homeObject;                  ///< `super` lookups start here
    Ref<DynFunction> fieldInit;                 ///< a class: what makes the fields of a new instance

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
        /// Without `next`: the capability of a promise some other constructor made, settled through its functions.
        Value capResolve = Value::undefined();
        Value capReject = Value::undefined();
        /// An `await`: what is to be resumed, in place of the two handlers and the promise for what they give.
        Ref<DynObject> coroutine;
    };

    State state = State::Pending;
    bool handled = false;
    Value result = Value::undefined();
    std::vector<Reaction> reactions;

    DynPromise() { cls = ObjClass::Promise; }
    void children(GcList& out) const override;
    void clearReferences() override;
};

struct Frame;

/// What a generator object is, and what an async function or async generator runs as: a frame of its own, to be run for a
/// while, left and come back to.
struct DynCoroutine : DynObject
{
    enum class Kind : std::uint8_t
    {
        Generator,
        AsyncFunction,
        AsyncGenerator,
    };
    enum class State : std::uint8_t
    {
        SuspendedStart,
        SuspendedYield,
        Running,          ///< an async one: this too while it waits for an `await`
        AwaitingReturn,   ///< async generator: finished, and waiting for what `return(v)` was given
        Completed,
    };
    enum class Resume : std::uint8_t
    {
        Next,
        Throw,
        Return,
    };
    struct Request
    {
        Resume mode;
        Value value;
        Ref<DynPromise> promise;
    };

    Kind coKind = Kind::Generator;
    State state = State::SuspendedStart;
    /// In a `yield*`: what comes in is the delegate's business.
    bool delegating = false;
    bool throwOnResume = false;
    Resume mode = Resume::Next;
    Value received = Value::undefined();
    std::uint32_t returnPc = 0;
    std::unique_ptr<Frame> frame;
    /// The function, which the frame only points at.
    Ref<DynObject> function;
    /// An async function: the promise for what it comes to.
    Ref<DynPromise> promise;
    /// An async generator: what has been asked of it, oldest first.
    std::vector<Request> requests;

    DynCoroutine();
    ~DynCoroutine() override;
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
inline bool DynObject::fnKindIsProxy() const noexcept
{
    return cls == ObjClass::Proxy || (cls == ObjClass::Function && static_cast<const DynFunction*>(this)->fnKind == FnKind::Proxy);
}
DynArray* asArray(const Value& v) noexcept;

/// SameValueZero, used by Map/Set keys and `Array.prototype.includes`.
bool sameValueZero(const Value& a, const Value& b) noexcept;
bool strictEquals(const Value& a, const Value& b) noexcept;
std::size_t hashValue(const Value& v) noexcept;

}  // namespace dyn
}  // namespace internal
}  // namespace script
