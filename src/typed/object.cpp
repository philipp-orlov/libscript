// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/typed/object.hpp"

#include <algorithm>
#include <cstring>

#include "script/internal/typed/runtime.hpp"

namespace script {

namespace internal {
namespace typed {

std::size_t hashValue(const Value& v) noexcept
{
    switch (v.type()) {
        case Type::Null: return 0;
        case Type::Bool: return detail::Access::boolean(v) ? 1231u : 1237u;
        case Type::Char:
        case Type::Int: return std::size_t(detail::Access::i4(v)) * 2654435761u;
        case Type::Long: return std::size_t(detail::Access::i8(v)) * 2654435761u;
        case Type::Float: return std::size_t(detail::Access::r4(v) * 1000.0f);
        case Type::Double: {
            const double d = detail::Access::r8(v);
            std::uint64_t bits = 0;
            std::memcpy(&bits, &d, sizeof bits);
            return std::size_t(bits ^ (bits >> 32));
        }
        case Type::String: {
            auto* s = static_cast<StringCell*>(detail::Access::ptr(v));
            return s->hash();
        }
        default:
            return std::size_t(reinterpret_cast<std::uintptr_t>(detail::Access::ptr(v)) >> 4);
    }
}

bool valueEquals(const Value& a, const Value& b) noexcept
{
    if (a.type() != b.type()) {
        // A boxed int and a boxed long compare by value, as `Equals` does.
        if (a.isNumber() && b.isNumber())
            return a.toDouble() == b.toDouble();
        return false;
    }
    switch (a.type()) {
        case Type::Null: return true;
        case Type::Bool: return detail::Access::boolean(a) == detail::Access::boolean(b);
        case Type::Char:
        case Type::Int: return detail::Access::i4(a) == detail::Access::i4(b);
        case Type::Long: return detail::Access::i8(a) == detail::Access::i8(b);
        case Type::Float: return detail::Access::r4(a) == detail::Access::r4(b);
        case Type::Double: return detail::Access::r8(a) == detail::Access::r8(b);
        case Type::String: {
            auto* x = static_cast<StringCell*>(detail::Access::ptr(a));
            auto* y = static_cast<StringCell*>(detail::Access::ptr(b));
            return x == y || (x && y && x->text() == y->text());
        }
        default: return detail::Access::ptr(a) == detail::Access::ptr(b);
    }
}

const TypeInfo* typeOfCell(const GCObject* cell) noexcept
{
    if (!cell)
        return nullptr;
    switch (cell->kind) {
        case CellKind::Instance: return static_cast<const CSInstance*>(cell)->type;
        case CellKind::Array: return static_cast<const CSArray*>(cell)->elemType;
        case CellKind::Delegate: return static_cast<const CSDelegate*>(cell)->type;
        case CellKind::List: return static_cast<const CSList*>(cell)->type;
        case CellKind::Dictionary: return static_cast<const CSDict*>(cell)->type;
        case CellKind::Builder: return static_cast<const CSBuilder*>(cell)->type;
        case CellKind::Native:
            return static_cast<const TypeInfo*>(static_cast<const NativeCell*>(cell)->typedType);
        default: return nullptr;
    }
}

// ------------------------------------------------------------ instances ---

void CSInstance::initSlots(std::uint32_t n) noexcept
{
    count = n;
    Value* first = slots();
    for (std::uint32_t i = 0; i < n; ++i)
        new (first + i) Value();
}

CSInstance::~CSInstance()
{
    Value* first = slots();
    for (std::uint32_t i = 0; i < count; ++i)
        first[i].~Value();
}

void CSInstance::children(GcList& out) const
{
    const Value* first = slots();
    for (std::uint32_t i = 0; i < count; ++i) {
        if (GCObject* p = detail::Access::ptr(first[i]))
            out.push_back(p);
    }
}

void CSInstance::clearReferences()
{
    Value* first = slots();
    for (std::uint32_t i = 0; i < count; ++i)
        first[i] = Value();
}

// --------------------------------------------------------------- arrays ---

ElemStore elemStoreOf(const TypeInfo* elementType) noexcept
{
    if (!elementType)
        return ElemStore::Ref;
    TypeKind kind = elementType->kind;
    if (kind == TypeKind::Enum && elementType->underlying)
        kind = elementType->underlying->kind;
    switch (kind) {
        case TypeKind::Bool:
        case TypeKind::SByte:
        case TypeKind::Byte: return ElemStore::I1;
        case TypeKind::Short:
        case TypeKind::UShort:
        case TypeKind::Char: return ElemStore::I2;
        case TypeKind::Int:
        case TypeKind::UInt: return ElemStore::I4;
        case TypeKind::Long:
        case TypeKind::ULong: return ElemStore::I8;
        case TypeKind::Float: return ElemStore::R4;
        case TypeKind::Double: return ElemStore::R8;
        default: return ElemStore::Ref;
    }
}

void CSArray::initElements() noexcept
{
    if (store == ElemStore::Ref) {
        Value* first = refs();
        for (std::uint32_t i = 0; i < length; ++i)
            new (first + i) Value();
        return;
    }
    const std::size_t bytes = std::size_t(length) * elemStoreWidth(store);
    if (bytes <= 64) {
        // The cell is carved in 16-byte granules after a header that is a multiple of 16, so whole words fit.
        auto* words = static_cast<std::uint64_t*>(data());
        for (std::size_t i = 0; i < (bytes + 7) / 8; ++i)
            words[i] = 0;
        return;
    }
    std::memset(data(), 0, bytes);
}

static_assert(sizeof(CSArray) % 16 == 0, "initElements clears whole words after the header");

CSArray::~CSArray()
{
    if (store != ElemStore::Ref)
        return;
    Value* first = refs();
    for (std::uint32_t i = 0; i < length; ++i)
        first[i].~Value();
}

Value CSArray::get(std::uint32_t i) const noexcept
{
    switch (store) {
        case ElemStore::Ref: return refs()[i];
        case ElemStore::I1: {
            const std::int8_t raw = static_cast<const std::int8_t*>(data())[i];
            if (elemKind == TypeKind::Bool)
                return Value(raw != 0);
            if (elemKind == TypeKind::Byte)
                return Value(std::int32_t(std::uint8_t(raw)));
            return Value(std::int32_t(raw));
        }
        case ElemStore::I2: {
            const std::int16_t raw = static_cast<const std::int16_t*>(data())[i];
            if (elemKind == TypeKind::Char)
                return Value(char16_t(raw));
            if (elemKind == TypeKind::UShort)
                return Value(std::int32_t(std::uint16_t(raw)));
            return Value(std::int32_t(raw));
        }
        case ElemStore::I4: return Value(static_cast<const std::int32_t*>(data())[i]);
        case ElemStore::I8: return Value(static_cast<const std::int64_t*>(data())[i]);
        case ElemStore::R4: return Value(static_cast<const float*>(data())[i]);
        case ElemStore::R8: return Value(static_cast<const double*>(data())[i]);
    }
    return Value();
}

void CSArray::set(std::uint32_t i, const Value& v) noexcept
{
    switch (store) {
        case ElemStore::Ref: refs()[i] = v; return;
        case ElemStore::I1:
            static_cast<std::int8_t*>(data())[i] =
                std::int8_t(v.isBool() ? (detail::Access::boolean(v) ? 1 : 0) : v.toInt());
            return;
        case ElemStore::I2: static_cast<std::int16_t*>(data())[i] = std::int16_t(v.toInt()); return;
        case ElemStore::I4: static_cast<std::int32_t*>(data())[i] = v.toInt(); return;
        case ElemStore::I8: static_cast<std::int64_t*>(data())[i] = v.toLong(); return;
        case ElemStore::R4: static_cast<float*>(data())[i] = float(v.toDouble()); return;
        case ElemStore::R8: static_cast<double*>(data())[i] = v.toDouble(); return;
    }
}

void CSArray::children(GcList& out) const
{
    if (store != ElemStore::Ref)
        return;
    const Value* first = refs();
    for (std::uint32_t i = 0; i < length; ++i) {
        if (GCObject* p = detail::Access::ptr(first[i]))
            out.push_back(p);
    }
}

void CSArray::clearReferences()
{
    if (store != ElemStore::Ref)
        return;
    Value* first = refs();
    for (std::uint32_t i = 0; i < length; ++i)
        first[i] = Value();
}

// ------------------------------------------------------------ delegates ---

void CSDelegate::children(GcList& out) const
{
    if (GCObject* p = detail::Access::ptr(target))
        out.push_back(p);
    for (const Ref<Box>& box : captured) {
        if (box)
            out.push_back(box.get());
    }
    if (next)
        out.push_back(next.get());
}

void CSDelegate::clearReferences()
{
    target = Value();
    captured.clear();
    next.reset();
}

// ---------------------------------------------------------- collections ---

void CSList::add(Value v)
{
    const std::size_t before = items.capacity();
    items.push_back(std::move(v));
    chargeIfGrown(items, before);
}

void CSList::children(GcList& out) const
{
    for (const Value& v : items) {
        if (GCObject* p = detail::Access::ptr(v))
            out.push_back(p);
    }
}

void CSList::clearReferences()
{
    items.clear();
    items.shrink_to_fit();
}

void CSDict::children(GcList& out) const
{
    for (const Entry& e : entries) {
        if (!e.live)
            continue;
        if (GCObject* p = detail::Access::ptr(e.key))
            out.push_back(p);
        if (GCObject* p = detail::Access::ptr(e.value))
            out.push_back(p);
    }
}

void CSDict::clearReferences()
{
    entries.clear();
    entries.shrink_to_fit();
    index.clear();
    index.shrink_to_fit();
    liveCount = 0;
}

CSDict::Entry* CSDict::find(const Value& key) noexcept
{
    if (index.empty())
        return nullptr;
    const std::size_t hash = hashValue(key);
    const std::size_t mask = index.size() - 1;
    std::size_t slot = hash & mask;
    for (std::size_t probe = 0; probe <= mask; ++probe) {
        const std::uint32_t at = index[slot];
        if (at == 0)
            return nullptr;
        Entry& e = entries[at - 1];
        if (e.live && e.hash == hash && valueEquals(e.key, key))
            return &e;
        slot = (slot + 1) & mask;
    }
    return nullptr;
}

void CSDict::rebuild()
{
    std::size_t wanted = 8;
    while (wanted < (entries.size() + 1) * 2)
        wanted *= 2;
    const std::size_t before = index.capacity();
    index.assign(wanted, 0);
    chargeIfGrown(index, before);

    const std::size_t mask = wanted - 1;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].live)
            continue;
        std::size_t slot = entries[i].hash & mask;
        while (index[slot] != 0)
            slot = (slot + 1) & mask;
        index[slot] = std::uint32_t(i + 1);
    }
}

