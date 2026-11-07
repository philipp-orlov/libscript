// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The heap: UTF conversion, cell lifetime, property tables, Map/Set storage.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "script/internal/dyn/bytecode.hpp"
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

const Shape::Entry* Shape::entries() const
{
    if (table_.empty() && count) {
        table_.resize(count);
        std::uint32_t at = count;
        for (const Shape* s = this; s->parent; s = s->parent)
            table_[--at] = Entry{s->key.get(), s->attrs};
    }
    return table_.data();
}

std::int32_t Shape::indexOf(const GCObject* k) const
{
    // Up the chain for a small shape, or one asked about only in passing: an object being built asks each shape it passes
    // through once, and a table for each of those would be a square of its size.
    constexpr std::uint32_t kWalkUpTo = 8;
    constexpr std::uint32_t kAskedBeforeTable = 4;
    if (lookup_.empty()) {
        if (count <= kWalkUpTo || ++asked_ < kAskedBeforeTable) {
            std::int32_t at = std::int32_t(count);
            for (const Shape* s = this; s->parent; s = s->parent) {
                --at;
                if (s->key.get() == k)
                    return at;
            }
            return -1;
        }
        const Entry* all = entries();
        lookup_.assign(tableSizeFor(std::size_t(count) * 2), 0);
        const std::size_t mask = lookup_.size() - 1;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::size_t slot = hashPointer(all[i].key) & mask;
            while (lookup_[slot])
                slot = (slot + 1) & mask;
            lookup_[slot] = i + 1;
        }
    }
    const std::size_t mask = lookup_.size() - 1;
    for (std::size_t slot = hashPointer(k) & mask;; slot = (slot + 1) & mask) {
        const std::uint32_t packed = lookup_[slot];
        if (!packed)
            return -1;
        if (table_[packed - 1].key == k)
            return std::int32_t(packed - 1);
    }
}

Prop* DynObject::findOwn(GCObject* key) noexcept
{
    if (shape) {
        const std::int32_t at = shape->indexOf(key);
        return at < 0 ? nullptr : &props[std::size_t(at)];
    }
    if (!rare)
        return nullptr;
    if (rare->index.empty()) {
        for (std::size_t i = 0; i < rare->keys.size(); ++i) {
            if (rare->keys[i].key.get() == key)
                return &props[i];
        }
        return nullptr;
    }
    // The table always keeps at least one empty slot, so the probe terminates.
    const Vec<std::uint32_t>& index = rare->index;
    const std::size_t mask = index.size() - 1;
    std::size_t slot = hashPointer(key) & mask;
    for (;;) {
        const std::uint32_t packed = index[slot];
        if (!packed)
            return nullptr;
        if (rare->keys[packed - 1].key.get() == key)
            return &props[packed - 1];
        slot = (slot + 1) & mask;
    }
}

void DynObject::setProto(DynObject* next)
{
    if (next)
        next->isProto = true;
    if (isProto && rt)
        ++rt->protoEpoch;
    proto.reset(next);
    // With nothing in it yet, it can start from what that prototype's other children start from.
    Runtime* engine = rt ? rt->dynIfAny() : nullptr;
    if (engine && props.empty())
        shape = engine->rootShapeFor(next);
    else
        moveKeysHere();
}

void DynObject::moveKeysHere() noexcept
{
    if (!shape)
        return;
    if (!props.empty()) {
        const Shape::Entry* all = shape->entries();
        Vec<Rare::Key>& keys = rareOf().keys;
        keys.resize(props.size());
        for (std::size_t i = 0; i < keys.size(); ++i) {
            keys[i].key.reset(all[i].key);
            keys[i].attrs = all[i].attrs;
        }
    }
    shape = nullptr;
    reindex();
}

void DynObject::dropShape() noexcept
{
    moveKeysHere();
    if (isProto && rt)
        ++rt->protoEpoch;
    tableChanged();
}

void DynObject::tableChanged() noexcept
{
    Runtime* engine = rt ? rt->dynIfAny() : nullptr;
    if (engine && engine->globalObject.get() == this)
        ++engine->globalEpoch;
}

void DynObject::appendKnown(Value value, Shape* next)
{
    const std::size_t before = props.capacity();
    props.emplace_back().value = std::move(value);
    if (props.capacity() != before)
        tableChanged();
    chargeIfGrown(props, before);
    shape = next;
    if (isProto)
        ++rt->protoEpoch;
}

