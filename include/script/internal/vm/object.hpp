// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Objects, arrays, functions and boxes of the new core (ls-jsc.md, 4.4). An object is a 16-byte header, a pointer to its
// out-of-line properties and `aux` inline ones: `{x, y}` is 48 bytes. Property i lives in `inl[i]` when i < aux and in
// `slots[i - aux]` otherwise; which key it is, is the shape's business.
#pragma once

#include "script/internal/vm/vm.hpp"

namespace script {
namespace internal {
namespace vm {

/// In front of an object's out-of-line slots: the rare state, and how many slots the buffer holds.
struct SlotsHeader
{
    Rare* rare;
    std::uint32_t capacity;
    std::uint32_t pad;
};
static_assert(sizeof(SlotsHeader) == 16, "slots stay 16-byte aligned behind their header");

/// The payload of an object that is more than properties: a generator's saved registers, the entries of a Map. Owned by
/// the object's `Rare`, deleted with it.
struct NativeState
{
    virtual ~NativeState() = default;
    // From the pool the other buffers of a cell come from.
    static void* operator new(std::size_t bytes) { return PoolAlloc<char>().allocate(bytes); }
    static void operator delete(void* p, std::size_t bytes) noexcept { PoolAlloc<char>().deallocate(static_cast<char*>(p), bytes); }
    /// Of a state in its object's own cell whose size is its own: that size (a generator's).
    virtual std::size_t cellBytes() const { return 0; }
    /// The values it holds, for the walk that finds cycles at teardown.
    virtual void values(Vec<JSValue>& out) const { (void)out; }
    /// Let go of every value it holds, now; safe to call twice.
    virtual void clear(VM& vm) noexcept { (void)vm; }
    /// A weak table: `values` lists its entries as key, value pairs, and a value lives as long as its key does.
    virtual bool ephemeron() const { return false; }
    /// Drop the entries whose key `dead` calls dead, letting go of both.
    virtual void dropEntries(bool (*dead)(void* context, JSValue key), void* context) { (void)dead; (void)context; }
};

/// Per-object state few objects have. Made on first need; its presence is `kHasRare`.
struct Rare
{
    Shape* childRoot = nullptr;           ///< the root of the shape tree whose prototype this object is
    Dictionary* dict = nullptr;           ///< dictionary mode: the object's own properties
    ValidityCell* validity = nullptr;     ///< holds a count
    Vec<ValidityCell*> protoUsers;        ///< each holds a count: the cells of chains that pass through this object
    void* nativeSlot = nullptr;
    // Functions.
    FastNative native = nullptr;
    void* nativeData = nullptr;
    Key fnName = nullptr;                 ///< holds a count
    std::uint32_t fnLength = 0;
    std::uint32_t fnState = 0;            ///< a native closure's own number, apart from its `length`
    /// Optimized code takes a method found here, or past here, for a constant: a change of the object moves `globalEpoch`.
    bool watched = false;
    std::uint8_t watchBroken = 0;         ///< how often that has happened
    // A bound function: what it calls, with what `this` and what leading arguments. Each holds a count.
    JSValue boundTarget = JSValue::undefined();
    JSValue boundThis = JSValue::undefined();
    JSArray* boundArgs = nullptr;
    // A method or a class constructor: the object `super` looks the prototype of up, and (of a constructor) what runs the
    // instance fields. Each holds a count.
    JSValue home = JSValue::undefined();
    JSValue fieldInit = JSValue::undefined();
    NativeState* ext = nullptr;           ///< owned
};

struct JSObject
{
    CellHeader h;
    JSValue* slots;  ///< out-of-line properties, behind a `SlotsHeader`; null when there are none

