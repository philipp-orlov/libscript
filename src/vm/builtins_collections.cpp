// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Map, Set, WeakMap and WeakSet. The weak ones hold their keys like the others: a cycle through one is freed with the
// context, not before.
#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>

#include "script/internal/dyn/object.hpp"
#include "builtins_util.hpp"

namespace script {
namespace internal {
namespace vm {
namespace bi {

namespace {

struct KeyBox
{
    JSValue v;
};
struct KeyHash
{
    std::size_t operator()(const KeyBox& k) const noexcept
    {
        const JSValue v = k.v;
        if (v.isNumber()) {
            double d = v.asNumber();
            if (d == 0)
                d = 0;  // -0 and +0 are one key
            if (d != d)
                return 0x9e3779b9u;
            return std::hash<double>()(d);
        }
        if (isString(v))
            return std::hash<StringView>()(asString(v)->text());
        if (isBigInt(v)) {
            const auto* big = static_cast<const dyn::BigIntCell*>(v.asCell());
            std::size_t h = big->negative ? 0x51ed27u : 0x2545f491u;
            for (std::uint32_t digit : big->digits)
                h = h * 1099511628211ull + digit;
            return h;
        }
        return std::hash<std::uint64_t>()(v.bits);
    }
};
struct KeyEq
{
    bool operator()(const KeyBox& a, const KeyBox& b) const noexcept { return sameValueZero(a.v, b.v); }
};

struct Entry_
{
    JSValue key = JSValue::undefined();
    JSValue value = JSValue::undefined();
    std::uint64_t seq = 0;
    bool live = false;
};

struct TableState : NativeState
{
    std::vector<Entry_> entries;
    std::unordered_map<KeyBox, std::uint32_t, KeyHash, KeyEq> index;
    std::uint64_t nextSeq = 1;
    std::uint32_t dead = 0;
    bool isMap;
    bool weak;

    TableState(bool map, bool w) : isMap(map), weak(w) {}
    ~TableState() override { clear_(); }
    void clear(VM&) noexcept override { clear_(); }
    void clear_() noexcept
    {
        for (Entry_& e : entries) {
            if (e.live) {
                release(e.key);
                release(e.value);
            }
        }
        entries.clear();
        index.clear();
        dead = 0;
    }
    void values(std::vector<JSValue>& out) const override
    {
        for (const Entry_& e : entries)
            if (e.live) {
                out.push_back(e.key);
                out.push_back(e.value);
            }
    }
    std::size_t size() const noexcept { return entries.size() - dead; }

    Entry_* find(JSValue key)
    {
        auto it = index.find(KeyBox{key});
        return it == index.end() ? nullptr : &entries[it->second];
    }
    /// Take counts of its own on both.
    void set(JSValue key, JSValue value)
    {
        if (key.isDouble() && key.asDouble() == 0)
            key = JSValue::int32(0);
        if (Entry_* e = find(key)) {
            retain(value);
            release(e->value);
            e->value = value;
            return;
        }
        retain(key);
        retain(value);
        entries.push_back(Entry_{key, value, nextSeq++, true});
        index[KeyBox{key}] = std::uint32_t(entries.size() - 1);
    }
    bool remove(JSValue key)
    {
        auto it = index.find(KeyBox{key});
        if (it == index.end())
            return false;
        Entry_& e = entries[it->second];
        index.erase(it);
        const JSValue k = e.key, v = e.value;
        e.live = false;
        e.key = e.value = JSValue::undefined();
        ++dead;
        release(k);
        release(v);
        if (dead > 32 && dead > entries.size() / 2)
            compact();
        return true;
    }
    void removeAll()
    {
        for (Entry_& e : entries)
            if (e.live) {
                release(e.key);
                release(e.value);
                e.live = false;
                e.key = e.value = JSValue::undefined();
                ++dead;
            }
        index.clear();
        compact();
    }
    // Sequence numbers stay in order, which is all an iterator in the middle of the table relies on.
    void compact()
    {
        std::size_t to = 0;
        for (std::size_t i = 0; i < entries.size(); ++i)
            if (entries[i].live)
                entries[to++] = entries[i];
        entries.resize(to);
        dead = 0;
        index.clear();
        for (std::uint32_t i = 0; i < entries.size(); ++i)
            index[KeyBox{entries[i].key}] = i;
    }
    /// The first live entry after sequence `after`.
    const Entry_* after(std::uint64_t seq) const
    {
        auto it = std::upper_bound(entries.begin(), entries.end(), seq,
                                   [](std::uint64_t s, const Entry_& e) { return s < e.seq; });
        while (it != entries.end() && !it->live)
            ++it;
        return it == entries.end() ? nullptr : &*it;
    }
};

struct TableIter : IteratorState
{
    JSValue table;
    std::uint64_t last = 0;
    int kind;  // 0 keys, 1 values, 2 entries

