// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Map, Set, WeakMap and WeakSet. The weak ones hold their keys like the others: a cycle through one is freed with the
// context, not before.
#include <algorithm>
#include <cmath>
#include <functional>
#include <cstring>
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
            // A whole number (the usual key) is mixed as an integer; the rest by their bits.
            std::uint64_t bits;
            if (d > -9.0e18 && d < 9.0e18 && d == double(std::int64_t(d)))
                bits = std::uint64_t(std::int64_t(d));
            else
                std::memcpy(&bits, &d, sizeof bits);
            bits *= 0x9e3779b97f4a7c15ull;
            return std::size_t(bits ^ (bits >> 32));
        }
        if (isString(v))
            return const_cast<StringCell*>(asString(v))->hash();  // kept by the string
        if (isBigInt(v)) {
            const auto* big = static_cast<const dyn::BigIntCell*>(v.asCell());
            std::size_t h = big->negative ? 0x51ed27u : 0x2545f491u;
            for (dyn::BigIntCell::Limb digit : big->digits())
                h = h * 1099511628211ull + digit;
            return h;
        }
        return std::hash<std::uint64_t>()(v.bits);
    }
};
struct KeyEq
{
    bool operator()(const KeyBox& a, const KeyBox& b) const noexcept { return a.v.bits == b.v.bits || sameValueZero(a.v, b.v); }
};

struct Entry_
{
    JSValue key = JSValue::undefined();
    JSValue value = JSValue::undefined();
    std::uint64_t seq = 0;
    bool live = false;
};

/// Where the entries of a table are, by key: open addressing over the entries' positions (0 empty, else position + 1). An entry
/// that was deleted stays in its slot until the table is rebuilt, so a probe goes past it.
struct KeyIndex
{
    /// Pointers and small integers differ in few bits, and the low ones are what a mask keeps: mixed first.
    static std::size_t spread(JSValue key) noexcept
    {
        std::uint64_t h = KeyHash()(KeyBox{key});
        h *= 0x9e3779b97f4a7c15ull;
        return std::size_t(h ^ (h >> 29));
    }
    Vec<std::uint32_t> slots;
    std::size_t used = 0;  ///< slots that are not empty

    template <class Entries>
    int find(const Entries& entries, JSValue key) const noexcept
    {
        if (slots.empty())
            return -1;
        const std::size_t mask = slots.size() - 1;
        for (std::size_t at = spread(key) & mask;; at = (at + 1) & mask) {
            const std::uint32_t s = slots[at];
            if (!s)
                return -1;
            const auto& e = entries[s - 1];
            if (e.live && KeyEq()(KeyBox{e.key}, KeyBox{key}))
                return int(s - 1);
        }
    }
    /// The entry at `position` has just been added.
    template <class Entries>
    void add(const Entries& entries, std::uint32_t position)
    {
        if ((used + 1) * 2 > slots.size())
            rebuild(entries);
        else
            place(entries[position].key, position);
    }
    template <class Entries>
    void rebuild(const Entries& entries)
    {
        std::size_t live = 0;
        for (const auto& e : entries)
            live += e.live;
        std::size_t cap = 8;
        while (cap < (live + 1) * 4)
            cap *= 2;
        slots.assign(cap, 0);
        used = 0;
        for (std::uint32_t i = 0; i < entries.size(); ++i)
            if (entries[i].live)
                place(entries[i].key, i);
    }
    void clear() noexcept
    {
        slots.clear();
        used = 0;
    }

private:
    void place(JSValue key, std::uint32_t position) noexcept
    {
        const std::size_t mask = slots.size() - 1;
        std::size_t at = spread(key) & mask;
        while (slots[at])
            at = (at + 1) & mask;
        slots[at] = position + 1;
        ++used;
    }
};

struct TableState : NativeState
{
    Vec<Entry_> entries;
    KeyIndex index;
    std::uint64_t nextSeq = 1;
    std::uint32_t dead = 0;
    bool isMap;
    bool weak;