    static constexpr std::size_t kHeaderBytes = 24;
    static std::size_t bytesFor(std::uint8_t inlineSlots) noexcept { return kHeaderBytes + 8u * inlineSlots; }
    /// Bytes after the slots in place of a promise, for its state (`embeddedState`).
    static constexpr std::size_t kPromiseStateBytes = 96;
    static constexpr std::size_t kRegExpStateBytes = 24;
    /// The native state an object keeps in its own cell, after its slots in place (a promise's, a RegExp's), or null.
    NativeState* embeddedState() noexcept
    {
        if (cls() == ObjClass::Generator) {
            // (One with too many registers for a cell has its state on the side.)
            Rare* r = rare();
            return r && r->ext ? nullptr : reinterpret_cast<NativeState*>(reinterpret_cast<char*>(this) + bytesFor(h.aux));
        }
        return cls() == ObjClass::Promise || cls() == ObjClass::RegExpCell ? reinterpret_cast<NativeState*>(reinterpret_cast<char*>(this) + bytesFor(h.aux))
                                          : nullptr;
    }

    JSValue* inl() noexcept { return reinterpret_cast<JSValue*>(reinterpret_cast<char*>(this) + kHeaderBytes); }
    JSValue* slotAt(std::uint32_t index) noexcept { return index < h.aux ? &inl()[index] : &slots[index - h.aux]; }
    SlotsHeader* slotsHeader() noexcept { return reinterpret_cast<SlotsHeader*>(slots) - 1; }
    std::uint32_t slotCapacity() noexcept { return slots ? slotsHeader()->capacity : 0; }
    Rare* rare() noexcept { return (h.flags & cellflags::kHasRare) ? slotsHeader()->rare : nullptr; }
    Rare& rareOf(VM& vm);
    ObjClass cls() const noexcept { return ObjClass(h.cls); }
    bool isProto() const noexcept { return (h.flags & cellflags::kIsProto) != 0; }

    Shape* shapeOf(VM& vm) const noexcept { return vm.shapeById(h.shape); }
    bool isDictionary(VM& vm) const noexcept { return shapeOf(vm)->isDictionary(); }
    JSObject* proto(VM& vm) const noexcept { return shapeOf(vm)->proto; }
    /// Move to the shape tree of `next` (null for none), replaying the properties; or, past `kMaxFastProperties`, make
    /// the object a dictionary. Invalidates every chain through the object. The caller checks for a cycle.
    void setProto(VM& vm, JSObject* next);
    /// Whether making `next` this object's prototype would make a loop.
    bool wouldCycle(VM& vm, JSObject* next) const noexcept;

    /// The number of own properties.
    std::uint32_t propertyCount(VM& vm) noexcept;
    /// The slot of an own property, or null; `attrs` receives its attributes.
    JSValue* findOwn(VM& vm, Key key, std::uint8_t* attrs = nullptr);
    /// Define an own data property, taking the count `value` carries: adds it, or replaces the value and attributes.
    void putOwn(VM& vm, Key key, JSValue value, std::uint8_t attrs = attr::kDefault);
    /// Remove an own property. False when it is not configurable; true when it is gone or was never there.
    bool deleteOwn(VM& vm, Key key);
    /// Change the attributes of an own property: false when there is none.
    bool setAttrs(VM& vm, Key key, std::uint8_t attrs);
    void toDictionary(VM& vm);
    /// A dictionary about to move, drop or change a key: a shape an inline cache holds is given up for a new one.
    void dictionaryChanging(VM& vm);
    /// Whether an inline cache may hold this dictionary's shape for a read of its own property: one read often enough.
    bool cacheDictionary(VM& vm) noexcept;
    /// Every own property in the order they were added.
    template <class F>
    void forEachOwn(VM& vm, F&& visit)
    {
        Shape* s = shapeOf(vm);
        if (s->isDictionary()) {
            const Vec<Entry>& entries = rare()->dict->entries;
            for (std::uint32_t i = 0; i < entries.size(); ++i)
                if (entries[i].key)
                    visit(entries[i].key, entries[i].attrs, *slotAt(i));
            return;
        }
        for (std::uint32_t i = 0; i < s->count; ++i) {
            const Entry e = s->entryAt(i);
            visit(e.key, e.attrs, *slotAt(i));
        }
    }

