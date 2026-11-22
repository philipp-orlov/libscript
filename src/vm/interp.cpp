// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Tier 0: the interpreter. One loop over the register bytecode with computed-goto dispatch, calls between bytecode
// functions made inside it (a new frame on the VM stack, no C++ recursion), and one inline cache per property
// instruction. Registers own their values: every write releases what was there.
#include <algorithm>
#include <unordered_set>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "script/internal/dyn/object.hpp"
#include "script/internal/stack_guard.hpp"
#include "script/internal/vm/census.hpp"
#include "script/internal/vm/convert.hpp"
#include "script/internal/vm/compiler.hpp"
#include "script/internal/vm/jit.hpp"
#include "script/internal/vm/object.hpp"
#include "script/internal/vm/operators.hpp"
#include "script/internal/vm/typed.hpp"
#include "builtins_util.hpp"
#include "jit/opt_analysis.hpp"

#if defined(__GNUC__)
// The interpreter dispatches through the address of a label (computed goto), a GNU extension.
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

namespace script {
namespace internal {
namespace vm {

namespace {

/// SetFunctionName for a function made under a computed key: the key's text, a symbol's description in brackets, an
/// accessor's after `get ` or `set `. A function that already has a name keeps it.
void setFunctionName(VM& vm, JSValue fnv, Key key, const char* prefix)
{
    if (!isFunction(fnv))
        return;
    auto* f = static_cast<JSFunction*>(asObject(fnv));
    if (f->flags & JSFunction::kNative)
        return;
    const bool hasName = f->code->name && !static_cast<StringCell*>(f->code->name)->text().empty();
    if (hasName)
        return;
    String text;
    if (key->kind == detail::CellKind::Symbol) {
        const auto* sym = static_cast<const dyn::SymbolCell*>(key);
        if (sym->hasDescription)
            text = u"[" + sym->description + u"]";
    } else {
        text = String(static_cast<StringCell*>(key)->text());
    }
    if (*prefix)
        text = fromUtf8(prefix) + text;
    Key name = vm.internKey(StringView(text));
    if (f->flags & JSFunction::kLazyProps) {
        Rare& r = f->rareOf(vm);
        if (r.fnName)
            detail::decRef(r.fnName);
        r.fnName = name;
        return;
    }
    std::uint8_t attrs = 0;
    JSValue* slot = f->findOwn(vm, vm.atoms.name, &attrs);
    if (slot && !(attrs & (attr::kAccessor | attr::kWritable)) && isString(*slot) && asString(*slot)->length == 0) {
        const JSValue old = *slot;
        *slot = JSValue::cell(name);
        release(old);
        return;
    }
    detail::decRef(name);
}

inline bool isConstructorFn(const JSFunction* f)
{
    return (f->flags & JSFunction::kNative) ? (f->flags & JSFunction::kConstructor) != 0 : f->code->constructor;
}


constexpr std::uint32_t kInterruptInterval = 4096;
/// Calls and back edges a function has had when the interpreter moves a loop that keeps going into compiled code.
constexpr std::uint32_t kOsrAfter = 1000;

/// `===`, with what is common settled without a call: equal bits, numbers, and values that are not both cells.
[[gnu::always_inline]] inline bool fastStrictEquals(JSValue a, JSValue b) noexcept
{
    if (a.isInt32() && b.isInt32())
        return a.bits == b.bits;
    if (a.isNumber() && b.isNumber())
        return a.asNumber() == b.asNumber();
    if (a.bits == b.bits)
        return true;
    if (!a.isCell() || !b.isCell())
        return false;
    return strictEquals(a, b);
}

// Registers and counts. `Def` is `VM::deferred`: a register holds its value without a count, and what no register holds
// any more waits for the next scan of the frames.

/// A register = `value`, whose count the caller has taken: the register keeps it (`Def`: lets go of it).
template <bool Def>
[[gnu::always_inline]] inline void setReg(VM& vm, JSValue& slot, JSValue value) noexcept
{
    if constexpr (Def) {
        slot = value;
        vm.releaseDeferred(value);
    } else {
        (void)vm;
        const JSValue old = slot;
        slot = value;
        release(old);
    }
}

/// A register = `value`, which the caller holds.
template <bool Def>
[[gnu::always_inline]] inline void copyReg(JSValue& slot, JSValue value) noexcept
{
    if constexpr (Def) {
        slot = value;
    } else {
        retain(value);
        const JSValue old = slot;
        slot = value;
        release(old);
    }
}

/// Let go of what a register holds and leave undefined in it.
template <bool Def>
[[gnu::always_inline]] inline void clearReg(JSValue& slot) noexcept
{
    const JSValue v = slot;
    slot = JSValue::undefined();
    if constexpr (!Def)
        release(v);
}

/// `clearReg` of registers [from, to).
template <bool Def>
inline void clearRegs(JSValue* R, std::uint32_t from, std::uint32_t to) noexcept
{
    for (std::uint32_t i = from; i < to; ++i)
        clearReg<Def>(R[i]);
}

/// The count a register of the mode holds: none in a deferred frame.
inline void retainReg(VM& vm, JSValue v) noexcept
{
    if (!vm.deferred)
        retain(v);
}

// The same, for code that runs in either mode.
inline void setReg(VM& vm, JSValue& slot, JSValue value) noexcept
{
    vm.deferred ? setReg<true>(vm, slot, value) : setReg<false>(vm, slot, value);
}
inline void copyReg(VM& vm, JSValue& slot, JSValue value) noexcept
{
    vm.deferred ? copyReg<true>(slot, value) : copyReg<false>(slot, value);
}
inline void clearReg(VM& vm, JSValue& slot) noexcept
{
    vm.deferred ? clearReg<true>(slot) : clearReg<false>(slot);
}
inline void clearRegs(VM& vm, JSValue* R, std::uint32_t from, std::uint32_t to) noexcept
{
    vm.deferred ? clearRegs<true>(R, from, to) : clearRegs<false>(R, from, to);
}

JSValue notCallable(VM& vm, JSValue callee)
{
    return vm.throwError(ErrorCode::TypeError, describe(vm, callee) + " is not a function");
}

/// Define or replace one half of an accessor property in an object literal.
bool initAccessor(VM& vm, JSObject* object, Key key, JSValue fn, bool isGetter,
                  std::uint8_t attrs = attr::kEnumerable | attr::kConfigurable)
{
    JSValue getter = JSValue::undefined(), setter = JSValue::undefined();
    std::uint8_t existing = 0;
    if (JSValue* slot = object->findOwn(vm, key, &existing)) {
        if (existing & attr::kAccessor) {
            const auto* gs = reinterpret_cast<const GetterSetter*>(slot->asCell());
            getter = gs->getter;
            setter = gs->setter;
        }
    }
    if (isGetter)
        getter = fn;
    else
        setter = fn;
    // Keep the half that was there alive across the replacement.
    retain(getter);
    retain(setter);
    const bool ok = defineAccessor(vm, object, key, getter, setter, attrs);
    release(getter);
    release(setter);
    return ok;
}

bool isPrivateKey(JSValue v) noexcept
{
    return isSymbol(v) && static_cast<dyn::SymbolCell*>(v.asCell())->isPrivate;
}

std::string privateName(JSValue symbol)
{
    return toUtf8(static_cast<dyn::SymbolCell*>(symbol.asCell())->description);
}

/// The site's `PrivateCache`, or null when the function has none.
inline PrivateCache* privateCacheAt(CodeBlock* code, std::uint32_t index) noexcept
{
    return index < code->privCaches.size() ? &code->privCaches[index] : nullptr;
}

/// Remember where `base[symbol]` found an own data property (writable, for a store).
void learnPrivate(VM& vm, PrivateCache* pc, JSValue base, JSValue symbol, bool store) noexcept
{
    if (!pc || !isObject(base))
        return;
    JSObject* o = asObject(base);
    if (o->isDictionary(vm) || proxyOf(o) ||
        (o->cls() == ObjClass::Function && (static_cast<JSFunction*>(o)->flags & JSFunction::kLazyProps)))
        return;
    std::uint8_t attrs = 0;
    const std::int32_t index = o->shapeOf(vm)->indexOf(symbol.asCell(), &attrs);
    if (index < 0 || (attrs & attr::kAccessor) || (store && !(attrs & attr::kWritable)))
        return;
    *pc = PrivateCache{symbol.asCell(), o->h.shape, std::uint32_t(index)};
}

/// The slot `pc` names for `base[symbol]`, or null when it does not apply.
inline JSValue* privateCached(const PrivateCache* pc, JSValue base, JSValue symbol) noexcept
{
    if (!pc || pc->key != symbol.asCell() || !isObject(base) || asObject(base)->h.shape != pc->shape)
        return nullptr;
    return asObject(base)->slotAt(pc->index);
}

/// `base.#name`: the property must be the object's own.
JSValue privateGet(VM& vm, JSValue base, JSValue symbol)
{
    std::uint8_t attrs = 0;
    JSValue* slot = isObject(base) ? asObject(base)->findOwn(vm, symbol.asCell(), &attrs) : nullptr;
    if (!slot)
        return vm.throwError(ErrorCode::TypeError, "Cannot read private member " + privateName(symbol) +
                                                        " from an object whose class did not declare it");
    if (attrs & attr::kAccessor) {
        const auto* gs = reinterpret_cast<const GetterSetter*>(slot->asCell());
        if (!isFunction(gs->getter))
            return vm.throwError(ErrorCode::TypeError, "'" + privateName(symbol) + "' was defined without a getter");
        return vm.call(gs->getter, base, nullptr, 0);
    }
    retain(*slot);
    return *slot;
}

/// `base.#name = value`: false with an exception pending.
bool privatePut(VM& vm, JSValue base, JSValue symbol, JSValue value)
{
    std::uint8_t attrs = 0;
    JSValue* slot = isObject(base) ? asObject(base)->findOwn(vm, symbol.asCell(), &attrs) : nullptr;
    if (!slot) {
        vm.throwError(ErrorCode::TypeError, "Cannot write private member " + privateName(symbol) +
                                                 " to an object whose class did not declare it");
        return false;
    }
    if (attrs & attr::kAccessor) {
        const auto* gs = reinterpret_cast<const GetterSetter*>(slot->asCell());
        if (!isFunction(gs->setter)) {
            vm.throwError(ErrorCode::TypeError, "'" + privateName(symbol) + "' was defined without a setter");
            return false;
        }
        JSValue r = vm.call(gs->setter, base, &value, 1);
        if (r.isException())
            return false;
        release(r);
        return true;
    }
    if (!(attrs & attr::kWritable)) {
        vm.throwError(ErrorCode::TypeError, "Private method '" + privateName(symbol) + "' is not writable");
        return false;
    }
    const JSValue old = *slot;
    *slot = value;
    retain(value);
    release(old);
    return true;
}

/// The enumerable string keys `for (k in value)` visits: integer-like names in ascending order, then the others in
/// the order they were added, own properties first and then the prototype chain's, a name once.
JSValue forInKeys(VM& vm, JSValue value)
{
    JSArray* result = vm.newArray(vm.protos.array);
    Handle keep = Handle::adopt(toJSValue(result));
    // The names of the objects above the one being looked at, enumerable or not: a name an earlier object has hides the
    // same name of a later one. The names of one object are all different, so it is compared with the earlier ones only.
    std::vector<Key> earlier;
    std::unordered_set<Key> earlierSet;  // made when there are many and one is asked about
    std::size_t inSet = 0;
    auto seenBefore = [&](Key k) {
        if (earlier.size() <= 16)
            return std::find(earlier.begin(), earlier.end(), k) != earlier.end();
        for (; inSet < earlier.size(); ++inSet)
            earlierSet.insert(earlier[inSet]);
        return earlierSet.count(k) != 0;
    };
    auto remember = [&](const std::vector<Key>& level) { earlier.insert(earlier.end(), level.begin(), level.end()); };
    auto add = [&](Key k) {
        retain(JSValue::cell(k));
        result->append(vm, JSValue::cell(k));
    };
    auto visit = [&](JSObject* o) {
        if (!o->proto(vm) && !hasElems(o) && o->cls() != ObjClass::TypedArray && !o->isDictionary(vm)) {
            // The last object of the chain, with no enumerable name of its own (Object.prototype, usually): nothing to
            // list, and nothing above it for its names to hide.
            bool any = false;
            for (const Shape* sh = o->shapeOf(vm); sh && sh->count && !any; sh = sh->parent)
                any = sh->key->kind == detail::CellKind::String && (sh->attrs & attr::kEnumerable);
            if (!any && !(o->cls() == ObjClass::Function && (static_cast<JSFunction*>(o)->flags & JSFunction::kLazyProps)))
                return;
        }
        std::vector<std::uint32_t> indexes;
        std::vector<Key> named, level;
        if (hasElems(o)) {
            auto* a = static_cast<JSArray*>(o);
            for (std::uint32_t i = 0; i < a->length; ++i)
                if (!a->elems[i].isHole())
                    indexes.push_back(i);
        } else if (o->cls() == ObjClass::TypedArray) {
            for (std::uint32_t i = 0; i < static_cast<JSTypedArray*>(o)->length; ++i)
                indexes.push_back(i);
        }
        o->forEachOwn(vm, [&](Key k, std::uint8_t attrs, JSValue) {
            if (k->kind != detail::CellKind::String)
                return;
            if (!(attrs & attr::kEnumerable)) {
                level.push_back(k);  // hides the same name further up, though it is not listed
                return;
            }
            std::uint32_t index;
            if (arrayIndexOf(k, &index))
                indexes.push_back(index);
            else
                named.push_back(k);
        });
        std::sort(indexes.begin(), indexes.end());
        indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
        const bool first = earlier.empty();
        for (std::uint32_t index : indexes) {
            Key k = toPropertyKey(vm, JSValue::boxNumber(double(index)));
            if (first || !seenBefore(k))
                add(k);
            level.push_back(k);
            release(JSValue::cell(k));  // the property name table holds it on
        }
        for (Key k : named) {
            if (first || !seenBefore(k))
                add(k);
            level.push_back(k);
        }
        remember(level);
    };
    if (isObject(value) && asObject(value)->cls() == ObjClass::Plain && !asObject(value)->isDictionary(vm)) {
        // A plain object under prototypes with no name to list: its own names, in order.
        JSObject* o = asObject(value);
        bool clean = true;
        for (JSObject* p = o->proto(vm); p && clean; p = p->proto(vm)) {
            VM::ForInProto& known = vm.forInProtos[(reinterpret_cast<std::uintptr_t>(p) >> 5) % VM::kForInProtos];
            if (known.object == p && known.shape == p->h.shape && p->cls() == ObjClass::Plain)
                continue;
            clean = p->cls() == ObjClass::Plain && !proxyOf(p);
            if (clean)
                p->forEachOwn(vm, [&](Key k, std::uint8_t attrs, JSValue) {
                    clean = clean && !(k->kind == detail::CellKind::String && (attrs & attr::kEnumerable));
                });
            if (clean) {
                known.object = p;
                known.shape = p->h.shape;
            }
        }
        Shape* shape = o->shapeOf(vm);
        const std::uint32_t count = shape->count;
        if (clean) {
            result->reserve(vm, count);
            for (std::uint32_t i = 0; i < count && clean; ++i) {
                const Entry e = shape->entryAt(i);
                if (e.key->kind != detail::CellKind::String || !(e.attrs & attr::kEnumerable))
                    continue;
                std::uint32_t index;
                if (arrayIndexOf(e.key, &index))
                    clean = false;
                else
                    add(e.key);
            }
            if (clean)
                return keep.take();
            result->resize(vm, 0);
        }
    }
    if (isObject(value)) {
        Handle cur = Handle::retained(value);
        while (isObject(cur.get())) {
            JSObject* o = asObject(cur.get());
            if (proxyOf(o)) {
                Vec<Handle> names;
                if (!bi::objOwnKeysFiltered(vm, o, true, false, true, names))
                    return JSValue::exception();
                std::vector<Key> level;
                for (Handle& name : names) {
                    const Key k = static_cast<Key>(name.get().asCell());
                    if (!seenBefore(k))
                        add(k);
                    level.push_back(k);
                }
                remember(level);
            } else {
                visit(o);
            }
            Handle next;
            if (bi::objGetPrototypeOf(vm, o, &next) < 0)
                return JSValue::exception();
            cur = std::move(next);
        }
    } else if (isString(value)) {
        const std::uint32_t n = asString(value)->length;
        for (std::uint32_t i = 0; i < n; ++i)
            result->append(vm, toStringValue(vm, JSValue::int32(std::int32_t(i))));
    }
    return keep.take();
}


// -------------------------------------------------------------- the caches ---

/// Whether a read of a hole of `a`, or past its end, is undefined without a look anywhere: a plain array (no properties
/// of its own, which an index given attributes would be) whose prototypes are the usual ones, with no elements.
inline bool plainHoleRead(VM& vm, JSArray* a)
{
    return a->proto(vm) == vm.protos.array && !a->isDictionary(vm) && a->shapeOf(vm)->count == 0 && vm.plainHoleStores();
}

/// Whether `o` may hand out the position of a property: in the shape tree, and not a function whose properties are not
/// made yet.
inline bool cacheable(VM& vm, JSObject* o) noexcept
{
    return !o->isDictionary(vm) && !(o->cls() == ObjClass::Function && (static_cast<JSFunction*>(o)->flags & JSFunction::kLazyProps)) &&
           !proxyOf(o);
}

/// Whether the properties `o` has for `key` are those its shape says: also a function whose own properties are not made yet,
/// for a name other than the three it would make (`length`, `name`, `prototype`). Its shape is then that of every such
/// function, and making them changes it.
inline bool cacheableKey(VM& vm, JSObject* o, Key key) noexcept
{
    if (cacheable(vm, o))
        return true;
    return o->cls() == ObjClass::Function && (static_cast<JSFunction*>(o)->flags & JSFunction::kLazyProps) && !proxyOf(o) &&
           key != vm.atoms.length && key != vm.atoms.name && key != vm.atoms.prototype;
}

/// The slot index of `o`'s own property `key` when a cache may hold it under `o`'s shape: in the tree, or a dictionary read
/// often enough (its shape changes when the key would move). -1 otherwise.
std::int32_t cachedOwnIndex(VM& vm, JSObject* o, Key key, std::uint8_t* attrs) noexcept
{
    if (cacheable(vm, o))
        return o->shapeOf(vm)->indexOf(key, attrs);
    if (!o->isDictionary(vm) || proxyOf(o) ||
        (o->cls() == ObjClass::Function && (static_cast<JSFunction*>(o)->flags & JSFunction::kLazyProps)))
        return -1;
    const Dictionary& d = *o->rare()->dict;
    const std::int32_t i = d.find(key);
    if (i < 0 || !o->cacheDictionary(vm))
        return -1;
    *attrs = d.entries[std::uint32_t(i)].attrs;
    return i;
}

void clearValidity(VM& vm, InlineCache& ic) noexcept
{
    if (ic.validity)
        vm.releaseValidity(ic.validity);
    ic.validity = nullptr;
    ic.holder = nullptr;
}

/// A read found `key` as an own data property of `o`, or on a prototype: remember where.
bool learnRead(VM& vm, InlineCache& ic, JSObject* o, Key key)
{
    std::uint8_t attrs = 0;
    std::int32_t index = cachedOwnIndex(vm, o, key, &attrs);
    if (!cacheableKey(vm, o, key) && index < 0)
        return false;  // a dictionary: only what it holds itself (a key it adds would hide one inherited)
    if (index >= 0) {
        if (attrs & attr::kAccessor)
            return false;
        clearValidity(vm, ic);
        ic.shape = o->h.shape;
        ic.index = std::uint32_t(index);
        ic.next = 0;
        ic.aux = std::uint8_t(o->h.aux + 1u);
        return true;
    }
    // A property of a prototype: the cache holds the holder and a cell that says the chain has not changed.
    for (JSObject* p = o->proto(vm); p; p = p->proto(vm)) {
        if (!cacheableKey(vm, p, key))
            return false;
        index = p->shapeOf(vm)->indexOf(key, &attrs);
        if (index < 0)
            continue;
        if (attrs & attr::kAccessor)
            return false;
        ValidityCell* cell = vm.validityOf(o->proto(vm));
        vm.retainValidity(cell);
        clearValidity(vm, ic);
        ic.shape = o->h.shape;
        ic.holder = p;
        ic.index = std::uint32_t(index);
        ic.validity = cell;
        ic.next = 0;
        return true;
    }
    return false;
}

/// Put `entry` into the site's `PolyCache`, over the one of its shape or, when all four are taken, the oldest.
void placePoly(VM& vm, CodeBlock* code, InlineCache& ic, PolyEntry entry)
{
    if (!ic.pad) {
        code->poly.emplace_back();
        ic.pad = std::uint16_t(code->poly.size());
    }
    PolyCache& pc = code->poly[ic.pad - 1];
    std::uint8_t at = pc.n;
    for (std::uint8_t i = 0; i < pc.n; ++i)
        if (pc.e[i].shape == entry.shape)
            at = i;
    const bool known = at != pc.n;
    if (!known) {
        if (pc.n < 4) {
            ++pc.n;
        } else {
            at = pc.next;
            pc.next = std::uint8_t((pc.next + 1) & 3);
        }
    }
    if (known && pc.e[at].aux != entry.aux)
        entry.aux = 0;  // objects of this shape come with different inline slot counts
    if (pc.e[at].validity)
        vm.releaseValidity(pc.e[at].validity);
    pc.e[at] = entry;
}

/// Whether `o` answers for a key without a property of its own (a string's characters, a typed array's numbers, an array's
/// length and elements): the receiver or a prototype that does cannot be part of a cached miss.
inline bool answersForKeys(JSObject* o) noexcept
{
    return o->cls() == ObjClass::TypedArray || o->cls() == ObjClass::String || hasElems(o);
}

/// A read of `key` on `o` found it nowhere: remember that for objects of this shape while the chain stays as it is. A prototype
/// that gains the key invalidates the chain (`invalidateChain`), and the receiver's shape fixes its own keys and its prototype.
bool learnAbsent(VM& vm, CodeBlock* code, InlineCache& ic, JSObject* o, Key key, bool numericKey)
{
    if (numericKey || !cacheableKey(vm, o, key))
        return false;
    for (JSObject* p = o; p; p = p->proto(vm)) {
        if (!cacheableKey(vm, p, key) || answersForKeys(p))
            return false;
    }
    PolyEntry entry;
    entry.kind = PolyEntry::Absent;
    entry.shape = o->h.shape;
    if (JSObject* p = o->proto(vm)) {
        entry.validity = vm.validityOf(p);
        vm.retainValidity(entry.validity);
    }
    entry.aux = std::uint8_t(o->h.aux + 1u);
    placePoly(vm, code, ic, entry);
    return true;
}

/// Remember where a read of `key` on `o` finds it in the site's `PolyCache`: a data property or an accessor, own or inherited.
bool learnPoly(VM& vm, CodeBlock* code, InlineCache& ic, JSObject* o, Key key)
{
    if (!cacheableKey(vm, o, key) && !(o->isDictionary(vm) && o->cls() != ObjClass::TypedArray))
        return false;
    // A typed array answers a canonical numeric name itself, whatever its prototypes have.
    double numeric;
    const bool numericKey = canonicalNumericIndex(key, &numeric);
    if (numericKey && o->cls() == ObjClass::TypedArray)
        return false;
    PolyEntry entry;
    std::uint8_t attrs = 0;
    std::int32_t index = cachedOwnIndex(vm, o, key, &attrs);
    bool inherited = false;
    JSObject* holder = nullptr;
    if (index < 0) {
        if (!cacheableKey(vm, o, key))
            return false;
        for (JSObject* p = o->proto(vm); p; p = p->proto(vm)) {
            if (!cacheableKey(vm, p, key) || (numericKey && p->cls() == ObjClass::TypedArray))
                return false;
            index = p->shapeOf(vm)->indexOf(key, &attrs);
            if (index >= 0) {
                holder = p;
                inherited = true;
                break;
            }
        }
        if (!inherited)
            return learnAbsent(vm, code, ic, o, key, numericKey);
    }
    entry.shape = o->h.shape;
    entry.index = std::uint32_t(index);
    entry.holder = holder;
    const bool accessor = (attrs & attr::kAccessor) != 0;
    entry.kind = inherited ? (accessor ? PolyEntry::ProtoAccessor : PolyEntry::ProtoData)
                           : (accessor ? PolyEntry::OwnAccessor : PolyEntry::OwnData);
    if (inherited) {
        entry.validity = vm.validityOf(o->proto(vm));
        vm.retainValidity(entry.validity);
    }
    entry.aux = std::uint8_t(o->h.aux + 1u);
    placePoly(vm, code, ic, entry);
    return true;
}

/// A read through the site's `PolyCache`: 1 with the value owned in `*out`, 0 when no entry applies, -1 on an exception (a getter
/// threw).
int readPoly(VM& vm, CodeBlock* code, InlineCache& ic, JSObject* o, JSValue base, JSValue* out)
{
    if (!ic.pad)
        return 0;
    PolyCache& pc = code->poly[ic.pad - 1];
    for (std::uint8_t i = 0; i < pc.n; ++i) {
        const PolyEntry& e = pc.e[i];
        if (e.shape != o->h.shape)
            continue;
        if ((e.kind == PolyEntry::ProtoData || e.kind == PolyEntry::ProtoAccessor) && !e.validity->valid)
            continue;
        if (e.kind == PolyEntry::Absent) {
            if (e.validity && !e.validity->valid)
                continue;
            *out = JSValue::undefined();
            return 1;
        }
        const JSValue slot = (e.holder ? e.holder : o)->slotAt(e.index)[0];
        if (e.kind == PolyEntry::OwnData || e.kind == PolyEntry::ProtoData) {
            retain(slot);
            *out = slot;
            return 1;
        }
        const auto* gs = reinterpret_cast<const GetterSetter*>(slot.asCell());
        if (gs->getter.isUndefined()) {
            *out = JSValue::undefined();
            return 1;
        }
        // The getter may replace the property that holds it.
        const JSValue getter = gs->getter;
        retain(getter);
        const JSValue r = vm.callHook(vm, getter, base, nullptr, 0, vm.callHookData);
        release(getter);
        if (r.isException())
            return -1;
        *out = r;
        return 1;
    }
    return 0;
}

/// The slow part of a cached property read once the site's one entry did not apply: the `PolyCache`, then learning (the first
/// shape goes into the inline cache itself, the others and accessors into the `PolyCache`). Same results as `readPoly`.
/// A data entry of the site's `PolyCache` for `shape` takes the place of its inline cache (which takes the entry's): what
/// compiled code reads from the cache as it runs is then the shape the site sees now.
void promotePoly(CodeBlock* code, InlineCache& ic, std::uint32_t shape) noexcept
{
    if (!ic.pad || ic.shape == shape || ic.next != 0)
        return;
    PolyCache& pc = code->poly[ic.pad - 1];
    for (std::uint8_t k = 0; k < pc.n; ++k) {
        PolyEntry& e = pc.e[k];
        if (e.shape != shape || (e.kind != PolyEntry::OwnData && e.kind != PolyEntry::ProtoData))
            continue;
        const PolyEntry was = e;
        if (ic.shape != 0) {
            e.shape = ic.shape;
            e.index = ic.index;
            e.holder = ic.holder;
            e.validity = ic.validity;
            e.kind = ic.holder ? PolyEntry::ProtoData : PolyEntry::OwnData;
            e.aux = ic.aux;
        } else {
            pc.e[k] = pc.e[pc.n - 1];
            pc.e[pc.n - 1] = PolyEntry{};
            --pc.n;
        }
        ic.shape = was.shape;
        ic.index = was.index;
        ic.holder = was.holder;
        ic.validity = was.validity;
        ic.aux = was.aux;
        return;
    }
}

int readCachedSlow(VM& vm, CodeBlock* code, InlineCache& ic, JSObject* o, JSValue base, Key key, JSValue* out)
{
    int r = readPoly(vm, code, ic, o, base, out);
    if (r == 1) {
        // A site that alternates between shapes would swap its entries at every read: the second entry takes the inline
        // cache's place only on its third hit (the count is in bits 1-2 of the flags).
        const unsigned seen = (ic.flags >> 1) & 3u;
        if (seen >= 2) {
            promotePoly(code, ic, o->h.shape);
            ic.flags = std::uint8_t(ic.flags & ~6u);
        } else {
            ic.flags = std::uint8_t(ic.flags + 2u);
        }
    }
    if (r)
        return r;
    if ((ic.shape == 0 || ic.shape == o->h.shape) && learnRead(vm, ic, o, key)) {
        const JSValue v = ic.holder ? *ic.holder->slotAt(ic.index) : *o->slotAt(ic.index);
        retain(v);
        *out = v;
        return 1;
    }
    if (learnPoly(vm, code, ic, o, key))
        return readPoly(vm, code, ic, o, base, out);
    return 0;
}

/// A by-name read through the site's `ElemCache`: the slot, or null.
inline JSValue* elemCached(CodeBlock* code, std::size_t at, JSObject* o, CellPtr key) noexcept
{
    if (at >= code->elemSite.size() || !code->elemSite[at])
        return nullptr;
    const ElemCache* ways = &code->elemCaches[code->elemSite[at] - 1];
    for (std::uint32_t k = 0; k < ElemCache::kWays; ++k) {
        const ElemCache& ec = ways[k];
        if (ec.key == key && ec.shape == o->h.shape && (!ec.validity || ec.validity->valid))
            return (ec.holder ? ec.holder : o)->slotAt(ec.index);
    }
    return nullptr;
}

/// Remember where `o[key]` found its data property, for the site at instruction `at`: in the way its key maps to.
void learnElem(VM& vm, CodeBlock* code, std::size_t at, JSObject* o, Key key)
{
    if (code->elemSite.size() <= at)
        code->elemSite.assign(code->code.size(), 0);
    std::uint32_t& site = code->elemSite[at];
    if (!site) {
        site = std::uint32_t(code->elemCaches.size()) + 1;
        for (std::uint32_t k = 0; k < ElemCache::kWays; ++k)
            code->elemCaches.emplace_back();
    }
    ElemCache* ways = &code->elemCaches[site - 1];
    if (ways[0].misses >= 24)
        return;
    double numeric;
    if (o->cls() == ObjClass::TypedArray || o->cls() == ObjClass::String || canonicalNumericIndex(key, &numeric) ||
        (hasElems(o) && key == vm.atoms.length) || !cacheable(vm, o)) {
        ++ways[0].misses;
        return;
    }
    InlineCache found;
    if (!learnRead(vm, found, o, key)) {
        ++ways[0].misses;
        return;
    }
    // The way of this key and shape, else an empty one, else the one the key's address names.
    std::uint32_t way = (reinterpret_cast<std::uintptr_t>(key) >> 4) & (ElemCache::kWays - 1);
    bool placed = false;
    for (std::uint32_t k = 0; k < ElemCache::kWays && !placed; ++k)
        if (ways[k].key == key && ways[k].shape == found.shape) {
            way = k;
            placed = true;
        }
    for (std::uint32_t k = 0; k < ElemCache::kWays && !placed; ++k)
        if (!ways[k].key) {
            way = k;
            placed = true;
        }
    ElemCache& ec = ways[way];
    if (ec.key && ec.key != key)
        ++ways[0].misses;  // a way taken by another key: a site that goes through many gives up
    if (ec.validity)
        vm.releaseValidity(ec.validity);
    if (ec.key)
        detail::decRef(ec.key);
    detail::incRef(key);
    ec.key = key;
    ec.shape = found.shape;
    ec.index = found.index;
    ec.holder = found.holder;
    ec.validity = found.validity;
}

/// A read of `key` on a string, for a name `String.prototype` has as an own data property (its methods): through the site's
/// cache, which then says what it says of an object of that shape, `String.prototype` itself. The slot, or null.
JSValue* stringMethodRead(VM& vm, InlineCache& ic, Key key)
{
    JSObject* sp = vm.protos.string;
    if (!sp || key == vm.atoms.length)
        return nullptr;
    if (ic.shape == sp->h.shape && !ic.holder && ic.next == 0 && ic.shape != 0)
        return sp->slotAt(ic.index);
    if (ic.shape != 0)
        return nullptr;  // the site is some other shape's: not for learning
    double numeric;
    std::uint8_t attrs = 0;
    if (canonicalNumericIndex(key, &numeric) || !cacheable(vm, sp))
        return nullptr;
    const std::int32_t index = sp->shapeOf(vm)->indexOf(key, &attrs);
    if (index < 0 || (attrs & attr::kAccessor))
        return nullptr;
    ic.shape = sp->h.shape;
    ic.index = std::uint32_t(index);
    ic.next = 0;
    ic.aux = std::uint8_t(sp->h.aux + 1u);
    return sp->slotAt(ic.index);
}

/// The slot a cached add stores into: one in place, or an out-of-line one (the buffer is made or grown to hold it).
inline JSValue* roomForAdd(VM& vm, JSObject* o, std::uint32_t index)
{
    if (index >= o->h.aux)
        o->reserveSlot(vm, index);
    return o->slotAt(index);
}

/// A store through the site's `PolyCache`: true when an entry applied and the store is done.
bool storePoly(VM& vm, CodeBlock* code, const InlineCache& ic, JSObject* o, JSValue v)
{
    if (!ic.pad)
        return false;
    const PolyCache& pc = code->poly[ic.pad - 1];
    for (std::uint8_t i = 0; i < pc.n; ++i) {
        const PolyEntry& e = pc.e[i];
        if (e.shape != o->h.shape)
            continue;
        if (e.kind == PolyEntry::Write) {
            JSValue* slot = o->slotAt(e.index);
            const JSValue old = *slot;
            *slot = v;
            retain(v);
            release(old);
            return true;
        }
        if (e.kind == PolyEntry::Add && (!e.validity || e.validity->valid) &&
            !(o->h.flags & (cellflags::kIsProto | cellflags::kNotExtensible))) {
            *roomForAdd(vm, o, e.index) = v;
            retain(v);
            o->h.shape = e.next;
            return true;
        }
        return false;
    }
    return false;
}

/// The store entry of the site's `PolyCache` for objects of shape `shape` takes the place of its inline cache (which takes
/// the entry's): compiled code that reads the cache as it runs then finds the shape the site sees now.
void promoteStore(CodeBlock* code, InlineCache& ic, std::uint32_t shape) noexcept
{
    if (!ic.pad || ic.shape == shape || ic.holder)
        return;
    PolyCache& pc = code->poly[ic.pad - 1];
    for (std::uint8_t k = 0; k < pc.n; ++k) {
        PolyEntry& e = pc.e[k];
        if (e.shape != shape || (e.kind != PolyEntry::Write && e.kind != PolyEntry::Add))
            continue;
        const PolyEntry was = e;
        if (ic.shape != 0) {
            e = PolyEntry{};
            e.shape = ic.shape;
            e.index = ic.index;
            e.next = ic.next;
            e.validity = ic.validity;
            e.kind = ic.next ? PolyEntry::Add : PolyEntry::Write;
            e.aux = ic.aux;
        } else {
            pc.e[k] = pc.e[pc.n - 1];
            pc.e[pc.n - 1] = PolyEntry{};
            --pc.n;
        }
        ic.shape = was.shape;
        ic.index = was.index;
        ic.next = was.kind == PolyEntry::Add ? was.next : 0;
        ic.validity = was.validity;
        ic.aux = was.aux;
        return;
    }
}

/// A store to an own writable data property. A site whose inline cache holds another shape remembers it in its
/// `PolyCache` (when `code` is given).
void learnOwnWrite(VM& vm, InlineCache& ic, JSObject* o, Key key, CodeBlock* code = nullptr)
{
    // (The global object's and watched prototypes' writes are the runtime's: `slotWritten`.)
    if (hasElems(o) || o->cls() == ObjClass::Global || o == vm.global)
        return;
    if (o->isProto())
        if (Rare* r = o->rare(); r && r->watched)
            return;
    std::uint8_t attrs = 0;
    const std::int32_t index = cachedOwnIndex(vm, o, key, &attrs);
    if (index < 0 || !(attrs & attr::kWritable) || (attrs & attr::kAccessor))
        return;
    if (code && ic.shape != 0 && ic.shape != o->h.shape) {
        PolyEntry e;
        e.kind = PolyEntry::Write;
        e.shape = o->h.shape;
        e.index = std::uint32_t(index);
        e.aux = std::uint8_t(o->h.aux + 1u);
        placePoly(vm, code, ic, e);
        return;
    }
    clearValidity(vm, ic);
    ic.shape = o->h.shape;
    ic.index = std::uint32_t(index);
    ic.next = 0;
    ic.aux = std::uint8_t(o->h.aux + 1u);
}

/// A store (or a literal's property) that added `key` to an object whose shape was `before`: remember the shape after.
/// A store that goes through the prototypes also holds the validity of the chain, since a setter or a read-only property
/// added to one of them would change what the store does.
void learnAdd(VM& vm, InlineCache& ic, JSObject* o, std::uint32_t before, Key key, bool throughChain, CodeBlock* code = nullptr)
{
    if (o->h.shape == before || !cacheable(vm, o) || hasElems(o) || o->isProto())
        return;
    Shape* now = o->shapeOf(vm);
    Shape* was = vm.shapeById(before);
    if (!was || now->parent != was || now->key != key || now->attrs != attr::kDefault)
        return;
    ValidityCell* cell = nullptr;
    if (throughChain) {
        if (JSObject* p = o->proto(vm)) {
            cell = vm.validityOf(p);
            vm.retainValidity(cell);
        }
    }
    if (code && ic.shape != 0 && ic.shape != before) {
        PolyEntry e;
        e.kind = PolyEntry::Add;
        e.shape = before;
        e.next = o->h.shape;
        e.index = was->count;
        e.validity = cell;
        placePoly(vm, code, ic, e);
        return;
    }
    clearValidity(vm, ic);
    ic.validity = cell;
    ic.shape = before;
    ic.next = o->h.shape;
    ic.index = was->count;
    ic.holder = nullptr;
}


/// The elements of `list` (holes as undefined) over the registers from `window` on, where the list itself is the first of them.
void spreadOverWindow(VM& vm, JSValue* window, JSArray* list, std::uint32_t n)
{
    const JSValue keep = toJSValue(list);
    retain(keep);  // the first element's register is the list's own
    vm.fillStack(window + n + 1);
    for (std::uint32_t i = 0; i < n; ++i) {
        JSValue v = list->elems[i];
        if (v.isHole())
            v = JSValue::undefined();
        retain(v);
        setReg(vm, window[i], v);
    }
    release(keep);
}

/// A call of a function bound to a target and a `this` and no arguments is a call of the target with that `this`: the bound
/// function's place in the window is taken by them, as often as it is bound again. (Bound arguments, `new` and the rest
/// go through the native.)
template <bool Def>
void unwrapBound(JSValue* R, std::uint32_t base) noexcept
{
    for (;;) {
        if (!isFunction(R[base]))
            return;
        auto* f = static_cast<JSFunction*>(asObject(R[base]));
        if (!(f->flags & JSFunction::kNative))
            return;
        Rare* r = f->rare();
        if (!r || !isFunction(r->boundTarget) || (r->boundArgs && r->boundArgs->length))
            return;
        const JSValue target = r->boundTarget, thisValue = r->boundThis;
        // The bound function's place may hold its last count: all is read from it already, and counted first.
        if constexpr (Def) {
            R[base] = target;
            R[base + 1] = thisValue;
        } else {
            retain(target);
            retain(thisValue);
            const JSValue oldCallee = R[base], oldThis = R[base + 1];
            R[base] = target;
            R[base + 1] = thisValue;
            release(oldCallee);
            release(oldThis);
        }
    }
}

/// The call's arguments, copied into an array for `arguments` and rest parameters.
/// What `kNeedsArgs` puts in the register after the parameters: the arguments object, or (`kRestOnly`) the rest parameter's array.
JSArray* captureArgumentsFor(VM& vm, const CodeBlock* c, const JSValue* args, std::uint32_t argc);
JSArray* captureArguments(VM& vm, const JSValue* args, std::uint32_t argc)
{
    JSArray* all = vm.newArray(vm.protos.arguments, 0, argc);
    all->h.cls = std::uint8_t(ObjClass::Arguments);
    all->appendAll(vm, args, argc);
    return all;
}

JSArray* captureArgumentsFor(VM& vm, const CodeBlock* c, const JSValue* args, std::uint32_t argc)
{
    if (!(c->flags & CodeBlock::kRestOnly))
        return captureArguments(vm, args, argc);
    const std::uint32_t n = argc > c->nparams ? argc - c->nparams : 0;
    JSArray* rest = vm.newArray(vm.protos.array, 0, n);
    if (n)
        rest->appendAll(vm, args + c->nparams, n);
    return rest;
}

/// Add the own enumerable properties of `source` to `target`, as `{...source}` does.
bool copyOwnProperties(VM& vm, JSObject* target, JSValue source, JSArray* excluded)
{
    auto skip = [&](Key k) {
        if (!excluded)
            return false;
        for (std::uint32_t i = 0; i < excluded->length; ++i)
            if (isString(excluded->elems[i]) && excluded->elems[i].asCell() == k)
                return true;
        return false;
    };
    auto put = [&](Key k, JSValue v) {
        if (skip(k))
            return;
        retain(v);
        target->putOwn(vm, k, v, attr::kDefault);
    };
    if (isObject(source) && asObject(source)->cls() == ObjClass::String)
        source = unwrapped(source, ObjClass::String);
    if (isObject(source)) {
        JSObject* o = asObject(source);
        // A proxy, or a property with a getter, has to be asked as the language asks: key by key, reading each value.
        bool plain = !proxyOf(o) && o->cls() != ObjClass::TypedArray;
        if (plain)
            o->forEachOwn(vm, [&](Key, std::uint8_t attrs, JSValue) {
                if (attrs & attr::kAccessor)
                    plain = false;
            });
        if (!plain) {
            Vec<Handle> keys;
            bi::ownKeys(vm, o, true, true, true, keys);
            if (!vm.exception.isUndefined())
                return false;
            for (Handle& key : keys) {
                const Key k = static_cast<Key>(key.get().asCell());
                if (skip(k))
                    continue;
                JSValue v = getProperty(vm, source, k);
                if (v.isException())
                    return false;
                if (!defineOwn(vm, target, k, v, attr::kDefault)) {
                    vm.throwError(ErrorCode::TypeError, "Cannot define property");
                    return false;
                }
            }
            return true;
        }
        if (hasElems(o)) {
            auto* a = static_cast<JSArray*>(o);
            for (std::uint32_t i = 0; i < a->length; ++i) {
                if (a->elems[i].isHole())
                    continue;
                Key k = toPropertyKey(vm, JSValue::boxNumber(double(i)));
                put(k, a->elems[i]);
                release(JSValue::cell(k));
            }
        }
        std::vector<std::pair<Key, JSValue>> props;
        o->forEachOwn(vm, [&](Key k, std::uint8_t attrs, JSValue v) {
            if ((attrs & attr::kEnumerable) && !(attrs & attr::kAccessor))
                props.push_back({k, v});
        });
        for (auto& [k, v] : props)
            put(k, v);
    } else if (isString(source)) {
        const std::uint32_t n = asString(source)->length;
        for (std::uint32_t i = 0; i < n; ++i) {
            Key k = toPropertyKey(vm, JSValue::int32(std::int32_t(i)));
            JSValue ch = makeString(vm, asString(source)->text().substr(i, 1));
            if (!skip(k))
                target->putOwn(vm, k, ch, attr::kDefault);
            else
                release(ch);
            release(JSValue::cell(k));
        }
    }
    return true;
}

}  // namespace

// --------------------------------------------------------------- the loop ---

namespace {

/// A suspended generator: the registers of its frame, and where to go on.
struct GenState : NativeState
{
    enum State : std::uint8_t { Start, Suspended, Running, Done };
    /// What a suspended frame keeps of its registers, in the same allocation as the state (`new (room) GenState`): the first
    /// `count` values are those of the registers that `planSuspends` lists for where it resumes; at the start, of the first
    /// registers. A running generator of a deferred frame keeps them, with the counts its registers do not have.
    std::uint32_t count = 0;
    JSValue* regs() noexcept { return reinterpret_cast<JSValue*>(this + 1); }
    const JSValue* regs() const noexcept { return reinterpret_cast<const JSValue*>(this + 1); }
    static void* operator new(std::size_t size, std::uint32_t registers) { return ::operator new(size + std::size_t(registers) * sizeof(JSValue)); }
    static void operator delete(void* p) { ::operator delete(p); }
    static void operator delete(void* p, std::uint32_t) { ::operator delete(p); }
    /// Keep the live registers `R` of the frame, which stops before instruction `pc` of `code`; the registers are undefined then.
    template <bool Def>
    void suspend(const CodeBlock* code, JSValue* R, std::uint32_t pc) noexcept
    {
        const std::uint16_t* live = code->suspendRegs.data() + code->suspendAt[pc];
        const std::uint32_t n = *live++;
        const std::uint32_t had = count;
        JSValue* kept = regs();
        for (std::uint32_t k = 0; k < n; ++k) {
            JSValue& slot = R[live[k]];
            const JSValue v = slot, old = k < had ? kept[k] : JSValue::undefined();
            if constexpr (!Def)
                slot = JSValue::undefined();
            if constexpr (Def) {
                kept[k] = v;
                if (v == old)
                    continue;
                retain(v);
                release(old);
            } else {
                kept[k] = v;
            }
        }
        count = std::max(n, had);
        for (std::uint32_t k = n; k < had; ++k) {
            const JSValue old = kept[k];
            kept[k] = JSValue::undefined();
            release(old);
        }
        count = n;
        if constexpr (!Def)  // (a deferred frame's registers stay as they are)
            for (std::uint32_t r = 0; r < code->nregs; ++r) {
                const JSValue old = R[r];
                R[r] = JSValue::undefined();
                release(old);
            }
        pcIndex = pc;
    }
    JSValue callee = JSValue::undefined();
    std::uint32_t pcIndex = 0;
    std::uint16_t resumeDst = 0;
    State state = Start;
    bool prologue = false;  ///< running the parameters and declarations, up to `GenStart`
    bool delegating = false;  ///< suspended at a `yield*`: a resumption is handed to the inner iterator
    bool raw = false;         ///< what it yielded is an inner iterator result, to be given out as it is
    bool unboxed = false;     ///< what a `yield*` got is an inner generator's value, with no result object made for it
    std::uint32_t room = 0;   ///< registers there is room for, where the state is in the generator's cell
    std::size_t cellBytes() const override { return sizeof(GenState) + 8u * std::size_t(room); }

