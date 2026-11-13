// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Tier 0: the interpreter. One loop over the register bytecode with computed-goto dispatch, calls between bytecode
// functions made inside it (a new frame on the VM stack, no C++ recursion), and one inline cache per property
// instruction. Registers own their values: every write releases what was there.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "script/internal/vm/census.hpp"
#include "script/internal/vm/convert.hpp"
#include "script/internal/vm/object.hpp"
#include "script/internal/vm/operators.hpp"

namespace script {
namespace internal {
namespace vm {

namespace {

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
bool initAccessor(VM& vm, JSObject* object, Key key, JSValue fn, bool isGetter)
{
    JSValue getter = JSValue::undefined(), setter = JSValue::undefined();
    std::uint8_t attrs = 0;
    if (JSValue* slot = object->findOwn(vm, key, &attrs)) {
        if (attrs & attr::kAccessor) {
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
    const bool ok = defineAccessor(vm, object, key, getter, setter, attr::kEnumerable | attr::kConfigurable);
    release(getter);
    release(setter);
    return ok;
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

}  // namespace

// --------------------------------------------------------------- the loop ---

namespace {

JSValue run(VM& vm)
{
    static const void* const kLabels[] = {
#define OP(name, kinds) &&L_##name,
#include "script/internal/vm/opcodes.def"
#undef OP
    };

    Frame* fr = &vm.frames.back();
    CodeBlock* code;
    const Ins* codeStart;
    const JSValue* K;
    Key* atoms;
    JSValue* R;
    JSFunction* fn;
    const Ins* pc;
    const Ins* ins;

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
    std::uint8_t attrs = 0;
    JSValue* slot = vm.global->findOwn(vm, key, &attrs);
    if (slot && !(attrs & attr::kAccessor)) {
        COPY(ins->a, *slot);
        NEXT();
    }
    if (!hasProperty(vm, toJSValue(vm.global), key)) {
        if (ins->opcode() == Op::GetGlobalOrUndef) {
            SET(ins->a, JSValue::undefined());
            NEXT();
        }
        vm.throwError(ErrorCode::ReferenceError, toUtf8(static_cast<StringCell*>(key)->text()) + " is not defined");
        FAIL();
    }
    fr->pc = pc;
    JSValue v = getProperty(vm, toJSValue(vm.global), key);
    if (v.isException())
        FAIL();
    SET(ins->a, v);
    NEXT();
}
L_PutGlobal: {
    Key key = atoms[ins->a];
    std::uint8_t attrs = 0;
    JSValue* slot = vm.global->findOwn(vm, key, &attrs);
    if (slot && (attrs & attr::kWritable) && !(attrs & attr::kAccessor)) {
        const JSValue old = *slot;
        *slot = R[ins->b];
        retain(*slot);
        release(old);
        NEXT();
    }
    if (!slot && STRICT() && !hasProperty(vm, toJSValue(vm.global), key)) {
        vm.throwError(ErrorCode::ReferenceError, toUtf8(static_cast<StringCell*>(key)->text()) + " is not defined");
        FAIL();
    }
    fr->pc = pc;
    if (!setProperty(vm, toJSValue(vm.global), key, R[ins->b], STRICT())) {
        if (!vm.exception.isUndefined())
            FAIL();
    }
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
#define ARITH(NAME, FAST)                                                         \
    L_##NAME : {                                                                  \
        const JSValue a = R[ins->b], b = R[ins->c];                               \
        if (a.isInt32() && b.isInt32()) {                                         \
            const std::int32_t x = a.asInt32(), y = b.asInt32();                  \
            FAST                                                                  \
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
})
ARITH(Mul, {
    std::int32_t r;
    if (!__builtin_mul_overflow(x, y, &r) && (r != 0 || (x >= 0 && y >= 0))) {
        SET(ins->a, JSValue::int32(r));
        NEXT();
    }
})
ARITH(Div, {})
ARITH(Mod, {
    if (x >= 0 && y > 0) {
        SET(ins->a, JSValue::int32(x % y));
        NEXT();
    }
})
ARITH(Exp, {})
ARITH(BitAnd, {
    SET(ins->a, JSValue::int32(x & y));
    NEXT();
})
ARITH(BitOr, {
    SET(ins->a, JSValue::int32(x | y));
    NEXT();
})
ARITH(BitXor, {
    SET(ins->a, JSValue::int32(x ^ y));
    NEXT();
})
ARITH(Shl, {
    SET(ins->a, JSValue::int32(std::int32_t(std::uint32_t(x) << (y & 31))));
    NEXT();
})
ARITH(Shr, {
    SET(ins->a, JSValue::int32(x >> (y & 31)));
    NEXT();
})
ARITH(UShr, {})

#define COMPARE(NAME, OPERATOR)                                       \
    L_##NAME : {                                                      \
        const JSValue a = R[ins->b], b = R[ins->c];                   \
        if (a.isInt32() && b.isInt32()) {                             \
            SET(ins->a, JSValue::boolean(a.asInt32() OPERATOR b.asInt32())); \
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
L_Jmp:
    pc += ins->jump();
    NEXT();
L_JTrue:
    if (toBoolean(R[ins->a]))
        pc += ins->jump();
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

// ---- properties ----
L_GetProp: {
    const JSValue base = R[ins->b];
    Key key = atoms[ins->c];
    if (isObject(base)) {
        JSObject* o = asObject(base);
        InlineCache* ic = &code->caches[std::size_t(ins - codeStart)];
        if (o->h.shape == ic->shape && ic->shape != 0) [[likely]] {
            COPY(ins->a, *o->slotAt(ic->index));
            NEXT();
        }
        if (key == vm.atoms.length && o->cls() == ObjClass::Array) {
            const std::uint32_t n = static_cast<JSArray*>(o)->length;
            SET(ins->a, n <= 0x7FFFFFFFu ? JSValue::int32(std::int32_t(n)) : JSValue::boxDouble(double(n)));
            NEXT();
        }
        SCRIPT_VM_CENSUS(SlowGetProp);
        // An own data property of an object in the shape tree is what a cache remembers.
        if (!o->isDictionary(vm)) {
            std::uint8_t attrs = 0;
            const std::int32_t index = o->shapeOf(vm)->indexOf(key, &attrs);
            if (index >= 0 && !(attrs & attr::kAccessor) && !(o->cls() == ObjClass::Function && static_cast<JSFunction*>(o)->flags & JSFunction::kLazyProps)) {
                SCRIPT_VM_CENSUS(IcMiss);
                ic->shape = o->h.shape;
                ic->index = std::uint32_t(index);
                COPY(ins->a, *o->slotAt(std::uint32_t(index)));
                NEXT();
            }
        }
    } else if (isString(base) && key == vm.atoms.length) {
        SET(ins->a, JSValue::int32(std::int32_t(asString(base)->length)));
        NEXT();
    }
    fr->pc = pc;
    const JSValue v = getProperty(vm, base, key);
    if (v.isException())
        FAIL();
    SET(ins->a, v);
    NEXT();
}
L_PutProp: {
    const JSValue base = R[ins->a];
    Key key = atoms[ins->b];
    if (isObject(base)) {
        JSObject* o = asObject(base);
        InlineCache* ic = &code->caches[std::size_t(ins - codeStart)];
        if (o->h.shape == ic->shape && ic->shape != 0) [[likely]] {
            JSValue* slot = o->slotAt(ic->index);
            const JSValue old = *slot;
            *slot = R[ins->c];
            retain(*slot);
            release(old);
            NEXT();
        }
        SCRIPT_VM_CENSUS(SlowPutProp);
        if (!o->isDictionary(vm) && o->cls() != ObjClass::Array) {
            std::uint8_t attrs = 0;
            const std::int32_t index = o->shapeOf(vm)->indexOf(key, &attrs);
            if (index >= 0 && (attrs & attr::kWritable) && !(attrs & attr::kAccessor) &&
                !(o->cls() == ObjClass::Function && static_cast<JSFunction*>(o)->flags & JSFunction::kLazyProps)) {
                ic->shape = o->h.shape;
                ic->index = std::uint32_t(index);
                JSValue* slot = o->slotAt(std::uint32_t(index));
                const JSValue old = *slot;
                *slot = R[ins->c];
                retain(*slot);
                release(old);
                NEXT();
            }
        }
    }
    fr->pc = pc;
    if (!setProperty(vm, base, key, R[ins->c], STRICT()) && !vm.exception.isUndefined())
        FAIL();
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
    JSObject* o = vm.newObject(vm.protos.object);
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
    retain(R[ins->c]);
    o->putOwn(vm, atoms[ins->b], R[ins->c], attr::kDefault);
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
L_Call:
L_New: {
    const bool isNew = ins->opcode() == Op::New;
    const std::uint32_t base = ins->b, argc = ins->c;
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
        vm.newTarget = isNew ? callee : JSValue::undefined();
        JSValue res = r->native(vm, R[base + 1], &R[base + 2], argc, r->nativeData);
        vm.newTarget = saved;
        if (res.isException())
            FAIL();
        if (isNew && !isObject(res)) {
            release(res);
            res = R[base + 1];
            retain(res);
        }
        SET(ins->a, res);
        NEXT();
    }
    CodeBlock* c = f->code;
    JSValue* newR = R + base + 1;
    if (vm.frames.size() + 2 >= vm.frames.capacity() || newR + c->nregs + 8 > vm.stackEnd) [[unlikely]] {
        vm.throwError(ErrorCode::StackOverflow, "Maximum call stack size exceeded");
        FAIL();
    }
    if (!(c->flags & (CodeBlock::kStrict | CodeBlock::kArrow)) && newR[0].isNullish()) {
        copyReg(newR[0], toJSValue(vm.global));
    }
    const std::uint32_t keep = 1 + (argc < c->nparams ? argc : c->nparams);
    for (std::uint32_t i = keep; i < c->nregs; ++i) {
        const JSValue v = newR[i];
        newR[i] = JSValue::undefined();
        release(v);
    }
    vm.frames.push_back(Frame{c, f, nullptr, newR, std::uint16_t(ins->a), argc, isNew, false});
    fr = &vm.frames.back();
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
    vm.frames.pop_back();
    if (entry) {
        (void)calleeR;
        return result;
    }
    fr = &vm.frames.back();
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
        vm.throwError(ErrorCode::TypeError, describe(vm, src) + " is not iterable");
        FAIL();
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
        vm.frames.pop_back();
        if (entry)
            return JSValue::exception();
        fr = &vm.frames.back();
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
#undef SET
#undef COPY
#undef FAIL
#undef STRICT
}

}  // namespace

// ----------------------------------------------------------------- calling ---

JSValue VM::call(JSValue callee, JSValue thisValue, const JSValue* args, std::uint32_t argc)
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
    JSValue* R = stackTop;
    if (frames.size() + 2 >= frames.capacity() || R + std::max<std::size_t>(c->nregs, argc + 1) + 8 > stackEnd)
        return throwError(ErrorCode::StackOverflow, "Maximum call stack size exceeded");
    // `this` and the arguments become the first registers of the new frame, each with a count of its own.
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
    for (std::uint32_t i = keep; i < c->nregs; ++i) {
        const JSValue v = R[i];
        R[i] = JSValue::undefined();
        release(v);
    }
    JSValue* savedTop = stackTop;
    frames.push_back(Frame{c, f, nullptr, R, 0, argc, false, true});
    const JSValue result = run(*this);
    stackTop = savedTop;
    // The frame let go of its own registers; `this` and the arguments are ours.
    clearRegs(R, 0, 1 + argc);
    return result;
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
        result = call(callee, made.get(), args, argc);
    }
    if (result.isException() || isObject(result))
        return result;
    release(result);
    return made.take();
}

}  // namespace vm
}  // namespace internal
}  // namespace script
