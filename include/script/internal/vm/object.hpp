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
    /// The values it holds, for the walk that finds cycles at teardown.
    virtual void values(std::vector<JSValue>& out) const { (void)out; }
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
    JSValue* elems;  ///< out-of-line elements, `capacity` of them; null when there are none
    std::uint32_t length;
    std::uint32_t capacity;

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
    void values(std::vector<JSValue>& out) const override { out.push_back(value); }
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
    void values(std::vector<JSValue>& out) const override
    {
        out.push_back(target);
        out.push_back(handler);
    }
};

/// What an ArrayBuffer's object keeps (in its `Rare::ext`).
struct BufferState : NativeState
{
    std::uint8_t* data = nullptr;
    std::size_t size = 0;      ///< the byte length
    std::size_t capacity = 0;  ///< what `data` holds: the size, or the maximum of a resizable buffer
    bool resizable = false;
    bool detached = false;
    bool shared = false;       ///< a SharedArrayBuffer
    VM* vm = nullptr;
    std::vector<JSTypedArray*> views;  ///< the typed arrays on this buffer, to follow a resize or a detach
    ~BufferState() override;
    /// Give each view the length the current size leaves it (none when it no longer fits, or the buffer is detached).
    void refreshViews() noexcept;
};

/// Take a typed array off its buffer's list of views.
void forgetView(JSTypedArray* view) noexcept;

struct Box
{
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

}  // namespace vm
}  // namespace internal
}  // namespace script