    ~GenState() override { clear_(); }
    void values(Vec<JSValue>& out) const override
    {
        out.push_back(callee);
        out.insert(out.end(), regs(), regs() + count);
    }
    void clear(VM&) noexcept override { clear_(); }
    void clear_() noexcept
    {
        for (std::uint32_t i = 0; i < count; ++i) {
            release(regs()[i]);
            regs()[i] = JSValue::undefined();
        }
        release(callee);
        callee = JSValue::undefined();
    }
};

using bi::iteratorStep;

GenState* genStateOf(JSObject* o) noexcept
{
    Rare* r = o->rare();
    return static_cast<GenState*>(r && r->ext ? r->ext : o->embeddedState());
}

/// A generator object, owned, whose state keeps no registers yet.
JSValue newGenerator(VM& vm, JSFunction* f, CodeBlock* c, JSObject* proto)
{
    if (!c->suspendMax)
        opt::planSuspends(*c);
    const std::size_t bytes = JSObject::bytesFor(0) + sizeof(GenState) + 8u * std::size_t(c->suspendMax);
    if (bytes <= mem::LocalHeap::kMaxSmall) {
        const std::uint32_t shape = vm.rootShapeFor(proto, 0)->id;
        auto* o = static_cast<JSObject*>(vm.allocCell(bytes, Kind::Object, ObjClass::Generator, 0));
        o->h.shape = shape;
        o->slots = nullptr;
        auto* g = ::new (static_cast<void*>(o->embeddedState())) GenState();
        g->room = c->suspendMax;
        g->callee = toJSValue(f);
        retain(g->callee);
        return toJSValue(o);
    }
    Handle keep = Handle::adopt(toJSValue(vm.newObject(proto)));
    JSObject* o = asObject(keep.get());
    Rare& r = o->rareOf(vm);
    auto* g = new (c->suspendMax) GenState();
    o->h.cls = std::uint8_t(ObjClass::Generator);
    r.ext = g;
    g->callee = toJSValue(f);
    retain(g->callee);
    return keep.take();
}

/// `yield` and `await`: the frame `fr` stops after `ins`, and what it yields (owned) goes to whoever resumed it.
template <bool Def>
JSValue suspendFrame(VM& vm, Frame* fr, const Ins* ins)
{
    vm.suspendedOnAwait = ins->opcode() == Op::Await;
    if (fr->gen.isUndefined())
        fr->gen = vm.becameGenerator = newGenerator(vm, fr->fn, fr->code, vm.protos.generator);
    GenState* g = genStateOf(asObject(fr->gen));
    g->delegating = ins->opcode() == Op::YieldDelegate;
    g->raw = g->delegating && ins->c == 0 && !g->unboxed;  // an async generator's `yield*` yields the value, not the inner result
    g->unboxed = false;
    const JSValue out = fr->R[ins->b];
    retain(out);
    g->resumeDst = ins->a;
    g->state = GenState::Suspended;
    g->suspend<Def>(fr->code, fr->R, std::uint32_t(ins + 1 - fr->code->code.data()));
    --vm.frameTop;
    return out;
}

/// Whether `o` is a generator whose `Symbol.iterator` and `next` are the language's own, as made.
bool generatorPristine(VM& vm, JSObject* o)
{
    JSObject* gp = vm.protos.generator;
    auto bare = [&](JSObject* x) {
        const Shape* shape = x->shapeOf(vm);
        return !shape->isDictionary() && shape->count == 0;
    };
    if (o->cls() != ObjClass::Generator || !gp || !bare(o))
        return false;
    JSObject* p = o->proto(vm);
    if (p != gp && (!p || !bare(p) || p->cls() != ObjClass::Plain || proxyOf(p) || p->proto(vm) != gp))
        return false;
    if (vm.generatorEpoch == vm.globalEpoch)
        return true;
    JSObject* ip = gp->proto(vm);
    std::uint8_t attrs = 0;
    const JSValue* slot = gp->findOwn(vm, vm.atoms.next, &attrs);
    if (!ip || !slot || (attrs & attr::kAccessor) || slot->bits != vm.origGeneratorNext.bits || gp->findOwn(vm, vm.atoms.symIterator))
        return false;
    slot = ip->findOwn(vm, vm.atoms.symIterator, &attrs);
    if (!slot || (attrs & attr::kAccessor) || slot->bits != vm.origIteratorSelf.bits)
        return false;
    if (vm.watchMethod(gp, gp) && vm.watchMethod(ip, ip))
        vm.generatorEpoch = vm.globalEpoch;
    return true;
}

/// GetIterator(src): the iterator and its `next`, owned; false with an exception pending.
bool iteratorOf(VM& vm, JSValue src, JSValue* iterator, JSValue* next)
{
    if (src.isNullish()) {
        vm.throwError(ErrorCode::TypeError, describe(vm, src) + " is not iterable");
        return false;
    }
    if (isObject(src) && generatorPristine(vm, asObject(src))) {
        retain(src);
        *iterator = src;
        *next = vm.origGeneratorNext;
        retain(*next);
        return true;
    }
    JSValue method = getProperty(vm, src, vm.atoms.symIterator);
    if (method.isException())
        return false;
    if (!isFunction(method)) {
        release(method);
        vm.throwError(ErrorCode::TypeError, describe(vm, src) + " is not iterable");
        return false;
    }
    JSValue it = vm.call(method, src, nullptr, 0);
    release(method);
    if (it.isException())
        return false;
    if (!isObject(it)) {
        release(it);
        vm.throwError(ErrorCode::TypeError, "Result of the Symbol.iterator method is not an object");
        return false;
    }
    JSValue n = getProperty(vm, it, vm.atoms.next);
    if (n.isException()) {
        release(it);
        return false;
    }
    *iterator = it;
    *next = n;
    return true;
}

int delegateCall(VM& vm, Frame* fr, const Ins* ins)
{
    JSValue* R = fr->R;
    // One step of `yield*`: what the generator was resumed with goes to the inner iterator's next, throw or return.
    // In an async generator the results are promises, awaited by the code that follows.
    const bool asyncDelegate = ins->opcode() == Op::DelegateCallAsync;
    const JSValue iterator = R[ins->b], nextFn = R[ins->b + 1];
    const int mode = R[ins->c + 1].asInt32();
    JSValue sent = R[ins->c];
    JSValue res;
    if (mode == 0) {
        if (!isFunction(nextFn)) {
            vm.throwError(ErrorCode::TypeError, describe(vm, nextFn) + " is not a function");
            return 0;
        }
        if (!asyncDelegate && nextFn.bits == vm.origGeneratorNext.bits && isObject(iterator) &&
            asObject(iterator)->cls() == ObjClass::Generator && !fr->gen.isUndefined()) {
            // A generator's own `next`: its value as it is, with no result object that the `yield` would give out to be read.
            JSValue value, raw = JSValue::undefined();
            bool done = false;
            if (!vm.resumeRaw(iterator, 0, sent, &value, &done, &raw))
                return 0;
            if (!isObject(raw) && !done) {
                genStateOf(asObject(fr->gen))->unboxed = true;
                setReg(vm, R[ins->a], value);
                return 1;
            }
            res = isObject(raw) ? raw : bi::iterResult(vm, value, true);
        } else {
            res = vm.call(nextFn, iterator, &sent, 1);
        }
    } else {
        Key k = vm.internKey(std::string_view(mode == 1 ? "throw" : "return"));
        JSValue m = getProperty(vm, iterator, k);
        detail::decRef(k);
        if (m.isException())
            return 0;
        if (m.isNullish()) {
            release(m);
            if (mode == 2) {
                // No `return` there: the generator returns what it was given.
                setReg(vm, R[ins->a], JSValue::hole());
                return 1;
            }
            // No `throw`: the iterator is closed, and that is a TypeError.
            JSValue closer = getProperty(vm, iterator, vm.atoms.return_);
            if (closer.isException())
                return 0;
            if (!closer.isNullish()) {
                JSValue r = isFunction(closer) ? vm.call(closer, iterator, nullptr, 0) : JSValue::undefined();
                release(closer);
                if (r.isException())
                    return 0;
                if (asyncDelegate) {
                    // What `return` gives is awaited first; the code after the await raises the TypeError (mode 3).
                    setReg(vm, R[ins->c + 1], JSValue::int32(3));
                    setReg(vm, R[ins->a], r);
                    return 1;
                }
                release(r);
            } else {
                release(closer);
            }
            vm.throwError(ErrorCode::TypeError, "The iterator does not provide a 'throw' method");
            return 0;
        }
        if (!isFunction(m)) {
            release(m);
            vm.throwError(ErrorCode::TypeError, "iterator method is not a function");
            return 0;
        }
        res = vm.call(m, iterator, &sent, 1);
        release(m);
    }
    if (res.isException())
        return 0;
    if (!asyncDelegate && !isObject(res)) {
        const std::string text = describe(vm, res);
        release(res);
        vm.throwError(ErrorCode::TypeError, "Iterator result " + text + " is not an object");
        return 0;
    }
    setReg(vm, R[ins->a], res);
    return 1;
}

/// Whether the result a `yield*` got says it is done.
int delegateDone(VM& vm, Frame* fr, const Ins* ins)
{
    JSValue* R = fr->R;
    if (!fr->gen.isUndefined() && genStateOf(asObject(fr->gen))->unboxed) {
        setReg(vm, R[ins->a], JSValue::boolean(false));
        return 1;
    }
    if (!isObject(R[ins->b])) {
        vm.throwError(ErrorCode::TypeError, "Iterator result " + describe(vm, R[ins->b]) + " is not an object");
        return 0;
    }
    JSValue d = getProperty(vm, R[ins->b], vm.atoms.done);
    if (d.isException())
        return 0;
    const bool done = toBoolean(d);
    release(d);
    setReg(vm, R[ins->a], JSValue::boolean(done));
    return 1;
}

/// The interpreter. With `Step`, it runs the top frame's one instruction (and whatever that calls) and then returns
/// `kStepFall` or `kStepJump` instead of going on; see jit.hpp.
template <bool Step, bool Def>
JSValue runImpl(VM& vm, const Ins* stepFall)
{
    static const void* const kLabels[] = {
#define OP(name, kinds) &&L_##name,
#include "script/internal/vm/opcodes.def"
#undef OP
    };

Frame* fr = vm.frameTop - 1;
    [[maybe_unused]] Frame* const home = fr;
    CodeBlock* code;
    const Ins* codeStart;
    const JSValue* K;
    Key* atoms;
    JSValue* R;
    JSFunction* fn;
    const Ins* pc;
    const Ins* ins;
    std::uint32_t callBase = 0, callArgc = 0;
    int callMode = 0;  // 0 call, 1 new, 2 super(...)

#define LOAD_FRAME()                    \
    do {                                \
        code = fr->code;                \
        codeStart = code->code.data();  \
        K = code->consts.data();        \
        atoms = code->atoms.data();     \
        R = fr->R;                      \
        fn = fr->fn;                    \
        pc = fr->pc ? fr->pc : codeStart; \
    } while (0)
#define DISPATCH()      \
    do {                \
        ins = pc++;     \
        goto* kLabels[ins->op]; \
    } while (0)
#define NEXT()                                                                           \
    do {                                                                                 \
        if constexpr (Step) {                                                            \
            if (fr == home) [[unlikely]] {                                               \
                fr->pc = pc;                                                             \
                return JSValue::raw(pc == stepFall ? kStepFall : kStepJump);             \
            }                                                                            \
        }                                                                                \
        DISPATCH();                                                                      \
    } while (0)
/// A loop's back edge or a call counts towards asking the host whether to stop.
#define TICK()                                                                           \
    do {                                                                                 \
        if (--vm.ticks == 0) [[unlikely]] {                                              \
            vm.ticks = kInterruptInterval;                                               \
            fr->pc = pc;                                                                 \
            if (vm.ctx.interruptAsked()) {                                               \
                vm.throwError(ErrorCode::Interrupted, "execution was interrupted");      \
                FAIL();                                                                  \
            }                                                                            \
        }                                                                                \
    } while (0)
/// A loop's back edge: as TICK, and a function that keeps looping moves into compiled code in the middle of the loop.
#define LOOP_TICK()                                                                      \
    do {                                                                                 \
        /* in a single step, the frames of a call it made may move into compiled code; the stepped one not */ \
        if (!Step || fr != home) {                                                       \
            if (vm.tier1Threshold && !code->tier1Failed && ++code->hotness >= kOsrAfter + code->code.size()) [[unlikely]] { \
                fr->pc = pc;                                                             \
                if (!code->tier1)                                                        \
                    compileTier1(vm, code);                                              \
                if (code->tier1) {                                                       \
                    const bool wasEntry_ = fr->entry;                                    \
                    const std::uint16_t dst_ = fr->retDst;                               \
                    fr->entry = true;                                                    \
                    const JSValue res_ = enterTier1(vm, code->tier1, std::uint32_t(pc - codeStart)); \
                    if (wasEntry_)                                                       \
                        return res_;                                                     \
                    fr = vm.frameTop - 1;                               \
                    LOAD_FRAME();                                                        \
                    vm.stackTop = R + code->nregs;                                       \
                    if (res_.isException())                                              \
                        goto throw_;                                                     \
                    SET(dst_, res_);                                                     \
                    NEXT();                                                              \
                }                                                                        \
            }                                                                            \
        }                                                                                \
        TICK();                                                                          \
    } while (0)
/// A taken jump of the 24-bit kind: a back edge counts as one of a loop's.
#define TAKE()                    \
    do {                          \
        const int o_ = ins->jump(); \
        pc += o_;                 \
        if (o_ < 0)               \
            LOOP_TICK();          \
    } while (0)
#define TAKE16()                  \
    do {                          \
        const int o_ = ins->jump16(); \
        pc += o_;                 \
        if (o_ < 0)               \
            LOOP_TICK();          \
    } while (0)
#define SET(r, v) setReg<Def>(vm, R[r], v)
/// SET and COPY that also note what kind of value the instruction made (the baseline compiler's type feedback).
#define SETP(r, v)                                                         \
    do {                                                                    \
        const JSValue pv_ = (v);                                            \
        code->prof[std::size_t(ins - codeStart)] |= profBit(pv_);           \
        setReg<Def>(vm, R[r], pv_);                                             \
    } while (0)
#define COPYP(r, v)                                                         \
    do {                                                                    \
        const JSValue pv_ = (v);                                            \
        code->prof[std::size_t(ins - codeStart)] |= profBit(pv_);           \
        copyReg<Def>(R[r], pv_);                                            \
    } while (0)
#define COPY(r, v) copyReg<Def>(R[r], v)
#define FAIL()  \
    do {         \
        fr->pc = pc; \
        goto throw_; \
    } while (0)
#define STRICT() ((code->flags & CodeBlock::kStrict) != 0)

    LOAD_FRAME();
    if (code->prof.empty()) [[unlikely]]
        code->prof.assign(code->code.size() + code->nparams + 2, 0);
    vm.stackTop = R + code->nregs;
    if (vm.resumeThrow) [[unlikely]] {
        vm.resumeThrow = false;
        fr->pc = pc;
        goto throw_;
    }
    DISPATCH();

L_Nop:
    NEXT();
L_Mov:
    COPY(ins->a, R[ins->b]);
    NEXT();
L_LoadK:
    COPY(ins->a, K[ins->kIndex()]);
    NEXT();
L_LoadInt:
    SET(ins->a, JSValue::int32(ins->imm()));
    NEXT();
L_LoadUndef:
    SET(ins->a, JSValue::undefined());
    NEXT();
L_LoadNull:
    SET(ins->a, JSValue::null());
    NEXT();
L_LoadTrue:
    SET(ins->a, JSValue::boolean(true));
    NEXT();
L_LoadFalse:
    SET(ins->a, JSValue::boolean(false));
    NEXT();
L_LoadHole:
    SET(ins->a, JSValue::hole());
    NEXT();
L_LoadUninit:
    SET(ins->a, JSValue::uninit());
    NEXT();
L_CheckInit:
    if (R[ins->a].isUninit()) [[unlikely]] {
        vm.throwError(ErrorCode::ReferenceError, "Cannot access '" + toUtf8(static_cast<StringCell*>(atoms[ins->b])->text()) +
                                                     "' before initialization");
        FAIL();
    }
    NEXT();

L_GetGlobal:
L_GetGlobalOrUndef: {
    Key key = atoms[ins->b];
    InlineCache scratch;
    const std::uint16_t cacheId = pc->a;
    InlineCache& ic = cacheId == kNoCache ? scratch : code->caches[cacheId];
    JSObject* g = vm.global;
    if (g->h.shape == ic.shape) [[likely]] {
        COPYP(ins->a, *g->slotAt(ic.index));
        ++pc;
        NEXT();
    }
    std::uint8_t attrs = 0;
    JSValue* slot = g->findOwn(vm, key, &attrs);
    if (slot && !(attrs & attr::kAccessor)) {
        if (std::uint8_t at = 0; cacheId != kNoCache) {
            if (const std::int32_t place = cachedOwnIndex(vm, g, key, &at); place >= 0) {
                ic.shape = g->h.shape;
                ic.index = std::uint32_t(place);
            }
        }
        COPYP(ins->a, *slot);
        ++pc;
        NEXT();
    }
    if (!hasProperty(vm, toJSValue(g), key)) {
        if (ins->opcode() == Op::GetGlobalOrUndef) {
            SETP(ins->a, JSValue::undefined());
            ++pc;
            NEXT();
        }
        vm.throwError(ErrorCode::ReferenceError, toUtf8(static_cast<StringCell*>(key)->text()) + " is not defined");
        FAIL();
    }
    fr->pc = pc;
    JSValue v = getProperty(vm, toJSValue(g), key);
    if (v.isException())
        FAIL();
    SETP(ins->a, v);
    ++pc;
    NEXT();
}
L_PutGlobal: {
    Key key = atoms[ins->a];
    InlineCache scratch;
    const std::uint16_t cacheId = pc->a;
    InlineCache& ic = cacheId == kNoCache ? scratch : code->caches[cacheId];
    JSObject* g = vm.global;
    if (g->h.shape == ic.shape) [[likely]] {
        JSValue* slot = g->slotAt(ic.index);
        const JSValue old = *slot;
        *slot = R[ins->b];
        vm.slotWritten(g, slot);
        retain(*slot);
        release(old);
        ++pc;
        NEXT();
    }
    std::uint8_t attrs = 0;
    JSValue* slot = g->findOwn(vm, key, &attrs);
    if (slot && (attrs & attr::kWritable) && !(attrs & attr::kAccessor)) {
        if (std::uint8_t at = 0; cacheId != kNoCache) {
            if (const std::int32_t place = cachedOwnIndex(vm, g, key, &at); place >= 0) {
                ic.shape = g->h.shape;
                ic.index = std::uint32_t(place);
            }
        }
        const JSValue old = *slot;
        *slot = R[ins->b];
        vm.slotWritten(g, slot);
        retain(*slot);
        release(old);
        ++pc;
        NEXT();
    }
    if (!slot && STRICT() && !hasProperty(vm, toJSValue(g), key)) {
        vm.throwError(ErrorCode::ReferenceError, toUtf8(static_cast<StringCell*>(key)->text()) + " is not defined");
        FAIL();
    }
    fr->pc = pc;
    if (!setProperty(vm, toJSValue(g), key, R[ins->b], STRICT())) {
        if (!vm.exception.isUndefined())
            FAIL();
    }
    ++pc;
    NEXT();
}
// Declarations of global code leave a property that cannot be deleted; those of `eval` code leave one that can (`c`/`b` = 1).
L_DefGlobal: {
    Key key = atoms[ins->a];
    const std::uint8_t attrs = ins->c ? attr::kDefault : std::uint8_t(attr::kWritable | attr::kEnumerable);
    std::uint8_t existing = 0;
    JSValue* slot = vm.global->findOwn(vm, key, &existing);
    retain(R[ins->b]);
    if (!slot) {
        vm.global->putOwn(vm, key, R[ins->b], attrs);
    } else if ((existing & attr::kAccessor) || (existing & attr::kConfigurable)) {
        // A configurable property, or an accessor, is replaced; a fixed data property only gets the new value.
        if (existing & attr::kAccessor)
            vm.global->deleteOwn(vm, key);
        vm.global->putOwn(vm, key, R[ins->b], attrs);
    } else if (existing & attr::kWritable) {
        const JSValue old = *slot;
        *slot = R[ins->b];
        vm.slotWritten(vm.global, slot);
        release(old);
    } else {
        release(R[ins->b]);
    }
    NEXT();
}
L_DeclGlobal: {
    Key key = atoms[ins->a];
    if (!vm.global->findOwn(vm, key))
        vm.global->putOwn(vm, key, JSValue::undefined(), ins->b ? attr::kDefault : std::uint8_t(attr::kWritable | attr::kEnumerable));
    NEXT();
}
L_DelGlobal: {
    fr->pc = pc;
    const bool ok = deleteProperty(vm, toJSValue(vm.global), atoms[ins->b], false);
    if (!vm.exception.isUndefined())
        FAIL();
    SET(ins->a, JSValue::boolean(ok));
    NEXT();
}
// The variables a sloppy direct `eval` declares in a function live on an object of the function's own (made at its entry).
L_NewEnv: {
    JSObject* o = vm.newObject(static_cast<JSObject*>(nullptr));
    SET(ins->a, toJSValue(o));
    NEXT();
}
L_EnvHas: {
    const JSValue env = R[ins->b];
    SET(ins->a, JSValue::boolean(isObject(env) && asObject(env)->findOwn(vm, atoms[ins->c]) != nullptr));
    NEXT();
}
L_EnvGet: {
    JSValue* slot = asObject(R[ins->b])->findOwn(vm, atoms[ins->c]);
    COPY(ins->a, slot ? *slot : JSValue::undefined());
    NEXT();
}
L_EnvSet: {
    JSValue* slot = asObject(R[ins->a])->findOwn(vm, atoms[ins->b]);
    if (slot) {
        const JSValue old = *slot;
        *slot = R[ins->c];
        retain(*slot);
        release(old);
    }
    NEXT();
}
L_DeclEnv: {
    JSObject* env = asObject(R[ins->a]);
    if (!env->findOwn(vm, atoms[ins->b]))
        env->putOwn(vm, atoms[ins->b], JSValue::undefined(), attr::kDefault);
    NEXT();
}
L_DefEnv: {
    JSObject* env = asObject(R[ins->a]);
    JSValue* slot = env->findOwn(vm, atoms[ins->b]);
    retain(R[ins->c]);
    if (slot) {
        const JSValue old = *slot;
        *slot = R[ins->c];
        release(old);
    } else {
        env->putOwn(vm, atoms[ins->b], R[ins->c], attr::kDefault);
    }
    NEXT();
}

L_NewBox: {
    if constexpr (Def)
        retain(R[ins->a]);  // the box's own count, where the register held none
    Box* box = vm.newBox(R[ins->a]);  // takes the register's count
    R[ins->a] = JSValue::cell(reinterpret_cast<CellPtr>(box));
    if constexpr (Def)
        release(R[ins->a]);
    NEXT();
}
L_GetBox:
    COPYP(ins->a, reinterpret_cast<Box*>(R[ins->b].asCell())->value);
    NEXT();
L_PutBox: {
    Box* box = reinterpret_cast<Box*>(R[ins->a].asCell());
    const JSValue old = box->value;
    vm.boxWritten(box);
    box->value = R[ins->b];
    retain(box->value);
    release(old);
    NEXT();
}
L_CopyBox: {
    Box* box = reinterpret_cast<Box*>(R[ins->a].asCell());
    retain(box->value);
    Box* fresh = vm.newBox(box->value);
    SET(ins->a, JSValue::cell(reinterpret_cast<CellPtr>(fresh)));
    NEXT();
}
L_GetUpval:
    COPYP(ins->a, fn->upvals()[ins->b]->value);
    NEXT();
L_PutUpval: {
    Box* box = fn->upvals()[ins->a];
    const JSValue old = box->value;
    vm.boxWritten(box);
    box->value = R[ins->b];
    retain(box->value);
    release(old);
    NEXT();
}
L_LoadCallee:
    COPY(ins->a, toJSValue(fn));
    NEXT();
L_JNotCallee:
    if (R[ins->a].bits != toJSValue(fn).bits)
        pc += ins->jump();
    NEXT();
L_JNotCode: {
    // The register holds a function of the code whose uid the constant names (or the constant itself, a function), and a
    // `this` it can take as it is.
    const JSValue v = R[ins->a], want = K[ins->b];
    bool same = false;
    if (isFunction(v)) {
        const CodeBlock* callee = static_cast<JSFunction*>(asObject(v))->code;
        same = callee && (want.isInt32() ? callee->uid == std::uint64_t(want.asInt32()) : v.bits == want.bits) &&
               ((callee->flags & (CodeBlock::kStrict | CodeBlock::kArrow | CodeBlock::kNoThis)) || isObject(R[ins->a + 1]));
    }
    if (!same)
        pc += ins->jump16();
    NEXT();
}

// ---- arithmetic ----
L_Add: {
    const JSValue a = R[ins->b], b = R[ins->c];
    if (a.isInt32() && b.isInt32()) {
        std::int32_t r;
        if (!__builtin_add_overflow(a.asInt32(), b.asInt32(), &r)) {
            SETP(ins->a, JSValue::int32(r));
            NEXT();
        }
    }
    if (a.isNumber() && b.isNumber()) {
        // A double stays a double: narrowing it back to an int32 costs more than it saves.
        SETP(ins->a, JSValue::boxDouble(a.asNumber() + b.asNumber()));
        NEXT();
    }
    if (ins->a == ins->b && isString(a) && appendInPlace(vm, R[ins->a], b, false))
        NEXT();
    fr->pc = pc;
    const JSValue r = add(vm, a, b);
    if (r.isException())
        FAIL();
    SETP(ins->a, r);
    NEXT();
}
L_Append: {
    const JSValue a = R[ins->a], b = R[ins->b];
    if (isString(a) && appendInPlace(vm, R[ins->a], b, true))
        NEXT();
    fr->pc = pc;
    const JSValue r = add(vm, a, b);
    if (r.isException())
        FAIL();
    SETP(ins->a, r);
    NEXT();
}
L_Unshare:
    if (isString(R[ins->a]))
        asString(R[ins->a])->bits &= std::uint8_t(~StringCell::kSole);
    NEXT();
L_AddI: {
    const JSValue a = R[ins->b];
    if (a.isInt32()) {
        std::int32_t r;
        if (!__builtin_add_overflow(a.asInt32(), std::int32_t(std::int16_t(ins->c)), &r)) {
            SETP(ins->a, JSValue::int32(r));
            NEXT();
        }
    }
    fr->pc = pc;
    const JSValue r = add(vm, a, JSValue::int32(std::int16_t(ins->c)));
    if (r.isException())
        FAIL();
    SETP(ins->a, r);
    NEXT();
}
L_SubI: {
    const JSValue a = R[ins->b];
    if (a.isInt32()) {
        std::int32_t r;
        if (!__builtin_sub_overflow(a.asInt32(), std::int32_t(std::int16_t(ins->c)), &r)) {
            SETP(ins->a, JSValue::int32(r));
            NEXT();
        }
    }
    fr->pc = pc;
    const JSValue r = arithmetic(vm, Op::Sub, a, JSValue::int32(std::int16_t(ins->c)));
    if (r.isException())
        FAIL();
    SETP(ins->a, r);
    NEXT();
}
#define ARITH(NAME, FAST, DOUBLE)                                                         \
    L_##NAME : {                                                                  \
        const JSValue a = R[ins->b], b = R[ins->c];                               \
        if (a.isInt32() && b.isInt32()) {                                         \
            [[maybe_unused]] const std::int32_t x = a.asInt32(), y = b.asInt32();  \
            FAST                                                                  \
        }                                                                         \
        if (a.isNumber() && b.isNumber()) {                                       \
            DOUBLE                                                                \
        }                                                                         \
        fr->pc = pc;                                                              \
        const JSValue r = arithmetic(vm, Op::NAME, a, b);                         \
        if (r.isException())                                                      \
            FAIL();                                                               \
        SETP(ins->a, r);                                                           \
        NEXT();                                                                   \
    }
ARITH(Sub, {
    std::int32_t r;
    if (!__builtin_sub_overflow(x, y, &r)) {
        SETP(ins->a, JSValue::int32(r));
        NEXT();
    }
}, { SETP(ins->a, JSValue::boxDouble(a.asNumber() - b.asNumber())); NEXT(); })
ARITH(Mul, {
    std::int32_t r;
    if (!__builtin_mul_overflow(x, y, &r) && (r != 0 || (x >= 0 && y >= 0))) {
        SETP(ins->a, JSValue::int32(r));
        NEXT();
    }
}, { SETP(ins->a, JSValue::boxDouble(a.asNumber() * b.asNumber())); NEXT(); })
ARITH(Div, {}, {
    const double q = a.asNumber() / b.asNumber();
    // Two integers may divide evenly: that is an int32 again.
    SETP(ins->a, a.isInt32() && b.isInt32() ? JSValue::boxNumber(q) : JSValue::boxDouble(q));
    NEXT();
})
ARITH(Mod, {
    if (x >= 0 && y > 0) {
        SETP(ins->a, JSValue::int32(x % y));
        NEXT();
    }
}, { })
ARITH(Exp, {}, { })
ARITH(BitAnd, {
    SETP(ins->a, JSValue::int32(x & y));
    NEXT();
}, { })
ARITH(BitOr, {
    SETP(ins->a, JSValue::int32(x | y));
    NEXT();
}, { })
ARITH(BitXor, {
    SETP(ins->a, JSValue::int32(x ^ y));
    NEXT();
}, { })
ARITH(Shl, {
    SETP(ins->a, JSValue::int32(std::int32_t(std::uint32_t(x) << (y & 31))));
    NEXT();
}, { })
ARITH(Shr, {
    SETP(ins->a, JSValue::int32(x >> (y & 31)));
    NEXT();
}, { })
ARITH(UShr, {}, { })

#define COMPARE(NAME, OPERATOR)                                       \
    L_##NAME : {                                                      \
        const JSValue a = R[ins->b], b = R[ins->c];                   \
        if (a.isInt32() && b.isInt32()) {                             \
            SET(ins->a, JSValue::boolean(a.asInt32() OPERATOR b.asInt32())); \
            NEXT();                                                   \
        }                                                             \
        if (a.isNumber() && b.isNumber()) {                           \
            SET(ins->a, JSValue::boolean(a.asNumber() OPERATOR b.asNumber())); \
            NEXT();                                                   \
        }                                                             \
        fr->pc = pc;                                                  \
        const int r = relational(vm, Op::NAME, a, b);                 \
        if (r < 0)                                                    \
            FAIL();                                                   \
        SET(ins->a, JSValue::boolean(r != 0));                        \
        NEXT();                                                       \
    }
COMPARE(Lt, <)
COMPARE(Le, <=)
COMPARE(Gt, >)
COMPARE(Ge, >=)

L_Eq:
L_Ne: {
    const JSValue a = R[ins->b], b = R[ins->c];
    bool ok = true, eq;
    if (a.isInt32() && b.isInt32()) {
        eq = a.bits == b.bits;
    } else {
        fr->pc = pc;
        eq = looseEquals(vm, a, b, &ok);
        if (!ok)
            FAIL();
    }
    SET(ins->a, JSValue::boolean(ins->opcode() == Op::Eq ? eq : !eq));
    NEXT();
}
L_StrictEq:
L_StrictNe: {
    const bool eq = fastStrictEquals(R[ins->b], R[ins->c]);
    SET(ins->a, JSValue::boolean(ins->opcode() == Op::StrictEq ? eq : !eq));
    NEXT();
}
L_In: {
    fr->pc = pc;
    const int r = inOperator(vm, R[ins->b], R[ins->c]);
    if (r < 0)
        FAIL();
    SET(ins->a, JSValue::boolean(r != 0));
    NEXT();
}
L_InstanceOf: {
    fr->pc = pc;
    const int r = instanceOf(vm, R[ins->b], R[ins->c]);
    if (r < 0)
        FAIL();
    SET(ins->a, JSValue::boolean(r != 0));
    NEXT();
}
L_Neg: {
    const JSValue a = R[ins->b];
    if (a.isInt32() && a.asInt32() != 0 && a.asInt32() != INT32_MIN) {
        SETP(ins->a, JSValue::int32(-a.asInt32()));
        NEXT();
    }
    fr->pc = pc;
    JSValue n = toNumeric(vm, a);
    if (n.isException())
        FAIL();
    if (isBigInt(n)) {
        JSValue r = bigintUnary(vm, 0, n);
        release(n);
        if (r.isException())
            FAIL();
        SETP(ins->a, r);
        NEXT();
    }
    SETP(ins->a, JSValue::boxNumber(-n.asNumber()));
    NEXT();
}
L_ToNumber: {
    const JSValue a = R[ins->b];
    if (a.isNumber()) {
        COPY(ins->a, a);
        NEXT();
    }
    fr->pc = pc;
    double d;
    if (!toNumber(vm, a, &d))
        FAIL();
    SET(ins->a, JSValue::boxNumber(d));
    NEXT();
}
L_ToNumeric: {
    const JSValue a = R[ins->b];
    if (a.isNumber()) {
        COPY(ins->a, a);
        NEXT();
    }
    fr->pc = pc;
    JSValue n = toNumeric(vm, a);
    if (n.isException())
        FAIL();
    SET(ins->a, n);
    NEXT();
}
L_ToString: {
    fr->pc = pc;
    const JSValue r = toStringValue(vm, R[ins->b]);
    if (r.isException())
        FAIL();
    SET(ins->a, r);
    NEXT();
}
L_Not:
    SET(ins->a, JSValue::boolean(!toBoolean(R[ins->b])));
    NEXT();
L_BitNot: {
    const JSValue a = R[ins->b];
    if (a.isInt32()) {
        SET(ins->a, JSValue::int32(~a.asInt32()));
        NEXT();
    }
    fr->pc = pc;
    JSValue n = toNumeric(vm, a);
    if (n.isException())
        FAIL();
    if (isBigInt(n)) {
        JSValue r = bigintUnary(vm, 1, n);
        release(n);
        if (r.isException())
            FAIL();
        SET(ins->a, r);
        NEXT();
    }
    SET(ins->a, JSValue::int32(~toInt32(n.asNumber())));
    NEXT();
}
L_TypeOf: {
    const JSValue r = typeOf(vm, R[ins->b]);
    SET(ins->a, r);
    NEXT();
}
L_Inc:
L_Dec: {
    const JSValue a = R[ins->b];
    const int delta = ins->opcode() == Op::Inc ? 1 : -1;
    if (a.isInt32()) {
        std::int32_t r;
        if (!__builtin_add_overflow(a.asInt32(), delta, &r)) {
            SETP(ins->a, JSValue::int32(r));
            NEXT();
        }
    }
    fr->pc = pc;
    JSValue n = toNumeric(vm, a);
    if (n.isException())
        FAIL();
    if (isBigInt(n)) {
        JSValue r = bigintUnary(vm, delta > 0 ? 2 : 3, n);
        release(n);
        if (r.isException())
            FAIL();
        SETP(ins->a, r);
        NEXT();
    }
    SETP(ins->a, JSValue::boxNumber(n.asNumber() + delta));
    NEXT();
}

// ---- jumps ----
L_Jmp: {
    const std::int32_t offset = ins->jump();
    pc += offset;
    if (offset < 0)
        LOOP_TICK();
    NEXT();
}
L_JTrue:
    if (toBoolean(R[ins->a])) {
        const std::int32_t offset = ins->jump();
        pc += offset;
        if (offset < 0)
            LOOP_TICK();
    }
    NEXT();
L_JFalse: {
    const JSValue v = R[ins->a];
    if (v.isBool() ? !v.asBool() : !toBoolean(v))
        TAKE();
    NEXT();
}
L_JNullish:
    if (R[ins->a].isNullish())
        TAKE();
    NEXT();
L_JNotNullish:
    if (!R[ins->a].isNullish())
        TAKE();
    NEXT();
L_JUndefined:
    if (R[ins->a].isUndefined())
        TAKE();
    NEXT();
L_JNotUndefined:
    if (!R[ins->a].isUndefined())
        TAKE();
    NEXT();
L_JHole:
    if (R[ins->a].isHole())
        TAKE();
    NEXT();
L_JLt:
L_JLe:
L_JGt:
L_JGe: {
    const JSValue a = R[ins->a], b = R[ins->b];
    bool holds;
    if (a.isInt32() && b.isInt32()) {
        const std::int32_t x = a.asInt32(), y = b.asInt32();
        switch (ins->opcode()) {
            case Op::JLt: holds = x < y; break;
            case Op::JLe: holds = x <= y; break;
            case Op::JGt: holds = x > y; break;
            default: holds = x >= y; break;
        }
    } else if (a.isNumber() && b.isNumber()) {
        const double x = a.asNumber(), y = b.asNumber();
        switch (ins->opcode()) {
            case Op::JLt: holds = x < y; break;
            case Op::JLe: holds = x <= y; break;
            case Op::JGt: holds = x > y; break;
            default: holds = x >= y; break;
        }
    } else {
        fr->pc = pc;
        const Op rel = ins->opcode() == Op::JLt ? Op::Lt : ins->opcode() == Op::JLe ? Op::Le : ins->opcode() == Op::JGt ? Op::Gt : Op::Ge;
        const int r = relational(vm, rel, a, b);
        if (r < 0)
            FAIL();
        holds = r != 0;
    }
    if (holds)
        TAKE16();
    NEXT();
}
L_JNotLt:
L_JNotLe:
L_JNotGt:
L_JNotGe: {
    const JSValue a = R[ins->a], b = R[ins->b];
    bool holds;
    if (a.isInt32() && b.isInt32()) {
        const std::int32_t x = a.asInt32(), y = b.asInt32();
        switch (ins->opcode()) {
            case Op::JNotLt: holds = x < y; break;
            case Op::JNotLe: holds = x <= y; break;
            case Op::JNotGt: holds = x > y; break;
            default: holds = x >= y; break;
        }
    } else if (a.isNumber() && b.isNumber()) {
        const double x = a.asNumber(), y = b.asNumber();
        switch (ins->opcode()) {
            case Op::JNotLt: holds = x < y; break;
            case Op::JNotLe: holds = x <= y; break;
            case Op::JNotGt: holds = x > y; break;
            default: holds = x >= y; break;
        }
    } else {
        fr->pc = pc;
        const Op rel = ins->opcode() == Op::JNotLt ? Op::Lt : ins->opcode() == Op::JNotLe ? Op::Le : ins->opcode() == Op::JNotGt ? Op::Gt : Op::Ge;
        const int r = relational(vm, rel, a, b);
        if (r < 0)
            FAIL();
        holds = r != 0;
    }
    if (!holds)
        pc += ins->jump16();
    NEXT();
}
L_JNe:
L_JEq: {
    const JSValue a = R[ins->a], b = R[ins->b];
    bool ok = true, eq;
    if (a.isInt32() && b.isInt32()) {
        eq = a.bits == b.bits;
    } else {
        fr->pc = pc;
        eq = looseEquals(vm, a, b, &ok);
        if (!ok)
            FAIL();
    }
    if (eq == (ins->opcode() == Op::JEq))
        TAKE16();
    NEXT();
}
L_JStrictNe:
L_JStrictEq: {
    const bool eq = fastStrictEquals(R[ins->a], R[ins->b]);
    if (eq == (ins->opcode() == Op::JStrictEq))
        TAKE16();
    NEXT();
}
L_MulI:
L_ModI:
L_BitAndI:
L_BitOrI:
L_BitXorI:
L_ShlI:
L_ShrI:
L_UShrI: {
    const JSValue a = R[ins->b];
    const std::int32_t y = std::int32_t(std::int16_t(ins->c));
    const Op op = ins->opcode();
    if (a.isInt32()) {
        const std::int32_t x = a.asInt32();
        std::int32_t r;
        switch (op) {
            case Op::MulI:
                if (!__builtin_mul_overflow(x, y, &r) && (r != 0 || (x >= 0 && y >= 0))) {
                    SETP(ins->a, JSValue::int32(r));
                    NEXT();
                }
                break;
            case Op::ModI:
                if (x >= 0 && y > 0) {
                    SETP(ins->a, JSValue::int32(x % y));
                    NEXT();
                }
                break;
            case Op::BitAndI: SETP(ins->a, JSValue::int32(x & y)); NEXT();
            case Op::BitOrI: SETP(ins->a, JSValue::int32(x | y)); NEXT();
            case Op::BitXorI: SETP(ins->a, JSValue::int32(x ^ y)); NEXT();
            case Op::ShlI: SETP(ins->a, JSValue::int32(std::int32_t(std::uint32_t(x) << (y & 31)))); NEXT();
            case Op::ShrI: SETP(ins->a, JSValue::int32(x >> (y & 31))); NEXT();
            default: SETP(ins->a, JSValue::boxNumber(double(std::uint32_t(x) >> (y & 31)))); NEXT();
        }
    } else if (op == Op::MulI && a.isDouble()) {
        SETP(ins->a, JSValue::boxDouble(a.asDouble() * double(y)));
        NEXT();
    }
    fr->pc = pc;
    const Op generic = op == Op::MulI ? Op::Mul : op == Op::ModI ? Op::Mod : op == Op::BitAndI ? Op::BitAnd : op == Op::BitOrI ? Op::BitOr
                       : op == Op::BitXorI ? Op::BitXor : op == Op::ShlI ? Op::Shl : op == Op::ShrI ? Op::Shr : Op::UShr;
    const JSValue r = arithmetic(vm, generic, a, JSValue::int32(y));
    if (r.isException())
        FAIL();
    SETP(ins->a, r);
    NEXT();
}
L_JLtI:
L_JLeI:
L_JGtI:
L_JGeI:
L_JNotLtI:
L_JNotLeI:
L_JNotGtI:
L_JNotGeI: {
    const JSValue a = R[ins->a];
    const std::int32_t y = std::int32_t(std::int16_t(ins->b));
    const Op op = ins->opcode();
    bool holds;
    if (a.isInt32() || a.isDouble()) {
        const double x = a.asNumber();
        const double yy = double(y);
        switch (op) {
            case Op::JLtI: case Op::JNotLtI: holds = x < yy; break;
            case Op::JLeI: case Op::JNotLeI: holds = x <= yy; break;
            case Op::JGtI: case Op::JNotGtI: holds = x > yy; break;
            default: holds = x >= yy; break;
        }
    } else {
        fr->pc = pc;
        const Op rel = (op == Op::JLtI || op == Op::JNotLtI) ? Op::Lt : (op == Op::JLeI || op == Op::JNotLeI) ? Op::Le
                       : (op == Op::JGtI || op == Op::JNotGtI) ? Op::Gt : Op::Ge;
        const int r = relational(vm, rel, a, JSValue::int32(y));
        if (r < 0)
            FAIL();
        holds = r != 0;
    }
    const bool negated = op == Op::JNotLtI || op == Op::JNotLeI || op == Op::JNotGtI || op == Op::JNotGeI;
    if (holds != negated)
        TAKE16();
    NEXT();
}
L_JStrictNeI:
L_JStrictEqI: {
    const JSValue a = R[ins->a];
    const std::int32_t y = std::int32_t(std::int16_t(ins->b));
    const bool eq = a.isInt32() ? a.asInt32() == y : (a.isDouble() && a.asDouble() == double(y));
    if (eq == (ins->opcode() == Op::JStrictEqI))
        TAKE16();
    NEXT();
}
L_Cache:
    // Never executed: the word after a property instruction, skipped by it.
    goto throw_;

// ---- properties ----
L_GetProp: {
    const JSValue base = R[ins->b];
    Key key = atoms[ins->c];
    InlineCache scratch;
    const std::uint16_t cacheId = pc->a;
    InlineCache& ic = cacheId == kNoCache ? scratch : code->caches[cacheId];
    if (isObject(base)) {
        JSObject* o = asObject(base);
        if (o->h.shape == ic.shape) [[likely]] {
            if (!ic.holder) {
                COPYP(ins->a, *o->slotAt(ic.index));
                ++pc;
                NEXT();
            }
            if (ic.validity->valid) {
                COPYP(ins->a, *ic.holder->slotAt(ic.index));
                ++pc;
                NEXT();
            }
        }
        if (key == vm.atoms.length && hasElems(o)) {
            const std::uint32_t n = static_cast<JSArray*>(o)->length;
            SETP(ins->a, n <= 0x7FFFFFFFu ? JSValue::int32(std::int32_t(n)) : JSValue::boxDouble(double(n)));
            ++pc;
            NEXT();
        }
        if (key == vm.atoms.length && o->cls() == ObjClass::TypedArray) {
            const std::uint32_t n = static_cast<JSTypedArray*>(o)->length;
            SETP(ins->a, n <= 0x7FFFFFFFu ? JSValue::int32(std::int32_t(n)) : JSValue::boxDouble(double(n)));
            ++pc;
            NEXT();
        }
        SCRIPT_VM_CENSUS(SlowGetProp);
        if (cacheId != kNoCache) {
            fr->pc = pc;
            JSValue got;
            const int r = readCachedSlow(vm, code, ic, o, base, key, &got);
            if (r < 0)
                FAIL();
            if (r > 0) {
                SCRIPT_VM_CENSUS(IcMiss);
                SETP(ins->a, got);
                ++pc;
                NEXT();
            }
        }
    } else if (isString(base) && key == vm.atoms.length) {
        SETP(ins->a, JSValue::int32(std::int32_t(asString(base)->length)));
        ++pc;
        NEXT();
    } else if (isString(base) && cacheId != kNoCache) {
        if (JSValue* slot = stringMethodRead(vm, ic, key)) {
            COPYP(ins->a, *slot);
            ++pc;
            NEXT();
        }
    }
    fr->pc = pc;
    const JSValue v = getProperty(vm, base, key);
    if (v.isException())
        FAIL();
    SETP(ins->a, v);
    ++pc;
    NEXT();
}
L_PutProp: {
    const JSValue base = R[ins->a];
    Key key = atoms[ins->b];
    InlineCache scratch;
    const std::uint16_t cacheId = pc->a;
    InlineCache& ic = cacheId == kNoCache ? scratch : code->caches[cacheId];
    if (isObject(base)) {
        JSObject* o = asObject(base);
        if (o->h.shape == ic.shape) [[likely]] {
            if (ic.next == 0) {
                JSValue* slot = o->slotAt(ic.index);
                const JSValue old = *slot;
                *slot = R[ins->c];
                retain(*slot);
                release(old);
                ++pc;
                NEXT();
            }
            // A store that adds the property, where the prototypes still have nothing to say about it.
            // The new property's slot is in place or out of line (made when it is the first): objects of one shape can
            // have different inline counts.
            if ((!ic.validity || ic.validity->valid) && !(o->h.flags & (cellflags::kIsProto | cellflags::kNotExtensible))) {
                JSValue* slot = roomForAdd(vm, o, ic.index);
                *slot = R[ins->c];
                retain(*slot);
                o->h.shape = ic.next;
                ++pc;
                NEXT();
            }
        }
        if (storePoly(vm, code, ic, o, R[ins->c])) {
            ++pc;
            NEXT();
        }
        SCRIPT_VM_CENSUS(SlowPutProp);
        const std::uint32_t before = o->h.shape;
        fr->pc = pc;
        if (!setProperty(vm, base, key, R[ins->c], STRICT())) {
            if (!vm.exception.isUndefined())
                FAIL();
            ++pc;
            NEXT();
        }
        if (cacheId != kNoCache) {
            if (o->h.shape == before)
                learnOwnWrite(vm, ic, o, key, code);
            else
                learnAdd(vm, ic, o, before, key, true, code);
        }
        ++pc;
        NEXT();
    }
    fr->pc = pc;
    if (!setProperty(vm, base, key, R[ins->c], STRICT()) && !vm.exception.isUndefined())
        FAIL();
    ++pc;
    NEXT();
}
L_GetPropBound: {
    // Python's attribute read: a method read as a value is bound to its receiver by the prelude's `__py.bindRead`.
    fr->pc = pc;
    const JSValue base = R[ins->b];
    Key key = static_cast<Key>(code->atoms[ins->c]);
    JSValue read = getProperty(vm, base, key);
    if (read.isException())
        FAIL();
    if (isFunction(read) || read.isUndefined()) {
        Key pyKey = vm.internKey(std::string_view("__py"));
        JSValue py = getProperty(vm, toJSValue(vm.global), pyKey);
        detail::decRef(pyKey);
        if (py.isException()) {
            release(read);
            FAIL();
        }
        Handle pyKeep = Handle::adopt(py);
        if (isObject(py)) {
            Key hookKey = vm.internKey(std::string_view("bindRead"));
            JSValue hook = getProperty(vm, py, hookKey);
            detail::decRef(hookKey);
            if (hook.isException()) {
                release(read);
                FAIL();
            }
            Handle hookKeep = Handle::adopt(hook);
            if (isFunction(hook)) {
                const JSValue callArgs[3] = {base, JSValue::cell(key), read};
                JSValue bound = vm.call(hook, JSValue::undefined(), callArgs, 3);
                release(read);
                if (bound.isException())
                    FAIL();
                read = bound;
            }
        }
    }
    SET(ins->a, read);
    NEXT();
}
L_GetElem: {
    const JSValue base = R[ins->b];
    JSValue k = R[ins->c];
    if (k.isDouble()) [[unlikely]] {
        const double d = k.asDouble();
        if (d >= 0 && d < 2147483648.0 && double(std::int32_t(d)) == d)
            k = JSValue::int32(std::int32_t(d));
    }
    if (k.isInt32() && k.asInt32() >= 0) {
        if (isObject(base) && hasElems(asObject(base))) {
            auto* a = static_cast<JSArray*>(asObject(base));
            const std::uint32_t i = std::uint32_t(k.asInt32());
            if (i < a->length && !a->elems[i].isHole()) {
                COPYP(ins->a, a->elems[i]);
                NEXT();
            }
            if (plainHoleRead(vm, a)) {
                SET(ins->a, JSValue::undefined());
                NEXT();
            }
        } else if (isObject(base) && asObject(base)->cls() == ObjClass::TypedArray) {
            auto* t = static_cast<JSTypedArray*>(asObject(base));
            const std::uint32_t i = std::uint32_t(k.asInt32());
            if (i < t->length && !isBigKind(t->kind)) {
                SETP(ins->a, loadNumber(t->kind, t->data + std::size_t(i) * kElemSize[std::size_t(t->kind)]));
                NEXT();
            }
        } else if (isString(base)) {
            StringCell* s = asString(base);
            const std::uint32_t i = std::uint32_t(k.asInt32());
            if (i < s->length) {
                fr->pc = pc;
                const JSValue ch = makeString(vm, s->text().substr(i, 1));
                SETP(ins->a, ch);
                NEXT();
            }
        }
    }
    fr->pc = pc;
    if (base.isNullish()) {
        vm.throwError(ErrorCode::TypeError, nullishReadMessage(base, k));
        FAIL();
    }
    if (isPrivateKey(k)) [[unlikely]] {
        const JSValue v = privateGet(vm, base, k);
        if (v.isException())
            FAIL();
        SETP(ins->a, v);
        NEXT();
    }
    if (isObject(base) && k.isCell() && (isSymbol(k) || (isString(k) && asString(k)->interned))) {
        if (JSValue* slot = elemCached(code, std::size_t(ins - codeStart), asObject(base), k.asCell())) {
            COPYP(ins->a, *slot);
            NEXT();
        }
    }
    JSValue prim = toPrimitive(vm, k, true);
    if (prim.isException())
        FAIL();
    Key key = toPropertyKey(vm, prim);
    release(prim);
    if (!key)
        FAIL();
    if (isObject(base) && prim.isCell() && (isSymbol(prim) || isString(prim)))
        learnElem(vm, code, std::size_t(ins - codeStart), asObject(base), key);
    const JSValue v = getProperty(vm, base, key);
    release(JSValue::cell(key));
    if (v.isException())
        FAIL();
    SETP(ins->a, v);
    NEXT();
}
L_PutElem: {
    const JSValue base = R[ins->a];
    JSValue k = R[ins->b];
    if (k.isDouble()) [[unlikely]] {
        const double d = k.asDouble();
        if (d >= 0 && d < 2147483648.0 && double(std::int32_t(d)) == d)
            k = JSValue::int32(std::int32_t(d));
    }
    if (k.isInt32() && k.asInt32() >= 0 && R[ins->c].isNumber() && isObject(base) && asObject(base)->cls() == ObjClass::TypedArray) {
        auto* t = static_cast<JSTypedArray*>(asObject(base));
        const std::uint32_t i = std::uint32_t(k.asInt32());
        if (!isBigKind(t->kind)) {
            if (i < t->length)
                storeNumber(t->kind, t->data + std::size_t(i) * kElemSize[std::size_t(t->kind)], R[ins->c].asNumber());
            NEXT();
        }
    }
    if (k.isInt32() && k.asInt32() >= 0 && isObject(base) && asObject(base)->cls() == ObjClass::Array &&
        !(asObject(base)->h.flags & (cellflags::kFrozen | cellflags::kNotExtensible))) {
        auto* a = static_cast<JSArray*>(asObject(base));
        const std::uint32_t i = std::uint32_t(k.asInt32());
        if (i < a->length && !a->elems[i].isHole()) {
            const JSValue old = a->elems[i];
            a->elems[i] = R[ins->c];
            retain(a->elems[i]);
            a->noteStore(i, a->elems[i]);
            if (!a->elems[i].isNumber())
                a->h.flags &= std::uint8_t(~cellflags::kAllNumbers);
            release(old);
            NEXT();
        }
        if (i == a->length) {
            retain(R[ins->c]);
            a->append(vm, R[ins->c]);
            NEXT();
        }
        // A hole of a plain array (no properties of its own, the usual prototype chain): nothing can intercept the store.
        if (i < a->length && a->proto(vm) == vm.protos.array && !a->isDictionary(vm) && a->shapeOf(vm)->count == 0 &&
            vm.plainHoleStores()) {
            retain(R[ins->c]);
            a->elems[i] = R[ins->c];
            a->noteStore(i, R[ins->c]);
            if (!R[ins->c].isNumber())
                a->h.flags &= std::uint8_t(~cellflags::kAllNumbers);
            NEXT();
        }
    }
    fr->pc = pc;
    if (base.isNullish()) {
        vm.throwError(ErrorCode::TypeError, "Cannot set properties of " + std::string(base.isNull() ? "null" : "undefined"));
        FAIL();
    }
    if (isPrivateKey(k)) [[unlikely]] {
        if (!privatePut(vm, base, k, R[ins->c]))
            FAIL();
        NEXT();
    }
    JSValue prim = toPrimitive(vm, k, true);
    if (prim.isException())
        FAIL();
    Key key = toPropertyKey(vm, prim);
    release(prim);
    if (!key)
        FAIL();
    const bool ok = setProperty(vm, base, key, R[ins->c], STRICT());
    release(JSValue::cell(key));
    if (!ok && !vm.exception.isUndefined())
        FAIL();
    NEXT();
}
L_DelProp: {
    fr->pc = pc;
    const bool ok = deleteProperty(vm, R[ins->b], atoms[ins->c], STRICT());
    if (!vm.exception.isUndefined())
        FAIL();
    SET(ins->a, JSValue::boolean(ok));
    NEXT();
}
L_DelElem: {
    fr->pc = pc;
    JSValue prim = toPrimitive(vm, R[ins->c], true);
    if (prim.isException())
        FAIL();
    Key key = toPropertyKey(vm, prim);
    release(prim);
    if (!key)
        FAIL();
    const bool ok = deleteProperty(vm, R[ins->b], key, STRICT());
    release(JSValue::cell(key));
    if (!vm.exception.isUndefined())
        FAIL();
    SET(ins->a, JSValue::boolean(ok));
    NEXT();
}

// ---- literals ----
L_NewObject: {
    JSObject* o = vm.newObject(vm.protos.object, std::uint8_t(ins->b));
    SET(ins->a, toJSValue(o));
    // `SET` released the previous value; the new object's creation count is the register's.
    NEXT();
}
L_NewArray: {
    JSArray* a = vm.newArray(vm.protos.array, 0, ins->b);
    SET(ins->a, toJSValue(a));
    NEXT();
}
L_ArrayPush: {
    auto* a = static_cast<JSArray*>(asObject(R[ins->a]));
    retain(R[ins->b]);
    a->append(vm, R[ins->b]);
    NEXT();
}
L_InitProp: {
    JSObject* o = asObject(R[ins->a]);
    InlineCache scratch;
    const std::uint16_t cacheId = pc->a;
    InlineCache& ic = cacheId == kNoCache ? scratch : code->caches[cacheId];
    if (o->h.shape == ic.shape) [[likely]] {
        // The literal's next property: the shape it had and the shape it gets.
        JSValue* slot = roomForAdd(vm, o, ic.index);
        *slot = R[ins->c];
        retain(*slot);
        o->h.shape = ic.next;
        ++pc;
        NEXT();
    }
    const std::uint32_t before = o->h.shape;
    retain(R[ins->c]);
    o->putOwn(vm, atoms[ins->b], R[ins->c], attr::kDefault);
    if (cacheId != kNoCache)
        learnAdd(vm, ic, o, before, atoms[ins->b], false);
    ++pc;
    NEXT();
}
L_InitElem: {
    JSObject* o = asObject(R[ins->a]);
    fr->pc = pc;
    JSValue prim = toPrimitive(vm, R[ins->b], true);
    if (prim.isException())
        FAIL();
    Key key = toPropertyKey(vm, prim);
    release(prim);
    if (!key)
        FAIL();
    retain(R[ins->c]);
    o->putOwn(vm, key, R[ins->c], attr::kDefault);
    release(JSValue::cell(key));
    NEXT();
}
L_InitGetter:
L_InitSetter: {
    initAccessor(vm, asObject(R[ins->a]), atoms[ins->b], R[ins->c], ins->opcode() == Op::InitGetter);
    NEXT();
}
L_InitGetterElem:
L_InitSetterElem: {
    JSValue prim = toPrimitive(vm, R[ins->b], true);
    if (prim.isException())
        FAIL();
    Key key = toPropertyKey(vm, prim);
    release(prim);
    if (!key)
        FAIL();
    setFunctionName(vm, R[ins->c], key, ins->opcode() == Op::InitGetterElem ? "get " : "set ");
    initAccessor(vm, asObject(R[ins->a]), key, R[ins->c], ins->opcode() == Op::InitGetterElem);
    release(JSValue::cell(key));
    NEXT();
}
L_SetProto: {
    const JSValue p = R[ins->b];
    JSObject* o = asObject(R[ins->a]);
    if (isObject(p)) {
        if (!o->wouldCycle(vm, asObject(p)))
            o->setProto(vm, asObject(p));
    } else if (p.isNull()) {
        o->setProto(vm, nullptr);
    }
    NEXT();
}
L_RestSlice: {
    auto* all = static_cast<JSArray*>(asObject(R[ins->b]));
    const std::uint32_t from = ins->c;
    JSArray* rest = vm.newArray(vm.protos.array, 0, all->length > from ? all->length - from : 0);
    if (all->length > from)
        rest->appendAll(vm, all->elems + from, all->length - from);
    SET(ins->a, toJSValue(rest));
    NEXT();
}
L_SpreadObj: {
    fr->pc = pc;
    if (!copyOwnProperties(vm, asObject(R[ins->a]), R[ins->b], nullptr))
        FAIL();
    NEXT();
}
L_ObjRest: {
    const JSValue src = R[ins->b];
    if (src.isNullish()) {
        fr->pc = pc;
        vm.throwError(ErrorCode::TypeError, "Cannot destructure '" + describe(vm, src) + "' as it is " + describe(vm, src) + ".");
        FAIL();
    }
    JSObject* out = vm.newObject(vm.protos.object);
    SET(ins->a, toJSValue(out));
    fr->pc = pc;
    if (!copyOwnProperties(vm, out, src, static_cast<JSArray*>(asObject(R[ins->c]))))
        FAIL();
    NEXT();
}
L_IterRest: {
    const std::uint32_t it = ins->b;
    const JSValue src = R[it];
    std::int32_t index = R[it + 1].isInt32() ? R[it + 1].asInt32() : 0;
    JSArray* out = vm.newArray(vm.protos.array);
    SET(ins->a, toJSValue(out));
    if (!R[it + 1].isInt32()) {
        fr->pc = pc;
        while (isFunction(R[it + 1])) {
            JSValue value;
            const int step = iteratorStep(vm, R[it], R[it + 1], &value);
            if (step <= 0) {
                clearReg<Def>(R[it + 1]);
                if (step < 0)
                    FAIL();
                break;
            }
            out->append(vm, value);
        }
        NEXT();
    }
    if (isArray(src)) {
        auto* a = static_cast<JSArray*>(asObject(src));
        for (; std::uint32_t(index) < a->length; ++index) {
            const JSValue e = a->elems[index];
            retain(e);
            out->append(vm, e.isHole() ? JSValue::undefined() : e);
        }
    } else {
        StringCell* s = asString(src);
        const StringView text = s->text();
        while (std::uint32_t(index) < s->length) {
            std::size_t n = 1;
            if (text[std::size_t(index)] >= 0xd800 && text[std::size_t(index)] < 0xdc00 && std::size_t(index) + 1 < text.size() &&
                text[std::size_t(index) + 1] >= 0xdc00 && text[std::size_t(index) + 1] < 0xe000)
                n = 2;
            out->append(vm, makeString(vm, text.substr(std::size_t(index), n)));
            index += std::int32_t(n);
        }
    }
    R[it + 1] = JSValue::int32(index);
    NEXT();
}
L_NewClass: {
    CodeBlock* ctor = code->children[ins->b];
    JSObject* parent = nullptr;
    JSObject* parentProto = vm.protos.object;
    if (ins->c) {
        const JSValue sup = R[ins->c - 1];
        if (sup.isNull()) {
            parentProto = nullptr;
        } else if (!isFunction(sup)) {
            fr->pc = pc;
            vm.throwError(ErrorCode::TypeError, "Class extends value " + describe(vm, sup) + " is not a constructor or null");
            FAIL();
        } else {
            parent = asObject(sup);
            fr->pc = pc;
            JSValue pp = getProperty(vm, sup, vm.atoms.prototype);
            if (pp.isException())
                FAIL();
            if (isObject(pp)) {
                parentProto = asObject(pp);
            } else if (pp.isNull()) {
                parentProto = nullptr;
            } else {
                release(pp);
                vm.throwError(ErrorCode::TypeError, "Class extends value does not have valid prototype property");
                FAIL();
            }
            release(pp);
        }
    }
    const std::uint32_t nUp = std::uint32_t(ctor->upvals.size());
    Box* upSmall[16];
    Box** upBoxes = upSmall;
    if (nUp > 16) {
        vm.boxScratch.resize(nUp);
        upBoxes = vm.boxScratch.data();
    }
    for (std::uint32_t i = 0; i < nUp; ++i) {
        const UpvalDesc& d = ctor->upvals[i];
        upBoxes[i] = d.fromParentLocal ? reinterpret_cast<Box*>(R[d.index].asCell()) : fn->upvals()[d.index];
    }
    JSFunction* f = vm.newFunction(parent ? parent : vm.protos.function, ctor, upBoxes, nUp);
    Handle fKeep = Handle::adopt(toJSValue(f));
    f->materialize(vm);  // length and name now, so that they come before `prototype`
    f->flags |= JSFunction::kConstructor;
    {
        // Its own scope: NEXT() leaves by a computed goto, which does not run the destructors of the scopes it leaves,
        // so a Handle still alive there would keep its count for good.
        JSObject* proto = vm.newObject(parentProto);
        Handle protoKeep = Handle::adopt(toJSValue(proto));
        retain(toJSValue(proto));
        f->putOwn(vm, vm.atoms.prototype, toJSValue(proto), 0);
        retain(toJSValue(f));
        proto->putOwn(vm, vm.atoms.constructor, toJSValue(f), attr::kWritable | attr::kConfigurable);
        // The constructor is its own home: `super.x` in a static method looks at the class's prototype.
        Rare& rare = f->rareOf(vm);
        rare.home = toJSValue(proto);
        retain(rare.home);
    }
    SET(ins->a, fKeep.take());
    NEXT();
}
L_DefMethod:
L_DefGetter:
L_DefSetter: {
    JSObject* target = asObject(R[ins->a]);
    Key key = atoms[ins->b];
    JSValue fnv = R[ins->c];
    auto* method = static_cast<JSFunction*>(asObject(fnv));
    Rare& rr = method->rareOf(vm);
    release(rr.home);
    rr.home = R[ins->a];
    retain(rr.home);
    if (ins->opcode() == Op::DefMethod) {
        retain(fnv);
        target->putOwn(vm, key, fnv, attr::kWritable | attr::kConfigurable);
    } else {
        initAccessor(vm, target, key, fnv, ins->opcode() == Op::DefGetter, attr::kConfigurable);
    }
    NEXT();
}
L_DefMethodElem:
L_DefGetterElem:
L_DefSetterElem: {
    JSObject* target = asObject(R[ins->a]);
    fr->pc = pc;
    JSValue prim = toPrimitive(vm, R[ins->b], true);
    if (prim.isException())
        FAIL();
    Key key = toPropertyKey(vm, prim);
    release(prim);
    if (!key)
        FAIL();
    JSValue fnv = R[ins->c];
    auto* method = static_cast<JSFunction*>(asObject(fnv));
    setFunctionName(vm, fnv, key, ins->opcode() == Op::DefMethodElem ? "" : ins->opcode() == Op::DefGetterElem ? "get " : "set ");
    Rare& rr = method->rareOf(vm);
    release(rr.home);
    rr.home = R[ins->a];
    retain(rr.home);
    if (ins->opcode() == Op::DefMethodElem) {
        retain(fnv);
        target->putOwn(vm, key, fnv, attr::kWritable | attr::kConfigurable);
    } else {
        initAccessor(vm, target, key, fnv, ins->opcode() == Op::DefGetterElem, attr::kConfigurable);
    }
    release(JSValue::cell(key));
    NEXT();
}
L_SetFieldInit: {
    Rare& rr = asObject(R[ins->a])->rareOf(vm);
    release(rr.fieldInit);
    rr.fieldInit = R[ins->b];
    retain(rr.fieldInit);
    NEXT();
}
L_InitFields: {
    // The class whose constructor this is runs its field initialisers on `this`.
    Rare* rr = fn->rare();
    if (rr && isFunction(rr->fieldInit)) {
        fr->pc = pc;
        JSValue r = vm.call(rr->fieldInit, R[0], nullptr, 0);
        if (r.isException())
            FAIL();
        release(r);
    }
    NEXT();
}
L_FieldInit: {
    // The field initialiser of the class whose constructor this is (undefined for none): the code calls it on `this`.
    Rare* rr = fn->rare();
    COPY(ins->a, rr && isFunction(rr->fieldInit) ? rr->fieldInit : JSValue::undefined());
    NEXT();
}
L_GetSuperCtor: {
    JSObject* p = fn->proto(vm);
    if (!p || !isFunction(toJSValue(p)) || !isConstructorFn(static_cast<JSFunction*>(p))) {
        fr->pc = pc;
        vm.throwError(ErrorCode::TypeError, "the super class is not a constructor");
        FAIL();
    }
    COPY(ins->a, toJSValue(p));
    NEXT();
}
L_GetSuperElem: {
    Rare* rr = fn->rare();
    JSObject* start = nullptr;
    if (rr && isObject(rr->home))
        start = asObject(rr->home)->proto(vm);
    fr->pc = pc;
    JSValue prim = toPrimitive(vm, R[ins->b], true);
    if (prim.isException())
        FAIL();
    Key key = toPropertyKey(vm, prim);
    release(prim);
    if (!key)
        FAIL();
    if (!start) {
        release(JSValue::cell(key));
        vm.throwError(ErrorCode::TypeError, "Cannot read properties of null (reading a property through 'super')");
        FAIL();
    }
    JSValue v = getPropertyFrom(vm, start, R[ins->c], key);
    release(JSValue::cell(key));
    if (v.isException())
        FAIL();
    SET(ins->a, v);
    NEXT();
}
L_GetSuperProp: {
    // `super.name`: looked up from the prototype of the home object, with `this` as the receiver.
    Rare* rr = fn->rare();
    JSObject* start = nullptr;
    if (rr && isObject(rr->home))
        start = asObject(rr->home)->proto(vm);
    fr->pc = pc;
    if (!start) {
        vm.throwError(ErrorCode::TypeError, "Cannot read properties of null (reading a property through 'super')");
        FAIL();
    }
    JSValue v = getPropertyFrom(vm, start, R[ins->c], atoms[ins->b]);
    if (v.isException())
        FAIL();
    SET(ins->a, v);
    NEXT();
}
L_NewTarget:
    COPY(ins->a, fr->newTarget);
    NEXT();
L_SetFnName: {
    // The value just stored under a computed key, when it is an anonymous function or class.
    JSValue prim = toPrimitive(vm, R[ins->b], true);
    if (prim.isException())
        FAIL();
    Key key = toPropertyKey(vm, prim);
    release(prim);
    if (!key)
        FAIL();
    setFunctionName(vm, R[ins->a], key, "");
    release(JSValue::cell(key));
    NEXT();
}
L_SetEvalScope:
    vm.evalCode = code;
    vm.evalIndex = ins->a;
    vm.evalFrames = vm.frameCount();
    NEXT();
L_GetIteratorObj: {
    fr->pc = pc;
    JSValue iterator, next;
    if (!iteratorOf(vm, R[ins->b], &iterator, &next))
        FAIL();
    setReg<Def>(vm, R[ins->a + 1], next);
    setReg<Def>(vm, R[ins->a], iterator);
    NEXT();
}
L_DelegateCallAsync:
L_DelegateCall:
    fr->pc = pc;
    if (!delegateCall(vm, fr, ins))
        FAIL();
    NEXT();
L_DelegateDone:
    fr->pc = pc;
    if (!delegateDone(vm, fr, ins))
        FAIL();
    NEXT();
L_GenStart: {
    // The end of what a call of a generator function does: it stands at the start of the body until resumed.
    GenState* g = genStateOf(asObject(fr->gen));
    if (!g->prologue)
        NEXT();
    g->prologue = false;
    g->state = GenState::Start;
    g->suspend<Def>(code, R, std::uint32_t(pc - codeStart));
    --vm.frameTop;
    return JSValue::undefined();
}
L_SetNewTarget:
    // An arrow function that calls `super()` runs it with the new.target of the constructor around it.
    fr->newTarget = R[ins->a];
    NEXT();
L_GetSuperCtorOf: {
    JSObject* p = asObject(R[ins->b])->proto(vm);
    if (!p || !isFunction(toJSValue(p)) || !isConstructorFn(static_cast<JSFunction*>(p))) {
        fr->pc = pc;
        vm.throwError(ErrorCode::TypeError, "the super class is not a constructor");
        FAIL();
    }
    COPY(ins->a, toJSValue(p));
    NEXT();
}
L_InitFieldsOf: {
    Rare* rr = asObject(R[ins->a])->rare();
    if (rr && isFunction(rr->fieldInit)) {
        fr->pc = pc;
        JSValue r = vm.call(rr->fieldInit, R[ins->b], nullptr, 0);
        if (r.isException())
            FAIL();
        release(r);
    }
    NEXT();
}
L_CheckBound:
    if (R[ins->a] != JSValue::boolean(true)) [[unlikely]] {
        fr->pc = pc;
        vm.throwError(ErrorCode::ReferenceError, "'this' is not there until super() has been called");
        FAIL();
    }
    NEXT();
L_CheckUnbound:
    if (R[ins->a] == JSValue::boolean(true)) [[unlikely]] {
        fr->pc = pc;
        vm.throwError(ErrorCode::ReferenceError, "super() has already been called");
        FAIL();
    }
    NEXT();
L_FreshThisIfBound:
    // A second `super()` still runs the parent constructor, on an object of its own, before it fails.
    if (R[ins->b] == JSValue::boolean(true)) [[unlikely]]
        setReg<Def>(vm, R[ins->a], toJSValue(vm.newObject(vm.protos.object)));
    NEXT();
L_CheckDerivedRet:
    if (!isObject(R[ins->a])) {
        fr->pc = pc;
        if (!R[ins->a].isUndefined()) {
            vm.throwError(ErrorCode::TypeError, "a derived constructor can only return an object or undefined");
            FAIL();
        }
        if (R[ins->b] != JSValue::boolean(true)) {
            vm.throwError(ErrorCode::ReferenceError, "a derived constructor did not call super()");
            FAIL();
        }
    }
    NEXT();
L_Closure: {
    CodeBlock* child = code->children[ins->b];
    const std::uint32_t n = std::uint32_t(child->upvals.size());
    Box* small[16];
    Box** boxes = small;
    if (n > 16) {
        vm.boxScratch.resize(n);
        boxes = vm.boxScratch.data();
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        const UpvalDesc& d = child->upvals[i];
        boxes[i] = d.fromParentLocal ? reinterpret_cast<Box*>(R[d.index].asCell()) : fn->upvals()[d.index];
    }
    JSFunction* f = vm.newFunction(vm.protos.function, child, boxes, n);
    SET(ins->a, toJSValue(f));
    NEXT();
}

// ---- calls ----
L_CallSpread:
L_NewSpread:
L_SuperSpread: {
    // The arguments are in an array in the register after `this`: spread them over the window.
    const std::uint32_t at = ins->b;
    auto* list = static_cast<JSArray*>(asObject(R[at + 2]));
    const std::uint32_t n = list->length;
    JSValue* window = R + at + 2;
    fr->pc = pc;
    if (window + n + 8 > vm.stackEnd || n > (1u << 20)) {
        vm.throwError(ErrorCode::RangeError, "Too many arguments");
        FAIL();
    }
    spreadOverWindow(vm, window, list, n);
    vm.stackTop = std::max(vm.stackTop, window + n);  // the window may reach past this frame's registers
    callBase = at;
    callArgc = n;
    callMode = ins->opcode() == Op::NewSpread ? 1 : ins->opcode() == Op::SuperSpread ? 2 : 0;
    goto do_call;
}
L_Call:
L_New:
L_SuperCall:
    callBase = ins->b;
    callArgc = ins->c;
    callMode = ins->opcode() == Op::New ? 1 : ins->opcode() == Op::SuperCall ? 2 : 0;
do_call: {
    const bool isNew = callMode == 1, isSuper = callMode == 2;
    const std::uint32_t base = callBase, argc = callArgc;
    if (!isNew && !isSuper)
        unwrapBound<Def>(R, base);
    const JSValue callee = R[base];
    fr->pc = pc;
    if (!isFunction(callee)) [[unlikely]] {
        if (isNew)
            vm.throwError(ErrorCode::TypeError, describe(vm, callee) + " is not a constructor");
        else
            notCallable(vm, callee);
        FAIL();
    }
    auto* f = static_cast<JSFunction*>(asObject(callee));
    if (!(f->flags & JSFunction::kNative) && (f->code->flags & CodeBlock::kLazy)) [[unlikely]] {
        if (!vm.ensureCompiled(f->code))
            FAIL();
    }
    if (isNew) {
        const bool canConstruct = (f->flags & JSFunction::kNative) ? (f->flags & JSFunction::kConstructor) != 0
                                                                     : f->code->constructor;
        if (!canConstruct) {
            vm.throwError(ErrorCode::TypeError, describe(vm, callee) + " is not a constructor");
            FAIL();
        }
        JSObject* made;
        if (vm.prototypeAt2(f)) [[likely]] {
            // Every ordinary function once its `prototype` is made, and every class: `length`, `name`, `prototype`, ...
            const JSValue pv = *f->slotAt(2);
            Shape* root = vm.rootShapeFor(isObject(pv) ? asObject(pv) : vm.protos.object, 3);
            made = vm.newObject(root, (f->flags & JSFunction::kNative) ? 3 : newSlotsOf(vm, f, root));
        } else {
            JSValue protoValue = getProperty(vm, callee, vm.atoms.prototype);
            if (protoValue.isException())
                FAIL();
            Shape* root = vm.rootShapeFor(isObject(protoValue) ? asObject(protoValue) : vm.protos.object, 3);
            made = vm.newObject(root, (f->flags & JSFunction::kNative) ? 3 : newSlotsOf(vm, f, root));
            release(protoValue);
        }
        setReg<Def>(vm, R[base + 1], toJSValue(made));
    }
    if (f->flags & JSFunction::kNative) {
        Rare* r = f->rare();
        vm.stackTop = std::max(R + code->nregs, R + base + 2 + argc);
        JSValue saved = vm.newTarget;
        vm.newTarget = isNew ? callee : isSuper ? fr->newTarget : JSValue::undefined();
        JSValue res = r->native(vm, R[base + 1], &R[base + 2], argc, r->nativeData);
        vm.newTarget = saved;
        vm.stackTop = R + code->nregs;
        if (res.isException())
            FAIL();
        if ((isNew || isSuper) && !isObject(res)) {
            release(res);
            res = R[base + 1];
            retain(res);
        } else if (isSuper && res != R[base + 1] && isObject(R[base + 1]) && !asObject(res)->isProto()) {
            // A built-in that makes its own object: it gets the prototype of the class being constructed.
            asObject(res)->setProto(vm, asObject(R[base + 1])->proto(vm));
        }
        SETP(ins->a, res);
        NEXT();
    }
    TICK();
    SCRIPT_VM_CENSUS(ScriptCalls);
    CodeBlock* c = f->code;
    if ((c->flags & CodeBlock::kClassConstructor) && !isNew && !isSuper) [[unlikely]] {
        vm.throwError(ErrorCode::TypeError, "class constructor " + (c->name ? toUtf8(static_cast<StringCell*>(c->name)->text()) : std::string()) +
                                                " must be called with 'new'");
        FAIL();
    }
    if (c->flags & CodeBlock::kGenerator) [[unlikely]] {
        JSValue made = vm.makeGenerator(f, R[base + 1], &R[base + 2], argc);
        if (made.isException())
            FAIL();
        SET(ins->a, made);
        NEXT();
    }
    if (!c->tier1 && !c->tier1Failed && vm.tier1Threshold && ++c->hotness >= vm.tier1Threshold) {
        CodeBlock* d = tierUpBlock(vm, f, c);
        if (d == c)
            compileTier1(vm, c);
        c = d;
    }
    JSValue* newR = R + base + 1;    if (c->prof.empty()) [[unlikely]]
        c->prof.assign(c->code.size() + c->nparams + 2, 0);
    if (!c->tier1) {
        // What the arguments are, for the compiler of this function.
        const std::size_t at = c->code.size();
        for (std::uint32_t k = 0; k < c->nparams; ++k)
            c->prof[at + k] |= k < argc ? profBit(R[base + 2 + k]) : std::uint8_t(8);
    }

    if (vm.frameTop >= vm.frameLimit || newR + c->nregs + 8 > vm.stackEnd || (c->tier1 && nativeStackLow())) [[unlikely]] {
        vm.throwError(ErrorCode::StackOverflow, "Maximum call stack size exceeded");
        FAIL();
    }
    if (!(c->flags & (CodeBlock::kStrict | CodeBlock::kArrow)) && newR[0].isNullish()) {
        copyReg<Def>(newR[0], toJSValue(vm.global));
    }
    const std::uint32_t keep = 1 + (argc < c->nparams ? argc : c->nparams);
    JSArray* argsArray = (c->flags & CodeBlock::kNeedsArgs) ? captureArgumentsFor(vm, c, newR + 1, argc) : nullptr;
    // Past what is initialised the registers are fresh; below it, what this frame sits on top of may be the caller's dead
    // temporaries or arguments the callee has no parameter for, which let go of their values now.
    if (newR + c->nregs + 1 > vm.stackInit) [[unlikely]]
        vm.fillStack(newR + c->nregs + 1);  // one past: compiled code's test of the stack wants it
    {
        // All of the caller's registers above the arguments, not only those the callee's frame covers: a call this frame
        // makes in turn may reach past its end, and must find undefined there, not a dead temporary of this caller.
        // (A deferred frame: its own, as what is above it is not known to be undefined; a scan frees nothing a stale
        // register names.)
        JSValue* callerEnd = R + code->nregs;
        const std::uint32_t overlap = Def ? c->nregs : callerEnd > newR ? std::uint32_t(callerEnd - newR) : 0;
        for (std::uint32_t i = keep; i < overlap; ++i) {
            clearReg<Def>(newR[i]);
        }
        // Arguments past the parameters, if they sit beyond the caller's own registers.
        for (std::uint32_t i = std::max(keep, overlap); i < 1 + argc && i < c->nregs; ++i) {
            clearReg<Def>(newR[i]);
        }
    }
    if (argsArray) {
        // The array goes in the register after the parameters, which the clearing above left empty.
        if (newR + 1 + c->nparams >= vm.stackInit)
            vm.fillStack(newR + 2 + c->nparams);
        setReg<Def>(vm, newR[1 + c->nparams], toJSValue(argsArray));
    }
    *vm.frameTop++ = Frame{c, f, nullptr, newR, std::uint16_t(ins->a), 0, argc, isNew || isSuper, false,
                                          isNew ? callee : isSuper ? fr->newTarget : JSValue::undefined()};
    if (c->tier1) {
        // Compiled code runs on the machine stack: the frame is its own entry, and the result comes back here.
        (vm.frameTop - 1)->entry = true;
        const JSValue res = enterTier1(vm, c->tier1, 0);
        vm.stackTop = R + code->nregs;
        if (res.isException())
            FAIL();
        SETP(ins->a, res);
        NEXT();
    }
    fr = vm.frameTop - 1;
    LOAD_FRAME();
    vm.stackTop = R + code->nregs;
    NEXT();
}
L_Ret: {
    JSValue result = R[ins->a];
    retain(result);
    if (fr->construct && !isObject(result)) {
        release(result);
        result = R[0];
        retain(result);
    }
    const std::uint32_t keep = 1 + (fr->argc < code->nparams ? fr->argc : code->nparams);
    clearRegs<Def>(R, keep, code->nregs);
    const bool entry = fr->entry;
    const std::uint16_t dst = fr->retDst;
    JSValue* calleeR = R;
    --vm.frameTop;
    if (entry) {
        (void)calleeR;
        return result;
    }
    fr = vm.frameTop - 1;
    LOAD_FRAME();
    vm.stackTop = R + code->nregs;
    if (!code->prof.empty())
        code->prof[std::size_t(pc - codeStart) - 1] |= profBit(result);
    SET(dst, result);
    NEXT();
}
L_Throw: {
    release(vm.exception);
    vm.exception = R[ins->a];
    retain(vm.exception);
    vm.exceptionCode = ErrorCode::Exception;
    SCRIPT_VM_CENSUS(ThrowCount);
    FAIL();
}
L_ThrowError: {
    vm.throwError(ErrorCode(ins->a), toUtf8(asString(K[ins->b])->text()));
    FAIL();
}

// ---- iteration ----
L_KeysOf: {
    fr->pc = pc;
    const JSValue keys = forInKeys(vm, R[ins->b]);
    if (keys.isException())
        FAIL();
    SET(ins->a, keys);
    NEXT();
}
L_IterInit: {
    const JSValue src = R[ins->b];
    if (!arrayIterationIntact(vm, src) && !isString(src)) {
        fr->pc = pc;
        JSValue iterator, next;
        if (!iteratorOf(vm, src, &iterator, &next))
            FAIL();
        // Registers a and a+1: the iterator and its `next`; the pair is an index for an array or a string.
        setReg<Def>(vm, R[ins->a + 1], next);
        setReg<Def>(vm, R[ins->a], iterator);
        NEXT();
    }
    // Registers a and a+1: the sequence and the next index.
    const JSValue keep = src;
    retain(keep);
    setReg<Def>(vm, R[ins->a + 1], JSValue::int32(0));
    setReg<Def>(vm, R[ins->a], keep);
    NEXT();
}
L_IterNext: {
    const std::uint32_t it = ins->b;
    if (!R[it + 1].isInt32()) [[unlikely]] {
        fr->pc = pc;
        if (!isFunction(R[it + 1])) {
            // Finished already.
            COPY(ins->a, ins->c ? JSValue::undefined() : JSValue::hole());
            NEXT();
        }
        JSValue value;
        const int step = iteratorStep(vm, R[it], R[it + 1], &value);
        if (step <= 0) {
            // Done, or `next` itself failed: either way the iterator is not closed afterwards.
            clearReg<Def>(R[it + 1]);
            if (step < 0)
                FAIL();
            COPY(ins->a, ins->c ? JSValue::undefined() : JSValue::hole());
        } else {
            SET(ins->a, value);
        }
        NEXT();
    }
    const JSValue src = R[it];
    const std::int32_t index = R[it + 1].asInt32();
    JSValue out = JSValue::undefined();
    bool owned = false, done;
    std::int32_t step = 1;
    if (isArray(src)) {
        auto* a = static_cast<JSArray*>(asObject(src));
        done = std::uint32_t(index) >= a->length;
        if (!done && !a->elems[index].isHole())
            out = a->elems[index];
    } else {
        StringCell* s = asString(src);
        done = std::uint32_t(index) >= s->length;
        if (!done) {
            fr->pc = pc;
            const StringView text = s->text();
            std::size_t n = 1;
            if (text[std::size_t(index)] >= 0xd800 && text[std::size_t(index)] < 0xdc00 && std::size_t(index) + 1 < text.size() &&
                text[std::size_t(index) + 1] >= 0xdc00 && text[std::size_t(index) + 1] < 0xe000)
                n = 2;
            out = makeString(vm, text.substr(std::size_t(index), n));
            owned = true;
            step = std::int32_t(n);
        }
    }
    if (done) {
        // A pattern reads undefined past the end; a loop is told to stop by the hole.
        if (!ins->c)
            out = JSValue::hole();
    } else {
        R[it + 1] = JSValue::int32(index + step);
    }
    if (owned)
        SET(ins->a, out);
    else
        COPY(ins->a, out);
    NEXT();
}

L_NewPrivateName: {
    JSValue sym = vm.newSymbol(static_cast<StringCell*>(atoms[ins->b])->text(), true);
    static_cast<dyn::SymbolCell*>(sym.asCell())->isPrivate = true;
    SET(ins->a, sym);
    NEXT();
}
L_DefPrivate: {
    fr->pc = pc;
    JSObject* target = asObject(R[ins->a]);
    if (target->findOwn(vm, R[ins->b].asCell())) {
        vm.throwError(ErrorCode::TypeError, "Cannot initialize " + privateName(R[ins->b]) + " twice on the same object");
        FAIL();
    }
    retain(R[ins->c]);
    target->putOwn(vm, R[ins->b].asCell(), R[ins->c], attr::kWritable);
    NEXT();
}
L_DefPrivMethod:
L_DefPrivGetter:
L_DefPrivSetter: {
    fr->pc = pc;
    JSObject* target = asObject(R[ins->a]);
    const Op which = ins->opcode();
    std::uint8_t existing = 0;
    if (target->findOwn(vm, R[ins->b].asCell(), &existing) && (which == Op::DefPrivMethod || !(existing & attr::kAccessor))) {
        vm.throwError(ErrorCode::TypeError, "Cannot initialize private methods of class twice on the same object");
        FAIL();
    }
    if (which == Op::DefPrivMethod) {
        retain(R[ins->c]);
        target->putOwn(vm, R[ins->b].asCell(), R[ins->c], 0);
    } else {
        initAccessor(vm, target, R[ins->b].asCell(), R[ins->c], which == Op::DefPrivGetter, attr::kConfigurable);
    }
    NEXT();
}
L_ToKey: {
    fr->pc = pc;
    JSValue prim = toPrimitive(vm, R[ins->b], true);
    if (prim.isException())
        FAIL();
    Key key = toPropertyKey(vm, prim);
    release(prim);
    if (!key)
        FAIL();
    SET(ins->a, JSValue::cell(key));
    NEXT();
}
L_GetHome: {
    Rare* rr = fn->rare();
    COPY(ins->a, rr && isObject(rr->home) ? rr->home : JSValue::undefined());
    NEXT();
}
L_SetHome: {
    auto* f = static_cast<JSFunction*>(asObject(R[ins->a]));
    Rare& rr = f->rareOf(vm);
    release(rr.home);
    rr.home = R[ins->b];
    retain(rr.home);
    NEXT();
}
L_NewRegExp: {
    fr->pc = pc;
    JSValue r = bi::newRegExpLiteral(vm, K[ins->b], K[ins->c]);
    if (r.isException())
        FAIL();
    SET(ins->a, r);
    NEXT();
}
L_AsyncIterInit: {
    // `for await` and `yield*` in an async generator: the object's async iterator, or its sync one wrapped to look async.
    fr->pc = pc;
    const JSValue src = R[ins->b];
    if (src.isNullish()) {
        vm.throwError(ErrorCode::TypeError, describe(vm, src) + " is not async iterable");
        FAIL();
    }
    JSValue method = getProperty(vm, src, vm.atoms.symAsyncIterator);
    if (method.isException())
        FAIL();
    JSValue iterator, next;
    if (isFunction(method)) {
        iterator = vm.call(method, src, nullptr, 0);
        release(method);
        if (iterator.isException())
            FAIL();
        if (!isObject(iterator)) {
            release(iterator);
            vm.throwError(ErrorCode::TypeError, "Result of the Symbol.asyncIterator method is not an object");
            FAIL();
        }
        next = getProperty(vm, iterator, vm.atoms.next);
        if (next.isException()) {
            release(iterator);
            FAIL();
        }
    } else {
        // A method that is there and not callable is an error; only an absent one falls back to the sync iterator.
        const bool absent = method.isNullish();
        release(method);
        if (!absent) {
            vm.throwError(ErrorCode::TypeError, describe(vm, src) + "[Symbol.asyncIterator] is not a function");
            FAIL();
        }
        Handle syncIt, syncNext;
        if (!bi::getIterator(vm, src, &syncIt, &syncNext))
            FAIL();
        iterator = bi::asyncFromSyncIterator(vm, syncIt.get(), syncNext.get());
        if (iterator.isException())
            FAIL();
        next = getProperty(vm, iterator, vm.atoms.next);
        if (next.isException()) {
            release(iterator);
            FAIL();
        }
    }
    setReg<Def>(vm, R[ins->a + 1], next);
    setReg<Def>(vm, R[ins->a], iterator);
    NEXT();
}
L_AsyncIterNext: {
    fr->pc = pc;
    const std::uint32_t it = ins->b;
    JSValue r = vm.call(R[it + 1], R[it], nullptr, 0);
    if (r.isException())
        FAIL();
    SET(ins->a, r);
    NEXT();
}
L_IterUnpack: {
    // The result of an awaited `next()`: the hole once it is done, else its value.
    fr->pc = pc;
    const JSValue res = R[ins->b];
    if (!isObject(res)) {
        vm.throwError(ErrorCode::TypeError, "Iterator result " + describe(vm, res) + " is not an object");
        FAIL();
    }
    JSValue d = getProperty(vm, res, vm.atoms.done);
    if (d.isException())
        FAIL();
    const bool done = toBoolean(d);
    release(d);
    if (done) {
        // The iterator is not to be closed afterwards; `it` is the pair before this instruction's operands.
        COPY(ins->a, JSValue::hole());
        NEXT();
    }
    JSValue v = getProperty(vm, res, vm.atoms.value);
    if (v.isException())
        FAIL();
    SET(ins->a, v);
    NEXT();
}
L_IterClose: {
    const std::uint32_t it = ins->a;
    if (!isFunction(R[it + 1]))
        NEXT();
    const bool quiet = ins->b != 0;
    const JSValue iterator = R[it];
    {
        clearReg<Def>(R[it + 1]);
    }
    fr->pc = pc;
    JSValue f = getProperty(vm, iterator, vm.atoms.return_);
    if (f.isException()) {
        if (!quiet)
            FAIL();
        vm.clearException();
        NEXT();
    }
    if (f.isNullish()) {
        NEXT();
    }
    if (!isFunction(f)) {
        release(f);
        if (!quiet) {
            vm.throwError(ErrorCode::TypeError, "iterator.return is not a function");
            FAIL();
        }
        NEXT();
    }
    JSValue r = vm.call(f, iterator, nullptr, 0);
    release(f);
    if (r.isException()) {
        if (!quiet)
            FAIL();
        vm.clearException();
        NEXT();
    }
    const bool object = isObject(r);
    release(r);
    if (!quiet && !object) {
        vm.throwError(ErrorCode::TypeError, "iterator.return() did not return an object");
        FAIL();
    }
    NEXT();
}
L_Await:
L_YieldDelegate:
L_Yield:
    return suspendFrame<Def>(vm, fr, ins);

throw_: {
    // Unwind: the innermost handler that covers the instruction that threw, in this frame or the callers'.
    for (;;) {
        const std::size_t at = std::size_t(fr->pc - code->code.data()) - 1;
        const Handler* found = nullptr;
        for (const Handler& h : code->handlers)
            if (at >= h.start && at < h.end && !(h.isCatch && vm.exception == vm.returnMarker)) {
                found = &h;
                break;
            }
        if (found) {
            const JSValue thrown = vm.exception;
            vm.exception = JSValue::undefined();
            vm.exceptionCode = ErrorCode::Ok;
            setReg<Def>(vm, R[found->reg], thrown);
            pc = codeStart + found->target;
            NEXT();
        }
        const std::uint32_t keep = 1 + (fr->argc < code->nparams ? fr->argc : code->nparams);
        clearRegs<Def>(R, keep, code->nregs);
        const bool entry = fr->entry;
        --vm.frameTop;
        if (entry)
            return JSValue::exception();
        fr = vm.frameTop - 1;
        code = fr->code;
        codeStart = code->code.data();
        R = fr->R;
        fn = fr->fn;
        pc = fr->pc;  // just after the call that failed
        K = code->consts.data();
        atoms = code->atoms.data();
        vm.stackTop = R + code->nregs;
    }
}
#undef LOAD_FRAME
#undef DISPATCH
#undef NEXT
#undef TICK
#undef LOOP_TICK
#undef TAKE16
#undef SET
#undef SETP
#undef COPYP
#undef COPY
#undef FAIL
#undef STRICT
}

JSValue run(VM& vm)
{
    return vm.deferred ? runImpl<false, true>(vm, nullptr) : runImpl<false, false>(vm, nullptr);
}

}  // namespace

namespace {
void noteElem(CodeBlock* code, std::uint32_t index, JSValue base);
}  // namespace

void noteElemBase(CodeBlock* code, std::uint32_t index, JSValue base)
{
    noteElem(code, index, base);
}

std::uint64_t stepInstruction(VM& vm, std::uint32_t index)
{
    Frame* fr = vm.frameTop - 1;
    const Ins* at = fr->code->code.data() + index;
    const Ins* fall = at + ((index + 1 < fr->code->code.size() && at[1].opcode() == Op::Cache) ? 2 : 1);
    // A comparison of two strings runs nothing.
    const Op op = at->opcode();
    if (op >= Op::JLt && op <= Op::JNotGe && isString(fr->R[at->a]) && isString(fr->R[at->b])) {
        const bool negated = op >= Op::JNotLt;
        const Op rel = Op(int(Op::Lt) + int(op) - int(negated ? Op::JNotLt : Op::JLt));
        const bool holds = relational(vm, rel, fr->R[at->a], fr->R[at->b]) != 0;
        fr->pc = holds == negated ? at + 1 : at + 1 + at->jump16();
        return holds == negated ? kStepFall : kStepJump;
    }
    fr->pc = at;
    return (vm.deferred ? runImpl<true, true>(vm, fall) : runImpl<true, false>(vm, fall)).bits;
}


// ------------------------------------------------- helpers for compiled code ---
// What the interpreter does for one instruction, without the dispatch: compiled code calls these where it has no template
// of its own, or for what a template gives up on. Each returns 1 when done and 0 for an exception (pending, the frame's
// pc set just after the instruction), as the handlers above do. They must agree with the handlers.

namespace {
struct JitCtx
{
    Frame* fr;
    CodeBlock* code;
    const Ins* ins;
    JSValue* R;
    Key* atoms;
    std::uint16_t cacheId;
    InlineCache* ic(InlineCache& scratch) const { return cacheId == kNoCache ? &scratch : &code->caches[cacheId]; }
};

/// What an element access the baseline code did not finish found as its base, for the optimizing compiler.
void noteElem(CodeBlock* code, std::uint32_t index, JSValue base)
{
    if (index >= code->elemProf.size())
        return;
    std::uint8_t bit = elemprof::kOther;
    if (isObject(base)) {
        JSObject* o = asObject(base);
        if (o->cls() == ObjClass::Array) {
            bit = elemprof::kArray;
        } else if (o->cls() == ObjClass::TypedArray) {
            const ElemKind k = static_cast<JSTypedArray*>(o)->kind;
            bit = k == ElemKind::Float64 ? elemprof::kFloat64 : k == ElemKind::Int32 ? elemprof::kInt32
                : k == ElemKind::Uint8 ? elemprof::kUint8 : elemprof::kOtherTyped;
            if (bit == elemprof::kOtherTyped && std::uint8_t(k) < 8 && index < code->elemKinds.size())
                code->elemKinds[index] |= std::uint8_t(1u << std::uint8_t(k));
        }
    }
    code->elemProf[index] |= bit;
}

inline JitCtx jitCtx(VM& vm, std::uint32_t index, bool hasCache)
{
    Frame* fr = vm.frameTop - 1;
    CodeBlock* code = fr->code;
    const Ins* ins = code->code.data() + index;
    fr->pc = ins + (hasCache ? 2 : 1);
    vm.stackTop = fr->R + code->nregs;
    return JitCtx{fr, code, ins, fr->R, code->atoms.data(), hasCache ? ins[1].a : std::uint16_t(kNoCache)};
}
}  // namespace

namespace {
/// Tell the baseline code of instruction `at` where the global object keeps the data property whose slot is `slot`.
void noteGlobalSlot(VM& vm, CodeBlock* code, std::uint32_t at, JSValue* slot)
{
    if (at >= code->globalRecs.size())
        return;
    JSObject* g = vm.global;
    JSValue* inl = g->inl();
    CodeBlock::GlobalRec& r = code->globalRecs[at];
    r.index = slot >= inl && slot < inl + g->h.aux ? std::uint32_t(slot - inl) : std::uint32_t(slot - g->slots) + g->h.aux;
    r.epoch = vm.globalEpoch;
}
}  // namespace

int jitPutGlobal(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, true);
    const Ins* ins = c.ins;
    Key key = c.atoms[ins->a];
    JSObject* g = vm.global;
    std::uint8_t attrs = 0;
    JSValue* slot = g->findOwn(vm, key, &attrs);
    if (slot && (attrs & attr::kWritable) && !(attrs & attr::kAccessor)) {
        noteGlobalSlot(vm, c.code, index, slot);
        const JSValue old = *slot;
        *slot = c.R[ins->b];
        vm.slotWritten(g, slot);
        retain(*slot);
        release(old);
        return 1;
    }
    const bool strict = (c.code->flags & CodeBlock::kStrict) != 0;
    if (!slot && strict && !hasProperty(vm, toJSValue(g), key)) {
        vm.throwError(ErrorCode::ReferenceError, toUtf8(static_cast<StringCell*>(key)->text()) + " is not defined");
        return 0;
    }
    if (!setProperty(vm, toJSValue(g), key, c.R[ins->b], strict) && !vm.exception.isUndefined())
        return 0;
    return 1;
}

