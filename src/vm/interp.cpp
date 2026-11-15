// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Tier 0: the interpreter. One loop over the register bytecode with computed-goto dispatch, calls between bytecode
// functions made inside it (a new frame on the VM stack, no C++ recursion), and one inline cache per property
// instruction. Registers own their values: every write releases what was there.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "script/internal/dyn/object.hpp"
#include "script/internal/stack_guard.hpp"
#include "script/internal/vm/census.hpp"
#include "script/internal/vm/convert.hpp"
#include "script/internal/vm/jit.hpp"
#include "script/internal/vm/object.hpp"
#include "script/internal/vm/operators.hpp"
#include "script/internal/vm/typed.hpp"
#include "builtins_util.hpp"

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

[[gnu::always_inline]] inline void setReg(JSValue& slot, JSValue value) noexcept
{
    const JSValue old = slot;
    slot = value;
    release(old);
}

[[gnu::always_inline]] inline void copyReg(JSValue& slot, JSValue value) noexcept
{
    retain(value);
    setReg(slot, value);
}

/// Let go of registers [from, to) and leave undefined in them.
inline void clearRegs(JSValue* R, std::uint32_t from, std::uint32_t to) noexcept
{
    for (std::uint32_t i = from; i < to; ++i) {
        const JSValue v = R[i];
        R[i] = JSValue::undefined();
        release(v);
    }
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
    std::vector<Key> seen;
    auto add = [&](Key k) {
        if (std::find(seen.begin(), seen.end(), k) != seen.end())
            return;
        seen.push_back(k);
        retain(JSValue::cell(k));
        result->append(vm, JSValue::cell(k));
    };
    auto visit = [&](JSObject* o) {
        std::vector<std::uint32_t> indexes;
        std::vector<Key> named;
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
            if (!(attrs & attr::kEnumerable) || k->kind != detail::CellKind::String)
                return;
            std::uint32_t index;
            if (arrayIndexOf(k, &index))
                indexes.push_back(index);
            else
                named.push_back(k);
        });
        std::sort(indexes.begin(), indexes.end());
        indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
        for (std::uint32_t index : indexes) {
            Key k = toPropertyKey(vm, JSValue::boxNumber(double(index)));
            add(k);
            release(JSValue::cell(k));
        }
        for (Key k : named)
            add(k);
    };
    if (isObject(value)) {
        Handle cur = Handle::retained(value);
        while (isObject(cur.get())) {
            JSObject* o = asObject(cur.get());
            if (proxyOf(o)) {
                Vec<Handle> names;
                if (!bi::objOwnKeysFiltered(vm, o, true, false, true, names))
                    return JSValue::exception();
                for (Handle& name : names)
                    add(static_cast<Key>(name.get().asCell()));
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

/// Whether `o` may hand out the position of a property: in the shape tree, and not a function whose properties are not
/// made yet.
inline bool cacheable(VM& vm, JSObject* o) noexcept
{
    return !o->isDictionary(vm) && !(o->cls() == ObjClass::Function && (static_cast<JSFunction*>(o)->flags & JSFunction::kLazyProps)) &&
           !proxyOf(o);
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
    if (!cacheable(vm, o))
        return false;
    std::uint8_t attrs = 0;
    std::int32_t index = o->shapeOf(vm)->indexOf(key, &attrs);
    if (index >= 0) {
        if (attrs & attr::kAccessor)
            return false;
        clearValidity(vm, ic);
        ic.shape = o->h.shape;
        ic.index = std::uint32_t(index);
        ic.next = 0;
        return true;
    }
    // A property of a prototype: the cache holds the holder and a cell that says the chain has not changed.
    for (JSObject* p = o->proto(vm); p; p = p->proto(vm)) {
        if (!cacheable(vm, p))
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

/// Remember where a read of `key` on `o` finds it in the site's `PolyCache`: a data property or an accessor, own or inherited.
bool learnPoly(VM& vm, CodeBlock* code, InlineCache& ic, JSObject* o, Key key)
{
    if (!cacheable(vm, o))
        return false;
    // A typed array answers a canonical numeric name itself, whatever its prototypes have.
    double numeric;
    const bool numericKey = canonicalNumericIndex(key, &numeric);
    if (numericKey && o->cls() == ObjClass::TypedArray)
        return false;
    PolyEntry entry;
    std::uint8_t attrs = 0;
    std::int32_t index = o->shapeOf(vm)->indexOf(key, &attrs);
    bool inherited = false;
    JSObject* holder = nullptr;
    if (index < 0) {
        for (JSObject* p = o->proto(vm); p; p = p->proto(vm)) {
            if (!cacheable(vm, p) || (numericKey && p->cls() == ObjClass::TypedArray))
                return false;
            index = p->shapeOf(vm)->indexOf(key, &attrs);
            if (index >= 0) {
                holder = p;
                inherited = true;
                break;
            }
        }
        if (!inherited)
            return false;
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
    if (!ic.pad) {
        code->poly.emplace_back();
        ic.pad = std::uint32_t(code->poly.size());
    }
    PolyCache& pc = code->poly[ic.pad - 1];
    std::uint8_t at = pc.n;
    for (std::uint8_t i = 0; i < pc.n; ++i)
        if (pc.e[i].shape == entry.shape)
            at = i;
    if (at == pc.n) {
        if (pc.n < 4) {
            ++pc.n;
        } else {
            at = pc.next;
            pc.next = std::uint8_t((pc.next + 1) & 3);
        }
    }
    if (pc.e[at].validity)
        vm.releaseValidity(pc.e[at].validity);
    pc.e[at] = entry;
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
int readCachedSlow(VM& vm, CodeBlock* code, InlineCache& ic, JSObject* o, JSValue base, Key key, JSValue* out)
{
    int r = readPoly(vm, code, ic, o, base, out);
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

/// A store to an own writable data property.
void learnOwnWrite(VM& vm, InlineCache& ic, JSObject* o, Key key)
{
    if (!cacheable(vm, o) || hasElems(o))
        return;
    std::uint8_t attrs = 0;
    const std::int32_t index = o->shapeOf(vm)->indexOf(key, &attrs);
    if (index < 0 || !(attrs & attr::kWritable) || (attrs & attr::kAccessor))
        return;
    clearValidity(vm, ic);
    ic.shape = o->h.shape;
    ic.index = std::uint32_t(index);
    ic.next = 0;
}

/// A store (or a literal's property) that added `key` to an object whose shape was `before`: remember the shape after.
/// A store that goes through the prototypes also holds the validity of the chain, since a setter or a read-only property
/// added to one of them would change what the store does.
void learnAdd(VM& vm, InlineCache& ic, JSObject* o, std::uint32_t before, Key key, bool throughChain)
{
    if (o->h.shape == before || !cacheable(vm, o) || hasElems(o) || o->isProto())
        return;
    Shape* now = o->shapeOf(vm);
    Shape* was = vm.shapeById(before);
    if (!was || now->parent != was || now->key != key || now->attrs != attr::kDefault || was->count >= o->h.aux)
        return;
    clearValidity(vm, ic);
    if (throughChain) {
        if (JSObject* p = o->proto(vm)) {
            ValidityCell* cell = vm.validityOf(p);
            vm.retainValidity(cell);
            ic.validity = cell;
        }
    }
    ic.shape = before;
    ic.next = o->h.shape;
    ic.index = was->count;
    ic.holder = nullptr;
}


/// The call's arguments, copied into an array for `arguments` and rest parameters.
JSArray* captureArguments(VM& vm, const JSValue* args, std::uint32_t argc)
{
    JSArray* all = vm.newArray(vm.protos.arguments, 0, argc);
    all->h.cls = std::uint8_t(ObjClass::Arguments);
    all->appendAll(vm, args, argc);
    return all;
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
    std::vector<JSValue> regs;
    JSValue callee = JSValue::undefined();
    std::uint32_t pcIndex = 0;
    std::uint16_t resumeDst = 0;
    State state = Start;
    bool prologue = false;  ///< running the parameters and declarations, up to `GenStart`
    bool delegating = false;  ///< suspended at a `yield*`: a resumption is handed to the inner iterator
    bool raw = false;         ///< what it yielded is an inner iterator result, to be given out as it is

    ~GenState() override { clear_(); }
    void values(Vec<JSValue>& out) const override
    {
        out.push_back(callee);
        out.insert(out.end(), regs.begin(), regs.end());
    }
    void clear(VM&) noexcept override { clear_(); }
    void clear_() noexcept
    {
        for (JSValue& v : regs) {
            release(v);
            v = JSValue::undefined();
        }
        release(callee);
        callee = JSValue::undefined();
    }
};

using bi::iteratorStep;

/// The interpreter. With `Step`, it runs the top frame's one instruction (and whatever that calls) and then returns
/// `kStepFall` or `kStepJump` instead of going on; see jit.hpp.
template <bool Step>
JSValue runImpl(VM& vm, const Ins* stepFall)
{
    static const void* const kLabels[] = {
#define OP(name, kinds) &&L_##name,
#include "script/internal/vm/opcodes.def"
#undef OP
    };

    Frame* fr = vm.frameBase + vm.frameCount - 1;
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
        if constexpr (!Step) {                                                           \
            if (vm.tier1Threshold && !code->tier1Failed && ++code->hotness >= kOsrAfter) [[unlikely]] { \
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
                    fr = &vm.frameBase[vm.frameCount - 1];                               \
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
#define TAKE16()                  \
    do {                          \
        const int o_ = ins->jump16(); \
        pc += o_;                 \
        if (o_ < 0)               \
            LOOP_TICK();          \
    } while (0)
#define SET(r, v) setReg(R[r], v)
/// SET and COPY that also note what kind of value the instruction made (the baseline compiler's type feedback).
#define SETP(r, v)                                                         \
    do {                                                                    \
        const JSValue pv_ = (v);                                            \
        code->prof[std::size_t(ins - codeStart)] |= profBit(pv_);           \
        setReg(R[r], pv_);                                                  \
    } while (0)
#define COPYP(r, v)                                                         \
    do {                                                                    \
        const JSValue pv_ = (v);                                            \
        code->prof[std::size_t(ins - codeStart)] |= profBit(pv_);           \
        copyReg(R[r], pv_);                                                 \
    } while (0)
#define COPY(r, v) copyReg(R[r], v)
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
    COPY(ins->a, K[ins->b]);
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
        if (cacheId != kNoCache && cacheable(vm, g)) {
            ic.shape = g->h.shape;
            ic.index = std::uint32_t(g->shapeOf(vm)->indexOf(key));
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
        retain(*slot);
        release(old);
        ++pc;
        NEXT();
    }
    std::uint8_t attrs = 0;
    JSValue* slot = g->findOwn(vm, key, &attrs);
    if (slot && (attrs & attr::kWritable) && !(attrs & attr::kAccessor)) {
        if (cacheId != kNoCache && cacheable(vm, g)) {
            ic.shape = g->h.shape;
            ic.index = std::uint32_t(g->shapeOf(vm)->indexOf(key));
        }
        const JSValue old = *slot;
        *slot = R[ins->b];
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
    Box* box = vm.newBox(R[ins->a]);  // takes the register's count
    R[ins->a] = JSValue::cell(reinterpret_cast<CellPtr>(box));
    NEXT();
}
L_GetBox:
    COPYP(ins->a, reinterpret_cast<Box*>(R[ins->b].asCell())->value);
    NEXT();
L_PutBox: {
    Box* box = reinterpret_cast<Box*>(R[ins->a].asCell());
    const JSValue old = box->value;
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
    box->value = R[ins->b];
    retain(box->value);
    release(old);
    NEXT();
}
L_LoadCallee:
    COPY(ins->a, toJSValue(fn));
    NEXT();

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
    if (ins->a == ins->b && isString(a) && appendInPlace(vm, R[ins->a], b))
        NEXT();
    fr->pc = pc;
    const JSValue r = add(vm, a, b);
    if (r.isException())
        FAIL();
    SETP(ins->a, r);
    NEXT();
}
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
            const std::int32_t x = a.asInt32(), y = b.asInt32();                  \
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
        pc += ins->jump();
    NEXT();
}
L_JNullish:
    if (R[ins->a].isNullish())
        pc += ins->jump();
    NEXT();
L_JNotNullish:
    if (!R[ins->a].isNullish())
        pc += ins->jump();
    NEXT();
L_JUndefined:
    if (R[ins->a].isUndefined())
        pc += ins->jump();
    NEXT();
L_JNotUndefined:
    if (!R[ins->a].isUndefined())
        pc += ins->jump();
    NEXT();
L_JHole:
    if (R[ins->a].isHole())
        pc += ins->jump();
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
            if ((!ic.validity || ic.validity->valid) && !(o->h.flags & (cellflags::kIsProto | cellflags::kNotExtensible))) {
                JSValue* slot = &o->inl()[ic.index];
                *slot = R[ins->c];
                retain(*slot);
                o->h.shape = ic.next;
                ++pc;
                NEXT();
            }
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
                learnOwnWrite(vm, ic, o, key);
            else
                learnAdd(vm, ic, o, before, key, true);
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
        if (isObject(base) && asObject(base)->cls() == ObjClass::Array) {
            auto* a = static_cast<JSArray*>(asObject(base));
            const std::uint32_t i = std::uint32_t(k.asInt32());
            if (i < a->length && !a->elems[i].isHole()) {
                COPYP(ins->a, a->elems[i]);
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
        vm.throwError(ErrorCode::TypeError, "Cannot read properties of " + std::string(base.isNull() ? "null" : "undefined"));
        FAIL();
    }
    if (isPrivateKey(k)) [[unlikely]] {
        const JSValue v = privateGet(vm, base, k);
        if (v.isException())
            FAIL();
        SETP(ins->a, v);
        NEXT();
    }
    JSValue prim = toPrimitive(vm, k, true);
    if (prim.isException())
        FAIL();
    Key key = toPropertyKey(vm, prim);
    release(prim);
    if (!key)
        FAIL();
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
            vm.protos.array->proto(vm) == vm.protos.object) {
            retain(R[ins->c]);
            a->elems[i] = R[ins->c];
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
        JSValue* slot = &o->inl()[ic.index];
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
                const JSValue old = R[it + 1];
                R[it + 1] = JSValue::undefined();
                release(old);
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
    SET(ins->a, fKeep.take());
    NEXT();
}
L_DefMethod:
L_DefGetter:
L_DefSetter: {
    JSObject* target = asObject(R[ins->a]);
    Key key = atoms[ins->b];
    JSValue fnv = R[ins->c];
    auto* fn = static_cast<JSFunction*>(asObject(fnv));
    Rare& rr = fn->rareOf(vm);
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
    auto* fn = static_cast<JSFunction*>(asObject(fnv));
    setFunctionName(vm, fnv, key, ins->opcode() == Op::DefMethodElem ? "" : ins->opcode() == Op::DefGetterElem ? "get " : "set ");
    Rare& rr = fn->rareOf(vm);
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
    vm.evalFrames = vm.frameCount;
    NEXT();
L_GetIteratorObj: {
    const JSValue src = R[ins->b];
    fr->pc = pc;
    if (src.isNullish()) {
        vm.throwError(ErrorCode::TypeError, describe(vm, src) + " is not iterable");
        FAIL();
    }
    JSValue method = getProperty(vm, src, vm.atoms.symIterator);
    if (method.isException())
        FAIL();
    if (!isFunction(method)) {
        release(method);
        vm.throwError(ErrorCode::TypeError, describe(vm, src) + " is not iterable");
        FAIL();
    }
    JSValue iterator = vm.call(method, src, nullptr, 0);
    release(method);
    if (iterator.isException())
        FAIL();
    if (!isObject(iterator)) {
        release(iterator);
        vm.throwError(ErrorCode::TypeError, "Result of the Symbol.iterator method is not an object");
        FAIL();
    }
    JSValue next = getProperty(vm, iterator, vm.atoms.next);
    if (next.isException()) {
        release(iterator);
        FAIL();
    }
    setReg(R[ins->a + 1], next);
    setReg(R[ins->a], iterator);
    NEXT();
}
L_DelegateCallAsync:
L_DelegateCall: {
    // One step of `yield*`: what the generator was resumed with goes to the inner iterator's next, throw or return.
    // In an async generator the results are promises, awaited by the code that follows.
    const bool asyncDelegate = ins->opcode() == Op::DelegateCallAsync;
    fr->pc = pc;
    const JSValue iterator = R[ins->b], nextFn = R[ins->b + 1];
    const int mode = R[ins->c + 1].asInt32();
    JSValue sent = R[ins->c];
    JSValue res;
    if (mode == 0) {
        if (!isFunction(nextFn)) {
            vm.throwError(ErrorCode::TypeError, describe(vm, nextFn) + " is not a function");
            FAIL();
        }
        res = vm.call(nextFn, iterator, &sent, 1);
    } else {
        Key k = vm.internKey(std::string_view(mode == 1 ? "throw" : "return"));
        JSValue m = getProperty(vm, iterator, k);
        detail::decRef(k);
        if (m.isException())
            FAIL();
        if (m.isNullish()) {
            release(m);
            if (mode == 2) {
                // No `return` there: the generator returns what it was given.
                SET(ins->a, JSValue::hole());
                NEXT();
            }
            // No `throw`: the iterator is closed, and that is a TypeError.
            Key rk = vm.internKey(std::string_view("return"));
            JSValue closer = getProperty(vm, iterator, rk);
            detail::decRef(rk);
            if (closer.isException())
                FAIL();
            if (!closer.isNullish()) {
                JSValue r = isFunction(closer) ? vm.call(closer, iterator, nullptr, 0) : JSValue::undefined();
                release(closer);
                if (r.isException())
                    FAIL();
                if (asyncDelegate) {
                    // What `return` gives is awaited first; the code after the await raises the TypeError (mode 3).
                    setReg(R[ins->c + 1], JSValue::int32(3));
                    SET(ins->a, r);
                    NEXT();
                }
                release(r);
            } else {
                release(closer);
            }
            vm.throwError(ErrorCode::TypeError, "The iterator does not provide a 'throw' method");
            FAIL();
        }
        if (!isFunction(m)) {
            release(m);
            vm.throwError(ErrorCode::TypeError, "iterator method is not a function");
            FAIL();
        }
        res = vm.call(m, iterator, &sent, 1);
        release(m);
    }
    if (res.isException())
        FAIL();
    if (!asyncDelegate && !isObject(res)) {
        const std::string text = describe(vm, res);
        release(res);
        vm.throwError(ErrorCode::TypeError, "Iterator result " + text + " is not an object");
        FAIL();
    }
    SET(ins->a, res);
    NEXT();
}
L_DelegateDone: {
    fr->pc = pc;
    if (!isObject(R[ins->b])) {
        vm.throwError(ErrorCode::TypeError, "Iterator result " + describe(vm, R[ins->b]) + " is not an object");
        FAIL();
    }
    JSValue d = getProperty(vm, R[ins->b], vm.atoms.done);
    if (d.isException())
        FAIL();
    const bool done = toBoolean(d);
    release(d);
    SET(ins->a, JSValue::boolean(done));
    NEXT();
}
L_GenStart: {
    // The end of what a call of a generator function does: it stands at the start of the body until resumed.
    auto* g = static_cast<GenState*>(asObject(fr->gen)->rare()->ext);
    if (!g->prologue)
        NEXT();
    g->prologue = false;
    g->pcIndex = std::uint32_t(pc - codeStart);
    g->state = GenState::Start;
    for (std::uint32_t i = 0; i < code->nregs; ++i) {
        g->regs[i] = R[i];
        R[i] = JSValue::undefined();
    }
    --vm.frameCount;
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
        setReg(R[ins->a], toJSValue(vm.newObject(vm.protos.object)));
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
    std::vector<JSValue> items(list->elems, list->elems + n);
    for (JSValue& v : items) {
        if (v.isHole())
            v = JSValue::undefined();
        retain(v);
    }
    vm.fillStack(window + n + 1);
    for (std::uint32_t i = 0; i < n; ++i)
        setReg(window[i], items[i]);
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
    if (isNew) {
        const bool canConstruct = (f->flags & JSFunction::kNative) ? (f->flags & JSFunction::kConstructor) != 0
                                                                     : f->code->constructor;
        if (!canConstruct) {
            vm.throwError(ErrorCode::TypeError, describe(vm, callee) + " is not a constructor");
            FAIL();
        }
        JSObject* made;
        if (f->h.shape == vm.fnProtoShape) [[likely]] {
            // The shape every ordinary function has once its `prototype` is made: `length`, `name`, `prototype`.
            const JSValue pv = *f->slotAt(2);
            made = vm.newObject(isObject(pv) ? asObject(pv) : vm.protos.object);
        } else {
            JSValue protoValue = getProperty(vm, callee, vm.atoms.prototype);
            if (protoValue.isException())
                FAIL();
            JSObject* proto = isObject(protoValue) ? asObject(protoValue) : vm.protos.object;
            made = vm.newObject(proto);
            release(protoValue);
            if (!(f->flags & JSFunction::kNative) && !f->isDictionary(vm) && f->shapeOf(vm)->indexOf(vm.atoms.prototype) == 2)
                vm.fnProtoShape = f->h.shape;
        }
        setReg(R[base + 1], toJSValue(made));
    }
    if (f->flags & JSFunction::kNative) {
        Rare* r = f->rare();
        vm.stackTop = R + code->nregs;
        JSValue saved = vm.newTarget;
        vm.newTarget = isNew ? callee : isSuper ? fr->newTarget : JSValue::undefined();
        JSValue res = r->native(vm, R[base + 1], &R[base + 2], argc, r->nativeData);
        vm.newTarget = saved;
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
    if (!c->tier1 && !c->tier1Failed && vm.tier1Threshold && ++c->hotness >= vm.tier1Threshold)
        compileTier1(vm, c);
    JSValue* newR = R + base + 1;    if (c->prof.empty()) [[unlikely]]
        c->prof.assign(c->code.size() + c->nparams + 2, 0);
    if (!c->tier1) {
        // What the arguments are, for the compiler of this function.
        const std::size_t at = c->code.size();
        for (std::uint32_t k = 0; k < c->nparams; ++k)
            c->prof[at + k] |= k < argc ? profBit(R[base + 2 + k]) : std::uint8_t(8);
    }

    if (vm.frameCount + 2 >= vm.frameCapacity || newR + c->nregs + 8 > vm.stackEnd || (c->tier1 && nativeStackLow())) [[unlikely]] {
        vm.throwError(ErrorCode::StackOverflow, "Maximum call stack size exceeded");
        FAIL();
    }
    if (!(c->flags & (CodeBlock::kStrict | CodeBlock::kArrow)) && newR[0].isNullish()) {
        copyReg(newR[0], toJSValue(vm.global));
    }
    const std::uint32_t keep = 1 + (argc < c->nparams ? argc : c->nparams);
    JSArray* argsArray = (c->flags & CodeBlock::kNeedsArgs) ? captureArguments(vm, newR + 1, argc) : nullptr;
    // Past what is initialised the registers are fresh; below it, what this frame sits on top of may be the caller's dead
    // temporaries or arguments the callee has no parameter for, which let go of their values now.
    if (newR + c->nregs > vm.stackInit) [[unlikely]]
        vm.fillStack(newR + c->nregs);
    {
        JSValue* callerEnd = R + code->nregs;
        const std::uint32_t overlap = callerEnd > newR ? std::uint32_t(std::min<std::ptrdiff_t>(callerEnd - newR, c->nregs)) : 0;
        for (std::uint32_t i = keep; i < overlap; ++i) {
            const JSValue v = newR[i];
            newR[i] = JSValue::undefined();
            release(v);
        }
        // Arguments past the parameters, if they sit beyond the caller's own registers.
        for (std::uint32_t i = std::max(keep, overlap); i < 1 + argc && i < c->nregs; ++i) {
            const JSValue v = newR[i];
            newR[i] = JSValue::undefined();
            release(v);
        }
    }
    if (argsArray) {
        // The array goes in the register after the parameters, which the clearing above left empty.
        if (newR + 1 + c->nparams >= vm.stackInit)
            vm.fillStack(newR + 2 + c->nparams);
        setReg(newR[1 + c->nparams], toJSValue(argsArray));
    }
    vm.frameBase[vm.frameCount++] = Frame{c, f, nullptr, newR, std::uint16_t(ins->a), argc, isNew || isSuper, false,
                                          isNew ? callee : isSuper ? fr->newTarget : JSValue::undefined()};
    if (c->tier1) {
        // Compiled code runs on the machine stack: the frame is its own entry, and the result comes back here.
        vm.frameBase[vm.frameCount - 1].entry = true;
        const JSValue res = enterTier1(vm, c->tier1, 0);
        vm.stackTop = R + code->nregs;
        if (res.isException())
            FAIL();
        SETP(ins->a, res);
        NEXT();
    }
    fr = &vm.frameBase[vm.frameCount - 1];
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
    clearRegs(R, keep, code->nregs);
    const bool entry = fr->entry;
    const std::uint16_t dst = fr->retDst;
    JSValue* calleeR = R;
    --vm.frameCount;
    if (entry) {
        (void)calleeR;
        return result;
    }
    fr = &vm.frameBase[vm.frameCount - 1];
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
        if (src.isNullish()) {
            vm.throwError(ErrorCode::TypeError, describe(vm, src) + " is not iterable");
            FAIL();
        }
        JSValue method = getProperty(vm, src, vm.atoms.symIterator);
        if (method.isException())
            FAIL();
        if (!isFunction(method)) {
            release(method);
            vm.throwError(ErrorCode::TypeError, describe(vm, src) + " is not iterable");
            FAIL();
        }
        JSValue iterator = vm.call(method, src, nullptr, 0);
        release(method);
        if (iterator.isException())
            FAIL();
        if (!isObject(iterator)) {
            release(iterator);
            vm.throwError(ErrorCode::TypeError, "Result of the Symbol.iterator method is not an object");
            FAIL();
        }
        JSValue next = getProperty(vm, iterator, vm.atoms.next);
        if (next.isException()) {
            release(iterator);
            FAIL();
        }
        // Registers a and a+1: the iterator and its `next`; the pair is an index for an array or a string.
        setReg(R[ins->a + 1], next);
        setReg(R[ins->a], iterator);
        NEXT();
    }
    // Registers a and a+1: the sequence and the next index.
    const JSValue keep = src;
    retain(keep);
    setReg(R[ins->a + 1], JSValue::int32(0));
    setReg(R[ins->a], keep);
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
            const JSValue old = R[it + 1];
            R[it + 1] = JSValue::undefined();
            release(old);
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
    JSValue r = bi::newRegExp(vm, asString(K[ins->b])->text(), asString(K[ins->c])->text());
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
    setReg(R[ins->a + 1], next);
    setReg(R[ins->a], iterator);
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
        const JSValue old = R[it + 1];
        R[it + 1] = JSValue::undefined();
        release(old);
    }
    fr->pc = pc;
    Key returnKey = vm.internKey(std::string_view("return"));
    JSValue f = getProperty(vm, iterator, returnKey);
    detail::decRef(returnKey);
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
L_Yield: {
    vm.suspendedOnAwait = ins->opcode() == Op::Await;
    auto* g = static_cast<GenState*>(asObject(fr->gen)->rare()->ext);
    g->delegating = ins->opcode() == Op::YieldDelegate;
    g->raw = g->delegating && ins->c == 0;  // an async generator's `yield*` yields the value, not the inner result
    JSValue out = R[ins->b];
    retain(out);
    g->pcIndex = std::uint32_t(pc - codeStart);
    g->resumeDst = ins->a;
    g->state = GenState::Suspended;
    for (std::uint32_t i = 0; i < code->nregs; ++i) {
        g->regs[i] = R[i];
        R[i] = JSValue::undefined();
    }
    --vm.frameCount;
    return out;
}

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
            setReg(R[found->reg], thrown);
            pc = codeStart + found->target;
            NEXT();
        }
        const std::uint32_t keep = 1 + (fr->argc < code->nparams ? fr->argc : code->nparams);
        clearRegs(R, keep, code->nregs);
        const bool entry = fr->entry;
        --vm.frameCount;
        if (entry)
            return JSValue::exception();
        fr = &vm.frameBase[vm.frameCount - 1];
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
    return runImpl<false>(vm, nullptr);
}

}  // namespace

std::uint64_t stepInstruction(VM& vm, std::uint32_t index)
{
    Frame* fr = vm.frameBase + vm.frameCount - 1;
    const Ins* at = fr->code->code.data() + index;
    const Ins* fall = at + ((index + 1 < fr->code->code.size() && at[1].opcode() == Op::Cache) ? 2 : 1);
    fr->pc = at;
    return runImpl<true>(vm, fall).bits;
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

inline JitCtx jitCtx(VM& vm, std::uint32_t index, bool hasCache)
{
    Frame* fr = vm.frameBase + vm.frameCount - 1;
    CodeBlock* code = fr->code;
    const Ins* ins = code->code.data() + index;
    fr->pc = ins + (hasCache ? 2 : 1);
    vm.stackTop = fr->R + code->nregs;
    return JitCtx{fr, code, ins, fr->R, code->atoms.data(), hasCache ? ins[1].a : std::uint16_t(kNoCache)};
}
}  // namespace

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
        if (c.cacheId != kNoCache && cacheable(vm, g)) {
            ic.shape = g->h.shape;
            ic.index = std::uint32_t(g->shapeOf(vm)->indexOf(key));
        }
        copyReg(c.R[ins->a], *slot);
        return 1;
    }
    if (!hasProperty(vm, toJSValue(g), key)) {
        if (ins->opcode() == Op::GetGlobalOrUndef) {
            setReg(c.R[ins->a], JSValue::undefined());
            return 1;
        }
        vm.throwError(ErrorCode::ReferenceError, toUtf8(static_cast<StringCell*>(key)->text()) + " is not defined");
        return 0;
    }
    JSValue v = getProperty(vm, toJSValue(g), key);
    if (v.isException())
        return 0;
    setReg(c.R[ins->a], v);
    return 1;
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
                copyReg(R[ins->a], *o->slotAt(ic.index));
                return 1;
            }
            if (ic.validity->valid) {
                copyReg(R[ins->a], *ic.holder->slotAt(ic.index));
                return 1;
            }
        }
        if (key == vm.atoms.length && hasElems(o)) {
            const std::uint32_t n = static_cast<JSArray*>(o)->length;
            setReg(R[ins->a], n <= 0x7FFFFFFFu ? JSValue::int32(std::int32_t(n)) : JSValue::boxDouble(double(n)));
            return 1;
        }
        if (key == vm.atoms.length && o->cls() == ObjClass::TypedArray) {
            const std::uint32_t n = static_cast<JSTypedArray*>(o)->length;
            setReg(R[ins->a], n <= 0x7FFFFFFFu ? JSValue::int32(std::int32_t(n)) : JSValue::boxDouble(double(n)));
            return 1;
        }
        if (c.cacheId != kNoCache) {
            JSValue got;
            const int r = readCachedSlow(vm, c.code, ic, o, base, key, &got);
            if (r < 0)
                return 0;
            if (r > 0) {
                setReg(R[ins->a], got);
                return 1;
            }
        }
    } else if (isString(base) && key == vm.atoms.length) {
        setReg(R[ins->a], JSValue::int32(std::int32_t(asString(base)->length)));
        return 1;
    }
    const JSValue v = getProperty(vm, base, key);
    if (v.isException())
        return 0;
    setReg(R[ins->a], v);
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
            if ((!ic.validity || ic.validity->valid) && !(o->h.flags & (cellflags::kIsProto | cellflags::kNotExtensible))) {
                JSValue* slot = &o->inl()[ic.index];
                *slot = R[ins->c];
                retain(*slot);
                o->h.shape = ic.next;
                return 1;
            }
        }
        const std::uint32_t before = o->h.shape;
        if (!setProperty(vm, base, key, R[ins->c], strict)) {
            if (!vm.exception.isUndefined())
                return 0;
            return 1;
        }
        if (c.cacheId != kNoCache) {
            if (o->h.shape == before)
                learnOwnWrite(vm, ic, o, key);
            else
                learnAdd(vm, ic, o, before, key, true);
        }
        return 1;
    }
    if (!setProperty(vm, base, key, R[ins->c], strict) && !vm.exception.isUndefined())
        return 0;
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
        JSValue* slot = &o->inl()[ic.index];
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
    setReg(c.R[c.ins->a], toJSValue(o));
    return 1;
}