Prop& DynObject::putOwn(GCObject* key, Value value, std::uint8_t attrs)
{
    if (Prop* existing = findOwn(key)) {
        existing->value = std::move(value);
        setSetterOf(key, Value::undefined());
        if (attrsOf(*existing) != attrs) {
            dropShape();
            setAttrsAt(std::size_t(existing - props.data()), attrs);
        }
        return *existing;
    }
    if (shape) {
        Runtime* engine = rt ? rt->dynIfAny() : nullptr;
        Shape* next = engine ? engine->shapeAfter(shape, key, attrs) : nullptr;
        if (next)
            shape = next;
        else
            moveKeysHere();
    }
    const std::size_t before = props.capacity();
    Prop& p = props.emplace_back();
    p.value = std::move(value);
    if (props.capacity() != before)
        tableChanged();
    chargeIfGrown(props, before);

    if (!shape) {
        Rare& r = rareOf();
        r.keys.emplace_back();
        r.keys.back().key.reset(key);
        r.keys.back().attrs = attrs;
        if (props.size() > kIndexThreshold && r.index.size() < props.size() * 2)
            reindex();
        else if (!r.index.empty())
            indexInsert(std::uint32_t(props.size() - 1));
    }
    if (isProto && rt)
        ++rt->protoEpoch;
    return p;
}

bool DynObject::deleteOwn(GCObject* key)
{
    Prop* p = findOwn(key);
    if (!p)
        return true;  // deleting a missing property succeeds
    if (!(attrsOf(*p) & PropConfigurable))
        return false;
    setSetterOf(key, Value::undefined());
    dropShape();
    const std::ptrdiff_t at = p - props.data();
    props.erase(p);
    rare->keys.erase(rare->keys.begin() + at);
    reindex();
    trim();
    return true;
}

void DynObject::indexInsert(std::uint32_t at) noexcept
{
    Vec<std::uint32_t>& index = rare->index;
    const std::size_t mask = index.size() - 1;
    std::size_t slot = hashPointer(rare->keys[at].key.get()) & mask;
    while (index[slot])
        slot = (slot + 1) & mask;
    index[slot] = at + 1;
}

void DynObject::reindex()
{
    if (shape || props.size() <= kIndexThreshold) {
        if (rare) {
            rare->index.clear();
            rare->index.shrink_to_fit();
        }
        return;
    }
    rareOf().index.assign(tableSizeFor(props.size() * 2), 0);
    for (std::uint32_t i = 0; i < props.size(); ++i)
        indexInsert(i);
}

void DynObject::trim()
{
    if (props.capacity() > 8 && props.capacity() > props.size() * 4) {
        props.shrink_to_fit();
        tableChanged();
    }
}

void DynObject::children(GcList& out) const
{
    // A pinned cell is not counted, so it is no edge.
    if (proto && !proto->pinned)
        out.push_back(proto.get());
    for (const Prop& p : props) {
        if (GCObject* v = detail::Access::ptr(p.value))
            out.push_back(v);
    }
    if (rare) {
        for (const Rare::Key& k : rare->keys) {
            if (k.key && !k.key->pinned)
                out.push_back(k.key.get());
        }
    }
    if (rare && rare->setters) {
        for (const auto& [key, fn] : *rare->setters) {
            if (GCObject* s = detail::Access::ptr(fn))
                out.push_back(s);
        }
    }
}

void DynObject::setSetterOf(GCObject* key, Value fn)
{
    if (!rare || !rare->setters) {
        if (fn.isUndefined())
            return;
        rareOf().setters = std::make_unique<Vec<std::pair<Ref<GCObject>, Value>>>();
    }
    auto& setters = *rare->setters;
    for (std::size_t i = 0; i < setters.size(); ++i) {
        if (setters[i].first.get() != key)
            continue;
        if (fn.isUndefined()) {
            setters[i] = std::move(setters.back());
            setters.pop_back();
        } else {
            setters[i].second = std::move(fn);
        }
        return;
    }
    if (!fn.isUndefined())
        setters.push_back({Ref<GCObject>(key), std::move(fn)});
}

void DynObject::clearReferences()
{
    shape = nullptr;
    proto.reset();
    props.clear();
    props.shrink_to_fit();
    if (rare) {
        rare->keys.clear();
        rare->keys.shrink_to_fit();
        rare->index.clear();
        rare->index.shrink_to_fit();
        rare->setters.reset();
    }
}

std::size_t DynObject::heapSize() const
{
    // A plain object made with room has its first properties in the cell, which `cellBytes` covers.
    const std::size_t cell = kind == CellKind::Object && cls == ObjClass::Plain && cellBytes > sizeof(*this)
                                 ? cellBytes
                                 : sizeof(*this);
    return cell + props.heapCapacity() * sizeof(Prop) +
           (rare ? rare->index.capacity() * sizeof(std::uint32_t) + rare->keys.capacity() * sizeof(Rare::Key) : 0);
}