    /// Make sure the buffer holds `index + 1` slots.
    void reserveSlot(VM& vm, std::uint32_t index);
    /// Function objects make `length`, `name` and `prototype` the first time a property is asked about.
    void materialize(VM& vm);

private:
    void addProperty(VM& vm, Key key, JSValue value, std::uint8_t attrs);
    void growSlots(VM& vm, std::uint32_t capacity);
    void compactDictionary(VM& vm);
};

/// Release what an object whose count reached zero holds, and its out-of-line storage. Not the cell itself.
void destroyObjectStorage(VM& vm, JSObject* object) noexcept;
static_assert(sizeof(JSObject) == JSObject::kHeaderBytes, "");

struct JSArray : JSObject
{
    JSValue* elems;  ///< the elements, `capacity` of them: `small` until there are more than it holds
    std::uint32_t length;
    std::uint32_t capacity;
    static constexpr std::uint32_t kSmall = 6;
    JSValue small[kSmall];  ///< the first elements of an array that has never had more
    /// The elements below `packed` are numbers: no hole and no cell among them, so a store of a number over one of them
    /// has nothing to look at or let go of, and nothing below it needs a look when the array is freed or traced. A
    /// lower bound (zero is always right) and never above `length`; never above zero in a frozen array, whose elements
    /// compiled code then leaves to the checks of the long way. Every writer of `elems` and `length` keeps it.
    std::uint32_t packed;

    /// After `elems[index] = v`, with `index < length`.
    void noteStore(std::uint32_t index, JSValue v) noexcept
    {
        if (v.isNumber())
            packed += index == packed && !(h.flags & cellflags::kFrozen);
        else if (index < packed)
            packed = index;
    }
    /// After elements from `from` on were moved or replaced: what follows the numbers still known is looked at.
    void repack(std::uint32_t from = 0) noexcept
    {
        std::uint32_t p = packed < from ? packed : from;
        if (h.flags & cellflags::kFrozen) {
            packed = 0;
            return;
        }
        while (p < length && elems[p].isNumber())
            ++p;
        packed = p;
    }

    bool ownsElems() const noexcept { return elems && elems != small; }

    static constexpr std::uint32_t kMaxDenseGap = 1u << 24;