int jitNewArray(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    JSArray* a = vm.newArray(vm.protos.array, 0, c.ins->b);
    setReg(c.R[c.ins->a], toJSValue(a));
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
        if (ins->a == ins->b && isString(a) && appendInPlace(vm, R[ins->a], b))
            return 1;
        r = add(vm, a, b);
        break;
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
    setReg(R[ins->a], r);
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
    setReg(R[ins->a], r);
    return 1;
}

int jitGetElem(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    const Ins* ins = c.ins;
    JSValue* R = c.R;
    const JSValue base = R[ins->b];
    JSValue k = R[ins->c];
    if (k.isDouble()) {
        const double d = k.asDouble();
        if (d >= 0 && d < 2147483648.0 && double(std::int32_t(d)) == d)
            k = JSValue::int32(std::int32_t(d));
    }
    if (k.isInt32() && k.asInt32() >= 0) {
        if (isObject(base) && asObject(base)->cls() == ObjClass::Array) {
            auto* a = static_cast<JSArray*>(asObject(base));
            const std::uint32_t i = std::uint32_t(k.asInt32());
            if (i < a->length && !a->elems[i].isHole()) {
                copyReg(R[ins->a], a->elems[i]);
                return 1;
            }
        } else if (isObject(base) && asObject(base)->cls() == ObjClass::TypedArray) {
            auto* t = static_cast<JSTypedArray*>(asObject(base));
            const std::uint32_t i = std::uint32_t(k.asInt32());
            if (i < t->length && !isBigKind(t->kind)) {
                setReg(R[ins->a], loadNumber(t->kind, t->data + std::size_t(i) * kElemSize[std::size_t(t->kind)]));
                return 1;
            }
        } else if (isString(base)) {
            StringCell* s = asString(base);
            const std::uint32_t i = std::uint32_t(k.asInt32());
            if (i < s->length) {
                const JSValue ch = makeString(vm, s->text().substr(i, 1));
                setReg(R[ins->a], ch);
                return 1;
            }
        }
    }
    if (base.isNullish()) {
        vm.throwError(ErrorCode::TypeError, "Cannot read properties of " + std::string(base.isNull() ? "null" : "undefined"));
        return 0;
    }
    if (isPrivateKey(k)) {
        const JSValue v = privateGet(vm, base, k);
        if (v.isException())
            return 0;
        setReg(R[ins->a], v);
        return 1;
    }
    JSValue prim = toPrimitive(vm, k, true);
    if (prim.isException())
        return 0;
    Key key = toPropertyKey(vm, prim);
    release(prim);
    if (!key)
        return 0;
    const JSValue v = getProperty(vm, base, key);
    release(JSValue::cell(key));
    if (v.isException())
        return 0;
    setReg(R[ins->a], v);
    return 1;
}