int jitGetGlobal(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, true);
    const Ins* ins = c.ins;
    Key key = c.atoms[ins->b];
    InlineCache scratch;
    InlineCache& ic = *c.ic(scratch);
    JSObject* g = vm.global;
    std::uint8_t attrs = 0;
    JSValue* slot = g->findOwn(vm, key, &attrs);
    if (slot && !(attrs & attr::kAccessor)) {
        if (std::uint8_t at = 0; c.cacheId != kNoCache) {
            if (const std::int32_t place = cachedOwnIndex(vm, g, key, &at); place >= 0) {
                ic.shape = g->h.shape;
                ic.index = std::uint32_t(place);
            }
        }
        noteGlobalSlot(vm, c.code, index, slot);
        copyReg(vm, c.R[ins->a], *slot);
        return 1;
    }
    if (!hasProperty(vm, toJSValue(g), key)) {
        if (ins->opcode() == Op::GetGlobalOrUndef) {
            setReg(vm, c.R[ins->a], JSValue::undefined());
            return 1;
        }
        vm.throwError(ErrorCode::ReferenceError, toUtf8(static_cast<StringCell*>(key)->text()) + " is not defined");
        return 0;
    }
    JSValue v = getProperty(vm, toJSValue(g), key);
    if (v.isException())
        return 0;
    setReg(vm, c.R[ins->a], v);
    return 1;
}