    /// The element, not counted for the caller; a hole or an index past the end reads as `hole`.
    JSValue at(std::uint32_t index) const noexcept { return index < length ? elems[index] : JSValue::hole(); }
    /// Store at `index`, taking the count `value` carries and growing the array past the end with holes. False, with
    /// `value` released, when the index is so far past the end that the array would no longer be dense.
    bool setAt(VM& vm, std::uint32_t index, JSValue value);
    void append(VM& vm, JSValue value);
    /// Append `n` values, counting each.
    void appendAll(VM& vm, const JSValue* values, std::uint32_t n);
    /// Length to `newLength`: holes when longer, elements released when shorter.
    void resize(VM& vm, std::uint32_t newLength);
    /// Remove `count` elements from `from`, closing the gap; they are released.
    void removeRange(VM& vm, std::uint32_t from, std::uint32_t count);
    /// Open a gap of `count` holes at `at`.
    void insertGap(VM& vm, std::uint32_t at, std::uint32_t count);
    /// Take the last element, with its count.
    JSValue pop(VM& vm);
    void reserve(VM& vm, std::uint32_t wanted);
    bool allNumbers() const noexcept { return (h.flags & cellflags::kAllNumbers) != 0; }
};

/// The element types of typed arrays and what a DataView reads.
enum class ElemKind : std::uint8_t
{
    Int8, Uint8, Uint8Clamped, Int16, Uint16, Int32, Uint32, Float32, Float64, BigInt64, BigUint64, Float16, Count,
};
inline constexpr std::uint32_t kElemSize[] = {1, 1, 1, 2, 2, 4, 4, 4, 8, 8, 8, 2};

struct ProxyState;
/// The proxy state of an object that is a proxy, else null.
ProxyState* proxyOf(JSObject* o) noexcept;
JSValue proxyGet(VM& vm, JSObject* proxy, Key key, JSValue receiver);
bool proxySet(VM& vm, JSObject* proxy, Key key, JSValue value, JSValue receiver, bool strict);
/// 1 yes, 0 no, -1 an exception is pending.
int proxyHas(VM& vm, JSObject* proxy, Key key);
int proxyDelete(VM& vm, JSObject* proxy, Key key, bool strict);

/// A typed array: a window on an ArrayBuffer. `data` points into the buffer, which this holds a count of; the buffer
/// does not move (the buffers of this core do not resize), so the pointer stays good.
struct JSTypedArray : JSObject
{
    std::uint8_t* data;
    std::uint32_t length;  ///< elements
    ElemKind kind;
    JSValue buffer;        ///< the ArrayBuffer object, counted
    std::uint32_t byteOffset;
    std::uint32_t fixedLength;  ///< the length it was made with
    bool tracking;              ///< made without a length on a resizable buffer: it follows the buffer's size
};

/// What the wrapper object of a primitive (`new Number(1)`, `Object('x')`) keeps: the primitive.
struct PrimitiveBox : NativeState
{
    JSValue value = JSValue::undefined();
    ~PrimitiveBox() override { release(value); }
    void values(Vec<JSValue>& out) const override { out.push_back(value); }
    void clear(VM&) noexcept override
    {
        release(value);
        value = JSValue::undefined();
    }
};

/// What a proxy's object keeps: where its operations go when its handler has no trap, and the handler (null once revoked).
struct ProxyState : NativeState
{
    JSValue target = JSValue::undefined();
    JSValue handler = JSValue::undefined();
    ~ProxyState() override
    {
        release(target);
        release(handler);
    }
    void clear(VM&) noexcept override
    {
        release(target);
        release(handler);
        target = handler = JSValue::undefined();
    }
    void values(Vec<JSValue>& out) const override
    {
        out.push_back(target);
        out.push_back(handler);
    }
};

/// What an ArrayBuffer's object keeps (in its `Rare::ext`).
/// The typed arrays on a buffer: the first two are kept in place, since a buffer almost always has one.
class ViewList
{
public:
    std::size_t size() const noexcept { return n_; }
    JSTypedArray** begin() noexcept { return data(); }
    JSTypedArray** end() noexcept { return data() + n_; }
    JSTypedArray*& operator[](std::size_t i) noexcept { return data()[i]; }
    JSTypedArray*& back() noexcept { return data()[n_ - 1]; }
    void push_back(JSTypedArray* v)
    {
        if (n_ < 2) {
            inline_[n_++] = v;
        } else {
            if (n_ == 2)
                heap_.assign(inline_, inline_ + 2);
            heap_.push_back(v);
            ++n_;
        }
    }
    void pop_back() noexcept
    {
        --n_;
        if (n_ == 2) {
            inline_[0] = heap_[0];
            inline_[1] = heap_[1];
            heap_.clear();
        }
    }

private:
    JSTypedArray** data() noexcept { return n_ <= 2 ? inline_ : heap_.data(); }
    JSTypedArray* inline_[2] = {nullptr, nullptr};
    std::vector<JSTypedArray*> heap_;
    std::size_t n_ = 0;
};

struct BufferState : NativeState
{
    std::uint8_t* data = nullptr;
    std::size_t size = 0;      ///< the byte length
    std::size_t capacity = 0;  ///< what `data` holds: the size, or the maximum of a resizable buffer
    bool resizable = false;
    bool detached = false;
    bool shared = false;       ///< a SharedArrayBuffer
    VM* vm = nullptr;
    ViewList views;  ///< the typed arrays on this buffer, to follow a resize or a detach
    ~BufferState() override;
    /// Give each view the length the current size leaves it (none when it no longer fits, or the buffer is detached).
    void refreshViews() noexcept;
};

/// Take a typed array off its buffer's list of views.
void forgetView(JSTypedArray* view) noexcept;

struct Box
{
    /// In `h.aux`: how often the variable was written (`kWrites`, as far as the runtime saw it), that optimized code takes its
    /// value for a constant (`kWatched`: the next write moves `VM::globalEpoch`), or that one did and was wrong.
    static constexpr std::uint8_t kWrites = 3, kWatched = 4, kNotConstant = 8;
    CellHeader h;
    JSValue value;
};
static_assert(sizeof(Box) == 24, "");

struct GetterSetter
{
    CellHeader h;
    JSValue getter;
    JSValue setter;
};
static_assert(sizeof(GetterSetter) == 32, "");

struct JSFunction : JSObject
{
    enum Flags : std::uint32_t
    {
        kLazyProps = 1,     ///< `length`, `name`, `prototype` not made yet
        kConstructor = 2,
        kNative = 4,
    };
    CodeBlock* code;
    std::uint32_t flags;
    std::uint32_t nUpvals;