int jitPutElem(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    const Ins* ins = c.ins;
    JSValue* R = c.R;
    const JSValue base = R[ins->a];
    JSValue k = R[ins->b];
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
            vm.protos.array->proto(vm) == vm.protos.object) {
            retain(R[ins->c]);
            a->elems[i] = R[ins->c];
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
        return privatePut(vm, base, k, R[ins->c]) ? 1 : 0;
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
    setReg(R[ins->a], toJSValue(f));
    return 1;
}

int jitNewBox(VM& vm, std::uint32_t index)
{
    JitCtx c = jitCtx(vm, index, false);
    Box* box = vm.newBox(c.R[c.ins->a]);
    c.R[c.ins->a] = JSValue::cell(reinterpret_cast<CellPtr>(box));
    return 1;
}

std::uint64_t stepThrow(VM& vm, std::uint32_t index)
{
    Frame* fr = vm.frameBase + vm.frameCount - 1;
    const Ins* at = fr->code->code.data() + index;
    fr->pc = at + ((index + 1 < fr->code->code.size() && at[1].opcode() == Op::Cache) ? 2 : 1);
    vm.resumeThrow = true;
    return runImpl<true>(vm, nullptr).bits;
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
    if ((c->flags & CodeBlock::kClassConstructor) && !construct)
        return throwError(ErrorCode::TypeError, "class constructor " + (c->name ? toUtf8(static_cast<StringCell*>(c->name)->text()) : std::string()) +
                                                    " must be called with 'new'");
    if (c->flags & CodeBlock::kGenerator)
        return makeGenerator(f, thisValue, args, argc);
    JSValue* R = stackTop;
    if (frameCount + 2 >= frameCapacity || R + std::max<std::size_t>(c->nregs, argc + 1) + 8 > stackEnd)
        return throwError(ErrorCode::StackOverflow, "Maximum call stack size exceeded");
    // `this` and the arguments become the first registers of the new frame, each with a count of its own.
    fillStack(R + std::max<std::size_t>(c->nregs, argc + 1) + 1);
    JSValue self = thisValue;
    if (!(c->flags & (CodeBlock::kStrict | CodeBlock::kArrow)) && self.isNullish())
        self = toJSValue(global);
    retain(self);
    R[0] = self;
    for (std::uint32_t i = 0; i < argc; ++i) {
        retain(args[i]);
        R[1 + i] = args[i];
    }
    const std::uint32_t keep = 1 + (argc < c->nparams ? argc : c->nparams);
    JSArray* argsArray = (c->flags & CodeBlock::kNeedsArgs) ? captureArguments(*this, R + 1, argc) : nullptr;
    // Above `this` and the arguments the stack holds undefined (a frame lets go of its registers when it ends); an argument
    // past the parameters is the one thing in the window that is not a register of the callee.
    for (std::uint32_t i = keep, end = std::min<std::uint32_t>(c->nregs, 1 + argc); i < end; ++i) {
        const JSValue v = R[i];
        R[i] = JSValue::undefined();
        release(v);
    }
    if (argsArray)
        setReg(R[1 + c->nparams], toJSValue(argsArray));
    JSValue* savedTop = stackTop;
    frameBase[frameCount++] = Frame{c, f, nullptr, R, 0, argc, construct, true,
                                          construct ? (target.isUndefined() ? callee : target) : JSValue::undefined()};
    if (!c->tier1 && !c->tier1Failed && tier1Threshold && ++c->hotness >= tier1Threshold)
        compileTier1(*this, c);
    const JSValue result = c->tier1 ? enterTier1(*this, c->tier1, 0) : run(*this);
    stackTop = savedTop;
    // The frame let go of its own registers; `this` and the arguments are ours.
    clearRegs(R, 0, 1 + argc);
    return result;
}