int jitGetSuperProp(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    const Ins* ins = c.ins;
    Rare* rr = c.fr->fn ? c.fr->fn->rare() : nullptr;
    JSObject* const home = rr && isObject(rr->home) ? asObject(rr->home) : nullptr;
    JSObject* const start = home ? home->proto(vm) : nullptr;
    if (!start) {
        vm.throwError(ErrorCode::TypeError, "Cannot read properties of null (reading a property through 'super')");
        return 0;
    }
    const Key key = c.atoms[ins->b];
    // A data property of an ordinary object on the chain: remembered for the compiled code, by slot.
    if (index < c.code->superCaches.size()) {
        // The cell first: making it may move the slots of the objects on the chain.
        ValidityCell* cell = vm.validityOf(home);
        for (JSObject* p = start; p; p = p->proto(vm)) {
            if (p->cls() != ObjClass::Plain && p->cls() != ObjClass::Function)
                break;
            std::uint8_t attrs = 0;
            JSValue* slot = p->findOwn(vm, key, &attrs);
            if (!slot)
                continue;
            if (!(attrs & attr::kAccessor) && cell->valid) {
                SuperCache& sc = c.code->superCaches[index];
                vm.retainValidity(cell);
                if (sc.validity)
                    vm.releaseValidity(sc.validity);
                sc = SuperCache{home, slot, cell};
                copyReg(vm, c.R[ins->a], *slot);
                return 1;
            }
            break;
        }
    }
    JSValue v = getPropertyFrom(vm, start, c.R[ins->c], key);
    if (v.isException())
        return 0;
    setReg(vm, c.R[ins->a], v);
    return 1;
}