    TableState(bool map, bool w) : isMap(map), weak(w) {}
    std::size_t cellBytes() const override { return sizeof *this; }  // (in its object's cell: `newTableObject`)
    // Made and dropped by the payload pool, so a table that comes and goes makes no call to the general allocator.
    static void* operator new(std::size_t n) { return PoolAlloc<char>().allocate(n); }
    static void operator delete(void* p, std::size_t n) noexcept { PoolAlloc<char>().deallocate(static_cast<char*>(p), n); }
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
    void values(Vec<JSValue>& out) const override
    {
        for (const Entry_& e : entries)
            if (e.live) {
                out.push_back(e.key);
                out.push_back(e.value);
            }
    }
    bool ephemeron() const override { return weak; }
    void dropEntries(bool (*isDead)(void*, JSValue), void* context) override
    {
        for (Entry_& e : entries) {
            if (!e.live || !isDead(context, e.key))
                continue;
            const JSValue k = e.key, v = e.value;
            e.live = false;
            e.key = e.value = JSValue::undefined();
            ++dead;
            release(k);
            release(v);
        }
    }
    std::size_t size() const noexcept { return entries.size() - dead; }

    Entry_* find(JSValue key)
    {
        const int at = index.find(entries, key);
        return at < 0 ? nullptr : &entries[std::size_t(at)];
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
        index.add(entries, std::uint32_t(entries.size() - 1));
    }
    bool remove(JSValue key)
    {
        const int at = index.find(entries, key);
        if (at < 0)
            return false;
        Entry_& e = entries[std::size_t(at)];
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
        index.rebuild(entries);
    }
    /// The first live entry after sequence `after`. `hint`: where the entry of `seq` was last (an iterator's), kept up.
    const Entry_* after(std::uint64_t seq, std::uint32_t* hint = nullptr) const
    {
        auto it = hint && *hint < entries.size() && entries[*hint].seq == seq
                      ? entries.begin() + std::ptrdiff_t(*hint) + 1
                      : std::upper_bound(entries.begin(), entries.end(), seq,
                                         [](std::uint64_t s, const Entry_& e) { return s < e.seq; });
        while (it != entries.end() && !it->live)
            ++it;
        if (hint && it != entries.end())
            *hint = std::uint32_t(it - entries.begin());
        return it == entries.end() ? nullptr : &*it;
    }
};

/// The state of a Map or Set object (its class checked by the caller): on the side when its constructor made it, in its cell
/// when compiled code did (`newTableObject`).
TableState* tableOf(JSObject* o) noexcept
{
    Rare* r = o->rare();
    return static_cast<TableState*>(r && r->ext ? r->ext : o->embeddedState());
}

struct TableIter : IteratorState
{
    JSValue table;
    std::uint64_t last = 0;
    std::uint32_t at = ~0u;  ///< where the entry of `last` is (`TableState::after`)
    int kind;  // 0 keys, 1 values, 2 entries

