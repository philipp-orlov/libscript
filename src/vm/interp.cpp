// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Tier 0: the interpreter. One loop over the register bytecode with computed-goto dispatch, calls between bytecode
// functions made inside it (a new frame on the VM stack, no C++ recursion), and one inline cache per property
// instruction. Registers own their values: every write releases what was there.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "script/internal/dyn/object.hpp"
#include "script/internal/vm/census.hpp"
#include "script/internal/vm/convert.hpp"
#include "script/internal/vm/object.hpp"
#include "script/internal/vm/operators.hpp"
#include "builtins_util.hpp"

namespace script {
namespace internal {
namespace vm {

namespace {

constexpr std::uint32_t kInterruptInterval = 4096;

inline void setReg(JSValue& slot, JSValue value) noexcept
{
    const JSValue old = slot;
    slot = value;
    release(old);
}

inline void copyReg(JSValue& slot, JSValue value) noexcept
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
        if (o->cls() == ObjClass::Array) {
            auto* a = static_cast<JSArray*>(o);
            for (std::uint32_t i = 0; i < a->length; ++i)
                if (!a->elems[i].isHole())
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
        for (JSObject* o = asObject(value); o; o = o->proto(vm))
            visit(o);
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
    return !o->isDictionary(vm) && !(o->cls() == ObjClass::Function && (static_cast<JSFunction*>(o)->flags & JSFunction::kLazyProps));
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

/// A store to an own writable data property.
void learnOwnWrite(VM& vm, InlineCache& ic, JSObject* o, Key key)
{
    if (!cacheable(vm, o) || o->cls() == ObjClass::Array)
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
    if (o->h.shape == before || !cacheable(vm, o) || o->cls() == ObjClass::Array || o->isProto())
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
    JSArray* all = vm.newArray(vm.protos.array, 0, argc);
    all->appendAll(vm, args, argc);
    return all;
}

/// Add the own enumerable properties of `source` to `target`, as `{...source}` does.
void copyOwnProperties(VM& vm, JSObject* target, JSValue source, JSArray* excluded)
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
    if (isObject(source)) {
        JSObject* o = asObject(source);
        if (o->cls() == ObjClass::Array) {
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

    ~GenState() override { clear_(); }
    void values(std::vector<JSValue>& out) const override
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

JSValue run(VM& vm)
{
    static const void* const kLabels[] = {
#define OP(name, kinds) &&L_##name,
#include "script/internal/vm/opcodes.def"
#undef OP
    };

    Frame* fr = vm.frameBase + vm.frameCount - 1;
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
#define NEXT()          \
    do {                \
        ins = pc++;     \
        goto* kLabels[ins->op]; \
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
#define TAKE16()                  \
    do {                          \
        const int o_ = ins->jump16(); \
        pc += o_;                 \
        if (o_ < 0)               \
            TICK();               \
    } while (0)
#define SET(r, v) setReg(R[r], v)
#define COPY(r, v) copyReg(R[r], v)
#define FAIL()  \
    do {         \
        fr->pc = pc; \
        goto throw_; \
    } while (0)
#define STRICT() ((code->flags & CodeBlock::kStrict) != 0)

    LOAD_FRAME();
    vm.stackTop = R + code->nregs;
    if (vm.resumeThrow) [[unlikely]] {
        vm.resumeThrow = false;
        fr->pc = pc;
        goto throw_;
    }
    NEXT();

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
        COPY(ins->a, *g->slotAt(ic.index));
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
        COPY(ins->a, *slot);
        ++pc;
        NEXT();
    }
    if (!hasProperty(vm, toJSValue(g), key)) {
        if (ins->opcode() == Op::GetGlobalOrUndef) {
            SET(ins->a, JSValue::undefined());
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
    SET(ins->a, v);
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
L_DefGlobal: {
    Key key = atoms[ins->a];
    retain(R[ins->b]);
    vm.global->putOwn(vm, key, R[ins->b], attr::kDefault);
    NEXT();
}
L_DeclGlobal: {
    Key key = atoms[ins->a];
    if (!vm.global->findOwn(vm, key))
        vm.global->putOwn(vm, key, JSValue::undefined(), attr::kDefault);
    NEXT();
}

L_NewBox: {
    Box* box = vm.newBox(R[ins->a]);  // takes the register's count
    R[ins->a] = JSValue::cell(reinterpret_cast<CellPtr>(box));
    NEXT();
}
L_GetBox:
    COPY(ins->a, reinterpret_cast<Box*>(R[ins->b].asCell())->value);
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
    COPY(ins->a, fn->upvals()[ins->b]->value);
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
            SET(ins->a, JSValue::int32(r));
            NEXT();
        }
    }
    if (a.isNumber() && b.isNumber()) {
        SET(ins->a, JSValue::boxNumber(a.asNumber() + b.asNumber()));
        NEXT();
    }
    if (ins->a == ins->b && isString(a) && appendInPlace(vm, R[ins->a], b))
        NEXT();
    fr->pc = pc;
    const JSValue r = add(vm, a, b);
    if (r.isException())
        FAIL();
    SET(ins->a, r);
    NEXT();
}
L_AddI: {
    const JSValue a = R[ins->b];
    if (a.isInt32()) {
        std::int32_t r;
        if (!__builtin_add_overflow(a.asInt32(), std::int32_t(std::int16_t(ins->c)), &r)) {
            SET(ins->a, JSValue::int32(r));
            NEXT();
        }
    }
    fr->pc = pc;
    const JSValue r = add(vm, a, JSValue::int32(std::int16_t(ins->c)));
    if (r.isException())
        FAIL();
    SET(ins->a, r);
    NEXT();
}
L_SubI: {
    const JSValue a = R[ins->b];
    if (a.isInt32()) {
        std::int32_t r;
        if (!__builtin_sub_overflow(a.asInt32(), std::int32_t(std::int16_t(ins->c)), &r)) {
            SET(ins->a, JSValue::int32(r));
            NEXT();
        }
    }
    fr->pc = pc;
    const JSValue r = arithmetic(vm, Op::Sub, a, JSValue::int32(std::int16_t(ins->c)));
    if (r.isException())
        FAIL();
    SET(ins->a, r);
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
        SET(ins->a, r);                                                           \
        NEXT();                                                                   \
    }
ARITH(Sub, {
    std::int32_t r;
    if (!__builtin_sub_overflow(x, y, &r)) {
        SET(ins->a, JSValue::int32(r));
        NEXT();
    }
}, { SET(ins->a, JSValue::boxNumber(a.asNumber() - b.asNumber())); NEXT(); })
ARITH(Mul, {
    std::int32_t r;
    if (!__builtin_mul_overflow(x, y, &r) && (r != 0 || (x >= 0 && y >= 0))) {
        SET(ins->a, JSValue::int32(r));
        NEXT();
    }
}, { SET(ins->a, JSValue::boxNumber(a.asNumber() * b.asNumber())); NEXT(); })
ARITH(Div, {}, { SET(ins->a, JSValue::boxNumber(a.asNumber() / b.asNumber())); NEXT(); })
ARITH(Mod, {
    if (x >= 0 && y > 0) {
        SET(ins->a, JSValue::int32(x % y));
        NEXT();
    }
}, { })
ARITH(Exp, {}, { })
ARITH(BitAnd, {
    SET(ins->a, JSValue::int32(x & y));
    NEXT();
}, { })
ARITH(BitOr, {
    SET(ins->a, JSValue::int32(x | y));
    NEXT();
}, { })
ARITH(BitXor, {
    SET(ins->a, JSValue::int32(x ^ y));
    NEXT();
}, { })
ARITH(Shl, {
    SET(ins->a, JSValue::int32(std::int32_t(std::uint32_t(x) << (y & 31))));
    NEXT();
}, { })
ARITH(Shr, {
    SET(ins->a, JSValue::int32(x >> (y & 31)));
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
    const bool eq = strictEquals(R[ins->b], R[ins->c]);
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
        SET(ins->a, JSValue::int32(-a.asInt32()));
        NEXT();
    }
    fr->pc = pc;
    double d;
    if (!toNumber(vm, a, &d))
        FAIL();
    SET(ins->a, JSValue::boxNumber(-d));
    NEXT();
}
L_ToNumber:
L_ToNumeric: {
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
    double d;
    if (!toNumber(vm, a, &d))
        FAIL();
    SET(ins->a, JSValue::int32(~toInt32(d)));
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
            SET(ins->a, JSValue::int32(r));
            NEXT();
        }
    }
    fr->pc = pc;
    double d;
    if (!toNumber(vm, a, &d))
        FAIL();
    SET(ins->a, JSValue::boxNumber(d + delta));
    NEXT();
}

// ---- jumps ----
L_Jmp: {
    const std::int32_t offset = ins->jump();
    pc += offset;
    if (offset < 0)
        TICK();
    NEXT();
}
L_JTrue:
    if (toBoolean(R[ins->a])) {
        const std::int32_t offset = ins->jump();
        pc += offset;
        if (offset < 0)
            TICK();
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
    const bool eq = strictEquals(R[ins->a], R[ins->b]);
    if (eq == (ins->opcode() == Op::JStrictEq))
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
                COPY(ins->a, *o->slotAt(ic.index));
                ++pc;
                NEXT();
            }
            if (ic.validity->valid) {
                COPY(ins->a, *ic.holder->slotAt(ic.index));
                ++pc;
                NEXT();
            }
        }
        if (key == vm.atoms.length && o->cls() == ObjClass::Array) {
            const std::uint32_t n = static_cast<JSArray*>(o)->length;
            SET(ins->a, n <= 0x7FFFFFFFu ? JSValue::int32(std::int32_t(n)) : JSValue::boxDouble(double(n)));
            ++pc;
            NEXT();
        }
        SCRIPT_VM_CENSUS(SlowGetProp);
        if (cacheId != kNoCache) {
            if (learnRead(vm, ic, o, key)) {
                SCRIPT_VM_CENSUS(IcMiss);
                COPY(ins->a, ic.holder ? *ic.holder->slotAt(ic.index) : *o->slotAt(ic.index));
                ++pc;
                NEXT();
            }
        }
    } else if (isString(base) && key == vm.atoms.length) {
        SET(ins->a, JSValue::int32(std::int32_t(asString(base)->length)));
        ++pc;
        NEXT();
    }
    fr->pc = pc;
    const JSValue v = getProperty(vm, base, key);
    if (v.isException())
        FAIL();
    SET(ins->a, v);
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
L_GetElem: {
    const JSValue base = R[ins->b], k = R[ins->c];
    if (k.isInt32() && k.asInt32() >= 0) {
        if (isObject(base) && asObject(base)->cls() == ObjClass::Array) {
            auto* a = static_cast<JSArray*>(asObject(base));
            const std::uint32_t i = std::uint32_t(k.asInt32());
            if (i < a->length && !a->elems[i].isHole()) {
                COPY(ins->a, a->elems[i]);
                NEXT();
            }
        } else if (isString(base)) {
            StringCell* s = asString(base);
            const std::uint32_t i = std::uint32_t(k.asInt32());
            if (i < s->length) {
                fr->pc = pc;
                const JSValue ch = makeString(vm, s->text().substr(i, 1));
                SET(ins->a, ch);
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
        SET(ins->a, v);
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
    SET(ins->a, v);
    NEXT();
}
L_PutElem: {
    const JSValue base = R[ins->a], k = R[ins->b];
    if (k.isInt32() && k.asInt32() >= 0 && isObject(base) && asObject(base)->cls() == ObjClass::Array &&
        !(asObject(base)->h.flags & (cellflags::kFrozen | cellflags::kNotExtensible))) {
        auto* a = static_cast<JSArray*>(asObject(base));
        const std::uint32_t i = std::uint32_t(k.asInt32());
        if (i < a->length) {
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
    copyOwnProperties(vm, asObject(R[ins->a]), R[ins->b], nullptr);
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
    copyOwnProperties(vm, out, src, static_cast<JSArray*>(asObject(R[ins->c])));
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
        for (;;) {
            JSValue value;
            const int step = iteratorStep(vm, R[it], R[it + 1], &value);
            if (step < 0)
                FAIL();
            if (step == 0)
                break;
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
        for (; std::uint32_t(index) < s->length; ++index)
            out->append(vm, makeString(vm, s->text().substr(std::size_t(index), 1)));
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
    std::vector<Box*> upBoxes(nUp);
    for (std::uint32_t i = 0; i < nUp; ++i) {
        const UpvalDesc& d = ctor->upvals[i];
        upBoxes[i] = d.fromParentLocal ? reinterpret_cast<Box*>(R[d.index].asCell()) : fn->upvals()[d.index];
    }
    JSFunction* f = vm.newFunction(parent ? parent : vm.protos.function, ctor, upBoxes.data(), nUp);
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
    if (!p || !isFunction(toJSValue(p))) {
        fr->pc = pc;
        vm.throwError(ErrorCode::TypeError, "Super constructor is not a constructor");
        FAIL();
    }
    COPY(ins->a, toJSValue(p));
    NEXT();
}
L_GetSuperProp: {
    // `super.name`: looked up from the prototype of the home object, with `this` as the receiver.
    Rare* rr = fn->rare();
    JSObject* start = nullptr;
    if (rr && isObject(rr->home))
        start = asObject(rr->home)->proto(vm);
    fr->pc = pc;
    JSValue v = getPropertyFrom(vm, start, R[ins->c], atoms[ins->b]);
    if (v.isException())
        FAIL();
    SET(ins->a, v);
    NEXT();
}
L_NewTarget:
    COPY(ins->a, fr->newTarget);
    NEXT();
L_Closure: {
    CodeBlock* child = code->children[ins->b];
    const std::uint32_t n = std::uint32_t(child->upvals.size());
    Box* small[16];
    std::vector<Box*> big;
    Box** boxes = small;
    if (n > 16) {
        big.resize(n);
        boxes = big.data();
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
        JSValue protoValue = getProperty(vm, callee, vm.atoms.prototype);
        if (protoValue.isException())
            FAIL();
        JSObject* proto = isObject(protoValue) ? asObject(protoValue) : vm.protos.object;
        JSObject* made = vm.newObject(proto);
        release(protoValue);
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
        SET(ins->a, res);
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
    JSValue* newR = R + base + 1;
    if (vm.frameCount + 2 >= vm.frameCapacity || newR + c->nregs + 8 > vm.stackEnd) [[unlikely]] {
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
    SET(ins->a, forInKeys(vm, R[ins->b]));
    NEXT();
}
L_IterInit: {
    const JSValue src = R[ins->b];
    if (!isArray(src) && !isString(src)) {
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
        JSValue value;
        const int step = iteratorStep(vm, R[it], R[it + 1], &value);
        if (step < 0)
            FAIL();
        if (step == 0)
            COPY(ins->a, ins->c ? JSValue::undefined() : JSValue::hole());
        else
            SET(ins->a, value);
        NEXT();
    }
    const JSValue src = R[it];
    const std::int32_t index = R[it + 1].asInt32();
    JSValue out = JSValue::undefined();
    bool owned = false, done;
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
            out = makeString(vm, s->text().substr(std::size_t(index), 1));
            owned = true;
        }
    }
    if (done) {
        // A pattern reads undefined past the end; a loop is told to stop by the hole.
        if (!ins->c)
            out = JSValue::hole();
    } else {
        R[it + 1] = JSValue::int32(index + 1);
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
L_Yield: {
    auto* g = static_cast<GenState*>(asObject(fr->gen)->rare()->ext);
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
            if (at >= h.start && at < h.end) {
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
#undef NEXT
#undef TICK
#undef TAKE16
#undef SET
#undef COPY
#undef FAIL
#undef STRICT
}

}  // namespace

// ----------------------------------------------------------------- calling ---

JSValue VM::call(JSValue callee, JSValue thisValue, const JSValue* args, std::uint32_t argc)
{
    return callInternal(callee, thisValue, args, argc, false);
}

JSValue VM::callInternal(JSValue callee, JSValue thisValue, const JSValue* args, std::uint32_t argc, bool construct)
{
    if (!isFunction(callee))
        return notCallable(*this, callee);
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
    for (std::uint32_t i = keep; i < c->nregs; ++i) {
        const JSValue v = R[i];
        R[i] = JSValue::undefined();
        release(v);
    }
    if (argsArray)
        setReg(R[1 + c->nparams], toJSValue(argsArray));
    JSValue* savedTop = stackTop;
    frameBase[frameCount++] = Frame{c, f, nullptr, R, 0, argc, construct, true, construct ? callee : JSValue::undefined()};
    const JSValue result = run(*this);
    stackTop = savedTop;
    // The frame let go of its own registers; `this` and the arguments are ours.
    clearRegs(R, 0, 1 + argc);
    return result;
}

JSValue VM::makeGenerator(JSFunction* f, JSValue thisValue, const JSValue* args, std::uint32_t argc)
{
    CodeBlock* c = f->code;
    JSValue protoValue = getProperty(*this, toJSValue(f), atoms.prototype);
    if (protoValue.isException())
        return protoValue;
    JSObject* proto = isObject(protoValue) ? asObject(protoValue) : protos.generator;
    JSObject* o = newObject(proto);
    Handle keep = Handle::adopt(toJSValue(o));
    release(protoValue);
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
    return keep.take();
}

JSValue VM::resumeGenerator(JSValue generator, int mode, JSValue sent)
{
    GenState* g = nullptr;
    if (isObject(generator) && asObject(generator)->cls() == ObjClass::Generator)
        if (Rare* r = asObject(generator)->rare())
            g = static_cast<GenState*>(r->ext);
    if (!g)
        return throwError(ErrorCode::TypeError, "next method called on an incompatible receiver");
    if (g->state == GenState::Running)
        return throwError(ErrorCode::TypeError, "Generator is already running");
    Handle keepGenerator = Handle::retained(generator);
    Handle keepSent = Handle::retained(sent);
    if (g->state == GenState::Start && mode != 0) {
        g->state = GenState::Done;
        g->clear(*this);
    }
    // A `return` into a suspended generator ends it where it stands: the `finally` blocks around the `yield` do not run.
    if (g->state == GenState::Suspended && mode == 2) {
        g->state = GenState::Done;
        g->clear(*this);
    }
    if (g->state == GenState::Done) {
        if (mode == 1) {
            release(exception);
            exception = sent;
            retain(exception);
            exceptionCode = ErrorCode::Exception;
            return JSValue::exception();
        }
        const JSValue v = mode == 2 ? sent : JSValue::undefined();
        retain(v);
        return bi::iterResult(*this, v, true);
    }
    auto* f = static_cast<JSFunction*>(asObject(g->callee));
    CodeBlock* c = f->code;
    JSValue* R = stackTop;
    if (frameCount + 2 >= frameCapacity || R + c->nregs + 8 > stackEnd)
        return throwError(ErrorCode::StackOverflow, "Maximum call stack size exceeded");
    fillStack(R + c->nregs + 1);
    for (std::uint32_t i = 0; i < c->nregs; ++i) {
        const JSValue old = R[i];
        R[i] = g->regs[i];
        g->regs[i] = JSValue::undefined();
        release(old);
    }
    if (g->state == GenState::Suspended) {
        if (mode == 0) {
            setReg(R[g->resumeDst], sent);
            retain(sent);
        } else {
            release(exception);
            exception = sent;
            retain(exception);
            exceptionCode = ErrorCode::Exception;
            resumeThrow = true;
        }
    }
    g->state = GenState::Running;
    JSValue* savedTop = stackTop;
    frameBase[frameCount++] = Frame{c, f, c->code.data() + g->pcIndex, R, 0, 0, false, true, JSValue::undefined(), generator};
    const JSValue result = run(*this);
    stackTop = savedTop;
    bool finished = g->state == GenState::Running;
    if (finished) {
        g->state = GenState::Done;
        g->clear(*this);
        clearRegs(R, 0, 1);
    }
    if (result.isException())
        return result;
    return bi::iterResult(*this, result, finished);
}

JSValue VM::construct(JSValue callee, const JSValue* args, std::uint32_t argc)
{
    if (!isFunction(callee))
        return throwError(ErrorCode::TypeError, describe(*this, callee) + " is not a constructor");
    auto* f = static_cast<JSFunction*>(asObject(callee));
    const bool canConstruct = (f->flags & JSFunction::kNative) ? (f->flags & JSFunction::kConstructor) != 0 : f->code->constructor;
    if (!canConstruct)
        return throwError(ErrorCode::TypeError, describe(*this, callee) + " is not a constructor");
    JSValue protoValue = getProperty(*this, callee, atoms.prototype);
    if (protoValue.isException())
        return protoValue;
    JSObject* proto = isObject(protoValue) ? asObject(protoValue) : protos.object;
    Handle made = Handle::adopt(toJSValue(newObject(proto)));
    release(protoValue);
    JSValue result;
    if (f->flags & JSFunction::kNative) {
        Rare* r = f->rare();
        const JSValue saved = newTarget;
        newTarget = callee;
        result = r->native(*this, made.get(), args, argc, r->nativeData);
        newTarget = saved;
    } else {
        result = callInternal(callee, made.get(), args, argc, true);
    }
    if (result.isException() || isObject(result))
        return result;
    release(result);
    return made.take();
}

}  // namespace vm
}  // namespace internal
}  // namespace script