/// After a read of `key` on `o` found a data property where a cache may say so: remember it in the megamorphic table.
void megaLearnRead(VM& vm, JSObject* o, Key key) noexcept
{
    if (!vm.megaRead || key == vm.atoms.length || hasElems(o))
        return;
    // (A name that is an index, or a typed array's, is answered by elements on objects of the same shape.)
    double numeric;
    if (key->kind == detail::CellKind::String && canonicalNumericIndex(key, &numeric))
        return;
    if (const MegaEntry& was = vm.megaRead[VM::megaSlot(o->h.shape, key)];
        was.shape == o->h.shape && was.key == key && (!was.validity || was.validity->valid) && (!was.absent || was.absent->valid))
        return;  // there already
    // A dictionary only once a site's cache took it (the table does not count its reads: a dictionary marked cached gives
    // up its shape for a new one on every delete).
    if (o->isDictionary(vm) && !(o->shapeOf(vm)->flags & Shape::kCachedDict))
        return;
    std::uint8_t attrs = 0;
    std::int32_t index = cachedOwnIndex(vm, o, key, &attrs);
    JSObject* holder = nullptr;
    ValidityCell* cell = nullptr;
    ValidityCell* absent = nullptr;
    if (index < 0) {
        // A dictionary receiver (one a site's cache took) that lacks the key: while it gains none, the prototypes answer.
        const bool dict = o->isDictionary(vm);
        if (!cacheable(vm, o) && !dict)
            return;
        for (JSObject* p = o->proto(vm); p; p = p->proto(vm)) {
            // A prototype that is a dictionary (one with very many methods) keeps a key's slot while the chain's
            // validity holds: its changes invalidate it.
            const bool dictProto = p->isDictionary(vm) && !proxyOf(p) &&
                                   !(p->cls() == ObjClass::Function && (static_cast<JSFunction*>(p)->flags & JSFunction::kLazyProps));
            if (!cacheable(vm, p) && !dictProto)
                return;
            if (dictProto) {
                const Dictionary& d = *p->rare()->dict;
                index = d.find(key);
                if (index >= 0)
                    attrs = d.entries[std::uint32_t(index)].attrs;
            } else {
                index = p->shapeOf(vm)->indexOf(key, &attrs);
            }
            if (index >= 0) {
                holder = p;
                break;
            }
        }
        if (!holder) {
            // Nowhere: undefined, as long as the prototypes stay as they are.
            for (JSObject* p = o; p; p = p->proto(vm))
                if (answersForKeys(p))
                    return;
            if (!(holder = o->proto(vm)))
                return;
            index = std::int32_t(VM::kMegaAbsent);
            attrs = 0;
        }
        try {
            cell = vm.validityOf(o->proto(vm));
        } catch (...) {
            return;
        }
        if (!cell->valid)
            return;
        if (dict) {
            try {
                absent = vm.absenceOf(o);
            } catch (...) {
                return;
            }
        }
    }
    if (attrs & attr::kAccessor)
        return;
    MegaEntry& e = vm.megaRead[VM::megaSlot(o->h.shape, key)];
    if (cell)
        vm.retainValidity(cell);
    if (absent)
        vm.retainValidity(absent);
    if (e.validity)
        vm.releaseValidity(e.validity);
    if (e.absent)
        vm.releaseValidity(e.absent);
    detail::incRef(key);
    if (e.key)
        detail::decRef(e.key);
    e = MegaEntry{o->h.shape, std::uint32_t(index), key, holder, cell, absent, {}};
}