JSValue VM::makeGenerator(JSFunction* f, JSValue thisValue, const JSValue* args, std::uint32_t argc)
{
    CodeBlock* c = f->code;
    // The generator's prototype is read once the parameters have been worked out, and falls back to its realm's.
    JSObject* fallback = (c->flags & CodeBlock::kAsyncGen) ? protos.asyncGenerator : protos.generator;
    JSObject* o = newObject(fallback);
    Handle keep = Handle::adopt(toJSValue(o));
    auto* g = new GenState();
    o->h.cls = std::uint8_t(ObjClass::Generator);
    o->rareOf(*this).ext = g;
    g->regs.assign(c->nregs, JSValue::undefined());
    g->callee = toJSValue(f);
    retain(g->callee);
    JSValue self = thisValue;
    if (!(c->flags & (CodeBlock::kStrict | CodeBlock::kArrow)) && self.isNullish())
        self = toJSValue(global);
    retain(self);
    g->regs[0] = self;
    const std::uint32_t keep_ = argc < c->nparams ? argc : c->nparams;
    for (std::uint32_t i = 0; i < keep_; ++i) {
        retain(args[i]);
        g->regs[1 + i] = args[i];
    }
    if (c->flags & CodeBlock::kNeedsArgs)
        g->regs[1 + c->nparams] = toJSValue(captureArguments(*this, args, argc));
    if ((c->flags & CodeBlock::kAsync) && !(c->flags & CodeBlock::kAsyncGen))
        return bi::asyncStart(*this, keep.take());
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
        if (Rare* r = asObject(generator)->rare())
            g = static_cast<GenState*>(r->ext);
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
    if (frameCount + 2 >= frameCapacity || R + c->nregs + 8 > stackEnd) {
        throwError(ErrorCode::StackOverflow, "Maximum call stack size exceeded");
        return false;
    }
    fillStack(R + c->nregs + 1);
    for (std::uint32_t i = 0; i < c->nregs; ++i) {
        const JSValue old = R[i];
        R[i] = g->regs[i];
        g->regs[i] = JSValue::undefined();
        release(old);
    }
    if (g->state == GenState::Suspended && g->delegating) {
        // At a `yield*`: what came in goes to the inner iterator, with what kind of resumption it was.
        setReg(R[g->resumeDst], sent);
        retain(sent);
        setReg(R[g->resumeDst + 1], JSValue::int32(mode));
    } else if (g->state == GenState::Suspended) {
        if (mode == 0) {
            setReg(R[g->resumeDst], sent);
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
    frameBase[frameCount++] = Frame{c, f, c->code.data() + g->pcIndex, R, 0, 0, false, true, JSValue::undefined(), generator};
    const JSValue result = run(*this);
    stackTop = savedTop;
    const bool finished = g->state == GenState::Running;
    g->prologue = false;
    if (finished) {
        g->state = GenState::Done;
        g->clear(*this);
        clearRegs(R, 0, 1);
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