// -------------------------------------------------------------- arrays ---

std::uint8_t ElemSite::of(const DynArray* array) noexcept
{
    return array->numbersOnly ? kNumbers : kValues;
}

DynArray::~DynArray()
{
    if (!numbersOnly) {
        Value* values = store.begin();
        for (std::uint32_t i = 0; i < store.size_; ++i)
            values[i].~Value();
    }
    freeStorage();
}

void DynArray::freeStorage() noexcept
{
    if (store.data_ && !inCell())
        PoolAlloc<char>().deallocate(static_cast<char*>(store.data_), std::size_t(store.cap_) * elementBytes());
}

void DynArray::adoptRoom(std::size_t bytes) noexcept
{
    store.data_ = this + 1;
    store.cap_ = std::uint32_t(bytes / sizeof(double));
}

void DynArray::children(GcList& out) const
{
    DynObject::children(out);
    if (numbersOnly)
        return;
    for (const Value& v : store) {
        if (GCObject* p = detail::Access::ptr(v))
            out.push_back(p);
    }
}

void DynArray::clearReferences()
{
    DynObject::clearReferences();
    if (!numbersOnly) {
        Value* values = store.begin();
        for (std::uint32_t i = store.size_; i-- > 0;)
            values[i].~Value();
    }
    store.size_ = 0;
    freeStorage();
    store.data_ = nullptr;
    store.cap_ = 0;
    numbersOnly = true;
    if (roomInCell())
        adoptRoom(roomInCell());
}

std::size_t DynArray::heapSize() const
{
    const std::size_t apart = inCell() ? 0 : std::size_t(store.cap_) * elementBytes();
    return DynObject::heapSize() + (sizeof(DynArray) - sizeof(DynObject)) + roomInCell() + apart;
}

void DynArray::moveTo(std::size_t capacity)
{
    const std::size_t bytes = capacity * elementBytes();
    const std::size_t room = roomInCell();
    if (bytes <= room && (inCell() || room)) {
        // Back into the cell, or staying there.
        if (!inCell()) {
            std::memcpy(static_cast<void*>(this + 1), store.data_, std::size_t(store.size_) * elementBytes());
            PoolAlloc<char>().deallocate(static_cast<char*>(store.data_), std::size_t(store.cap_) * elementBytes());
            store.data_ = this + 1;
        }
        store.cap_ = std::uint32_t(room / elementBytes());
        return;
    }
    if (capacity == 0) {
        freeStorage();
        store.data_ = nullptr;
        store.cap_ = 0;
        return;
    }
    const std::size_t before = inCell() ? 0 : std::size_t(store.cap_) * elementBytes();
    char* fresh = PoolAlloc<char>().allocate(bytes);
    if (store.size_)
        std::memcpy(static_cast<void*>(fresh), store.data_, std::size_t(store.size_) * elementBytes());
    freeStorage();
    store.data_ = fresh;
    store.cap_ = std::uint32_t(capacity);
    if (bytes > before)
        chargeGrowth(bytes - before);
}

void DynArray::reserveElements(std::size_t n)
{
    if (n > store.cap_) {
        reserveGrowth((n - store.cap_) * elementBytes());
        moveTo(n);
    }
}

void DynArray::growFor(std::size_t n)
{
    if (n > store.cap_)
        reserveElements(std::max<std::size_t>({n, std::size_t(store.cap_) * 2, 4}));
}

void DynArray::box()
{
    if (!numbersOnly)
        return;
    const std::uint32_t count = store.size_;
    const std::size_t bytes = std::size_t(count) * sizeof(Value);
    const double* from = numbers();
    Value* to;
    char* fresh = nullptr;
    // Back to front, so that this works in the same room when it fits in the cell.
    const bool fits = inCell() && bytes <= roomInCell();
    if (fits) {
        to = reinterpret_cast<Value*>(store.data_);
    } else if (count) {
        reserveGrowth(bytes);
        fresh = PoolAlloc<char>().allocate(bytes);
        to = reinterpret_cast<Value*>(fresh);
    } else {
        to = nullptr;
    }
    for (std::uint32_t i = count; i-- > 0;) {
        std::uint64_t bits;
        std::memcpy(&bits, from + i, 8);
        if (bits == kUndefinedElement)
            new (to + i) Value(Value::undefined());
        else
            new (to + i) Value(from[i]);
    }
    if (fits) {
        store.cap_ = std::uint32_t(roomInCell() / sizeof(Value));
    } else {
        const std::size_t before = inCell() ? 0 : std::size_t(store.cap_) * sizeof(double);
        freeStorage();
        if (fresh) {
            store.data_ = fresh;
            store.cap_ = count;
            if (bytes > before)
                chargeGrowth(bytes - before);
        } else {
            store.data_ = roomInCell() ? static_cast<void*>(this + 1) : nullptr;
            store.cap_ = std::uint32_t(roomInCell() / sizeof(Value));
        }
    }
    numbersOnly = false;
}