int jitGetProp(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, true);
    const Ins* ins = c.ins;
    JSValue* R = c.R;
    const JSValue base = R[ins->b];
    Key key = c.atoms[ins->c];
    InlineCache scratch;
    InlineCache& ic = *c.ic(scratch);
    if (isObject(base)) {
        JSObject* o = asObject(base);
        if (o->h.shape == ic.shape) {
            if (!ic.holder) {
                copyReg(vm, R[ins->a], *o->slotAt(ic.index));
                return 1;
            }
            if (ic.validity->valid) {
                copyReg(vm, R[ins->a], *ic.holder->slotAt(ic.index));
                return 1;
            }
        }
        if (key == vm.atoms.length && hasElems(o)) {
            const std::uint32_t n = static_cast<JSArray*>(o)->length;
            setReg(vm, R[ins->a], n <= 0x7FFFFFFFu ? JSValue::int32(std::int32_t(n)) : JSValue::boxDouble(double(n)));
            return 1;
        }
        if (key == vm.atoms.length && o->cls() == ObjClass::TypedArray) {
            const std::uint32_t n = static_cast<JSTypedArray*>(o)->length;
            setReg(vm, R[ins->a], n <= 0x7FFFFFFFu ? JSValue::int32(std::int32_t(n)) : JSValue::boxDouble(double(n)));
            return 1;
        }
        // A getter may let go of the object (writing the register it came from): it is held until it is learned from.
        retain(base);
        struct Hold
        {
            JSValue v;
            ~Hold() { release(v); }
        } hold{base};
        // A site whose cache cannot hold the read, or has every entry taken: it reads through the megamorphic table.
        const bool full = ic.pad && c.code->poly[ic.pad - 1].n == 4;
        // (A marked site has done with the caches: no learning into them, only the table.)
        if (c.cacheId != kNoCache && !(ic.flags & InlineCache::kMegaSite)) {
            JSValue got;
            const int r = readCachedSlow(vm, c.code, ic, o, base, key, &got);
            if (r < 0)
                return 0;
            if (r > 0) {
                setReg(vm, R[ins->a], got);
                if (full) {
                    ic.flags |= InlineCache::kMegaSite;
                    megaLearnRead(vm, o, key);
                }
                return 1;
            }
            ic.flags |= InlineCache::kMegaSite;
        }
        const JSValue v = getProperty(vm, base, key);
        if (v.isException())
            return 0;
        setReg(vm, R[ins->a], v);
        if (ic.flags & InlineCache::kMegaSite)
            megaLearnRead(vm, o, key);
        return 1;
    } else if (isString(base) && key == vm.atoms.length) {
        setReg(vm, R[ins->a], JSValue::int32(std::int32_t(asString(base)->length)));
        return 1;
    } else if (isString(base) && c.cacheId != kNoCache) {
        if (JSValue* slot = stringMethodRead(vm, ic, key)) {
            copyReg(vm, R[ins->a], *slot);
            return 1;
        }
    }
    const JSValue v = getProperty(vm, base, key);
    if (v.isException())
        return 0;
    setReg(vm, R[ins->a], v);
    return 1;
}