    TableIter(JSValue t, int k) : table(t), kind(k) { retain(table); }
    ~TableIter() override { release(table); }
    void values(std::vector<JSValue>& out) const override { out.push_back(table); }
    void clear(VM&) noexcept override
    {
        release(table);
        table = JSValue::undefined();
    }
    int step(VM& vm, JSValue* out) override
    {
        if (!isObject(table))
            return 0;
        auto* t = static_cast<TableState*>(asObject(table)->rare()->ext);
        const Entry_* e = t->after(last);
        if (!e) {
            release(table);
            table = JSValue::undefined();
            return 0;
        }
        last = e->seq;
        if (kind == 0) {
            retain(e->key);
            *out = e->key;
        } else if (kind == 1) {
            const JSValue v = t->isMap ? e->value : e->key;
            retain(v);
            *out = v;
        } else {
            JSArray* pair = vm.newArray(vm.protos.array, 0, 2);
            Handle keep = Handle::adopt(toJSValue(pair));
            retain(e->key);
            pair->append(vm, e->key);
            const JSValue v = t->isMap ? e->value : e->key;
            retain(v);
            pair->append(vm, v);
            *out = keep.take();
        }
        return 1;
    }
};

TableState* thisTable(VM& vm, JSValue self, ObjClass cls, const char* method)
{
    if (isObject(self) && asObject(self)->cls() == cls)
        if (Rare* r = asObject(self)->rare(); r && r->ext)
            return static_cast<TableState*>(r->ext);
    typeError(vm, std::string("Method ") + method + " called on incompatible receiver " + describe(vm, self));
    return nullptr;
}

// What `data` says: the class in the low bits.
struct Kind
{
    ObjClass cls;
    bool isMap;
    bool weak;
    const char* name;
};
constexpr Kind kKinds[] = {{ObjClass::Map, true, false, "Map"},
                           {ObjClass::Set, false, false, "Set"},
                           {ObjClass::WeakMap, true, true, "WeakMap"},
                           {ObjClass::WeakSet, false, true, "WeakSet"}};

const Kind& kindOf(void* data) { return kKinds[reinterpret_cast<std::intptr_t>(data)]; }

bool validKey(VM& vm, const Kind& k, JSValue key)
{
    if (!k.weak || isObject(key))
        return true;
    typeError(vm, std::string("Invalid value used ") + (k.isMap ? "as weak map key" : "in weak set"));
    return false;
}

NATIVE(tableCtor)
{
    UNUSED();
    const Kind& k = kindOf(data);
    if (vm.newTarget.isUndefined() || !isObject(self))
        return typeError(vm, std::string("Constructor ") + k.name + " requires 'new'");
    JSObject* o = asObject(self);
    o->h.cls = std::uint8_t(k.cls);
    auto* t = new TableState(k.isMap, k.weak);
    o->rareOf(vm).ext = t;
    const JSValue init = arg(args, argc, 0);
    if (!init.isNullish()) {
        const bool ok = forEachIterated(vm, init, [&](JSValue item) {
            if (!k.isMap) {
                if (!validKey(vm, k, item))
                    return Step::Fail;
                t->set(item, JSValue::undefined());
                return Step::Continue;
            }
            if (!isObject(item)) {
                typeError(vm, "Iterator value " + describe(vm, item) + " is not an entry object");
                return Step::Fail;
            }
            JSValue key = getIndex(vm, item, 0);
            if (key.isException())
                return Step::Fail;
            Handle kk = Handle::adopt(key);
            JSValue value = getIndex(vm, item, 1);
            if (value.isException())
                return Step::Fail;
            Handle vv = Handle::adopt(value);
            if (!validKey(vm, k, key))
                return Step::Fail;
            t->set(key, value);
            return Step::Continue;
        });
        if (!ok)
            return JSValue::exception();
    }
    retain(self);
    return self;
}

NATIVE(tableGet)
{
    UNUSED();
    const Kind& k = kindOf(data);
    TableState* t = thisTable(vm, self, k.cls, "get");
    if (!t)
        return JSValue::exception();
    Entry_* e = t->find(arg(args, argc, 0));
    if (!e)
        return JSValue::undefined();
    retain(e->value);
    return e->value;
}

NATIVE(tableSet)
{
    UNUSED();
    const Kind& k = kindOf(data);
    TableState* t = thisTable(vm, self, k.cls, k.isMap ? "set" : "add");
    if (!t)
        return JSValue::exception();
    const JSValue key = arg(args, argc, 0);
    if (!validKey(vm, k, key))
        return JSValue::exception();
    t->set(key, k.isMap ? arg(args, argc, 1) : JSValue::undefined());
    retain(self);
    return self;
}

NATIVE(tableHas)
{
    UNUSED();
    TableState* t = thisTable(vm, self, kindOf(data).cls, "has");
    if (!t)
        return JSValue::exception();
    return JSValue::boolean(t->find(arg(args, argc, 0)) != nullptr);
}

NATIVE(tableDelete)
{
    UNUSED();
    TableState* t = thisTable(vm, self, kindOf(data).cls, "delete");
    if (!t)
        return JSValue::exception();
    return JSValue::boolean(t->remove(arg(args, argc, 0)));
}

NATIVE(tableClear)
{
    UNUSED();
    TableState* t = thisTable(vm, self, kindOf(data).cls, "clear");
    if (!t)
        return JSValue::exception();
    t->removeAll();
    return JSValue::undefined();
}

NATIVE(tableSize)
{
    UNUSED();
    TableState* t = thisTable(vm, self, kindOf(data).cls, "size");
    if (!t)
        return JSValue::exception();
    return JSValue::boxNumber(double(t->size()));
}

NATIVE(tableForEach)
{
    UNUSED();
    const Kind& k = kindOf(data);
    TableState* t = thisTable(vm, self, k.cls, "forEach");
    if (!t)
        return JSValue::exception();
    const JSValue callback = arg(args, argc, 0);
    if (!isFunction(callback))
        return typeError(vm, describe(vm, callback) + " is not a function");
    Handle keep = Handle::retained(self);
    std::uint64_t last = 0;
    while (const Entry_* e = t->after(last)) {
        last = e->seq;
        Handle key = Handle::retained(e->key);
        Handle value = Handle::retained(k.isMap ? e->value : e->key);
        JSValue callArgs[3] = {value.get(), key.get(), self};
        JSValue r = vm.call(callback, arg(args, argc, 1), callArgs, 3);
        if (r.isException())
            return r;
        release(r);
    }
    return JSValue::undefined();
}

NATIVE(tableIterator)
{
    UNUSED();
    const std::intptr_t packed = reinterpret_cast<std::intptr_t>(data);
    const Kind& k = kKinds[packed & 3];
    if (!thisTable(vm, self, k.cls, "iterator"))
        return JSValue::exception();
    return newNativeIterator(vm, new TableIter(self, int(packed >> 2)));
}

}  // namespace

void installCollections(VM& vm)
{
    JSObject* objectProto = vm.protos.object;
    JSObject** protoSlots[] = {&vm.protos.map, &vm.protos.set, &vm.protos.weakMap, &vm.protos.weakSet};
    for (int i = 0; i < 4; ++i) {
        const Kind& k = kKinds[i];
        JSObject* proto = vm.newObject(objectProto);
        vm.setIntrinsic(protoSlots[i], proto);
        release(toJSValue(proto));
        void* data = reinterpret_cast<void*>(std::intptr_t(i));
        JSFunction* ctor = constructor(vm, k.name, tableCtor, 0, proto);
        ctor->rareOf(vm).nativeData = data;
        if (!k.isMap || true) {
            method(vm, proto, k.isMap ? "get" : "add", k.isMap ? tableGet : tableSet, k.isMap ? 1 : 1, data);
        }
        if (k.isMap) {
            method(vm, proto, "set", tableSet, 2, data);
        }
        method(vm, proto, "has", tableHas, 1, data);
        method(vm, proto, "delete", tableDelete, 1, data);
        proto->putOwn(vm, vm.atoms.symToStringTag, makeString(vm, std::string_view(k.name)), attr::kConfigurable);
        if (k.weak)
            continue;
        method(vm, proto, "clear", tableClear, 0, data);
        method(vm, proto, "forEach", tableForEach, 1, data);
        {
            Key key = vm.internKey(std::string_view("size"));
            Key fnName = vm.internKey(std::string_view("get size"));
            JSFunction* getter = vm.newNativeFunction(vm.protos.function, tableSize, data, fnName, 0);
            Handle keep = Handle::adopt(toJSValue(getter));
            defineAccessor(vm, proto, key, toJSValue(getter), JSValue::undefined(), attr::kConfigurable);
            detail::decRef(key);
            detail::decRef(fnName);
        }
        auto packed = [&](int kind) { return reinterpret_cast<void*>(std::intptr_t(i | (kind << 2))); };
        JSFunction* entries = method(vm, proto, "entries", tableIterator, 0, packed(2));
        JSFunction* values = method(vm, proto, "values", tableIterator, 0, packed(1));
        JSFunction* keys = k.isMap ? method(vm, proto, "keys", tableIterator, 0, packed(0)) : values;
        if (!k.isMap) {
            retain(toJSValue(values));
            value(vm, proto, "keys", toJSValue(values));
        }
        (void)keys;
        JSFunction* iter = k.isMap ? entries : values;
        retain(toJSValue(iter));
        proto->putOwn(vm, vm.atoms.symIterator, toJSValue(iter), attr::kWritable | attr::kConfigurable);
    }
}

}  // namespace bi
}  // namespace vm
}  // namespace internal
}  // namespace script