void DynArray::append(Value v)
{
    if (numbersOnly && v.type() != Type::Double && !v.isUndefined())
        box();
    if (store.size_ == store.cap_)
        reserveElements(store.cap_ ? std::size_t(store.cap_) * 2 : 4);
    if (numbersOnly) {
        if (v.isUndefined())
            std::memcpy(numbers() + store.size_, &kUndefinedElement, 8);
        else
            numbers()[store.size_] = detail::Access::r8(v);
    } else {
        new (store.begin() + store.size_) Value(std::move(v));
    }
    ++store.size_;
}

void DynArray::appendAll(const Value* items, std::size_t count)
{
    growFor(std::size_t(store.size_) + count);
    for (std::size_t i = 0; i < count; ++i)
        append(items[i]);
}

void DynArray::resize(std::size_t n)
{
    if (n > store.size_)
        reserveElements(n);
    if (numbersOnly) {
        for (std::size_t i = store.size_; i < n; ++i)
            std::memcpy(numbers() + i, &kUndefinedElement, 8);
    } else {
        Value* values = store.begin();
        for (std::size_t i = n; i < store.size_; ++i)
            values[i].~Value();
        for (std::size_t i = store.size_; i < n; ++i)
            new (values + i) Value(Value::undefined());
    }
    store.size_ = std::uint32_t(n);
    trimElements();
}

void DynArray::removeRange(std::size_t from, std::size_t to)
{
    if (from >= to)
        return;
    const std::size_t count = store.size_;
    if (numbersOnly) {
        std::memmove(static_cast<void*>(numbers() + from), numbers() + to, (count - to) * sizeof(double));
    } else {
        Value* values = store.begin();
        for (std::size_t i = from; i < to; ++i)
            values[i].~Value();
        // A value moves by its bytes: nothing in one points at itself.
        std::memmove(static_cast<void*>(values + from), static_cast<const void*>(values + to), (count - to) * sizeof(Value));
    }
    store.size_ = std::uint32_t(count - (to - from));
}

void DynArray::insertRange(std::size_t at, const Value* items, std::size_t count)
{
    if (!count)
        return;
    if (numbersOnly) {
        for (std::size_t i = 0; i < count && numbersOnly; ++i)
            if (items[i].type() != Type::Double && !items[i].isUndefined())
                box();
    }
    growFor(std::size_t(store.size_) + count);
    const std::size_t tail = store.size_ - at;
    char* base = static_cast<char*>(store.data_) + at * elementBytes();
    std::memmove(static_cast<void*>(base + count * elementBytes()), static_cast<const void*>(base), tail * elementBytes());
    const std::size_t end = store.size_ + count;
    store.size_ = std::uint32_t(at);
    for (std::size_t i = 0; i < count; ++i)
        append(items[i]);
    store.size_ = std::uint32_t(end);
}

void DynArray::assignAll(const Value* items, std::size_t count)
{
    bool bare = true;
    for (std::size_t i = 0; bare && i < count; ++i)
        bare = items[i].type() == Type::Double || items[i].isUndefined();
    if (!numbersOnly) {
        Value* values = store.begin();
        for (std::uint32_t i = 0; i < store.size_; ++i)
            values[i].~Value();
    }
    store.size_ = 0;
    if (bare != numbersOnly) {
        freeStorage();
        store.data_ = nullptr;
        store.cap_ = 0;
        numbersOnly = bare;
        if (roomInCell()) {
            store.data_ = this + 1;
            store.cap_ = std::uint32_t(roomInCell() / elementBytes());
        }
    }
    reserveElements(count);
    for (std::size_t i = 0; i < count; ++i)
        append(items[i]);
}

void DynArray::reverse()
{
    if (numbersOnly)
        std::reverse(numbers(), numbers() + store.size_);
    else
        std::reverse(store.begin(), store.end());
}

void DynArray::trimElements()
{
    if (store.cap_ > 16 && store.cap_ > std::size_t(store.size_) * 4 && !inCell())
        moveTo(store.size_);
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
            // Whole numbers differ in the top of the mantissa only, and the table goes by the low bits.
            bits *= 0x9E3779B97F4A7C15ull;
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