int jitPutProp(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, true);
    const Ins* ins = c.ins;
    JSValue* R = c.R;
    const JSValue base = R[ins->a];
    Key key = c.atoms[ins->b];
    InlineCache scratch;
    InlineCache& ic = *c.ic(scratch);
    const bool strict = (c.code->flags & CodeBlock::kStrict) != 0;
    if (isObject(base)) {
        JSObject* o = asObject(base);
        if (o->h.shape == ic.shape) {
            if (ic.next == 0) {
                JSValue* slot = o->slotAt(ic.index);
                const JSValue old = *slot;
                *slot = R[ins->c];
                retain(*slot);
                release(old);
                return 1;
            }
            // The new property's slot is in place or out of line (made when it is the first): objects of one shape can
            // have different inline counts.
            if ((!ic.validity || ic.validity->valid) && !(o->h.flags & (cellflags::kIsProto | cellflags::kNotExtensible))) {
                JSValue* slot = roomForAdd(vm, o, ic.index);
                *slot = R[ins->c];
                retain(*slot);
                o->h.shape = ic.next;
                return 1;
            }
        }
        if (const std::uint32_t was = o->h.shape; storePoly(vm, c.code, ic, o, R[ins->c])) {
            promoteStore(c.code, ic, was);
            return 1;
        }
        const std::uint32_t before = o->h.shape;
        if (!setProperty(vm, base, key, R[ins->c], strict)) {
            if (!vm.exception.isUndefined())
                return 0;
            return 1;
        }
        if (c.cacheId != kNoCache) {
            if (o->h.shape == before)
                learnOwnWrite(vm, ic, o, key, c.code);
            else
                learnAdd(vm, ic, o, before, key, true, c.code);
        }
        return 1;
    }
    if (!setProperty(vm, base, key, R[ins->c], strict) && !vm.exception.isUndefined())
        return 0;
    return 1;
}

int jitDefPrivate(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    const Ins* ins = c.ins;
    JSValue* R = c.R;
    JSObject* target = asObject(R[ins->a]);
    const JSValue key = R[ins->b];
    if (target->findOwn(vm, key.asCell())) {
        vm.throwError(ErrorCode::TypeError, "Cannot initialize " + privateName(key) + " twice on the same object");
        return 0;
    }
    const std::uint32_t before = target->h.shape;
    retain(R[ins->c]);
    target->putOwn(vm, key.asCell(), R[ins->c], attr::kWritable);
    // The add, for the compiled code: from a shape in the tree, into a slot in place, of an object nothing inherits from.
    if (PrivateCache* pc = privateCacheAt(c.code, index); pc && !target->isProto() && !target->isDictionary(vm) && !proxyOf(target)) {
        Shape* now = target->shapeOf(vm);
        Shape* was = vm.shapeById(before);
        if (was && now->parent == was && now->key == key.asCell() && now->attrs == attr::kWritable && was->count < target->h.aux)
            *pc = PrivateCache{key.asCell(), before, was->count, target->h.shape};
    }
    return 1;
}

int jitInitProp(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, true);
    const Ins* ins = c.ins;
    JSValue* R = c.R;
    JSObject* o = asObject(R[ins->a]);
    InlineCache scratch;
    InlineCache& ic = *c.ic(scratch);
    if (o->h.shape == ic.shape) {
        JSValue* slot = roomForAdd(vm, o, ic.index);
        *slot = R[ins->c];
        retain(*slot);
        o->h.shape = ic.next;
        return 1;
    }
    const std::uint32_t before = o->h.shape;
    retain(R[ins->c]);
    o->putOwn(vm, c.atoms[ins->b], R[ins->c], attr::kDefault);
    if (c.cacheId != kNoCache)
        learnAdd(vm, ic, o, before, c.atoms[ins->b], false);
    return 1;
}

int jitNewObject(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    JSObject* o = vm.newObject(vm.protos.object, std::uint8_t(c.ins->b));
    setReg(vm, c.R[c.ins->a], toJSValue(o));
    return 1;
}

int jitNewArray(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    JSArray* a = vm.newArray(vm.protos.array, 0, c.ins->b);
    setReg(vm, c.R[c.ins->a], toJSValue(a));
    return 1;
}

int jitArrayPush(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    auto* a = static_cast<JSArray*>(asObject(c.R[c.ins->a]));
    retain(c.R[c.ins->b]);
    a->append(vm, c.R[c.ins->b]);
    return 1;
}


int jitDelegateCall(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    return delegateCall(vm, vm.frameTop - 1, c.ins);
}

int jitDelegateDone(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    return delegateDone(vm, vm.frameTop - 1, c.ins);
}

int jitGetIteratorObj(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    JSValue iterator, next;
    if (!iteratorOf(vm, c.R[c.ins->b], &iterator, &next))
        return 0;
    setReg(vm, c.R[c.ins->a + 1], next);
    setReg(vm, c.R[c.ins->a], iterator);
    return 1;
}

JSValue jitSuspend(VM& vm, std::uint32_t index)
{
    Frame* fr = vm.frameTop - 1;
    const Ins* ins = fr->code->code.data() + index;
    return vm.deferred ? suspendFrame<true>(vm, fr, ins) : suspendFrame<false>(vm, fr, ins);
}

int jitBinary(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    const Ins* ins = c.ins;
    JSValue* R = c.R;
    const JSValue a = R[ins->b], b = R[ins->c];
    const Op op = ins->opcode();
    JSValue r;
    switch (op) {
    case Op::Add:
        if (ins->a == ins->b && isString(a) && appendInPlace(vm, R[ins->a], b, false))
            return 1;
        r = add(vm, a, b);
        break;
    case Op::Append: {
        const JSValue to = R[ins->a];
        if (isString(to) && appendInPlace(vm, R[ins->a], a, true))
            return 1;
        r = add(vm, to, a);
        break;
    }
    case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: {
        const int rel = relational(vm, op, a, b);
        if (rel < 0)
            return 0;
        r = JSValue::boolean(rel != 0);
        break;
    }
    case Op::Eq: case Op::Ne: {
        bool ok = true;
        const bool eq = looseEquals(vm, a, b, &ok);
        if (!ok)
            return 0;
        r = JSValue::boolean(op == Op::Eq ? eq : !eq);
        break;
    }
    case Op::StrictEq: case Op::StrictNe: {
        const bool eq = fastStrictEquals(a, b);
        r = JSValue::boolean(op == Op::StrictEq ? eq : !eq);
        break;
    }
    default:
        r = arithmetic(vm, op, a, b);
        break;
    }
    if (r.isException())
        return 0;
    setReg(vm, R[ins->a], r);
    return 1;
}

// Compiled code's helpers for opcodes that have no template: 1 done, 0 an exception, and 2 "not handled, nothing changed:
// run the instruction in the interpreter" for the rare cases a helper does not take.

int jitIn(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    const JSValue key = c.R[c.ins->b], o = c.R[c.ins->c];
    const int r = inOperator(vm, key, o);
    if (r < 0)
        return 0;
    if (isString(key) && asString(key)->interned && isObject(c.R[c.ins->c]) && c.R[c.ins->c] == o)
        megaLearnRead(vm, asObject(o), key.asCell());
    setReg(vm, c.R[c.ins->a], JSValue::boolean(r != 0));
    return 1;
}

int jitToString(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    const JSValue r = toStringValue(vm, c.R[c.ins->b]);
    if (r.isException())
        return 0;
    setReg(vm, c.R[c.ins->a], r);
    return 1;
}

int jitToNumber(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    const JSValue a = c.R[c.ins->b];
    if (a.isNumber()) {
        copyReg(vm, c.R[c.ins->a], a);
        return 1;
    }
    double d;
    if (!toNumber(vm, a, &d))
        return 0;
    setReg(vm, c.R[c.ins->a], JSValue::boxNumber(d));
    return 1;
}

int jitUnary(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    const Ins* ins = c.ins;
    const JSValue a = c.R[ins->b];
    const bool neg = ins->opcode() == Op::Neg;
    if (a.isInt32() && (!neg || (a.asInt32() != 0 && a.asInt32() != INT32_MIN))) {
        setReg(vm, c.R[ins->a], JSValue::int32(neg ? -a.asInt32() : ~a.asInt32()));
        return 1;
    }
    JSValue n = toNumeric(vm, a);
    if (n.isException())
        return 0;
    if (isBigInt(n)) {
        JSValue r = bigintUnary(vm, neg ? 0 : 1, n);
        release(n);
        if (r.isException())
            return 0;
        setReg(vm, c.R[ins->a], r);
        return 1;
    }
    setReg(vm, c.R[ins->a], neg ? JSValue::boxNumber(-n.asNumber()) : JSValue::int32(~toInt32(n.asNumber())));
    return 1;
}

int jitCopyBox(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    Box* box = reinterpret_cast<Box*>(c.R[c.ins->a].asCell());
    retain(box->value);
    Box* fresh = vm.newBox(box->value);
    setReg(vm, c.R[c.ins->a], JSValue::cell(reinterpret_cast<CellPtr>(fresh)));
    return 1;
}

int jitRestSlice(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    auto* all = static_cast<JSArray*>(asObject(c.R[c.ins->b]));
    const std::uint32_t from = c.ins->c;
    JSArray* rest = vm.newArray(vm.protos.array, 0, all->length > from ? all->length - from : 0);
    if (all->length > from)
        rest->appendAll(vm, all->elems + from, all->length - from);
    setReg(vm, c.R[c.ins->a], toJSValue(rest));
    return 1;
}

int jitSpreadObj(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    return copyOwnProperties(vm, asObject(c.R[c.ins->a]), c.R[c.ins->b], nullptr) ? 1 : 0;
}

int jitKeysOf(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    const JSValue keys = forInKeys(vm, c.R[c.ins->b]);
    if (keys.isException())
        return 0;
    setReg(vm, c.R[c.ins->a], keys);
    return 1;
}

int jitLoadCallee(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    copyReg(vm, c.R[c.ins->a], toJSValue(c.fr->fn));
    return 1;
}

int jitNewTarget(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    copyReg(vm, c.R[c.ins->a], c.fr->newTarget);
    return 1;
}

int jitGetSuperCtor(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    JSObject* p = c.fr->fn->proto(vm);
    if (!p || !isFunction(toJSValue(p)) || !isConstructorFn(static_cast<JSFunction*>(p))) {
        vm.throwError(ErrorCode::TypeError, "the super class is not a constructor");
        return 0;
    }
    copyReg(vm, c.R[c.ins->a], toJSValue(p));
    return 1;
}

int jitFreshThisIfBound(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    if (c.R[c.ins->b] == JSValue::boolean(true))
        setReg(vm, c.R[c.ins->a], toJSValue(vm.newObject(vm.protos.object)));
    return 1;
}

int jitCallSpread(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    const Ins* ins = c.ins;
    if (ins->opcode() != Op::CallSpread)
        return 2;
    JSValue* R = c.R;
    const std::uint32_t at = ins->b;
    auto* list = static_cast<JSArray*>(asObject(R[at + 2]));
    const std::uint32_t n = list->length;
    JSValue* window = R + at + 2;
    if (window + n + 8 > vm.stackEnd || n > (1u << 20))
        return 2;
    const JSValue callee = R[at];
    if (!isFunction(callee))
        return 2;
    spreadOverWindow(vm, window, list, n);
    vm.stackTop = std::max(vm.stackTop, window + n);
    const JSValue res = vm.call(callee, R[at + 1], window, n);
    if (res.isException())
        return 0;
    setReg(vm, R[ins->a], res);
    return 1;
}

int jitIterInit(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    const Ins* ins = c.ins;
    JSValue* R = c.R;
    const JSValue src = R[ins->b];
    if (!arrayIterationIntact(vm, src) && !isString(src)) {
        // Any other iterable.
        JSValue iterator, next;
        if (!iteratorOf(vm, src, &iterator, &next))
            return 0;
        setReg(vm, R[ins->a + 1], next);
        setReg(vm, R[ins->a], iterator);
        return 1;
    }
    retain(src);
    setReg(vm, R[ins->a + 1], JSValue::int32(0));
    setReg(vm, R[ins->a], src);
    return 1;
}

int jitNewRegExp(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    const Ins* ins = c.ins;
    JSValue r = bi::newRegExpLiteral(vm, c.code->consts[ins->b], c.code->consts[ins->c]);
    if (r.isException())
        return 0;
    setReg(vm, c.R[ins->a], r);
    return 1;
}

int jitEnvHas(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    const Ins* ins = c.ins;
    const JSValue env = c.R[ins->b];
    setReg(vm, c.R[ins->a], JSValue::boolean(isObject(env) && asObject(env)->findOwn(vm, c.atoms[ins->c]) != nullptr));
    return 1;
}

int jitDefPrivMethod(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    const Ins* ins = c.ins;
    JSValue* R = c.R;
    JSObject* target = asObject(R[ins->a]);
    const Op which = ins->opcode();
    std::uint8_t existing = 0;
    if (target->findOwn(vm, R[ins->b].asCell(), &existing) && (which == Op::DefPrivMethod || !(existing & attr::kAccessor))) {
        vm.throwError(ErrorCode::TypeError, "Cannot initialize private methods of class twice on the same object");
        return 0;
    }
    if (which == Op::DefPrivMethod) {
        const std::uint32_t before = target->h.shape;
        retain(R[ins->c]);
        target->putOwn(vm, R[ins->b].asCell(), R[ins->c], 0);
        // The add, for the compiled code, as for a private field (`jitDefPrivate`).
        if (PrivateCache* pc = privateCacheAt(c.code, index); pc && !target->isProto() && !target->isDictionary(vm) && !proxyOf(target)) {
            Shape* now = target->shapeOf(vm);
            Shape* was = vm.shapeById(before);
            if (was && now->parent == was && now->key == R[ins->b].asCell() && now->attrs == 0 && was->count < target->h.aux)
                *pc = PrivateCache{R[ins->b].asCell(), before, was->count, target->h.shape};
        }
    } else {
        initAccessor(vm, target, R[ins->b].asCell(), R[ins->c], which == Op::DefPrivGetter, attr::kConfigurable);
    }
    return 1;
}

int jitIterNext(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    const Ins* ins = c.ins;
    JSValue* R = c.R;
    const std::uint32_t it = ins->b;
    if (!R[it + 1].isInt32()) {
        if (!isFunction(R[it + 1])) {
            copyReg(vm, R[ins->a], ins->c ? JSValue::undefined() : JSValue::hole());
            return 1;
        }
        JSValue value;
        const int step = iteratorStep(vm, R[it], R[it + 1], &value);
        if (step <= 0) {
            clearReg(vm, R[it + 1]);
            if (step < 0)
                return 0;
            copyReg(vm, R[ins->a], ins->c ? JSValue::undefined() : JSValue::hole());
        } else {
            setReg(vm, R[ins->a], value);
        }
        return 1;
    }
    const JSValue src = R[it];
    const std::int32_t at = R[it + 1].asInt32();
    JSValue out = JSValue::undefined();
    bool owned = false, done;
    std::int32_t step = 1;
    if (isArray(src)) {
        auto* a = static_cast<JSArray*>(asObject(src));
        done = std::uint32_t(at) >= a->length;
        if (!done && !a->elems[at].isHole())
            out = a->elems[at];
    } else {
        StringCell* s = asString(src);
        done = std::uint32_t(at) >= s->length;
        if (!done) {
            const StringView text = s->text();
            std::size_t n = 1;
            if (text[std::size_t(at)] >= 0xd800 && text[std::size_t(at)] < 0xdc00 && std::size_t(at) + 1 < text.size() &&
                text[std::size_t(at) + 1] >= 0xdc00 && text[std::size_t(at) + 1] < 0xe000)
                n = 2;
            out = makeString(vm, text.substr(std::size_t(at), n));
            owned = true;
            step = std::int32_t(n);
        }
    }
    if (done) {
        if (!ins->c)
            out = JSValue::hole();
    } else {
        R[it + 1] = JSValue::int32(at + step);
    }
    if (owned)
        setReg(vm, R[ins->a], out);
    else
        copyReg(vm, R[ins->a], out);
    return 1;
}

int jitIterClose(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    return isFunction(c.R[c.ins->a + 1]) ? 2 : 1;
}

void jitUnwrapBound(VM& vm, JSValue* R, std::uint32_t base)
{
    if (vm.deferred)
        unwrapBound<true>(R, base);
    else
        unwrapBound<false>(R, base);
}

JSArray* jitCaptureArguments(VM& vm, const CodeBlock* c, const JSValue* args, std::uint32_t argc)
{
    return captureArgumentsFor(vm, c, args, argc);
}

int jitInstanceOf(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    const int r = instanceOf(vm, c.R[c.ins->b], c.R[c.ins->c]);
    if (r < 0)
        return 0;
    setReg(vm, c.R[c.ins->a], JSValue::boolean(r != 0));
    return 1;
}

int jitBinaryImm(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    const Ins* ins = c.ins;
    JSValue* R = c.R;
    const JSValue a = R[ins->b];
    const JSValue y = JSValue::int32(std::int16_t(ins->c));
    const Op op = ins->opcode();
    const Op generic = op == Op::AddI ? Op::Add : op == Op::SubI ? Op::Sub : op == Op::MulI ? Op::Mul : op == Op::ModI ? Op::Mod
                       : op == Op::BitAndI ? Op::BitAnd : op == Op::BitOrI ? Op::BitOr : op == Op::BitXorI ? Op::BitXor
                       : op == Op::ShlI ? Op::Shl : op == Op::ShrI ? Op::Shr : Op::UShr;
    const JSValue r = generic == Op::Add ? add(vm, a, y) : arithmetic(vm, generic, a, y);
    if (r.isException())
        return 0;
    setReg(vm, R[ins->a], r);
    return 1;
}

int jitGetElem(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    const Ins* ins = c.ins;
    JSValue* R = c.R;
    const JSValue base = R[ins->b];
    JSValue k = R[ins->c];
    noteElem(c.code, index, base);
    if (k.isDouble()) {
        const double d = k.asDouble();
        if (d >= 0 && d < 2147483648.0 && double(std::int32_t(d)) == d)
            k = JSValue::int32(std::int32_t(d));
    }
    if (k.isInt32() && k.asInt32() >= 0) {
        if (isObject(base) && hasElems(asObject(base))) {
            auto* a = static_cast<JSArray*>(asObject(base));
            const std::uint32_t i = std::uint32_t(k.asInt32());
            if (i < a->length && !a->elems[i].isHole()) {
                copyReg(vm, R[ins->a], a->elems[i]);
                return 1;
            }
            if (plainHoleRead(vm, a)) {
                setReg(vm, R[ins->a], JSValue::undefined());
                return 1;
            }
        } else if (isObject(base) && asObject(base)->cls() == ObjClass::TypedArray) {
            auto* t = static_cast<JSTypedArray*>(asObject(base));
            const std::uint32_t i = std::uint32_t(k.asInt32());
            if (i < t->length && !isBigKind(t->kind)) {
                setReg(vm, R[ins->a], loadNumber(t->kind, t->data + std::size_t(i) * kElemSize[std::size_t(t->kind)]));
                return 1;
            }
        } else if (isString(base)) {
            StringCell* s = asString(base);
            const std::uint32_t i = std::uint32_t(k.asInt32());
            if (i < s->length) {
                const JSValue ch = makeString(vm, s->text().substr(i, 1));
                setReg(vm, R[ins->a], ch);
                return 1;
            }
        }
    }
    if (base.isNullish()) {
        vm.throwError(ErrorCode::TypeError, nullishReadMessage(base, k));
        return 0;
    }
    if (isPrivateKey(k)) {
        PrivateCache* pc = privateCacheAt(c.code, index);
        if (JSValue* slot = privateCached(pc, base, k)) {
            copyReg(vm, R[ins->a], *slot);
            return 1;
        }
        const JSValue v = privateGet(vm, base, k);
        if (v.isException())
            return 0;
        setReg(vm, R[ins->a], v);
        learnPrivate(vm, pc, base, k, false);
        if (isObject(R[ins->b]) && R[ins->b] == base)
            megaLearnRead(vm, asObject(base), k.asCell());
        return 1;
    }
    if (isObject(base) && k.isCell() && (isSymbol(k) || (isString(k) && asString(k)->interned))) {
        if (JSValue* slot = elemCached(c.code, index, asObject(base), k.asCell())) {
            copyReg(vm, R[ins->a], *slot);
            return 1;
        }
    }
    JSValue prim = toPrimitive(vm, k, true);
    if (prim.isException())
        return 0;
    Key key = toPropertyKey(vm, prim);
    release(prim);
    if (!key)
        return 0;
    if (isObject(base) && prim.isCell() && (isSymbol(prim) || isString(prim)))
        learnElem(vm, c.code, index, asObject(base), key);
    const JSValue v = getProperty(vm, base, key);
    if (!v.isException()) {
        // (A getter may have let go of the object.)
        if (isObject(R[ins->b]) && R[ins->b] == base)
            megaLearnRead(vm, asObject(base), key);
        setReg(vm, R[ins->a], v);
    }
    release(JSValue::cell(key));
    return !v.isException();
}