    Box** upvals() noexcept { return reinterpret_cast<Box**>(reinterpret_cast<char*>(this) + sizeof(JSFunction)); }
    static std::size_t bytesFor(std::uint32_t upvals) noexcept { return sizeof(JSFunction) + 8u * upvals; }
};
static_assert(sizeof(JSFunction) == 40, "");

inline JSObject* asObject(JSValue v) noexcept
{
    return reinterpret_cast<JSObject*>(v.asCell());
}
inline bool isObject(JSValue v) noexcept
{
    return v.isCell() && isVmCell(v.asCell()) && kindOf(v.asCell()) == Kind::Object;
}
/// An object with the elements of a `JSArray`: an array, or the `arguments` of a call.
inline bool hasElems(const JSObject* o) noexcept
{
    return o->cls() == ObjClass::Array || o->cls() == ObjClass::Arguments;
}

/// The primitive a wrapper object of class `cls` stands for; anything else as it is.
inline JSValue unwrapped(JSValue v, ObjClass cls) noexcept
{
    if (isObject(v) && asObject(v)->cls() == cls)
        if (Rare* r = asObject(v)->rare(); r && r->ext)
            return static_cast<PrimitiveBox*>(r->ext)->value;
    return v;
}

inline bool hasElems(JSValue v) noexcept
{
    return isObject(v) && hasElems(asObject(v));
}

inline bool isArray(JSValue v) noexcept
{
    return isObject(v) && asObject(v)->cls() == ObjClass::Array;
}
inline bool isFunction(JSValue v) noexcept
{
    return isObject(v) && asObject(v)->cls() == ObjClass::Function;
}
inline JSValue toJSValue(const JSObject* o) noexcept
{
    return JSValue::cell(reinterpret_cast<CellPtr>(const_cast<JSObject*>(o)));
}

// ---- property access through the prototype chain (property.cpp) ----

/// `base[key]`, as an owned value. Primitives read through their prototypes; an accessor is called through
/// `VM::callHook`. The exception sentinel, with `vm.exception` set, when a getter throws or `base` is nullish.
JSValue getProperty(VM& vm, JSValue base, Key key);
/// The same by the megamorphic table where that knows: for a read the runtime makes often of few names.
JSValue getPropertyMega(VM& vm, JSValue base, Key key);
/// The message of a read of a property of null or undefined, naming the property as V8 does: "Cannot read properties of
/// undefined (reading 'x')". `key` is a property key or the value an element read was given.
std::string nullishReadMessage(JSValue base, JSValue key);
/// `base[key] = value` (the caller keeps its count of `value`). False when the store is refused, with `vm.exception`
/// set if `strict` or a setter threw.
bool setProperty(VM& vm, JSValue base, Key key, JSValue value, bool strict = true);
bool hasProperty(VM& vm, JSValue base, Key key);
/// A read that starts at `start` and calls getters with `receiver` as `this`: what `super.x` does.
JSValue getPropertyFrom(VM& vm, JSObject* start, JSValue receiver, Key key);
bool deleteProperty(VM& vm, JSValue base, Key key, bool strict = true);
/// Define an own property of an object (`Object.defineProperty` for a data property or an accessor).
bool defineOwn(VM& vm, JSObject* object, Key key, JSValue value, std::uint8_t attrs);
/// Move the element at `index` of an array to a property of its own (by name, behind a hole), so that its attributes can change.
bool demoteIndexElement(VM& vm, JSArray* a, std::uint32_t index, Key key);
bool defineAccessor(VM& vm, JSObject* object, Key key, JSValue getter, JSValue setter, std::uint8_t attrs);
/// The canonical array index a property name spells, if it does.
bool arrayIndexOf(Key key, std::uint32_t* index) noexcept;
/// CanonicalNumericIndexString: a property name that is what the number it spells prints as (or "-0").
bool canonicalNumericIndex(Key key, double* out) noexcept;
/// Whether `n` is an integer index of the typed array as it is now.
bool validTypedIndex(const JSTypedArray* t, double n) noexcept;

}  // namespace vm
}  // namespace internal
}  // namespace script
