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
    // A bound function: what it calls, with what `this` and what leading arguments. Each holds a count.
    JSValue boundTarget = JSValue::undefined();
    JSValue boundThis = JSValue::undefined();
    JSArray* boundArgs = nullptr;
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
bool deleteProperty(VM& vm, JSValue base, Key key, bool strict = true);
/// Define an own property of an object (`Object.defineProperty` for a data property or an accessor).
bool defineOwn(VM& vm, JSObject* object, Key key, JSValue value, std::uint8_t attrs);
bool defineAccessor(VM& vm, JSObject* object, Key key, JSValue getter, JSValue setter, std::uint8_t attrs);
/// The canonical array index a property name spells, if it does.
bool arrayIndexOf(Key key, std::uint32_t* index) noexcept;

}  // namespace vm
}  // namespace internal
}  // namespace script