    TableIter(JSValue t, int k) : table(t), kind(k) { retain(table); }
    ~TableIter() override { release(table); }
    std::size_t cellBytes() const override { return sizeof *this; }
    void values(Vec<JSValue>& out) const override { out.push_back(table); }
    void clear(VM&) noexcept override
    {
        release(table);
        table = JSValue::undefined();
    }
    int step(VM& vm, JSValue* out) override
    {
        if (!isObject(table))
            return 0;
        TableState* t = tableOf(asObject(table));
        const Entry_* e = t->after(last, &at);
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
        if (TableState* t = tableOf(asObject(self)))
            return t;
    (void)method;
    typeError(vm, std::string("not a ") + (cls == ObjClass::Map ? "Map" : cls == ObjClass::Set ? "Set" : cls == ObjClass::WeakMap ? "WeakMap" : "WeakSet"));
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
    // An object, or a symbol that `Symbol.for` did not make.
    if (!k.weak || isObject(key) || (isSymbol(key) && !static_cast<dyn::SymbolCell*>(key.asCell())->registered))
        return true;
    typeError(vm, k.isMap ? "invalid value used as a WeakMap key" : "invalid value used in a WeakSet");
    return false;
}

NATIVE(tableSet);

NATIVE(tableCtor)
{
    UNUSED();
    const Kind& k = kindOf(data);
    if (vm.newTarget.isUndefined() || !isObject(self))
        return typeError(vm, std::string("Constructor ") + k.name + " requires 'new'");
    JSObject* o = asObject(self);
    auto* t = new TableState(k.isMap, k.weak);
    o->rareOf(vm).ext = t;
    o->h.cls = std::uint8_t(k.cls);  // (after the state is there: the class says where to find it)
    const JSValue init = arg(args, argc, 0);
    if (!init.isNullish()) {
        // The entries go in through the `set` or `add` the new object has, which a subclass may have replaced.
        Handle adder = Handle::adopt(getProperty(vm, self, k.isMap ? vm.atoms.set : vm.atoms.add));
        if (adder.get().isException())
            return JSValue::exception();
        if (!isFunction(adder.get()))
            return typeError(vm, std::string("'") + (k.isMap ? "set" : "add") + "' returned for property of the new " + k.name + " is not a function");
        auto* fn = static_cast<JSFunction*>(asObject(adder.get()));
        const bool pristine = (fn->flags & JSFunction::kNative) && fn->rare() && fn->rare()->native == tableSet &&
                              fn->rare()->nativeData == data;
        const bool ok = forEachIterated(vm, init, [&](JSValue item) {
            if (k.isMap && !isObject(item)) {
                typeError(vm, "Iterator value " + describe(vm, item) + " is not an entry object");
                return Step::Fail;
            }
            if (!k.isMap) {
                if (pristine) {
                    if (!validKey(vm, k, item))
                        return Step::Fail;
                    t->set(item, JSValue::undefined());
                    return Step::Continue;
                }
                JSValue r = vm.call(adder.get(), self, &item, 1);
                if (r.isException())
                    return Step::Fail;
                release(r);
                return Step::Continue;
            }
            JSValue key = getIndex(vm, item, 0);
            if (key.isException())
                return Step::Fail;
            Handle kk = Handle::adopt(key);
            JSValue value = getIndex(vm, item, 1);
            if (value.isException())
                return Step::Fail;
            Handle vv = Handle::adopt(value);
            if (pristine) {
                if (!validKey(vm, k, key))
                    return Step::Fail;
                t->set(key, value);
                return Step::Continue;
            }
            JSValue pair[2] = {key, value};
            JSValue r = vm.call(adder.get(), self, pair, 2);
            if (r.isException())
                return Step::Fail;
            release(r);
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
    std::uint32_t at = ~0u;
    const VM::RepeatedCall call = vm.prepareCall(callback, arg(args, argc, 1));
    while (const Entry_* e = t->after(last, &at)) {
        last = e->seq;
        Handle key = Handle::retained(e->key);
        Handle value = Handle::retained(k.isMap ? e->value : e->key);
        JSValue callArgs[3] = {value.get(), key.get(), self};
        JSValue r = vm.callRepeated(call, callArgs, 3);
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
    return newNativeIterator<TableIter>(vm, k.cls == ObjClass::Map ? kMapIterator : kSetIterator, self, int(packed >> 2));
}

}  // namespace

/// The iterator of a Map or Set as made, whose prototype gives the iterator it was made with: that iterator and its `next`,
/// owned, with nothing looked up or called.
bool tableIterate(VM& vm, JSValue src, JSValue* iterator, JSValue* next)
{
    JSObject* o = asObject(src);
    const bool map = o->cls() == ObjClass::Map;
    if (!map && o->cls() != ObjClass::Set)
        return false;
    JSObject* proto = map ? vm.protos.map : vm.protos.set;
    const int family = map ? kMapIterator : kSetIterator;
    JSObject* ip = vm.protos.iteratorFamilies[family];
    if (!proto || !ip || !tableOf(o) || o->isDictionary(vm) || o->shapeOf(vm)->count != 0 || o->proto(vm) != proto)
        return false;
    std::uint32_t& epoch = vm.tableEpoch[map];
    if (epoch != vm.globalEpoch) {
        std::uint8_t attrs = 0;
        const JSValue* slot = proto->findOwn(vm, vm.atoms.symIterator, &attrs);
        if (!slot || (attrs & attr::kAccessor) || slot->bits != vm.tableIterators[map].bits)
            return false;
        slot = ip->findOwn(vm, vm.atoms.next, &attrs);
        if (!slot || (attrs & attr::kAccessor) || slot->bits != vm.origNativeNext[family].bits)
            return false;
        if (vm.watchMethod(proto, proto) && vm.watchMethod(ip, ip))
            epoch = vm.globalEpoch;
    }
    *iterator = newNativeIterator<TableIter>(vm, family, src, map ? 2 : 1);
    *next = vm.origNativeNext[family];
    retain(*next);
    return true;
}

namespace {

// ---------------------------------------------------------- WeakRef and friends ---

struct RefState : NativeState
{
    JSValue target = JSValue::undefined();
    ~RefState() override { release(target); }
    void clear(VM&) noexcept override
    {
        release(target);
        target = JSValue::undefined();
    }
    void values(Vec<JSValue>& out) const override { out.push_back(target); }
};

NATIVE(weakRefCtor)
{
    UNUSED();
    if (vm.newTarget.isUndefined() || !isObject(self))
        return typeError(vm, "Constructor WeakRef requires 'new'");
    const JSValue t = arg(args, argc, 0);
    if (!isObject(t) && !isSymbol(t))
        return typeError(vm, "WeakRef: invalid target");
    auto* state = new RefState();
    retain(t);
    state->target = t;
    delete asObject(self)->rareOf(vm).ext;
    asObject(self)->rareOf(vm).ext = state;
    retain(self);
    return self;
}

NATIVE(weakRefDeref)
{
    UNUSED();
    if (isObject(self))
        if (Rare* r = asObject(self)->rare())
            if (auto* state = dynamic_cast<RefState*>(r->ext)) {
                retain(state->target);
                return state->target;
            }
    return typeError(vm, "WeakRef.prototype.deref called on incompatible receiver");
}

/// A FinalizationRegistry keeps its callback and the unregister tokens of its cells. The collector here counts references, so a
/// target never goes away while the program can name it and nothing is ever called back; what a program can observe -- the
/// checks `register` and `unregister` make, and what `unregister` answers -- is kept.
struct RegistryState : NativeState
{
    JSValue cleanup = JSValue::undefined();
    Vec<JSValue> tokens;
    ~RegistryState() override { clear_(); }
    void clear(VM&) noexcept override { clear_(); }
    void clear_() noexcept
    {
        release(cleanup);
        cleanup = JSValue::undefined();
        for (JSValue t : tokens)
            release(t);
        tokens.clear();
    }
    void values(Vec<JSValue>& out) const override
    {
        out.push_back(cleanup);
        out.insert(out.end(), tokens.begin(), tokens.end());
    }
};

bool canBeHeldWeakly(JSValue v)
{
    return isObject(v) || (isSymbol(v) && !static_cast<dyn::SymbolCell*>(v.asCell())->registered);
}

RegistryState* thisRegistry(VM& vm, JSValue self, const char* method)
{
    if (isObject(self))
        if (Rare* r = asObject(self)->rare())
            if (auto* state = dynamic_cast<RegistryState*>(r->ext))
                return state;
    typeError(vm, std::string("FinalizationRegistry.prototype.") + method + " called on incompatible receiver");
    return nullptr;
}

NATIVE(registryCtor)
{
    UNUSED();
    if (vm.newTarget.isUndefined() || !isObject(self))
        return typeError(vm, "Constructor FinalizationRegistry requires 'new'");
    if (!isFunction(arg(args, argc, 0)))
        return typeError(vm, "FinalizationRegistry: cleanup must be callable");
    auto* state = new RegistryState();
    retain(args[0]);
    state->cleanup = args[0];
    delete asObject(self)->rareOf(vm).ext;
    asObject(self)->rareOf(vm).ext = state;
    retain(self);
    return self;
}

NATIVE(registryRegister)
{
    UNUSED();
    RegistryState* state = thisRegistry(vm, self, "register");
    if (!state)
        return JSValue::exception();
    const JSValue target = arg(args, argc, 0), held = arg(args, argc, 1), token = arg(args, argc, 2);
    if (!canBeHeldWeakly(target))
        return typeError(vm, "FinalizationRegistry.prototype.register: invalid target");
    if (target == held || (target.isNumber() && held.isNumber() && target.asNumber() == held.asNumber()))
        return typeError(vm, "FinalizationRegistry.prototype.register: target and holdings must not be same");
    if (!canBeHeldWeakly(token)) {
        if (!token.isUndefined())
            return typeError(vm, "FinalizationRegistry.prototype.register: invalid unregister token");
    } else {
        retain(token);
        state->tokens.push_back(token);
    }
    return JSValue::undefined();
}

NATIVE(registryUnregister)
{
    UNUSED();
    RegistryState* state = thisRegistry(vm, self, "unregister");
    if (!state)
        return JSValue::exception();
    const JSValue token = arg(args, argc, 0);
    if (!canBeHeldWeakly(token))
        return typeError(vm, "FinalizationRegistry.prototype.unregister: invalid unregister token");
    bool removed = false;
    for (std::size_t i = 0; i < state->tokens.size();) {
        if (state->tokens[i] == token) {
            release(state->tokens[i]);
            state->tokens.erase(state->tokens.begin() + std::ptrdiff_t(i));
            removed = true;
        } else {
            ++i;
        }
    }
    return JSValue::boolean(removed);
}

}  // namespace

double collectionSize(JSValue v, ObjClass cls)
{
    if (isObject(v) && asObject(v)->cls() == cls)
        if (TableState* t = tableOf(asObject(v)))
            return double(t->size());
    return -1;
}

int mapLookup(JSValue map, JSValue key, JSValue* out)
{
    if (isObject(map) && asObject(map)->cls() == ObjClass::Map)
        if (TableState* t = tableOf(asObject(map)))
            if (Entry_* e = t->find(key)) {
                retain(e->value);
                *out = e->value;
                return 1;
            }
    return 0;
}

bool mapStore(JSValue map, JSValue key, JSValue value)
{
    if (isObject(map) && asObject(map)->cls() == ObjClass::Map)
        if (TableState* t = tableOf(asObject(map))) {
            t->set(key, value);
            return true;
        }
    return false;
}

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
        if (k.weak && i == 3) {
            // The last of the weak ones: the references that go with them.
            JSObject* refProto = vm.newObject(vm.protos.object);
            constructor(vm, "WeakRef", weakRefCtor, 1, refProto);
            method(vm, refProto, "deref", weakRefDeref, 0);
            refProto->putOwn(vm, vm.atoms.symToStringTag, makeString(vm, std::string_view("WeakRef")), attr::kConfigurable);
            release(toJSValue(refProto));
            JSObject* regProto = vm.newObject(vm.protos.object);
            constructor(vm, "FinalizationRegistry", registryCtor, 1, regProto);
            method(vm, regProto, "register", registryRegister, 2);
            method(vm, regProto, "unregister", registryUnregister, 1);
            regProto->putOwn(vm, vm.atoms.symToStringTag, makeString(vm, std::string_view("FinalizationRegistry")), attr::kConfigurable);
            release(toJSValue(regProto));
        }
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
        if (!k.weak) {
            vm.tableIterators[k.isMap] = toJSValue(iter);
            retain(toJSValue(iter));
        }
    }
}

}  // namespace bi

JSObject* newTableObject(VM& vm, JSFunction* f)
{
    const bi::Kind& k = bi::kindOf(f->rare()->nativeData);
    const JSValue pv = *f->slotAt(2);
    Shape* root = vm.rootShapeFor(isObject(pv) ? asObject(pv) : vm.protos.object, 3);
    const std::size_t bytes = JSObject::bytesFor(3) + sizeof(bi::TableState);
    if (k.weak || bytes > mem::LocalHeap::kMaxSmall) {
        JSObject* o = vm.newObject(root, 3);
        Handle keep = Handle::adopt(toJSValue(o));
        auto* t = new bi::TableState(k.isMap, k.weak);
        o->rareOf(vm).ext = t;
        o->h.cls = std::uint8_t(k.cls);
        return asObject(keep.take());
    }
    auto* o = static_cast<JSObject*>(vm.allocCell(bytes, Kind::Object, k.cls, 3));
    o->h.shape = root->id;
    o->slots = nullptr;
    ::new (static_cast<void*>(o->embeddedState())) bi::TableState(k.isMap, false);
    return o;
}

FastNative tableCtorNative() noexcept
{
    return bi::tableCtor;
}
}  // namespace vm
}  // namespace internal
}  // namespace script
