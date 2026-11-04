// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The heap: UTF conversion, cell lifetime, property tables, Map/Set storage.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "script/internal/dyn/object.hpp"
#include "script/internal/dyn/runtime.hpp"

namespace script {

namespace internal {
namespace dyn {

SymbolCell::~SymbolCell()
{
    Runtime* engine = rt && !rt->tearingDown ? rt->dynIfAny() : nullptr;
    if (registered && engine) {
        auto it = engine->symbolRegistry.find(description);
        if (it != engine->symbolRegistry.end() && it->second == this)
            engine->symbolRegistry.erase(it);
    }
}

// ---------------------------------------------------------- properties ---

Prop* DynObject::findOwn(GCObject* key) noexcept
{
    if (index.empty()) {
        for (Prop& p : props) {
            if (p.key.get() == key)
                return &p;
        }
        return nullptr;
    }
    // The table always keeps at least one empty slot, so the probe terminates.
    const std::size_t mask = index.size() - 1;
    std::size_t slot = hashPointer(key) & mask;
    for (;;) {
        const std::uint32_t packed = index[slot];
        if (!packed)
            return nullptr;
        Prop& p = props[packed - 1];
        if (p.key.get() == key)
            return &p;
        slot = (slot + 1) & mask;
    }
}

Prop& DynObject::putOwn(GCObject* key, Value value, std::uint8_t attrs)
{
    if (Prop* existing = findOwn(key)) {
        existing->value = std::move(value);
        existing->setter = Value::undefined();
        existing->attrs = attrs;
        return *existing;
    }
    const std::size_t before = props.capacity();
    props.emplace_back();
    Prop& p = props.back();
    p.key.reset(key);
    p.value = std::move(value);
    p.attrs = attrs;
    chargeIfGrown(props, before);

    if (props.size() > kIndexThreshold && index.size() < props.size() * 2)
        reindex();
    else if (!index.empty())
        indexInsert(std::uint32_t(props.size() - 1));
    return p;
}

bool DynObject::deleteOwn(GCObject* key)
{
    for (std::size_t i = 0; i < props.size(); ++i) {
        if (props[i].key.get() != key)
            continue;
        if (!(props[i].attrs & PropConfigurable))
            return false;
        props.erase(props.begin() + std::ptrdiff_t(i));
        reindex();
        trim();
        return true;
    }
    return true;  // deleting a missing property succeeds
}

void DynObject::indexInsert(std::uint32_t at) noexcept
{
    const std::size_t mask = index.size() - 1;
    std::size_t slot = hashPointer(props[at].key.get()) & mask;
    while (index[slot])
        slot = (slot + 1) & mask;
    index[slot] = at + 1;
}

void DynObject::reindex()
{
    if (props.size() <= kIndexThreshold) {
        index.clear();
        index.shrink_to_fit();
        return;
    }
    index.assign(tableSizeFor(props.size() * 2), 0);
    for (std::uint32_t i = 0; i < props.size(); ++i)
        indexInsert(i);
}

void DynObject::trim()
{
    if (props.capacity() > 8 && props.capacity() > props.size() * 4)
        props.shrink_to_fit();
}

void DynObject::children(GcList& out) const
{
    if (proto)
        out.push_back(proto.get());
    for (const Prop& p : props) {
        if (p.key)
            out.push_back(p.key.get());
        if (GCObject* v = detail::Access::ptr(p.value))
            out.push_back(v);
        if (GCObject* s = detail::Access::ptr(p.setter))
            out.push_back(s);
    }
}

void DynObject::clearReferences()
{
    proto.reset();
    props.clear();
    props.shrink_to_fit();
    index.clear();
    index.shrink_to_fit();
}

std::size_t DynObject::heapSize() const
{
    return sizeof(*this) + props.capacity() * sizeof(Prop) +
           index.capacity() * sizeof(std::uint32_t);
}

// -------------------------------------------------------------- arrays ---

void DynArray::children(GcList& out) const
{
    DynObject::children(out);
    for (const Value& v : elements) {
        if (GCObject* p = detail::Access::ptr(v))
            out.push_back(p);
    }
}

void DynArray::clearReferences()
{
    DynObject::clearReferences();
    elements.clear();
    elements.shrink_to_fit();
}

std::size_t DynArray::heapSize() const
{
    return DynObject::heapSize() + elements.capacity() * sizeof(Value);
}

void DynArray::append(Value v)
{
    const std::size_t before = elements.capacity();
    if (elements.size() == before)  // the vector is about to double
        reserveGrowth((before ? before : 4) * sizeof(Value));
    elements.push_back(std::move(v));
    chargeIfGrown(elements, before);
}

void DynArray::appendAll(const Value* items, std::size_t count)
{
    const std::size_t before = elements.capacity();
    reserveFor(elements, elements.size() + count);
    elements.insert(elements.end(), items, items + count);
    chargeIfGrown(elements, before);
}

void DynArray::resize(std::size_t n)
{
    const std::size_t before = elements.capacity();
    reserveFor(elements, n);
    elements.resize(n, Value::undefined());
    chargeIfGrown(elements, before);
    trimElements();
}

void DynArray::trimElements()
{
    if (elements.capacity() > 16 && elements.capacity() > elements.size() * 4)
        elements.shrink_to_fit();
}

void DynWrapper::children(GcList& out) const
{
    DynObject::children(out);
    if (GCObject* p = detail::Access::ptr(primitive))
        out.push_back(p);
}

// ----------------------------------------------------------- functions ---

void DynFunction::children(GcList& out) const
{
    DynObject::children(out);
    for (const Ref<Box>& c : upvalues) {
        if (c)
            out.push_back(c.get());
    }
    if (homeObject)
        out.push_back(homeObject.get());
    if (boundTarget)
        out.push_back(boundTarget.get());
    if (GCObject* p = detail::Access::ptr(boundThis))
        out.push_back(p);
    for (const Value& v : boundArgs) {
        if (GCObject* p = detail::Access::ptr(v))
            out.push_back(p);
    }
    if (code) {
        for (const Value& v : code->consts) {
            if (GCObject* p = detail::Access::ptr(v))
                out.push_back(p);
        }
    }
}

void DynFunction::clearReferences()
{
    DynObject::clearReferences();
    upvalues.clear();
    homeObject.reset();
    boundTarget.reset();
    boundThis = Value::undefined();
    boundArgs.clear();
    // `native` can own captured Values through its std::function state, so it
    // has to go too: a native closure is just as able to keep a cycle alive.
    native = nullptr;
    code.reset();
}

std::size_t DynFunction::heapSize() const
{
    return DynObject::heapSize() + upvalues.capacity() * sizeof(void*) +
           boundArgs.capacity() * sizeof(Value) + 64;
}

DynFunction* asFunction(const Value& v) noexcept
{
    DynObject* o = asObject(v);
    return (o && o->cls == ObjClass::Function) ? static_cast<DynFunction*>(o) : nullptr;
}

DynArray* asArray(const Value& v) noexcept
{
    DynObject* o = asObject(v);
    return (o && o->cls == ObjClass::Array) ? static_cast<DynArray*>(o) : nullptr;
}

// ------------------------------------------------------------- equality ---

bool strictEquals(const Value& a, const Value& b) noexcept
{
    // A number is a number whatever tag it carries: a host method's `int`
    // result and the literal `2` are `===`.
    if (a.isNumber() && b.isNumber())
        return numberOf(a) == numberOf(b);
    if (a.type() != b.type())
        return false;
    switch (a.type()) {
        case Type::Undefined:
        case Type::Null:
            return true;
        case Type::Bool:
            return boolOf(a) == boolOf(b);
        case Type::Int:
        case Type::Long:
        case Type::Float:
        case Type::Char:
        case Type::Double:
            return numberOf(a) == numberOf(b);
        case Type::String: {
            StringCell* x = asString(a);
            StringCell* y = asString(b);
            return x == y || x->text() == y->text();
        }
        default:
            return detail::Access::ptr(a) == detail::Access::ptr(b);
    }
}

bool sameValueZero(const Value& a, const Value& b) noexcept
{
    if (a.isNumber() && b.isNumber()) {
        const double x = numberOf(a);
        const double y = numberOf(b);
        if (std::isnan(x) && std::isnan(y))
            return true;
        return x == y;
    }
    return strictEquals(a, b);
}

std::size_t hashValue(const Value& v) noexcept
{
    switch (v.type()) {
        case Type::Undefined:
            return 0x9e3779b9;
        case Type::Null:
            return 0x85ebca6b;
        case Type::Bool:
            return boolOf(v) ? 0xc2b2ae35 : 0x27d4eb2f;
        case Type::Int:
        case Type::Long:
        case Type::Float:
        case Type::Char:
        case Type::Double: {
            double d = numberOf(v);
            if (d == 0)
                d = 0;  // -0 hashes like +0, matching SameValueZero
            if (std::isnan(d))
                return 0x7ff80000;
            std::uint64_t bits = 0;
            std::memcpy(&bits, &d, sizeof bits);
            return std::size_t(bits ^ (bits >> 32));
        }
        case Type::String:
            return asString(v)->hash();
        default:
            return std::size_t(reinterpret_cast<std::uintptr_t>(detail::Access::ptr(v)) >> 4);
    }
}

// -------------------------------------------------------------- Map/Set ---

void DynMap::children(GcList& out) const
{
    DynObject::children(out);
    for (const Entry& e : entries) {
        if (!e.live)
            continue;
        if (GCObject* p = detail::Access::ptr(e.key))
            out.push_back(p);
        if (GCObject* p = detail::Access::ptr(e.value))
            out.push_back(p);
    }
}

void DynMap::clearReferences()
{
    DynObject::clearReferences();
    clear();
}

std::size_t DynMap::heapSize() const
{
    return DynObject::heapSize() + entries.capacity() * sizeof(Entry) +
           lookup.capacity() * sizeof(std::uint32_t);
}

std::int64_t DynMap::find(const Value& key) const
{
    if (lookup.empty())
        return -1;
    const std::size_t mask = lookup.size() - 1;
    std::size_t slot = hashValue(key) & mask;
    for (;;) {
        const std::uint32_t packed = lookup[slot];
        if (!packed)
            return -1;
        const std::uint32_t at = packed - 1;
        if (entries[at].live && sameValueZero(entries[at].key, key))
            return std::int64_t(at);
        slot = (slot + 1) & mask;
    }
}

void DynMap::place(std::uint32_t at)
{
    const std::size_t mask = lookup.size() - 1;
    std::size_t slot = hashValue(entries[at].key) & mask;
    while (lookup[slot])
        slot = (slot + 1) & mask;
    lookup[slot] = at + 1;
}

void DynMap::rehash(std::size_t wanted)
{
    const std::size_t before = lookup.capacity();
    lookup.assign(tableSizeFor(wanted), 0);
    chargeIfGrown(lookup, before);
    for (std::uint32_t i = 0; i < entries.size(); ++i) {
        if (entries[i].live)
            place(i);
    }
}

void DynMap::insert(const Value& key, const Value& value)
{
    const std::int64_t at = find(key);
    if (at >= 0) {
        entries[std::size_t(at)].value = value;
        return;
    }
    const std::size_t before = entries.capacity();
    entries.push_back(Entry{key, value, true});
    chargeIfGrown(entries, before);
    ++liveCount;
    // Keep the table under half full so probes stay short and terminate.
    if (entries.size() * 2 >= lookup.size())
        rehash(entries.size() * 4);
    else
        place(std::uint32_t(entries.size() - 1));
}

bool DynMap::erase(const Value& key)
{
    const std::int64_t at = find(key);
    if (at < 0)
        return false;
    Entry& e = entries[std::size_t(at)];
    e.live = false;
    e.key = Value::undefined();
    e.value = Value::undefined();
    --liveCount;
    compact();
    return true;
}

void DynMap::clear()
{
    entries.clear();
    entries.shrink_to_fit();
    lookup.clear();
    lookup.shrink_to_fit();
    liveCount = 0;
}

void DynMap::compact()
{
    if (entries.size() < 32 || liveCount * 2 > entries.size())
        return;
    Vec<Entry> kept;
    kept.reserve(liveCount);
    for (Entry& e : entries) {
        if (e.live)
            kept.push_back(std::move(e));
    }
    entries = std::move(kept);
    rehash(entries.size() * 4);
}

// ------------------------------------------------------------ promises ---

void DynPromise::children(GcList& out) const
{
    DynObject::children(out);
    if (GCObject* p = detail::Access::ptr(result))
        out.push_back(p);
    for (const Reaction& r : reactions) {
        if (GCObject* p = detail::Access::ptr(r.onFulfilled))
            out.push_back(p);
        if (GCObject* p = detail::Access::ptr(r.onRejected))
            out.push_back(p);
        if (r.next)
            out.push_back(r.next.get());
    }
}

void DynPromise::clearReferences()
{
    DynObject::clearReferences();
    result = Value::undefined();
    reactions.clear();
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