void CSDict::put(const Value& key, Value value)
{
    if (Entry* existing = find(key)) {
        existing->value = std::move(value);
        return;
    }
    if (index.empty() || (entries.size() + 1) * 2 > index.size()) {
        compact();
        rebuild();
    }
    const std::size_t before = entries.capacity();
    Entry entry;
    entry.key = key;
    entry.value = std::move(value);
    entry.hash = hashValue(key);
    entry.live = true;
    entries.push_back(std::move(entry));
    chargeIfGrown(entries, before);
    ++liveCount;

    const std::size_t mask = index.size() - 1;
    std::size_t slot = entries.back().hash & mask;
    while (index[slot] != 0)
        slot = (slot + 1) & mask;
    index[slot] = std::uint32_t(entries.size());
}

bool CSDict::erase(const Value& key)
{
    Entry* entry = find(key);
    if (!entry)
        return false;
    entry->live = false;
    entry->key = Value();
    entry->value = Value();
    --liveCount;
    if (entries.size() > 16 && liveCount * 2 < entries.size()) {
        compact();
        rebuild();
    }
    return true;
}

void CSDict::compact()
{
    if (liveCount == entries.size())
        return;
    Vec<Entry> kept;
    kept.reserve(liveCount);
    for (Entry& e : entries) {
        if (e.live)
            kept.push_back(std::move(e));
    }
    entries.swap(kept);
}

}  // namespace typed
}  // namespace internal

}  // namespace script