int jitPutElem(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    const Ins* ins = c.ins;
    JSValue* R = c.R;
    const JSValue base = R[ins->a];
    JSValue k = R[ins->b];
    noteElem(c.code, index, base);
    if (k.isDouble()) {
        const double d = k.asDouble();
        if (d >= 0 && d < 2147483648.0 && double(std::int32_t(d)) == d)
            k = JSValue::int32(std::int32_t(d));
    }
    if (k.isInt32() && k.asInt32() >= 0 && R[ins->c].isNumber() && isObject(base) && asObject(base)->cls() == ObjClass::TypedArray) {
        auto* t = static_cast<JSTypedArray*>(asObject(base));
        const std::uint32_t i = std::uint32_t(k.asInt32());
        if (!isBigKind(t->kind)) {
            if (i < t->length)
                storeNumber(t->kind, t->data + std::size_t(i) * kElemSize[std::size_t(t->kind)], R[ins->c].asNumber());
            return 1;
        }
    }
    if (k.isInt32() && k.asInt32() >= 0 && isObject(base) && asObject(base)->cls() == ObjClass::Array &&
        !(asObject(base)->h.flags & (cellflags::kFrozen | cellflags::kNotExtensible))) {
        auto* a = static_cast<JSArray*>(asObject(base));
        const std::uint32_t i = std::uint32_t(k.asInt32());
        if (i < a->length && !a->elems[i].isHole()) {
            const JSValue old = a->elems[i];
            a->elems[i] = R[ins->c];
            retain(a->elems[i]);
            a->noteStore(i, a->elems[i]);
            if (!a->elems[i].isNumber())
                a->h.flags &= std::uint8_t(~cellflags::kAllNumbers);
            release(old);
            return 1;
        }
        if (i == a->length) {
            retain(R[ins->c]);
            a->append(vm, R[ins->c]);
            return 1;
        }
        if (i < a->length && a->proto(vm) == vm.protos.array && !a->isDictionary(vm) && a->shapeOf(vm)->count == 0 &&
            vm.plainHoleStores()) {
            retain(R[ins->c]);
            a->elems[i] = R[ins->c];
            a->noteStore(i, R[ins->c]);
            if (!R[ins->c].isNumber())
                a->h.flags &= std::uint8_t(~cellflags::kAllNumbers);
            return 1;
        }
    }
    if (base.isNullish()) {
        vm.throwError(ErrorCode::TypeError, "Cannot set properties of " + std::string(base.isNull() ? "null" : "undefined"));
        return 0;
    }
    if (isPrivateKey(k)) {
        PrivateCache* pc = privateCacheAt(c.code, index);
        if (JSValue* slot = privateCached(pc, base, k)) {
            const JSValue old = *slot;
            *slot = R[ins->c];
            retain(*slot);
            release(old);
            return 1;
        }
        if (!privatePut(vm, base, k, R[ins->c]))
            return 0;
        learnPrivate(vm, pc, base, k, true);
        return 1;
    }
    JSValue prim = toPrimitive(vm, k, true);
    if (prim.isException())
        return 0;
    Key key = toPropertyKey(vm, prim);
    release(prim);
    if (!key)
        return 0;
    const bool ok = setProperty(vm, base, key, R[ins->c], (c.code->flags & CodeBlock::kStrict) != 0);
    release(JSValue::cell(key));
    if (!ok && !vm.exception.isUndefined())
        return 0;
    return 1;
}

int jitClosure(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    const Ins* ins = c.ins;
    JSValue* R = c.R;
    CodeBlock* child = c.code->children[ins->b];
    const std::uint32_t n = std::uint32_t(child->upvals.size());
    Box* small[16];
    Box** boxes = small;
    if (n > 16) {
        vm.boxScratch.resize(n);
        boxes = vm.boxScratch.data();
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        const UpvalDesc& d = child->upvals[i];
        boxes[i] = d.fromParentLocal ? reinterpret_cast<Box*>(R[d.index].asCell()) : c.fr->fn->upvals()[d.index];
    }
    JSFunction* f = vm.newFunction(vm.protos.function, child, boxes, n);
    setReg(vm, R[ins->a], toJSValue(f));
    return 1;
}

int jitNewBox(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    if (vm.deferred)
        retain(c.R[c.ins->a]);  // the box's own count, where the register held none
    Box* box = vm.newBox(c.R[c.ins->a]);  // takes the register's count
    c.R[c.ins->a] = JSValue::cell(reinterpret_cast<CellPtr>(box));
    if (vm.deferred)
        release(c.R[c.ins->a]);
    return 1;
}

std::uint64_t stepThrow(VM& vm, std::uint32_t index)
{
    Frame* fr = vm.frameTop - 1;
    const Ins* at = fr->code->code.data() + index;
    fr->pc = at + ((index + 1 < fr->code->code.size() && at[1].opcode() == Op::Cache) ? 2 : 1);
    vm.resumeThrow = true;
    return (vm.deferred ? runImpl<true, true>(vm, nullptr) : runImpl<true, false>(vm, nullptr)).bits;
}

// ----------------------------------------------------------------- calling ---

JSValue VM::call(JSValue callee, JSValue thisValue, const JSValue* args, std::uint32_t argc)
{
    return callInternal(callee, thisValue, args, argc, false);
}

JSValue VM::callInternal(JSValue callee, JSValue thisValue, const JSValue* args, std::uint32_t argc, bool construct, JSValue target)
{
    if (!isFunction(callee))
        return notCallable(*this, callee);
    if (nativeStackLow() && !overflowing) {
        // Making the error calls code of its own, which must be let through on what stack is left.
        overflowing = true;
        JSValue e = throwError(ErrorCode::StackOverflow, "Maximum call stack size exceeded");
        overflowing = false;
        return e;
    }
    auto* f = static_cast<JSFunction*>(asObject(callee));
    if (f->flags & JSFunction::kNative) {
        Rare* r = f->rare();
        const JSValue saved = newTarget;
        newTarget = JSValue::undefined();
        const JSValue result = r->native(*this, thisValue, args, argc, r->nativeData);
        newTarget = saved;
        return result;
    }
    CodeBlock* c = f->code;
    if (!ensureCompiled(c)) [[unlikely]]
        return JSValue::exception();
    if (!construct && c->tier1 &&
        !(c->flags & (CodeBlock::kClassConstructor | CodeBlock::kGenerator | CodeBlock::kAsync | CodeBlock::kAsyncGen |
                      CodeBlock::kNeedsArgs))) {
        // A compiled ordinary function, the usual callback: `this` and the arguments it has parameters for go in. The rest
        // of its registers are undefined unless the frame sits on dead temporaries of a frame below (a native called
        // from a function whose own frame began inside its caller's): those are let go of, as a call between two
        // script functions does.
        JSValue* R = stackTop;
        if (frameTop < frameLimit && R + c->nregs + 8 <= stackEnd) {
            if (R + c->nregs + 1 > stackInit) [[unlikely]]
                fillStack(R + c->nregs + 1);
            JSValue self = thisValue;
            if (!(c->flags & (CodeBlock::kStrict | CodeBlock::kArrow | CodeBlock::kNoThis)) && self.isNullish())
                self = toJSValue(global);
            const std::uint32_t n = argc < c->nparams ? argc : c->nparams;
            R[0] = self;
            for (std::uint32_t i = 0; i < n; ++i)
                R[1 + i] = args[i];
            for (std::uint32_t i = 0; i <= n; ++i)
                retainReg(*this, R[i]);
            for (std::uint32_t i = 1 + n; i < c->nregs; ++i)
                if (!R[i].isUndefined()) [[unlikely]]
                    clearReg(*this, R[i]);
            *frameTop++ = Frame{c, f, nullptr, R, 0, 0, n, false, true, JSValue::undefined()};
            const JSValue result = enterTier1(*this, c->tier1, 0);
            stackTop = R;
            clearRegs(*this, R, 0, 1 + n);
            if (deferred && frameTop == frameBase)
                scanAtHost();
            return result;
        }
    }
    if ((c->flags & CodeBlock::kClassConstructor) && !construct)
        return throwError(ErrorCode::TypeError, "class constructor " + (c->name ? toUtf8(static_cast<StringCell*>(c->name)->text()) : std::string()) +
                                                    " must be called with 'new'");
    if (c->flags & CodeBlock::kGenerator)
        return makeGenerator(f, thisValue, args, argc);
    JSValue* R = stackTop;
    if (frameTop >= frameLimit || R + std::max<std::size_t>(c->nregs, argc + 1) + 8 > stackEnd)
        return throwError(ErrorCode::StackOverflow, "Maximum call stack size exceeded");
    // `this` and the arguments become the first registers of the new frame, each with a count of its own.
    fillStack(R + std::max<std::size_t>(c->nregs, argc + 1) + 1);
    JSValue self = thisValue;
    if (!(c->flags & (CodeBlock::kStrict | CodeBlock::kArrow)) && self.isNullish())
        self = toJSValue(global);
    R[0] = self;
    for (std::uint32_t i = 0; i < argc; ++i)
        R[1 + i] = args[i];
    for (std::uint32_t i = 0; i <= argc; ++i)
        retainReg(*this, R[i]);
    const std::uint32_t keep = 1 + (argc < c->nparams ? argc : c->nparams);
    JSArray* argsArray = nullptr;
    if (c->flags & CodeBlock::kNeedsArgs) {
        stackTop = R + 1 + argc;  // what is above the top is nobody's
        argsArray = captureArgumentsFor(*this, c, R + 1, argc);
        stackTop = R;
    }
    // The callee's registers above its parameters start undefined. What can be there instead: an argument past the
    // parameters, and dead temporaries of a frame below when this one sits on them (a native called from a function
    // whose own frame began inside its caller's).
    for (std::uint32_t i = keep; i < c->nregs; ++i)
        if (!R[i].isUndefined())
            clearReg(*this, R[i]);
    if (argsArray)
        setReg(*this, R[1 + c->nparams], toJSValue(argsArray));
    JSValue* savedTop = stackTop;
    *frameTop++ = Frame{c, f, nullptr, R, 0, 0, argc, construct, true,
                                          construct ? (target.isUndefined() ? callee : target) : JSValue::undefined()};
    if (!c->tier1 && !c->tier1Failed && tier1Threshold && ++c->hotness >= tier1Threshold)
        compileTier1(*this, c);
    const JSValue result = c->tier1 ? enterTier1(*this, c->tier1, 0) : run(*this);
    stackTop = savedTop;
    // The frame let go of its own registers; `this` and the arguments are ours.
    clearRegs(*this, R, 0, 1 + argc);
    // Back to the host: what the program left behind goes now, while no frame is there to scan.
    if (deferred && frameTop == frameBase)
        scanAtHost();
    return result;
}

bool VM::ensureCompiled(CodeBlock* c)
{
    if (!(c->flags & CodeBlock::kLazy)) [[likely]]
        return true;
    try {
        compileLazy(*this, c);
    } catch (const Error& e) {
        throwError(e.code() == ErrorCode::CompileError ? ErrorCode::SyntaxError : e.code(), e.what());
        return false;
    }
    return true;
}

JSValue VM::makeGenerator(JSFunction* f, JSValue thisValue, const JSValue* args, std::uint32_t argc)
{
    CodeBlock* c = f->code;
    if ((c->flags & CodeBlock::kAsync) && !(c->flags & CodeBlock::kAsyncGen))
        return startAsync(f, thisValue, args, argc);
    // The generator's prototype is read once the parameters have been worked out, and falls back to its realm's.
    JSObject* fallback = (c->flags & CodeBlock::kAsyncGen) ? protos.asyncGenerator : protos.generator;
    JSValue self0 = thisValue;
    if (!(c->flags & (CodeBlock::kStrict | CodeBlock::kArrow)) && self0.isNullish())
        self0 = toJSValue(global);
    if (c->code[0].opcode() == Op::GenStart) {
        // Nothing runs before the body: the state it would stop in is made without a frame.
        JSObject* proto = fallback;
        Handle read;
        if (prototypeAt2(f) && isObject(*f->slotAt(2))) {
            proto = asObject(*f->slotAt(2));
        } else {
            read = Handle::adopt(getProperty(*this, toJSValue(f), atoms.prototype));
            if (read.get().isException())
                return read.get();
            if (isObject(read.get()))
                proto = asObject(read.get());
        }
        Handle keep = Handle::adopt(newGenerator(*this, f, c, proto));
        GenState* g = genStateOf(asObject(keep.get()));
        const std::uint16_t* live = c->suspendRegs.data() + c->suspendAt[1];
        const std::uint32_t n = *live++;
        for (std::uint32_t k = 0; k < n; ++k) {
            const std::uint32_t r = live[k];
            JSValue v = JSValue::undefined();
            if (r == 0)
                v = self0;
            else if (r <= c->nparams)
                v = r <= argc ? args[r - 1] : v;
            else if (r == 1u + c->nparams && (c->flags & CodeBlock::kNeedsArgs))
                v = toJSValue(captureArgumentsFor(*this, c, args, argc));
            if (r <= c->nparams)
                retain(v);
            g->regs()[k] = v;
            g->count = k + 1;
        }
        g->pcIndex = 1;
        if (c->flags & CodeBlock::kAsyncGen)
            return bi::asyncGeneratorStart(*this, keep.take(), proto);
        return keep.take();
    }
    Handle keep = Handle::adopt(newGenerator(*this, f, c, fallback));
    JSObject* o = asObject(keep.get());
    GenState* g = genStateOf(o);
    g->count = 1u + c->nparams + (c->flags & CodeBlock::kNeedsArgs ? 1 : 0);
    for (std::uint32_t i = 0; i < g->count; ++i)
        g->regs()[i] = JSValue::undefined();
    retain(self0);
    g->regs()[0] = self0;
    const std::uint32_t keep_ = argc < c->nparams ? argc : c->nparams;
    for (std::uint32_t i = 0; i < keep_; ++i) {
        retain(args[i]);
        g->regs()[1 + i] = args[i];
    }
    if (c->flags & CodeBlock::kNeedsArgs)
        g->regs()[1 + c->nparams] = toJSValue(captureArgumentsFor(*this, c, args, argc));
    // The parameters (their defaults may throw) and the declarations run now.
    g->prologue = true;
    JSValue ignored;
    bool finished;
    if (!resumeRaw(keep.get(), 0, JSValue::undefined(), &ignored, &finished))
        return JSValue::exception();
    release(ignored);
    JSValue protoValue = getProperty(*this, toJSValue(f), atoms.prototype);
    if (protoValue.isException())
        return protoValue;
    JSObject* proto = isObject(protoValue) ? asObject(protoValue) : fallback;
    if (proto != fallback)
        o->setProto(*this, proto);
    release(protoValue);
    if (c->flags & CodeBlock::kAsyncGen)
        return bi::asyncGeneratorStart(*this, keep.take(), proto);
    return keep.take();
}

/// A call of an async function. Its body runs in an ordinary frame, which becomes a generator if it comes to an `await`.
JSValue VM::startAsync(JSFunction* f, JSValue thisValue, const JSValue* args, std::uint32_t argc)
{
    CodeBlock* c = f->code;
    JSValue* R = stackTop;
    if (frameTop >= frameLimit || R + c->nregs + 8 > stackEnd || nativeStackLow()) {
        throwError(ErrorCode::StackOverflow, "Maximum call stack size exceeded");
        return bi::asyncStarted(*this, JSValue::undefined(), JSValue::exception());
    }
    fillStack(R + c->nregs + 1);
    JSValue self = thisValue;
    if (!(c->flags & (CodeBlock::kStrict | CodeBlock::kArrow)) && self.isNullish())
        self = toJSValue(global);
    JSArray* argsArray = (c->flags & CodeBlock::kNeedsArgs) ? captureArgumentsFor(*this, c, args, argc) : nullptr;
    const std::uint32_t n = argc < c->nparams ? argc : c->nparams;
    for (std::uint32_t i = 0; i < c->nregs; ++i)
        copyReg(*this, R[i], i == 0 ? self : i <= n ? args[i - 1] : JSValue::undefined());
    if (argsArray)
        setReg(*this, R[1 + c->nparams], toJSValue(argsArray));
    *frameTop++ = Frame{c, f, nullptr, R, 0, 0, argc, false, true, JSValue::undefined()};
    if (!c->tier1 && !c->tier1Failed && tier1Threshold && ++c->hotness >= tier1Threshold)
        compileTier1(*this, c);
    const JSValue result = c->tier1 ? enterTier1(*this, c->tier1, 0) : run(*this);
    stackTop = R;
    clearRegs(*this, R, 0, 1 + n);
    if (deferred && frameTop == frameBase)
        scanAtHost();
    const JSValue generator = becameGenerator;
    becameGenerator = JSValue::undefined();
    return bi::asyncStarted(*this, generator, result);
}

JSValue VM::resumeGenerator(JSValue generator, int mode, JSValue sent)
{
    JSValue value, raw = JSValue::undefined();
    bool done;
    if (!resumeRaw(generator, mode, sent, &value, &done, &raw))
        return JSValue::exception();
    if (isObject(raw))
        return raw;
    return bi::iterResult(*this, value, done);
}

bool VM::resumeRaw(JSValue generator, int mode, JSValue sent, JSValue* out, bool* done, JSValue* rawResult)
{
    GenState* g = nullptr;
    if (isObject(generator) && asObject(generator)->cls() == ObjClass::Generator)
        g = genStateOf(asObject(generator));
    if (!g) {
        throwError(ErrorCode::TypeError, "next method called on an incompatible receiver");
        return false;
    }
    if (g->state == GenState::Running) {
        throwError(ErrorCode::TypeError, "Generator is already running");
        return false;
    }
    Handle keepGenerator = Handle::retained(generator);
    Handle keepSent = Handle::retained(sent);
    if (g->state == GenState::Start && mode != 0) {
        g->state = GenState::Done;
        g->clear(*this);
    }
    if (g->state == GenState::Done) {
        if (mode == 1) {
            release(exception);
            exception = sent;
            retain(exception);
            exceptionCode = ErrorCode::Exception;
            return false;
        }
        const JSValue v = mode == 2 ? sent : JSValue::undefined();
        retain(v);
        *out = v;
        *done = true;
        return true;
    }
    auto* f = static_cast<JSFunction*>(asObject(g->callee));
    CodeBlock* c = f->code;
    JSValue* R = stackTop;
    if (frameTop >= frameLimit || R + c->nregs + 8 > stackEnd) {
        throwError(ErrorCode::StackOverflow, "Maximum call stack size exceeded");
        return false;
    }
    fillStack(R + c->nregs + 1);
    // (A deferred frame that goes on: only what is live there is read.)
    if (!deferred || !g->pcIndex)
        for (std::uint32_t i = 0; i < c->nregs; ++i) {
            const JSValue old = R[i];
            R[i] = JSValue::undefined();
            if (!deferred)
                release(old);
        }
    {
        // A deferred register has no count: the generator keeps the value with its own (`GenState::suspend`).
        const std::uint16_t* live = g->pcIndex ? c->suspendRegs.data() + c->suspendAt[g->pcIndex] + 1 : nullptr;
        for (std::uint32_t k = 0; k < g->count; ++k) {
            R[live ? live[k] : k] = g->regs()[k];
            if (!deferred)
                g->regs()[k] = JSValue::undefined();
        }
        if (!deferred)
            g->count = 0;
    }
    if (g->state == GenState::Suspended && g->delegating) {
        // At a `yield*`: what came in goes to the inner iterator, with what kind of resumption it was.
        setReg(*this, R[g->resumeDst], sent);
        retain(sent);
        setReg(*this, R[g->resumeDst + 1], JSValue::int32(mode));
    } else if (g->state == GenState::Suspended) {
        if (mode == 0) {
            setReg(*this, R[g->resumeDst], sent);
            retain(sent);
        } else if (mode == 1) {
            release(exception);
            exception = sent;
            retain(exception);
            exceptionCode = ErrorCode::Exception;
            resumeThrow = true;
        } else {
            // `return(v)`: a throw only `finally` blocks see; what it ends in is `v`.
            release(exception);
            exception = returnMarker;
            retain(exception);
            exceptionCode = ErrorCode::Exception;
            release(returnValue);
            returnValue = sent;
            retain(returnValue);
            resumeThrow = true;
        }
    }
    g->state = GenState::Running;
    JSValue* savedTop = stackTop;
    *frameTop++ = Frame{c, f, c->code.data() + g->pcIndex, R, 0, 0, 0, false, true, JSValue::undefined(), generator};
    // A body resumed often runs in compiled code from where it stopped (a `yield` or `await` there is one instruction of
    // the interpreter's, which leaves the frame as it does). A resumption that throws starts in the interpreter.
    if (!c->tier1 && !c->tier1Failed && tier1Threshold && ++c->hotness >= tier1Threshold)
        compileTier1(*this, c);
    const JSValue result = c->tier1 && !resumeThrow ? enterTier1(*this, c->tier1, g->pcIndex) : run(*this);
    stackTop = savedTop;
    if (deferred && frameTop == frameBase)
        scanAtHost();
    const bool finished = g->state == GenState::Running;
    g->prologue = false;
    if (finished) {
        g->state = GenState::Done;
        g->clear(*this);
        clearRegs(*this, R, 0, 1);
    }
    if (result.isException()) {
        if (finished && exception == returnMarker) {
            clearException();
            *out = returnValue;
            returnValue = JSValue::undefined();
            *done = true;
            return true;
        }
        return false;
    }
    if (g->raw && !finished) {
        // What a `yield*` passes on is the inner iterator's own result object.
        g->raw = false;
        if (rawResult) {
            *rawResult = result;
            *out = JSValue::undefined();
            *done = false;
            return true;
        }
        JSValue v = getProperty(*this, result, atoms.value);
        release(result);
        if (v.isException())
            return false;
        *out = v;
        *done = false;
        return true;
    }
    g->raw = false;
    *out = result;
    *done = finished;
    return true;
}

JSValue VM::construct(JSValue callee, const JSValue* args, std::uint32_t argc, JSValue target)
{
    if (!isFunction(callee))
        return throwError(ErrorCode::TypeError, describe(*this, callee) + " is not a constructor");
    auto* f = static_cast<JSFunction*>(asObject(callee));
    const bool canConstruct = (f->flags & JSFunction::kNative) ? (f->flags & JSFunction::kConstructor) != 0 : f->code->constructor;
    if (!canConstruct)
        return throwError(ErrorCode::TypeError, describe(*this, callee) + " is not a constructor");
    JSValue protoValue = getProperty(*this, target.isUndefined() ? callee : target, atoms.prototype);
    if (protoValue.isException())
        return protoValue;
    JSObject* proto = isObject(protoValue) ? asObject(protoValue) : protos.object;
    if (!isObject(protoValue) && (f->flags & JSFunction::kNative)) {
        // GetPrototypeFromConstructor: without a usable `prototype` on new.target the constructor's own is what the object gets.
        JSValue own = getProperty(*this, callee, atoms.prototype);
        if (own.isException())
            return own;
        if (isObject(own))
            proto = asObject(own);
        release(own);
    }
    Handle made = Handle::adopt(toJSValue(newObject(proto)));
    release(protoValue);
    JSValue result;
    if (f->flags & JSFunction::kNative) {
        Rare* r = f->rare();
        const JSValue saved = newTarget;
        newTarget = target.isUndefined() ? callee : target;
        result = r->native(*this, made.get(), args, argc, r->nativeData);
        newTarget = saved;
    } else {
        result = callInternal(callee, made.get(), args, argc, true, target);
    }
    if (result.isException() || isObject(result))
        return result;
    release(result);
    return made.take();
}

}  // namespace vm
}  // namespace internal
}  // namespace script
