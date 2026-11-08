// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Abstract operations, the call protocol and the dispatch loop.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "script/internal/dyn/vm.hpp"
#if SCRIPT_ENABLE_DYN_JIT
#include "script/internal/dyn/jit.hpp"
#endif
#include "script/internal/dyn/jit_helpers.hpp"
#include "script/internal/dyn/bigint.hpp"
#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/compiler.hpp"
#include "script/internal/dyn/numconv.hpp"
#include "script/internal/dyn/parser.hpp"
#include "script/internal/dyn/proxy.hpp"
#include "script/internal/dyn/typedarray.hpp"
#include "script/internal/stack_guard.hpp"

namespace script {
namespace internal {
namespace dyn {

namespace {
constexpr std::uint32_t kRethrow = 0xffffffffu;
constexpr std::uint32_t kInterruptInterval = 4096;
}  // namespace

void checkLongLoopSlow(Runtime& rt, std::size_t tempBytes)
{
    rt.ctx.reserve(tempBytes);
    if (rt.ctx.interruptAsked())
        throwError(rt, ErrorCode::Interrupted, "execution was interrupted");
}

void Frame::begin(DynFunction* function, const FunctionCode* body, Value self,
                  DynFunction* target)
{
    fn = function;
    code = body;
    pc = 0;
    thisValue = std::move(self);
    newTarget.reset(target);
    locals.assign(body->numLocals, Value::undefined());
    cells.assign(body->numLocals, Ref<Box>());
    stack.clear();
    stack.reserve(body->maxStack);
    subStack.clear();
}

void Frame::end() noexcept
{
    fn = nullptr;
    code = nullptr;
    locals.clear();
    cells.clear();
    stack.clear();
    subStack.clear();
    thisValue = Value::undefined();
    pendingException = Value::undefined();
    newTarget.reset();
    coroutine = nullptr;
    left = Left::Finished;
}

// ------------------------------------------------------------- errors ---

Value makeErrorValue(Runtime& rt, ErrorCode code, const std::string& message)
{
    auto it = rt.errorProtos.find(int(code));
    DynObject* proto = it != rt.errorProtos.end() ? it->second.get() : rt.errorProto.get();
    DynObject* err = rt.newObject(proto);
    err->cls = ObjClass::Error;
    err->putOwn(rt.atom.message.get(), rt.str(fromUtf8(message)),
                PropWritable | PropConfigurable);
    err->putOwn(rt.atom.stack.get(), rt.str(fromUtf8(rt.captureStack())),
                PropWritable | PropConfigurable);
    return detail::Access::adopt(Type::Object, err);
}

void throwError(Runtime& rt, ErrorCode code, const std::string& message)
{
    Value err = makeErrorValue(rt, code, message);
    throw Exception(code, message, err, rt.captureStack());
}

void rethrowAsScript(Runtime& rt)
{
    try {
        throw;
    } catch (Exception&) {
        throw;
    } catch (const Error& e) {
        throwError(rt, e.code(), e.what());
    } catch (const std::exception& e) {
        throwError(rt, ErrorCode::InternalError, e.what());
    }
}

/// Throw an arbitrary value the way `throw` does, without letting a hostile
/// `toString` derail the unwind.
[[noreturn]] void throwValue(Runtime& rt, const Value& v)
{
    std::string message;
    try {
        message = toUtf8(toStringValue(rt, v));
    } catch (...) {
        message = "uncaught exception";
    }
    // An `Error` keeps its identity across the boundary: the code is the one
    // whose prototype the thrown object inherits from.
    ErrorCode code = ErrorCode::Exception;
    if (DynObject* obj = asObject(v)) {
        for (DynObject* cursor = obj->proto.get(); cursor && code == ErrorCode::Exception;
             cursor = cursor->proto.get()) {
            for (const auto& entry : rt.errorProtos) {
                // StackOverflow shares RangeError's prototype and is only ever
                // raised by the engine itself.
                if (entry.second.get() == cursor && entry.first != int(ErrorCode::Exception) &&
                    entry.first != int(ErrorCode::StackOverflow)) {
                    code = ErrorCode(entry.first);
                    break;
                }
            }
        }
    }
    throw Exception(code, message, v, rt.captureStack());
}

namespace {
}  // namespace

// -------------------------------------------------------- conversions ---

/// `typeof v`, as the language spells it.
const char* typeOfName(const Value& v) noexcept
{
    switch (v.type()) {
        case Type::Undefined: return "undefined";
        case Type::Null: return "object";
        case Type::Bool: return "boolean";
        case Type::Char:
        case Type::Int:
        case Type::Long:
        case Type::Float:
        case Type::Double: return "number";
        case Type::String: return "string";
        case Type::BigInt: return "bigint";
        case Type::Symbol: return "symbol";
        default: return asFunction(v) ? "function" : "object";
    }
}

Value toBigInt(Runtime& rt, const Value& v)
{
    const Value primitive = toPrimitive(rt, v, 1);
    switch (primitive.type()) {
        case Type::BigInt: return primitive;
        case Type::Bool: return bigint::fromInt64(rt, boolOf(primitive) ? 1 : 0);
        case Type::String: {
            Value parsed;
            if (!bigint::parseString(rt, asString(primitive)->text(), parsed))
                throwError(rt, ErrorCode::SyntaxError, "cannot convert the string to a BigInt");
            return parsed;
        }
        default:
            throwError(rt, ErrorCode::TypeError, "cannot convert the value to a BigInt");
    }
}

/// Arithmetic with a bigint on either side: both must be, or it is an error.
Value numericSlow(Runtime& rt, bigint::BinaryOp op, const Value& a, const Value& b)
{
    if (!a.isBigInt() || !b.isBigInt())
        throwError(rt, ErrorCode::TypeError, "cannot mix BigInt and other types");
    return bigint::binary(rt, op, bigint::cellOf(a), bigint::cellOf(b));
}

/// ToNumeric: a number or a bigint.
Value toNumeric(Runtime& rt, const Value& v)
{
    if (v.isNumber() || v.isBigInt())
        return v;
    const Value primitive = toPrimitive(rt, v, 1);
    if (primitive.isBigInt())
        return primitive;
    return Value(toNumber(rt, primitive));
}

bool toBoolean(const Value& v) noexcept
{
    switch (v.type()) {
        case Type::Undefined:
        case Type::Null:
            return false;
        case Type::Bool:
            return boolOf(v);
        case Type::Int:
        case Type::Long:
        case Type::Float:
        case Type::Char:
        case Type::Double: {
            const double d = numberOf(v);
            return d != 0 && !std::isnan(d);
        }
        case Type::String:
            return !asString(v)->text().empty();
        case Type::BigInt:
            return !static_cast<BigIntCell*>(detail::Access::ptr(v))->digits.empty();
        default:
            return true;
    }
}

Value toPrimitive(Runtime& rt, const Value& v, int hint)
{
    DynObject* obj = asObject(v);
    if (!obj)
        obj = hostProtoOf(rt, v);  // a host wrapper converts through its class
    if (!obj)
        return v;

    if (Value exotic = getPropertyOfObject(rt, obj, rt.sym.toPrimitive.get(), v);
        !exotic.isNullish()) {
        const char* hintName = hint == 1 ? "number" : hint == 2 ? "string" : "default";
        Value r = callValue(rt, exotic, v, {rt.str(hintName)});
        if (!r.isObject())
            return r;
        throwError(rt, ErrorCode::TypeError, "Symbol.toPrimitive returned an object");
    }

    GCObject* first = hint == 2 ? rt.atom.toString.get() : rt.atom.valueOf.get();
    GCObject* second = hint == 2 ? rt.atom.valueOf.get() : rt.atom.toString.get();
    for (GCObject* name : {first, second}) {
        Value fn = getPropertyOfObject(rt, obj, name, v);
        if (!asFunction(fn))
            continue;
        Value r = callValue(rt, fn, v, {});
        if (!r.isObject())
            return r;
    }
    throwError(rt, ErrorCode::TypeError, "cannot convert this object to a primitive value");
}

double toNumber(Runtime& rt, const Value& v)
{
    switch (v.type()) {
        case Type::Undefined:
            return std::numeric_limits<double>::quiet_NaN();
        case Type::Null:
            return 0;
        case Type::Bool:
            return boolOf(v) ? 1 : 0;
        case Type::Int:
        case Type::Long:
        case Type::Float:
        case Type::Char:
        case Type::Double:
            return numberOf(v);
        case Type::String:
            return stringToNumber(asString(v)->text());
        case Type::BigInt:
            throwError(rt, ErrorCode::TypeError, "cannot convert a BigInt to a number");
        case Type::Symbol:
            throwError(rt, ErrorCode::TypeError, "cannot convert a symbol to a number");
        default:
            return toNumber(rt, toPrimitive(rt, v, 1));
    }
}

String toStringValue(Runtime& rt, const Value& v)
{
    switch (v.type()) {
        case Type::Undefined:
            return ascii("undefined");
        case Type::Null:
            return ascii("null");
        case Type::Bool:
            return ascii(boolOf(v) ? "true" : "false");
        case Type::Int:
        case Type::Long:
        case Type::Float:
        case Type::Char:
        case Type::Double:
            return numberToString(numberOf(v));
        case Type::String:
            return String(asString(v)->text());
        case Type::BigInt:
            return bigint::toString(bigint::cellOf(v));
        case Type::Symbol:
            throwError(rt, ErrorCode::TypeError, "cannot convert a symbol to a string");
        default:
            return toStringValue(rt, toPrimitive(rt, v, 2));
    }
}

std::int32_t toInt32(double d) noexcept
{
    if (d >= -2147483648.0 && d <= 2147483647.0)
        return std::int32_t(d);
    if (!std::isfinite(d) || d == 0)
        return 0;
    const double m = std::fmod(std::trunc(d), 4294967296.0);
    const double u = m < 0 ? m + 4294967296.0 : m;
    return std::int32_t(std::uint32_t(u));
}

std::uint32_t toUint32(double d) noexcept
{
    return std::uint32_t(toInt32(d));
}

std::int64_t toIntegerOrInfinity(double d) noexcept
{
    if (std::isnan(d))
        return 0;
    if (d > 9.2233720368547758e18)
        return std::numeric_limits<std::int64_t>::max();
    if (d < -9.2233720368547758e18)
        return std::numeric_limits<std::int64_t>::min();
    return std::int64_t(std::trunc(d));
}

Value toObject(Runtime& rt, const Value& v)
{
    if (v.isObject())
        return v;
    if (v.isNullish())
        throwError(rt, ErrorCode::TypeError, "cannot convert null or undefined to an object");
    DynObject* proto = rt.objectProto.get();
    ObjClass cls = ObjClass::Plain;
    switch (v.type()) {
        case Type::String: proto = rt.stringProto.get(); cls = ObjClass::StringObject; break;
        case Type::Int:
        case Type::Long:
        case Type::Float:
        case Type::Char:
        case Type::Double: proto = rt.numberProto.get(); cls = ObjClass::NumberObject; break;
        case Type::Bool: proto = rt.booleanProto.get(); cls = ObjClass::BooleanObject; break;
        case Type::BigInt: proto = rt.bigintProto.get(); cls = ObjClass::BigIntObject; break;
        case Type::Symbol: proto = rt.symbolProto.get(); cls = ObjClass::SymbolObject; break;
        default: break;
    }
    auto* wrapper = rt.newCell<DynWrapper>();
    wrapper->cls = cls;
    // Plain is otherwise only ever a bare object, which `destroyCell` frees without asking; this holds more than one has.
    if (cls == ObjClass::Plain)
        wrapper->rareOf();
    wrapper->setProto(proto);
    wrapper->primitive = v;
    return detail::Access::adopt(Type::Object, wrapper);
}

Ref<GCObject> toPropertyKey(Runtime& rt, const Value& v)
{
    if (v.isSymbol())
        return Ref<GCObject>(detail::Access::ptr(v));
    return Ref<GCObject>(rt.intern(toStringValue(rt, v)));
}

bool arrayIndexOf(const StringView& key, std::uint32_t& out) noexcept
{
    if (key.empty() || key.size() > 10)
        return false;
    if (key[0] == u'0' && key.size() > 1)
        return false;
    std::uint64_t acc = 0;
    for (Char c : key) {
        if (c < u'0' || c > u'9')
            return false;
        acc = acc * 10 + std::uint64_t(c - u'0');
        if (acc > 0xfffffffeull)
            return false;
    }
    out = std::uint32_t(acc);
    return true;
}

// ---------------------------------------------------------- properties ---

namespace {

bool arrayElementKey(DynArray* arr, GCObject* key, std::uint32_t& index)
{
    if (key->kind != CellKind::String)
        return false;
    return arrayIndexOf(static_cast<StringCell*>(key)->text(), index) && index < arr->size() && arr->hasElement(index);
}

}  // namespace

Value getPropertyOfObject(Runtime& rt, DynObject* obj, GCObject* key, const Value& receiver)
{
    for (DynObject* cursor = obj; cursor; cursor = cursor->proto.get()) {
        if (cursor->fnKindIsProxy())
            return proxyGet(rt, asProxy(cursor), key, receiver);
        if (cursor->cls == ObjClass::Array) {
            auto* arr = static_cast<DynArray*>(cursor);
            std::uint32_t index = 0;
            if (arrayElementKey(arr, key, index))
                return arr->at(index);
            if (key == rt.atom.length.get())
                return Value(double(arr->size()));
        }
        if (cursor->cls == ObjClass::StringObject && key == rt.atom.length.get()) {
            auto* w = static_cast<DynWrapper*>(cursor);
            if (StringCell* s = asString(w->primitive))
                return Value(double(s->text().size()));
        }
        if (cursor->cls == ObjClass::TypedArray) {
            auto* ta = static_cast<DynTypedArray*>(cursor);
            double index = 0;
            if (numericKey(key, index))
                return typedArrayHas(ta, index) ? typedArrayGet(rt, ta, std::size_t(index)) : Value::undefined();
        }
        if (const Prop* p = cursor->findOwn(key)) {
            if (!cursor->accessorOf(*p))
                return p->value;
            if (p->value.isUndefined())
                return Value::undefined();
            return callValue(rt, p->value, receiver, {});
        }
    }
    return Value::undefined();
}

/// The prototype a host wrapper cell reads through, or null when `v` is not one.
DynObject* hostProtoOf(Runtime& rt, const Value& v)
{
    GCObject* cell = detail::Access::ptr(v);
    if (!cell || cell->kind != CellKind::Native)
        return nullptr;
    return rt.hostPrototypeFor(static_cast<NativeCell*>(cell)->slot.typeKey);
}

static inline bool isPrivateName(const GCObject* key) noexcept
{
    return key->kind == CellKind::Symbol && static_cast<const SymbolCell*>(key)->isPrivate;
}

/// Only the object's own, and to not have it is an error.
static Prop* privateOf(Runtime& rt, const Value& base, GCObject* key, DynObject*& object)
{
    object = asObject(base);
    Prop* p = object ? object->findOwn(key) : nullptr;
    if (!p)
        throwError(rt, ErrorCode::TypeError, "this object has no private member " + toUtf8(static_cast<SymbolCell*>(key)->description));
    return p;
}

Value getProperty(Runtime& rt, const Value& base, GCObject* key)
{
    if (isPrivateName(key)) {
        DynObject* object = nullptr;
        Prop* p = privateOf(rt, base, key, object);
        if (!object->accessorOf(*p))
            return p->value;
        const Value getter = p->value;
        if (getter.isUndefined())
            throwError(rt, ErrorCode::TypeError, "a private accessor has no getter");
        return callValue(rt, getter, base, {});
    }
    if (DynObject* proto = hostProtoOf(rt, base))
        return getPropertyOfObject(rt, proto, key, base);
    switch (base.type()) {
        case Type::Undefined:
        case Type::Null:
            throwError(rt, ErrorCode::TypeError,
                       "cannot read property '" + (key->kind == CellKind::String
                                                       ? toUtf8(static_cast<StringCell*>(key)->text())
                                                       : std::string("<symbol>")) +
                           "' of " + (base.isNull() ? "null" : "undefined"));
        case Type::String: {
            StringCell* s = asString(base);
            if (key == rt.atom.length.get())
                return Value(double(s->text().size()));
            if (key->kind == CellKind::String) {
                std::uint32_t index = 0;
                if (arrayIndexOf(static_cast<StringCell*>(key)->text(), index)) {
                    if (index >= s->text().size())
                        return Value::undefined();
                    return rt.str(s->text().substr(index, 1));
                }
            }
            return getPropertyOfObject(rt, rt.stringProto.get(), key, base);
        }
        case Type::Int:
        case Type::Long:
        case Type::Float:
        case Type::Char:
        case Type::Double:
            return getPropertyOfObject(rt, rt.numberProto.get(), key, base);
        case Type::Bool:
            return getPropertyOfObject(rt, rt.booleanProto.get(), key, base);
        case Type::BigInt:
            return getPropertyOfObject(rt, rt.bigintProto.get(), key, base);
        case Type::Symbol:
            return getPropertyOfObject(rt, rt.symbolProto.get(), key, base);
        default:
            return getPropertyOfObject(rt, asObject(base), key, base);
    }
}

void setProperty(Runtime& rt, const Value& base, GCObject* key, const Value& v)
{
    if (isPrivateName(key)) {
        DynObject* object = nullptr;
        Prop* p = privateOf(rt, base, key, object);
        if (object->accessorOf(*p)) {
            const Value setter = object->setterOf(*p);
            if (setter.isUndefined())
                throwError(rt, ErrorCode::TypeError, "a private accessor has no setter");
            callValue(rt, setter, base, {v});
            return;
        }
        if (!(object->attrsAt(std::size_t(p - object->props.data())) & PropWritable))
            throwError(rt, ErrorCode::TypeError, "a private method cannot be assigned to");
        p->value = v;
        return;
    }
    DynObject* obj = asObject(base);
    if (!obj) {
        if (base.isNullish())
            throwError(rt, ErrorCode::TypeError, "cannot set a property of null or undefined");
        if (DynObject* proto = hostProtoOf(rt, base)) {
            // A host wrapper has no own properties: only a bound field's
            // setter can take the write.
            for (DynObject* cursor = proto; cursor; cursor = cursor->proto.get()) {
                if (const Prop* p = cursor->findOwn(key)) {
                    if (cursor->accessorOf(*p) && !cursor->setterOf(*p).isUndefined()) {
                        callValue(rt, cursor->setterOf(*p), base, {v});
                        return;
                    }
                    break;
                }
            }
            throwError(rt, ErrorCode::TypeError, "cannot add a property to a host object");
        }
        return;  // primitives silently ignore writes, as in sloppy mode
    }

    // An accessor anywhere on the prototype chain wins over a plain own write.
    for (DynObject* cursor = obj; cursor; cursor = cursor->proto.get()) {
        if (cursor->fnKindIsProxy()) {
            proxySet(rt, asProxy(cursor), key, v, base);
            return;
        }
        if (cursor->cls == ObjClass::TypedArray) {
            auto* ta = static_cast<DynTypedArray*>(cursor);
            double index = 0;
            if (numericKey(key, index)) {
                if (cursor == obj) {
                    typedArraySet(rt, ta, index, v);
                    return;
                }
                // Reached by inheritance: nothing happens unless there is such an element, and then it is the
                // receiver's own property that gets written.
                if (!typedArrayHas(ta, index))
                    return;
                break;
            }
        }
        const Prop* p = cursor->findOwn(key);
        if (!p)
            continue;
        if (cursor->accessorOf(*p)) {
            const Value setter = cursor->setterOf(*p);
            if (setter.isUndefined())
                return;
            callValue(rt, setter, base, {v});
            return;
        }
        if (cursor == obj) {
            if (!(obj->attrsOf(*p) & PropWritable))
                return;
            obj->findOwn(key)->value = v;
            return;
        }
        break;
    }

    if (obj->cls == ObjClass::Array) {
        auto* arr = static_cast<DynArray*>(obj);
        if (key == rt.atom.length.get()) {
            const double n = toNumber(rt, v);
            if (n < 0 || n != std::floor(n) || n > 4294967295.0)
                throwError(rt, ErrorCode::RangeError, "invalid array length");
            if (obj->frozen)
                return;
            if (!rt.ctx.options.memoryLimit && n > double(kMaxDenseArrayLength))
                throwError(rt, ErrorCode::RangeError, "arrays are dense; this length is too large");
            arr->resize(std::size_t(n));
            return;
        }
        if (key->kind == CellKind::String) {
            std::uint32_t index = 0;
            if (arrayIndexOf(static_cast<StringCell*>(key)->text(), index)) {
                if (obj->frozen || (index >= arr->size() && !obj->extensible))
                    return;
                if (index >= arr->size()) {
                    if (index > arr->size() + (1u << 22))
                        throwError(rt, ErrorCode::RangeError,
                                   "arrays are dense; this index would leave too large a gap");
                    arr->resize(index + 1);
                }
                arr->put(index, v);
                return;
            }
        }
    }

    if (obj->frozen || !obj->extensible)
        return;
    obj->putOwn(key, v);
}

// --------------------------------------------------------- inline caches ---

namespace {

/// The site of an access, the first access to a property in this function making the table.
inline PropertySite* siteOf(const FunctionCode& code, std::uint32_t site) noexcept
{
    if (site == kNoSite)
        return nullptr;
    if (code.sites.empty())
        code.sites.assign(code.siteCount, PropertySite());
    return &code.sites[site];
}

/// A read that missed: do it the long way and teach the site, when what it finds will stay where it is.
Value getPropertyLearning(Runtime& rt, const Value& base, GCObject* key, PropertySite* site)
{
    DynObject* obj = asObject(base);
    if (!site || !obj || !obj->shape || key->kind != CellKind::String) {
        if (site)
            ++site->misses;
        return getProperty(rt, base, key);
    }
    // An array answers `length` and its indices itself, and a string object its `length`, ahead of any table. Any other
    // name is in the tables, an array's included: `Array.prototype` is one, and where `push` and `map` are.
    std::uint32_t index = 0;
    const bool answeredApart = key == rt.atom.length.get() || arrayIndexOf(static_cast<StringCell*>(key)->text(), index);
    // A typed array anywhere on the way answers for anything that reads as a number.
    double numeric = 0;
    if (numericKey(key, numeric)) {
        for (DynObject* cursor = obj; cursor; cursor = cursor->proto.get()) {
            if (cursor->cls == ObjClass::TypedArray)
                return getProperty(rt, base, key);
        }
    }
    if ((obj->cls == ObjClass::StringObject || obj->cls == ObjClass::Array) && answeredApart)
        return getProperty(rt, base, key);
    for (DynObject* cursor = obj; cursor; cursor = cursor->proto.get()) {
        if (cursor != obj && answeredApart && (cursor->cls == ObjClass::StringObject || cursor->cls == ObjClass::Array))
            break;
        const Prop* p = cursor->findOwn(key);
        if (!p)
            continue;
        const std::uint32_t at = std::uint32_t(p - cursor->props.data());
        if (cursor->accessorOf(*p)) {
            // A getter that is a function, called with the receiver each time; the property itself stays put.
            if (!p->value.isObject() || cursor == obj)
                break;
            site->shape = obj->shape;
            site->holder = cursor;
            site->addTo = nullptr;
            site->getter = true;
            site->index = at;
            site->epoch = rt.ctx.protoEpoch;
            site->moreCount = 0;
            const Value getter = p->value;
            return callValue(rt, getter, base, {});
        }
        if (site->plainRead() && obj->shape != site->shape && site->moreFind(obj->shape) < 0 &&
            site->moreCount < PropertySite::kMoreShapes) {
            // One more receiver shape. One whose property is up the chain needs every such entry to share an epoch.
            DynObject* where = cursor == obj ? nullptr : cursor;
            const bool sharesEpoch = !site->anyHolder() || site->epoch == rt.ctx.protoEpoch;
            if (!where || sharesEpoch) {
                if (where && !site->anyHolder())
                    site->epoch = rt.ctx.protoEpoch;
                site->addShape(obj->shape, at, where);
                return p->value;
            }
        }
        site->shape = obj->shape;
        site->holder = cursor == obj ? nullptr : cursor;
        site->addTo = nullptr;
        site->getter = false;
        site->index = at;
        site->epoch = rt.ctx.protoEpoch;
        site->moreCount = 0;
        return p->value;
    }
    ++site->misses;
    return getProperty(rt, base, key);
}

inline Value getPropertySiteOnce(Runtime& rt, const Value& base, GCObject* key, PropertySite* site)
{
    if (site && base.type() == Type::Object && detail::Access::ptr(base)->kind == CellKind::Object) {
        auto* obj = static_cast<DynObject*>(detail::Access::ptr(base));
        if (obj->shape && obj->shape == site->shape) {
            if (!site->holder)
                return obj->props[site->index].value;
            if (site->epoch == rt.ctx.protoEpoch) {
                if (site->getter) {
                    const Value getter = site->holder->props[site->index].value;
                    return callValue(rt, getter, base, {});
                }
                return site->holder->props[site->index].value;
            }
        } else if (obj->shape && site->moreCount) {
            const int i = site->moreFind(obj->shape);
            if (i >= 0) {
                if (DynObject* where = site->moreHolder[i]) {
                    if (site->epoch == rt.ctx.protoEpoch)
                        return where->props[site->moreIndex[i]].value;
                } else {
                    return obj->props[site->moreIndex[i]].value;
                }
            }
        }
    }
    return getPropertyLearning(rt, base, key, site);
}

/// `base.key`, with what the site knows; the site remembers what kind of value it was, for native code to count on.
inline Value getPropertySite(Runtime& rt, const FunctionCode& code, const Value& base, GCObject* key, std::uint32_t siteIndex)
{
    PropertySite* site = siteOf(code, siteIndex);
    Value v = getPropertySiteOnce(rt, base, key, site);
    if (site)
        site->see(v);
    return v;
}

void setPropertyLearning(Runtime& rt, const Value& base, GCObject* key, const Value& v, PropertySite* site)
{
    DynObject* obj = asObject(base);
    Shape* const before = obj ? obj->shape : nullptr;
    if (site)
        ++site->misses;
    if (!site || !before || key->kind != CellKind::String || obj->cls == ObjClass::Array ||
        obj->cls == ObjClass::StringObject || obj->cls == ObjClass::TypedArray) {
        setProperty(rt, base, key, v);
        return;
    }
    if (const Prop* own = obj->findOwn(key)) {
        if ((obj->attrsOf(*own) & (PropAccessor | PropWritable)) == PropWritable) {
            if (site->shape) {
                site->addStore(before, std::uint32_t(own - obj->props.data()), nullptr);
                setProperty(rt, base, key, v);
                return;
            }
            site->getter = false;
            site->moreCount = 0;
            site->shape = before;
            site->holder = nullptr;
            site->addTo = nullptr;
            site->index = std::uint32_t(own - obj->props.data());
        }
        setProperty(rt, base, key, v);
        return;
    }
    const std::size_t count = obj->props.size();
    setProperty(rt, base, key, v);
    // It went on the end of the table, with the plain attributes: the same will happen for this shape as long as nothing
    // on the prototype chain changes, since nothing there has a setter for it now.
    if (obj->shape && obj->shape != before && obj->props.size() == count + 1 && obj->keyAt(count) == key &&
        obj->attrsAt(count) == PropDefault) {
        // Objects whose fields arrive in different orders: `a.left = x` where `a` may or may not have its `right` yet.
        const bool adds = site->anyAdds();
        if (site->shape && before != site->shape && (!adds || site->epoch == rt.ctx.protoEpoch)) {
            site->epoch = rt.ctx.protoEpoch;
            site->addStore(before, std::uint32_t(count), obj->shape);
            return;
        }
        site->shape = before;
        site->holder = nullptr;
        site->getter = false;
        site->moreCount = 0;
        site->addTo = obj->shape;
        site->epoch = rt.ctx.protoEpoch;
    }
}

/// `base.key = v`, with what the site knows.
inline void setPropertySite(Runtime& rt, const FunctionCode& code, const Value& base, GCObject* key, const Value& v,
                            std::uint32_t siteIndex)
{
    PropertySite* site = siteOf(code, siteIndex);
    if (site && base.type() == Type::Object && detail::Access::ptr(base)->kind == CellKind::Object) {
        auto* obj = static_cast<DynObject*>(detail::Access::ptr(base));
        if (obj->shape && obj->shape == site->shape) {
            if (!site->addTo) {
                obj->props[site->index].value = v;
                return;
            }
            if (site->epoch == rt.ctx.protoEpoch) {
                obj->appendKnown(v, site->addTo);
                return;
            }
        } else if (obj->shape && site->moreCount) {
            const int i = site->moreFind(obj->shape);
            if (i >= 0) {
                if (Shape* to = site->moreAddTo[i]) {
                    if (site->epoch == rt.ctx.protoEpoch) {
                        obj->appendKnown(v, to);
                        return;
                    }
                } else {
                    obj->props[site->moreIndex[i]].value = v;
                    return;
                }
            }
        }
    }
    setPropertyLearning(rt, base, key, v, site);
}

}  // namespace

bool hasProperty(Runtime& rt, const Value& base, GCObject* key)
{
    if (base.isString()) {
        StringCell* s = asString(base);
        std::uint32_t index = 0;
        if (key->kind == CellKind::String &&
            arrayIndexOf(static_cast<StringCell*>(key)->text(), index))
            return index < s->text().size();
    }
    DynObject* obj = asObject(base);
    if (!obj)
        obj = hostProtoOf(rt, base);
    if (!obj) {
        if (base.isNullish())
            throwError(rt, ErrorCode::TypeError, "'in' needs an object on the right");
        return false;
    }
    for (DynObject* cursor = obj; cursor; cursor = cursor->proto.get()) {
        if (cursor->fnKindIsProxy())
            return proxyHas(rt, asProxy(cursor), key);
        if (cursor->cls == ObjClass::StringObject && key->kind == CellKind::String) {
            if (StringCell* text = asString(static_cast<DynWrapper*>(cursor)->primitive)) {
                std::uint32_t index = 0;
                if (key == rt.atom.length.get() || (arrayIndexOf(static_cast<StringCell*>(key)->text(), index) && index < text->text().size()))
                    return true;
            }
        }
        if (cursor->cls == ObjClass::Array) {
            auto* arr = static_cast<DynArray*>(cursor);
            std::uint32_t index = 0;
            if (arrayElementKey(arr, key, index))
                return true;
            if (key == rt.atom.length.get())
                return true;
        }
        if (cursor->cls == ObjClass::TypedArray) {
            double index = 0;
            if (numericKey(key, index))
                return typedArrayHas(static_cast<DynTypedArray*>(cursor), index);
        }
        if (cursor->findOwn(key))
            return true;
    }
    return false;
}

bool deleteProperty(Runtime& rt, const Value& base, GCObject* key)
{
    DynObject* obj = asObject(base);
    if (!obj)
        return true;
    if (obj->fnKindIsProxy())
        return proxyDelete(rt, asProxy(obj), key);
    if (obj->cls == ObjClass::Array && key->kind == CellKind::String) {
        auto* arr = static_cast<DynArray*>(obj);
        std::uint32_t index = 0;
        if (arrayIndexOf(static_cast<StringCell*>(key)->text(), index)) {
            if (index < arr->size() && arr->hasElement(index)) {
                if (obj->frozen)
                    return false;
                arr->makeHole(index);
                return true;
            }
            if (!obj->findOwn(key))
                return true;
        }
    }
    if (obj->cls == ObjClass::TypedArray) {
        double index = 0;
        if (numericKey(key, index))
            return !typedArrayHas(static_cast<DynTypedArray*>(obj), index);
    }
    if (obj->frozen)
        return false;
    return obj->deleteOwn(key);
}

Vec<Value> ownKeys(Runtime& rt, DynObject* obj, bool enumerableOnly,
                           bool includeSymbols)
{
    Vec<Value> keys;
    if (obj->fnKindIsProxy()) {
        for (const Value& k : ownPropertyKeys(rt, obj)) {
            if (k.isSymbol() && !includeSymbols)
                continue;
            if (enumerableOnly) {
                Ref<GCObject> key = toPropertyKey(rt, k);
                Descriptor d;
                if (!getOwnProperty(rt, obj, key.get(), d) || !d.enumerable)
                    continue;
            }
            keys.push_back(k);
        }
        return keys;
    }
    if (obj->cls == ObjClass::Array) {
        auto* arr = static_cast<DynArray*>(obj);
        for (std::uint32_t i = 0; i < arr->size(); ++i) {
            if (arr->hasElement(i))
                keys.push_back(rt.str(numberToString(i)));
        }
        // `length` is an own property, only not an enumerable one.
        if (!enumerableOnly)
            keys.push_back(detail::Access::make(Type::String, rt.atom.length.get()));
    }
    if (obj->cls == ObjClass::StringObject) {
        auto* w = static_cast<DynWrapper*>(obj);
        if (StringCell* s = asString(w->primitive)) {
            for (std::uint32_t i = 0; i < s->text().size(); ++i)
                keys.push_back(rt.str(numberToString(i)));
            if (!enumerableOnly)
                keys.push_back(detail::Access::make(Type::String, rt.atom.length.get()));
        }
    }
    if (obj->cls == ObjClass::TypedArray) {
        const std::size_t count = static_cast<DynTypedArray*>(obj)->length;
        for (std::size_t i = 0; i < count; ++i)
            keys.push_back(rt.str(numberToString(double(i))));
    }
    for (std::size_t i = 0; i < obj->props.size(); ++i) {
        if (enumerableOnly && !(obj->attrsAt(i) & PropEnumerable))
            continue;
        GCObject* const key = obj->keyAt(i);
        if (key->kind == CellKind::Symbol) {
            if (includeSymbols && !static_cast<SymbolCell*>(key)->isPrivate)
                keys.push_back(detail::Access::make(Type::Symbol, key));
            continue;
        }
        keys.push_back(detail::Access::make(Type::String, key));
    }
    // Names that are array indexes come first, in order, whenever they were added: an array's own elements among them,
    // and any index it keeps as a named property.
    if (obj->cls != ObjClass::Array || !obj->props.empty()) {
        const auto indexOf = [](const Value& k, double& out) {
            if (k.type() != Type::String)
                return false;
            const StringView text = static_cast<StringCell*>(detail::Access::ptr(k))->text();
            if (text.empty() || text.size() > 10 || (text[0] == u'0' && text.size() > 1))
                return false;
            double n = 0;
            for (const Char c : text) {
                if (c < u'0' || c > u'9')
                    return false;
                n = n * 10 + double(c - u'0');
            }
            out = n;
            return n < 4294967295.0;
        };
        std::vector<std::pair<double, Value>> numbered;
        Vec<Value> named;
        for (const Value& k : keys) {
            double n = 0;
            if (indexOf(k, n))
                numbered.emplace_back(n, k);
            else
                named.push_back(k);
        }
        if (!numbered.empty()) {
            std::stable_sort(numbered.begin(), numbered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            keys.clear();
            for (const auto& entry : numbered)
                keys.push_back(entry.second);
            keys.insert(keys.end(), named.begin(), named.end());
        }
    }
    return keys;
}

bool getElementByNumber(Runtime& rt, const Value& base, const Value& key, Value& out)
{
    if (!key.isDouble())
        return false;
    DynObject* o = asObject(base);
    if (!o)
        return false;
    const double d = detail::Access::r8(key);
    if (o->cls == ObjClass::TypedArray) {
        auto* ta = static_cast<DynTypedArray*>(o);
        if (d >= 0 && d < double(ta->length) && d == std::floor(d)) {
            out = typedArrayGet(rt, ta, std::size_t(d));
            return true;
        }
        if (d < 0 || d != std::floor(d) || d >= double(ta->length)) {
            // Any number is answered by the array itself, as no element.
            if (!std::isnan(d) && !(d == 0 && std::signbit(d)) && d >= 0) {
                out = Value::undefined();
                return true;
            }
        }
        return false;
    }
    if (o->cls == ObjClass::Array) {
        auto* arr = static_cast<DynArray*>(o);
        if (d >= 0 && d < double(arr->size()) && d == std::floor(d) && arr->hasElement(std::size_t(d))) {
            out = arr->at(std::size_t(d));
            return true;
        }
    }
    return false;
}

bool setElementByNumber(Runtime& rt, const Value& base, const Value& key, const Value& v)
{
    if (!key.isDouble())
        return false;
    DynObject* o = asObject(base);
    if (!o)
        return false;
    const double d = detail::Access::r8(key);
    if (o->cls == ObjClass::TypedArray) {
        if (d >= 0 && d == std::floor(d)) {
            typedArraySet(rt, static_cast<DynTypedArray*>(o), d, v);
            return true;
        }
        return false;
    }
    if (o->cls == ObjClass::Array) {
        auto* arr = static_cast<DynArray*>(o);
        if (d >= 0 && d < double(arr->size()) && d == std::floor(d) && arr->hasElement(std::size_t(d)) && !arr->frozen && arr->props.empty()) {
            arr->put(std::size_t(d), v);
            return true;
        }
    }
    return false;
}

Value getIndex(Runtime& rt, const Value& base, std::uint32_t i)
{
    if (DynArray* arr = asArray(base)) {
        if (i < arr->size())
            return arr->at(i);
        return Value::undefined();
    }
    if (DynObject* o = asObject(base); o && o->cls == ObjClass::TypedArray) {
        auto* ta = static_cast<DynTypedArray*>(o);
        return i < ta->length ? typedArrayGet(rt, ta, i) : Value::undefined();
    }
    return getProperty(rt, base, rt.intern(numberToString(i)));
}

void setIndex(Runtime& rt, const Value& base, std::uint32_t i, const Value& v)
{
    if (DynArray* arr = asArray(base)) {
        if (i < arr->size()) {
            arr->put(i, v);
            return;
        }
    }
    setProperty(rt, base, rt.intern(numberToString(i)), v);
}

std::uint32_t lengthOf(Runtime& rt, const Value& v)
{
    if (DynArray* arr = asArray(v))
        return std::uint32_t(arr->size());
    if (StringCell* s = asString(v))
        return std::uint32_t(s->text().size());
    const double n = toNumber(rt, getProperty(rt, v, rt.atom.length.get()));
    if (std::isnan(n) || n <= 0)
        return 0;
    return std::uint32_t(std::min(n, 4294967295.0));
}

// ----------------------------------------------------------- operators ---

Value add(Runtime& rt, const Value& a, const Value& b)
{
    const Value pa = toPrimitive(rt, a, 0);
    const Value pb = toPrimitive(rt, b, 0);
    if (pa.isString() || pb.isString())
    {
        if (pa.isString() && pb.isString())
            return rt.ctx.concat(asString(pa)->text(), asString(pb)->text());
        return rt.str(toStringValue(rt, pa) + toStringValue(rt, pb));
    }
    if (pa.isBigInt() || pb.isBigInt())
        return numericSlow(rt, bigint::BinaryOp::Add, pa, pb);
    return Value(toNumber(rt, pa) + toNumber(rt, pb));
}

int compare(Runtime& rt, const Value& a, const Value& b)
{
    const Value pa = toPrimitive(rt, a, 1);
    const Value pb = toPrimitive(rt, b, 1);
    if (pa.isString() && pb.isString()) {
        const StringView x = asString(pa)->text();
        const StringView y = asString(pb)->text();
        if (x < y)
            return -1;
        return x == y ? 0 : 1;
    }
    if (pa.isBigInt() || pb.isBigInt()) {
        if (pa.isBigInt() && pb.isBigInt())
            return bigint::compare(bigint::cellOf(pa), bigint::cellOf(pb));
        if (pa.isBigInt() && pb.isString()) {
            Value parsed;
            if (!bigint::parseString(rt, asString(pb)->text(), parsed))
                return 2;
            return bigint::compare(bigint::cellOf(pa), bigint::cellOf(parsed));
        }
        if (pa.isString()) {
            Value parsed;
            if (!bigint::parseString(rt, asString(pa)->text(), parsed))
                return 2;
            return bigint::compare(bigint::cellOf(parsed), bigint::cellOf(pb));
        }
        if (pa.isBigInt())
            return bigint::compareDouble(bigint::cellOf(pa), toNumber(rt, pb));
        const int order = bigint::compareDouble(bigint::cellOf(pb), toNumber(rt, pa));
        return order == 2 ? 2 : -order;
    }
    const double x = toNumber(rt, pa);
    const double y = toNumber(rt, pb);
    if (std::isnan(x) || std::isnan(y))
        return 2;
    if (x < y)
        return -1;
    return x == y ? 0 : 1;
}

bool looseEquals(Runtime& rt, const Value& a, const Value& b)
{
    if (a.type() == b.type() || (a.isNumber() && b.isNumber()))
        return strictEquals(a, b);
    if (a.isNullish() && b.isNullish())
        return true;
    if (a.isNullish() || b.isNullish())
        return false;
    if (a.isNumber() && b.isString())
        return numberOf(a) == stringToNumber(asString(b)->text());
    if (a.isString() && b.isNumber())
        return stringToNumber(asString(a)->text()) == numberOf(b);
    if (a.isBigInt() && b.isNumber())
        return bigint::compareDouble(bigint::cellOf(a), numberOf(b)) == 0;
    if (a.isNumber() && b.isBigInt())
        return bigint::compareDouble(bigint::cellOf(b), numberOf(a)) == 0;
    if (a.isBigInt() && b.isString()) {
        Value parsed;
        return bigint::parseString(rt, asString(b)->text(), parsed) && bigint::equal(bigint::cellOf(a), bigint::cellOf(parsed));
    }
    if (a.isString() && b.isBigInt())
        return looseEquals(rt, b, a);
    if (a.isBool())
        return looseEquals(rt, Value(boolOf(a) ? 1.0 : 0.0), b);
    if (b.isBool())
        return looseEquals(rt, a, Value(boolOf(b) ? 1.0 : 0.0));
    if (a.isObject() && !b.isObject() && !b.isSymbol())
        return looseEquals(rt, toPrimitive(rt, a, 0), b);
    if (b.isObject() && !a.isObject() && !a.isSymbol())
        return looseEquals(rt, a, toPrimitive(rt, b, 0));
    return false;
}

bool instanceOf(Runtime& rt, const Value& object, const Value& ctor)
{
    DynFunction* fn = asFunction(ctor);
    if (!fn)
        throwError(rt, ErrorCode::TypeError, "the right side of 'instanceof' is not callable");
    // A function straight off Function.prototype, with nothing on that chain defining `Symbol.hasInstance` since the
    // prototypes last changed, has no custom test to call.
    bool plain = false;
    if (fn->proto.get() == rt.functionProto.get() && !fn->findOwn(rt.sym.hasInstance.get())) {
        if (rt.hasInstanceAbsentAt != rt.ctx.protoEpoch) {
            bool found = false;
            for (DynObject* cursor = rt.functionProto.get(); cursor && !found; cursor = cursor->proto.get())
                found = cursor->findOwn(rt.sym.hasInstance.get()) != nullptr;
            if (!found)
                rt.hasInstanceAbsentAt = rt.ctx.protoEpoch;
        }
        plain = rt.hasInstanceAbsentAt == rt.ctx.protoEpoch;
    }
    if (!plain) {
        Value custom = getPropertyOfObject(rt, fn, rt.sym.hasInstance.get(), ctor);
        if (asFunction(custom))
            return toBoolean(callValue(rt, custom, ctor, {object}));
    }
    DynObject* obj = asObject(object);
    DynObject* start = obj ? obj->proto.get() : hostProtoOf(rt, object);
    if (!start)
        return false;
    const Prop* own = plain ? fn->findOwn(rt.atom.prototype.get()) : nullptr;
    Value proto = own && !fn->accessorOf(*own) ? own->value : getPropertyOfObject(rt, fn, rt.atom.prototype.get(), ctor);
    DynObject* target = asObject(proto);
    if (!target)
        throwError(rt, ErrorCode::TypeError, "the constructor has no object prototype");
    for (DynObject* cursor = start; cursor; cursor = cursor->proto.get()) {
        if (cursor == target)
            return true;
    }
    return false;
}

// --------------------------------------------------------------- calls ---

namespace {

Value execute(Runtime& rt, Frame& frame);

/// Push a frame, run it, pop it. `newTarget` is set for construction.
Value invokeBytecode(Runtime& rt, DynFunction* fn, const Value& thisValue,
                     const std::vector<Value>& args, DynFunction* newTarget)
{
    const FunctionCode& code = *fn->code;
#if SCRIPT_ENABLE_DYN_JIT
    if (code.jitEntry)
        return enterNative(rt, fn, code, thisValue, args.data(), args.size());
    if (!code.jitRefused && rt.ctx.options.jit.mode != JitOptions::Mode::Off &&
        (rt.ctx.options.jit.mode == JitOptions::Mode::Eager || ++code.jitHotness >= code.jitThreshold(rt.ctx.options.jit.threshold)) &&
        compileNative(rt, code))
        return enterNative(rt, fn, code, thisValue, args.data(), args.size());
#endif
#if SCRIPT_ENABLE_DYN_JIT
    // What native code compiled later may take the parameters to be.
    if (code.argsNotNumber != ~0u) {
        const std::size_t arity = std::min<std::size_t>(std::size_t(std::max(code.arity, 0)), 32);
        for (std::size_t i = 0; i < arity; ++i) {
            if (i >= args.size() || !args[i].isDouble()) {
                code.argsNotNumber |= 1u << i;
            } else {
                const double d = detail::Access::r8(args[i]);
                if (!(d >= -2147483648.0 && d <= 2147483647.0) || double(std::int32_t(d)) != d || (d == 0 && std::signbit(d)))
                    code.argsNotInt |= 1u << i;
            }
        }
    }
#endif
    Frame& frame = rt.acquireFrame(rt.frames.size());
    frame.begin(fn, &code, thisValue, newTarget);
    rt.frames.push_back(&frame);
    struct PopFrame
    {
        Runtime* rt;
        Frame* frame;
        ~PopFrame()
        {
            rt->frames.pop_back();
            frame->end();
        }
    } popper{&rt, &frame};

    const std::size_t plain = std::min<std::size_t>(
        code.hasRest ? std::size_t(code.arity) : args.size(), code.numLocals);
    for (std::size_t i = 0; i < plain && i < args.size(); ++i)
        frame.locals[i] = args[i];
    if (code.hasRest) {
        DynArray* rest = rt.newArray(args.size() > std::size_t(code.arity)
                                        ? args.size() - std::size_t(code.arity)
                                        : 0);
        for (std::size_t i = std::size_t(code.arity); i < args.size(); ++i)
            rest->append(args[i]);
        const std::size_t slot = std::size_t(code.arity);
        if (slot < frame.locals.size())
            frame.locals[slot] = detail::Access::adopt(Type::Object, rest);
        else
            detail::decRef(rest);
    }
    if (code.argumentsSlot != FunctionCode::kNoSlot) {
        Value v = rt.makeArray(args);
        if (code.argumentsBoxed) {
            auto* cell = rt.newCell<Box>();
            cell->value = v;
            frame.cells[code.argumentsSlot].reset(cell, true);
        } else {
            frame.locals[code.argumentsSlot] = v;
        }
    }

    return execute(rt, frame);
}

Value callNative(Runtime& rt, DynFunction* fn, const Value& thisValue,
                 const std::vector<Value>& args, bool isNew)
{
    CallInfo info = detail::Access::makeCallInfo(rt.ctx.owner, &thisValue, &args, isNew);
    ContextImpl::ActiveEngine active(rt.ctx, EngineKind::Dynamic);
    try {
        return fn->native(info);
    } catch (...) {
        rethrowAsScript(rt);
    }
}

struct DepthGuard
{
    Runtime& rt;
    explicit DepthGuard(Runtime& r) : rt(r)
    {
        // The depth limit is a count of frames; the thread's own stack may be smaller than the limit assumes (a worker
        // thread with 256 KiB, say), so the real headroom is checked too.
        if (++rt.ctx.callDepth > rt.ctx.options.stackLimit || nativeStackLow()) {
            --rt.ctx.callDepth;
            throwError(rt, ErrorCode::StackOverflow, "maximum call depth exceeded");
        }
    }
    ~DepthGuard() { --rt.ctx.callDepth; }
};

Value callInternal(Runtime& rt, DynFunction* fn, const Value& thisValue,
                   const std::vector<Value>& args, bool isNew, DynFunction* newTarget)
{
    DepthGuard guard(rt);
    switch (fn->fnKind) {
        case FnKind::Native:
            return callNative(rt, fn, thisValue, args, isNew);
        case FnKind::Proxy:
            return proxyCall(rt, static_cast<DynProxy*>(fn), thisValue, args);
        case FnKind::Bound: {
            ContextImpl::ArgBuffer buffer(rt.ctx);
            std::vector<Value>& merged = buffer.args;
            merged.assign(fn->boundArgs.begin(), fn->boundArgs.end());
            merged.insert(merged.end(), args.begin(), args.end());
            DynFunction* target = fn->boundTarget.get();
            return callInternal(rt, target, isNew ? thisValue : fn->boundThis, merged, isNew,
                                newTarget);
        }
        default:
            return invokeBytecode(rt, fn, thisValue, args, newTarget);
    }
}

}  // namespace

Value callValue(Runtime& rt, const Value& callee, const Value& thisValue,
                const std::vector<Value>& args)
{
    DynFunction* fn = asFunction(callee);
    if (!fn)
        throwError(rt, ErrorCode::TypeError, toUtf8(toStringValue(rt, callee)) +
                                                 " is not a function");
    if (fn->isClassConstructor)
        throwError(rt, ErrorCode::TypeError,
                   "class constructor " + toUtf8(fn->name) + " must be called with 'new'");
    return callInternal(rt, fn, thisValue, args, false, nullptr);
}

/// Whether `new` can be used on this.
bool isConstructor(Runtime& rt, DynFunction* fn)
{
    if (fn->fnKind == FnKind::Bound)
        return isConstructor(rt, fn->boundTarget.get());
    if (fn->fnKind == FnKind::Proxy)
        return static_cast<DynProxy*>(fn)->constructible;
    if (fn->isArrow || fn == rt.functionProto.get())
        return false;
    if (fn->fnKind == FnKind::Native)
        return fn->isNativeConstructor;
    return !(fn->code && (fn->code->isGenerator || fn->code->isAsync || fn->code->isMethod));
}

static void runFieldInit(Runtime& rt, DynFunction* ctor, const Value& self)
{
    if (ctor->fieldInit)
        callInternal(rt, ctor->fieldInit.get(), self, {}, false, nullptr);
}

Value constructValue(Runtime& rt, const Value& callee, const std::vector<Value>& args, DynFunction* newTarget)
{
    DynFunction* fn = asFunction(callee);
    if (!fn || !isConstructor(rt, fn))
        throwError(rt, ErrorCode::TypeError, (fn && !fn->name.empty() ? toUtf8(fn->name) : std::string("value")) + " is not a constructor");
    if (!newTarget)
        newTarget = fn;
    if (fn->fnKind == FnKind::Bound)
        return constructValue(rt, detail::Access::make(Type::Object, fn->boundTarget.get()), args,
                              newTarget == fn ? fn->boundTarget.get() : newTarget);
    if (fn->fnKind == FnKind::Proxy)
        return proxyConstruct(rt, static_cast<DynProxy*>(fn), args, newTarget == fn ? nullptr : newTarget);
    // A derived class has no object until `super()` gives it one.
    if (fn->isDerivedConstructor)
        return callInternal(rt, fn, Value::undefined(), args, true, newTarget);

    const Value target = detail::Access::make(Type::Object, newTarget);
    Value protoValue = getPropertyOfObject(rt, newTarget, rt.atom.prototype.get(), target);
    DynObject* proto = asObject(protoValue);
    // A built-in whose new target has no usable prototype keeps the one it gives its own kind.
    const bool keepNativeProto = !proto;
    if (!proto)
        proto = rt.objectProto.get();
    // Room for what objects made the same way have gone on to hold.
    Shape* root = rt.rootShapeFor(proto);
    DynObject* self = rt.newObjectSized(proto, root ? std::min<std::uint32_t>(root->expect, 32) : 0);
    const Value thisValue = detail::Access::adopt(Type::Object, self);
    runFieldInit(rt, fn, thisValue);

    const Value result = callInternal(rt, fn, thisValue, args, true, newTarget);
    if (!result.isObject())
        return thisValue;
    // A built-in makes its own kind of object, which a class extending it inherits from the class.
    if (fn->fnKind == FnKind::Native && newTarget != fn && !keepNativeProto) {
        if (DynObject* made = asObject(result))
            made->setProto(proto);
    }
    return result;
}

#if !SCRIPT_ENABLE_DYN_JIT
// Without native code for the dynamic languages a callback loop is a plain call each time.
struct CallbackLoop::Session
{};

CallbackLoop::CallbackLoop(Runtime& rt, const Value& callee) : rt_(rt), callee_(callee) {}

CallbackLoop::~CallbackLoop() = default;

Value CallbackLoop::slow(const Value& thisValue, std::initializer_list<Value> args)
{
    ContextImpl::ArgBuffer buffer(rt_.ctx);
    buffer.args.assign(args.begin(), args.end());
    return callValue(rt_, callee_, thisValue, buffer.args);
}

Value CallbackLoop::operator()(const Value& thisValue, std::initializer_list<Value> args)
{
    return slow(thisValue, args);
}

Value CallbackLoop::ofTwoNumbers(double a, double b)
{
    return (*this)(Value::undefined(), {Value(a), Value(b)});
}

double CallbackLoop::numberOfTwoNumbers(double a, double b)
{
    return toNumber(rt_, ofTwoNumbers(a, b));
}

Value CallbackLoop::withArgs(const Value& thisValue, const Value* args, std::size_t count)
{
    ContextImpl::ArgBuffer buffer(rt_.ctx);
    buffer.args.assign(args, args + count);
    return callValue(rt_, callee_, thisValue, buffer.args);
}
#endif

Value invokeMethod(Runtime& rt, const Value& object, GCObject* name,
                   const std::vector<Value>& args)
{
    const Value fn = getProperty(rt, object, name);
    return callValue(rt, fn, object, args);
}

// ------------------------------------------------------------ closures ---

/// A generator's or async function's object differs from a plain function's in what it inherits, and in what `prototype` it
/// has. False if it is neither.
static bool setUpCoroutineFunction(Runtime& rt, DynFunction* fn, const FunctionCode& code)
{
    if (!code.isGenerator && !code.isAsync)
        return false;
    if (!code.isGenerator) {
        fn->setProto(rt.asyncFunctionProto.get());
        return true;
    }
    fn->setProto(code.isAsync ? rt.asyncGeneratorFunctionProto.get() : rt.generatorFunctionProto.get());
    DynObject* proto = rt.newObject(code.isAsync ? rt.asyncGeneratorProto.get() : rt.generatorProto.get());
    fn->putOwn(rt.atom.prototype.get(), detail::Access::adopt(Type::Object, proto), PropWritable);
    return true;
}

Value makeClosureForNative(Runtime& rt, const std::shared_ptr<const FunctionCode>& code, DynFunction* parent,
                           Box* const* boxes)
{
    DynFunction* fn = rt.newFunction();
    Value handle = detail::Access::adopt(Type::Object, fn);
    fn->fnKind = FnKind::Bytecode;
    fn->code = code;
    fn->name = code->wideName;
    fn->arity = code->arity;
    fn->isArrow = code->isArrow;
    fn->isClassConstructor = code->isClassConstructor;
    fn->isDerivedConstructor = code->isDerivedConstructor;
    fn->props.reserve(3);
    fn->upvalues.reserve(code->upvals.size());
    for (std::size_t i = 0; i < code->upvals.size(); ++i)
        fn->upvalues.emplace_back(boxes[i]);
    if (parent && code->isArrow)
        fn->homeObject = parent->homeObject;
    fn->putOwn(rt.atom.length.get(), Value(double(code->arity)), PropConfigurable);
    fn->putOwn(rt.atom.name.get(), rt.str(fn->name), PropConfigurable);
    if (!setUpCoroutineFunction(rt, fn, *code) && !code->isArrow && !code->isClassConstructor) {
        DynObject* proto = rt.newObject(rt.objectProto.get());
        Value protoValue = detail::Access::adopt(Type::Object, proto);
        proto->putOwn(rt.atom.constructor.get(), handle, PropWritable | PropConfigurable);
        fn->putOwn(rt.atom.prototype.get(), protoValue, PropWritable);
    }
    return handle;
}

Value makeClosure(Runtime& rt, const std::shared_ptr<const FunctionCode>& code, Frame* parent,
                  bool withPrototype)
{
    DynFunction* fn = rt.newFunction();
    fn->fnKind = FnKind::Bytecode;
    fn->code = code;
    fn->name = code->wideName;
    fn->arity = code->arity;
    fn->isArrow = code->isArrow;
    fn->isClassConstructor = code->isClassConstructor;
    fn->isDerivedConstructor = code->isDerivedConstructor;

    fn->props.reserve(3);
    fn->upvalues.reserve(code->upvals.size());
    for (const UpvalDesc& desc : code->upvals) {
        if (!parent) {
            fn->upvalues.emplace_back();
            continue;
        }
        if (desc.fromParentLocal)
            fn->upvalues.push_back(parent->cells[desc.index]);
        else
            fn->upvalues.push_back(parent->fn->upvalues[desc.index]);
    }
    if (parent && parent->fn && code->isArrow)
        fn->homeObject = parent->fn->homeObject;

    fn->putOwn(rt.atom.length.get(), Value(double(code->arity)), PropConfigurable);
    fn->putOwn(rt.atom.name.get(), rt.str(fn->name), PropConfigurable);
    if (!setUpCoroutineFunction(rt, fn, *code) && withPrototype && !code->isArrow && !code->isClassConstructor) {
        DynObject* proto = rt.newObject(rt.objectProto.get());
        Value protoValue = detail::Access::adopt(Type::Object, proto);
        proto->putOwn(rt.atom.constructor.get(), detail::Access::make(Type::Object, fn),
                      PropWritable | PropConfigurable);
        fn->putOwn(rt.atom.prototype.get(), protoValue, PropWritable);
    }
    return detail::Access::adopt(Type::Object, fn);
}

Value evalString(Runtime& rt, const Value& source, Frame* caller, const EvalScope* scope)
{
    if (!rt.ctx.options.runtimeCode)
        throwError(rt, ErrorCode::EvalError, "eval is disabled: the engine runs only the source its host gives it");
    StringCell* text = asString(source);
    if (!text)
        return source;
    std::shared_ptr<FunctionCode> code;
    try {
        const std::string utf8 = toUtf8(text->text());
        Parser parser(utf8, "<eval>", /*typescript=*/false);
        NodePtr program = parser.parseProgram();
        code = scope && caller ? compileEval(rt, *program, "<eval>", *scope) : compileProgram(rt, *program, "<eval>", /*sloppyThis=*/true);
    } catch (Exception&) {
        throw;
    } catch (const Error& e) {
        throwError(rt, e.code() == ErrorCode::CompileError ? ErrorCode::SyntaxError : e.code(), e.what());
    }
    rt.adoptEvalCode(code);
    if (!scope || !caller)
        return runProgram(rt, code);
    Value closure = makeClosure(rt, code, caller, /*withPrototype=*/false);
    return callValue(rt, closure, caller->thisValue, {});
}

Value runProgram(Runtime& rt, const std::shared_ptr<const FunctionCode>& code)
{
    // A program is never `new`ed, so it gets no prototype object: that object
    // and the function would form a cycle only the collector could free, one
    // per run, and a rule engine runs the same program millions of times.
    Value closure = makeClosure(rt, code, nullptr, /*withPrototype=*/false);
    DynFunction* fn = asFunction(closure);
    const Value globalValue = detail::Access::make(Type::Object, rt.globalObject.get());
    DepthGuard guard(rt);
    return invokeBytecode(rt, fn, globalValue, {}, nullptr);
}

// --------------------------------------------------------- iteration ---

Value getIterator(Runtime& rt, const Value& obj)
{
    const Value method = getProperty(rt, obj, rt.sym.iterator.get());
    if (!asFunction(method))
        throwError(rt, ErrorCode::TypeError,
                   toUtf8(toStringValue(rt, obj)) + " is not iterable");
    const Value iterator = callValue(rt, method, obj, {});
    if (!iterator.isObject())
        throwError(rt, ErrorCode::TypeError, "the iterator is not an object");
    return iterator;
}

bool iteratorStep(Runtime& rt, const Value& iterator, Value& out)
{
    // No result object to make and take apart when `next` is the one it started with.
    if (DynObject* o = asObject(iterator); o && o->cls == ObjClass::ArrayIterator && o->proto.get() == rt.arrayIteratorProto.get() && o->props.empty()) {
        const Prop* next = rt.arrayIteratorProto->findOwn(rt.atom.next.get());
        if (next && detail::Access::ptr(next->value) == rt.intrinsics[Runtime::ArrayIteratorNext].get())
            return arrayIteratorStep(rt, static_cast<DynArrayIterator*>(o), out);
    }
    const Value result = invokeMethod(rt, iterator, rt.atom.next.get(), {});
    if (!result.isObject())
        throwError(rt, ErrorCode::TypeError, "the iterator result is not an object");
    if (toBoolean(getProperty(rt, result, rt.atom.done.get())))
        return false;
    out = getProperty(rt, result, rt.atom.value.get());
    return true;
}

void iteratorClose(Runtime& rt, const Value& iterator)
{
    if (!iterator.isObject())
        return;
    const Value fn = getProperty(rt, iterator, rt.intern("return"));
    if (!asFunction(fn))
        return;
    try {
        callValue(rt, fn, iterator, {});
    } catch (const Exception&) {
        // A failure while closing must not mask the reason we are unwinding.
    }
}

Vec<Value> iterableToVector(Runtime& rt, const Value& iterable)
{
    if (DynArray* arr = asArray(iterable)) {
        // Fast path: a plain array with the untouched built-in iterator.
        if (arr->proto.get() == rt.arrayProto.get() && !arr->findOwn(rt.sym.iterator.get()))
        {
            Vec<Value> items;
            items.reserve(arr->size());
            for (std::size_t i = 0; i < arr->size(); ++i)
                items.push_back(arr->at(i));
            return items;
        }
    }
    Vec<Value> out;
    const Value iterator = getIterator(rt, iterable);
    Value item = Value::undefined();
    while (iteratorStep(rt, iterator, item)) {
        // An iterator is script, and may never finish. Each step is a short call that does not run long enough to
        // reach the interpreter's own poll, so this loop has to offer the interrupt handler and the memory limit its
        // chance itself; the vector it fills is not charged to any cell yet.
        checkLongLoop(rt, out.size(), out.size() * sizeof(Value) * 2);
        out.push_back(item);
    }
    return out;
}

// ------------------------------------------------- what native code needs ---

Value getPropertyForNative(Runtime& rt, const FunctionCode& code, const Value& base, std::uint32_t operand)
{
    return getPropertySite(rt, code, base, code.atoms[operand & kAtomMask].get(), operand >> kSiteShift);
}

void setPropertyForNative(Runtime& rt, const FunctionCode& code, const Value& base, std::uint32_t operand, const Value& v)
{
    setPropertySite(rt, code, base, code.atoms[operand & kAtomMask].get(), v, operand >> kSiteShift);
}

Value getGlobalForNative(Runtime& rt, GCObject* key)
{
    const Prop* p = rt.globalObject->findOwn(key);
    if (p && !rt.globalObject->accessorOf(*p))
        return p->value;
    const Value g = detail::Access::make(Type::Object, rt.globalObject.get());
    if (!p && !hasProperty(rt, g, key))
        throwError(rt, ErrorCode::ReferenceError, toUtf8(static_cast<StringCell*>(key)->text()) + " is not defined");
    return getProperty(rt, g, key);
}

void setGlobalForNative(Runtime& rt, GCObject* key, const Value& v)
{
    setProperty(rt, detail::Access::make(Type::Object, rt.globalObject.get()), key, v);
}

void throwValueForNative(Runtime& rt, const Value& v)
{
    throwValue(rt, v);
}

void classSetupForNative(Runtime& rt, bool hasSuper, const Value& superValue, const Value& ctorValue)
{
    DynFunction* ctor = asFunction(ctorValue);
    DynFunction* super = asFunction(superValue);
    if (hasSuper && !super && !superValue.isNull())
        throwError(rt, ErrorCode::TypeError, "a class can only extend a constructor or null");
    DynObject* protoParent = super ? asObject(getPropertyOfObject(rt, super, rt.atom.prototype.get(), superValue))
                                   : (hasSuper ? nullptr : rt.objectProto.get());
    DynObject* proto = rt.newObject(protoParent);
    Value protoValue = detail::Access::adopt(Type::Object, proto);
    proto->putOwn(rt.atom.constructor.get(), ctorValue, PropWritable | PropConfigurable);
    ctor->putOwn(rt.atom.prototype.get(), protoValue, 0);
    ctor->homeObject.reset(proto);
    ctor->isClassConstructor = true;
    // What Python's classes are not: see `Node::allocatesThis`.
    ctor->isDerivedConstructor = hasSuper && ctor->code && ctor->code->isDerivedConstructor;
    if (super)
        ctor->setProto(super);
}

void defineMemberForNative(Runtime& rt, int mode, DynObject* target, GCObject* key, const Value& v)
{
    (void)rt;
    if (mode == 3) {
        target->putOwn(key, v);
        return;
    }
    if (DynFunction* m = asFunction(v))
        m->homeObject.reset(target);
    if (mode == 0) {
        target->putOwn(key, v, PropWritable | PropConfigurable);
        return;
    }
    Prop* existing = target->findOwn(key);
    if (!existing || !target->accessorOf(*existing))
        existing = &target->putOwn(key, Value::undefined(), PropEnumerable | PropConfigurable | PropAccessor);
    if (mode == 1)
        existing->value = v;
    else
        target->setSetter(*existing, v);
}

DynArray* forInKeysForNative(Runtime& rt, const Value& src)
{
    DynArray* keys = rt.newArray();
    Value keysValue = detail::Access::adopt(Type::Object, keys);
    if (DynObject* obj = asObject(src)) {
        GcList seen;
        for (DynObject* cursor = obj; cursor; cursor = cursor->proto.get()) {
            for (const Value& k : ownKeys(rt, cursor, true, false)) {
                GCObject* atom = rt.intern(asString(k)->text());
                if (std::find(seen.begin(), seen.end(), atom) != seen.end())
                    continue;
                seen.push_back(atom);
                keys->append(k);
            }
        }
    } else if (StringCell* s = asString(src)) {
        for (std::uint32_t i = 0; i < s->text().size(); ++i)
            keys->append(rt.str(numberToString(i)));
    }
    detail::incRef(keys);
    return keys;
}

// ---------------------------------------------------------- dispatch ---

namespace {

bool findHandler(Frame& frame, const Value& exception)
{
    for (const Handler& h : frame.code->handlers) {
        if (frame.pc < h.start || frame.pc >= h.end)
            continue;
        frame.stack.resize(h.stackDepth);
        if (h.kind != HandlerKind::Finally) {
            frame.stack.push_back(exception);
        } else {
            frame.pendingException = exception;
            frame.subStack.push_back(kRethrow);
        }
        frame.pc = h.target;
        return true;
    }
    return false;
}

Value startCoroutine(Runtime& rt, Frame& frame, std::uint32_t pc);

/// `super(...)`: the constructor above `ctor`, made to make `this`, which then gets `ctor`'s fields.
Value superCall(Runtime& rt, DynFunction* ctor, DynFunction* newTarget, const std::vector<Value>& args, const Value& thisNow)
{
    DynObject* above = ctor->proto.get();
    DynFunction* super = above ? asFunction(detail::Access::make(Type::Object, above)) : nullptr;
    if (!super || !isConstructor(rt, super))
        throwError(rt, ErrorCode::TypeError, "the super class is not a constructor");
    Value made = constructValue(rt, detail::Access::make(Type::Object, super), args, newTarget);
    if (!thisNow.isUndefined())
        throwError(rt, ErrorCode::ReferenceError, "super() has already been called");
    return made;
}

Value execute(Runtime& rt, Frame& frame)
{
    const FunctionCode& code = *frame.code;
    const std::uint8_t* c = code.code.data();
    // Zero for a fresh call; part way through for one native code handed back.
    std::uint32_t pc = frame.pc;
    std::uint32_t ticks = 0;
    Vec<Value>& sp = frame.stack;
#if SCRIPT_ENABLE_DYN_JIT
    // Back edges until this function is worth compiling in the middle of running it.
    std::uint32_t backEdges =
        !code.jitRefused && rt.jitBackend && rt.ctx.options.jit.mode != JitOptions::Mode::Off ? code.jitThreshold(rt.ctx.options.jit.threshold) : 0;
    const auto loopBack = [&](std::uint32_t target, Value& result) {
        if (backEdges == 0 || --backEdges != 0)
            return false;
        frame.pc = target;
        return enterNativeAtLoop(rt, frame, target, result);
    };
#else
    const auto loopBack = [](std::uint32_t, Value&) { return false; };
#endif

    auto pop = [&sp]() -> Value {
        Value v = std::move(sp.back());
        sp.pop_back();
        return v;
    };
    auto operandAt = [&](std::uint32_t at) { return readOperand(code.code, at); };
    /// The arguments of a call: either `argc` values on the stack or, for the
    /// spread forms, one array holding them all.
    auto popArgs = [&](std::vector<Value>& args, bool spread) {
        if (spread) {
            DynArray* items = asArray(sp.back());
            args.clear();
            args.reserve(items->size());
            for (std::size_t i = 0; i < items->size(); ++i)
                args.push_back(items->at(i));
            sp.pop_back();
        } else {
            const std::uint32_t argc = operandAt(pc);
            pc += 4;
            args.assign(std::make_move_iterator(sp.end() - argc), std::make_move_iterator(sp.end()));
            sp.resize(sp.size() - argc);
        }
    };
    auto fastIndex = [](const Value& base, const Value& key, DynArray*& arr, std::uint32_t& index) {
        if (key.type() != Type::Double)
            return false;
        DynObject* obj = asObject(base);
        if (!obj || obj->cls != ObjClass::Array)
            return false;
        const double d = detail::Access::r8(key);
        if (!(d >= 0 && d < 4294967295.0))
            return false;
        index = std::uint32_t(d);
        if (double(index) != d)
            return false;
        arr = static_cast<DynArray*>(obj);
        // A hole may stand for a property of that name, or one up the chain.
        return index < arr->size() && arr->hasElement(index);
    };
    /// Pop two operands, convert both to numbers left to right, push `f(a, b)`.
    auto numeric = [&](bigint::BinaryOp big, auto f) {
        {
            const std::size_t n = sp.size();
            Value& fa = sp[n - 2];
            Value& fb = sp[n - 1];
            if (fa.type() == Type::Double && fb.type() == Type::Double) {
                const double r = f(detail::Access::r8(fa), detail::Access::r8(fb));
                sp.pop_back();
                fa = Value(r);
                return;
            }
        }
        Value b = pop();
        Value a = pop();
        const Value x = toNumeric(rt, a);
        const Value y = toNumeric(rt, b);
        if (x.isBigInt() || y.isBigInt()) {
            sp.push_back(numericSlow(rt, big, x, y));
            return;
        }
        sp.emplace_back(f(numberOf(x), numberOf(y)));
    };

    for (;;) {
        try {
            // Resumed by `throw`, or by an `await` of what was rejected: thrown from where it stopped.
            if (frame.coroutine && frame.coroutine->throwOnResume) {
                frame.coroutine->throwOnResume = false;
                frame.pc = pc - 1;
                const Value thrown = std::move(frame.coroutine->received);
                frame.coroutine->received = Value::undefined();
                throwValue(rt, thrown);
            }
            for (;;) {
                frame.pc = pc;
                if (++ticks >= kInterruptInterval) {
                    ticks = 0;
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
                    if (!code.interpreted++ && std::getenv("SCRIPT_INTERP_PROFILE"))
                        rt.interpretedCodes.push_back(&code);
#endif
                    if (rt.ctx.interruptAsked())
                        throwError(rt, ErrorCode::Interrupted, "execution was interrupted");
                    // A loop that only grows containers it already owns never
                    // reaches cell allocation, so budgets are checked here too.
                    rt.maybeCollect();
                }
                const Op op = Op(c[pc++]);
                switch (op) {
                    case Op::Nop:
                        break;
                    case Op::PushUndefined:
                        sp.push_back(Value::undefined());
                        break;
                    case Op::PushNull:
                        sp.emplace_back(nullptr);
                        break;
                    case Op::PushTrue:
                        sp.emplace_back(true);
                        break;
                    case Op::PushFalse:
                        sp.emplace_back(false);
                        break;
                    case Op::PushConst:
                        sp.push_back(code.consts[operandAt(pc)]);
                        pc += 4;
                        break;
                    case Op::PushThis:
                        if (code.sloppyThis && frame.thisValue.isNullish())
                            sp.push_back(detail::Access::make(Type::Object, rt.globalObject.get()));
                        else
                            sp.push_back(frame.thisValue);
                        break;
                    case Op::PushCallee:
                        sp.push_back(frame.fn ? detail::Access::make(Type::Object, frame.fn)
                                              : Value::undefined());
                        break;
                    case Op::Pop:
                        sp.pop_back();
                        break;
                    case Op::PopN:
                        sp.resize(sp.size() - operandAt(pc));
                        pc += 4;
                        break;
                    case Op::Dup: {
                        Value top = sp.back();
                        sp.push_back(std::move(top));
                        break;
                    }
                    case Op::Dup2: {
                        Value low = sp[sp.size() - 2];
                        Value high = sp.back();
                        sp.push_back(std::move(low));
                        sp.push_back(std::move(high));
                        break;
                    }
                    case Op::Swap:
                        std::swap(sp[sp.size() - 1], sp[sp.size() - 2]);
                        break;
                    case Op::Rot3: {
                        Value top = pop();
                        Value mid = pop();
                        Value low = pop();
                        sp.push_back(std::move(top));
                        sp.push_back(std::move(low));
                        sp.push_back(std::move(mid));
                        break;
                    }

                    case Op::GetLocal:
                        sp.push_back(frame.locals[operandAt(pc)]);
                        pc += 4;
                        break;
                    case Op::SetLocal:
                        frame.locals[operandAt(pc)] = pop();
                        pc += 4;
                        break;
                    case Op::GetCell: {
                        const Ref<Box>& cell = frame.cells[operandAt(pc)];
                        sp.push_back(cell ? cell->value : Value::undefined());
                        pc += 4;
                        break;
                    }
                    case Op::SetCell: {
                        const Ref<Box>& cell = frame.cells[operandAt(pc)];
                        if (cell)
                            cell->value = pop();
                        else
                            sp.pop_back();
                        pc += 4;
                        break;
                    }
                    case Op::NewCell:
                    case Op::RenewCell: {
                        const std::uint32_t slot = operandAt(pc);
                        auto* cell = rt.newCell<Box>();
                        cell->value = op == Op::RenewCell && frame.cells[slot]
                                          ? frame.cells[slot]->value
                                          : Value::undefined();
                        frame.cells[slot].reset(cell, true);
                        pc += 4;
                        break;
                    }
                    case Op::GetUpval: {
                        const Ref<Box>& cell = frame.fn->upvalues[operandAt(pc)];
                        sp.push_back(cell ? cell->value : Value::undefined());
                        pc += 4;
                        break;
                    }
                    case Op::SetUpval: {
                        const Ref<Box>& cell = frame.fn->upvalues[operandAt(pc)];
                        if (cell)
                            cell->value = pop();
                        else
                            sp.pop_back();
                        pc += 4;
                        break;
                    }

                    case Op::GetGlobal: {
                        GCObject* key = code.atoms[operandAt(pc)].get();
                        pc += 4;
                        const Prop* p = rt.globalObject->findOwn(key);
                        if (p && !rt.globalObject->accessorOf(*p)) {
                            sp.push_back(p->value);
                            break;
                        }
                        const Value g = detail::Access::make(Type::Object, rt.globalObject.get());
                        if (!p && !hasProperty(rt, g, key))
                            throwError(rt, ErrorCode::ReferenceError,
                                       toUtf8(static_cast<StringCell*>(key)->text()) +
                                           " is not defined");
                        sp.push_back(getProperty(rt, g, key));
                        break;
                    }
                    case Op::SetGlobal: {
                        GCObject* key = code.atoms[operandAt(pc)].get();
                        pc += 4;
                        setProperty(rt, detail::Access::make(Type::Object, rt.globalObject.get()),
                                    key, pop());
                        break;
                    }
                    case Op::DefGlobal: {
                        GCObject* key = code.atoms[operandAt(pc)].get();
                        const std::uint32_t attrs = operandAt(pc + 4);
                        pc += 8;
                        if (!rt.globalObject->findOwn(key))
                            rt.globalObject->putOwn(key, Value::undefined(), std::uint8_t(attrs));
                        break;
                    }
                    case Op::TypeofGlobal: {
                        GCObject* key = code.atoms[operandAt(pc)].get();
                        pc += 4;
                        const Value g = detail::Access::make(Type::Object, rt.globalObject.get());
                        if (!hasProperty(rt, g, key)) {
                            sp.push_back(rt.str("undefined"));
                            break;
                        }
                        sp.push_back(rt.str(typeOfName(getProperty(rt, g, key))));
                        break;
                    }
                    case Op::DeleteGlobal: {
                        GCObject* key = code.atoms[operandAt(pc)].get();
                        pc += 4;
                        sp.emplace_back(rt.globalObject->deleteOwn(key));
                        break;
                    }

                    case Op::GetProp: {
                        const std::uint32_t operand = operandAt(pc);
                        pc += 4;
                        Value r = getPropertySite(rt, code, sp.back(), code.atoms[operand & kAtomMask].get(),
                                                  operand >> kSiteShift);
                        sp.back() = std::move(r);
                        break;
                    }
                    case Op::GetPropBound: {
                        GCObject* key = code.atoms[operandAt(pc) & kAtomMask].get();
                        pc += 4;
                        Value base = pop();
                        Value read = getProperty(rt, base, key);
                        // Data attributes pay for one tag check; only a method read as a value goes further,
                        // or a Python method JavaScript has no spelling of, which reads as undefined.
                        if (asFunction(read) || read.isUndefined()) {
                            const Value py = getProperty(rt, detail::Access::make(Type::Object, rt.globalObject.get()), rt.intern(u"__py"));
                            const Value hook = py.isObject() ? getProperty(rt, py, rt.intern(u"bindRead")) : Value::undefined();
                            if (asFunction(hook))
                                read = callValue(rt, hook, Value::undefined(), {base, detail::Access::make(Type::String, key), read});
                        }
                        sp.push_back(std::move(read));
                        break;
                    }
                    case Op::GetPropDup: {
                        const std::uint32_t operand = operandAt(pc);
                        pc += 4;
                        Value fn = getPropertySite(rt, code, sp.back(), code.atoms[operand & kAtomMask].get(),
                                                   operand >> kSiteShift);
                        sp.push_back(std::move(fn));
                        std::swap(sp[sp.size() - 1], sp[sp.size() - 2]);
                        break;
                    }
                    case Op::SetProp: {
                        const std::uint32_t operand = operandAt(pc);
                        pc += 4;
                        const std::size_t n = sp.size();
                        setPropertySite(rt, code, sp[n - 2], code.atoms[operand & kAtomMask].get(), sp[n - 1],
                                        operand >> kSiteShift);
                        // Leaves the value, as an assignment expression does.
                        std::swap(sp[n - 2], sp[n - 1]);
                        sp.pop_back();
                        break;
                    }
                    case Op::GetElem: {
                        std::uint8_t& elemSaw = code.elemSaw[operandAt(pc)];
                        pc += 4;
                        const auto note = [&elemSaw](std::uint8_t what) { elemSaw |= what; };
                        {
                            DynArray* arr = nullptr;
                            std::uint32_t index = 0;
                            const std::size_t n = sp.size();
                            if (fastIndex(sp[n - 2], sp[n - 1], arr, index)) {
                                note(ElemSite::of(arr));
                                Value r = arr->at(index);
                                sp.pop_back();
                                sp.back() = std::move(r);
                                break;
                            }
                        }
                        {
                            const std::size_t n = sp.size();
                            DynObject* holder = asObject(sp[n - 2]);
                            if (holder && holder->cls == ObjClass::TypedArray && sp[n - 1].isDouble()) {
                                const double d = detail::Access::r8(sp[n - 1]);
                                auto* ta = static_cast<DynTypedArray*>(holder);
                                if (d >= 0 && d < double(ta->length) && d == std::floor(d)) {
                                    ElemSite::noteTyped(elemSaw, int(ta->element));
                                    Value r = typedArrayGet(rt, ta, std::size_t(d));
                                    sp.pop_back();
                                    sp.back() = std::move(r);
                                    break;
                                }
                            }
                        }
                        note(ElemSite::kOther);
                        Ref<GCObject> key = toPropertyKey(rt, pop());
                        Value base = pop();
                        sp.push_back(getProperty(rt, base, key.get()));
                        break;
                    }
                    case Op::GetElemDup: {
                        std::uint8_t& elemSaw = code.elemSaw[operandAt(pc)];
                        pc += 4;
                        const auto note = [&elemSaw](std::uint8_t what) { elemSaw |= what; };
                        {
                            DynArray* arr = nullptr;
                            std::uint32_t index = 0;
                            const std::size_t n = sp.size();
                            if (fastIndex(sp[n - 2], sp[n - 1], arr, index)) {
                                note(ElemSite::of(arr));
                                Value r = arr->at(index);
                                Value base = sp[n - 2];
                                sp[n - 2] = std::move(r);
                                sp[n - 1] = std::move(base);
                                break;
                            }
                        }
                        note(ElemSite::kOther);
                        Ref<GCObject> key = toPropertyKey(rt, pop());
                        Value base = sp.back();
                        sp.back() = getProperty(rt, base, key.get());
                        sp.push_back(std::move(base));
                        break;
                    }
                    case Op::SetElem: {
                        std::uint8_t& elemSaw = code.elemSaw[operandAt(pc)];
                        pc += 4;
                        const auto note = [&elemSaw](std::uint8_t what) { elemSaw |= what; };
                        {
                            DynArray* arr = nullptr;
                            std::uint32_t index = 0;
                            const std::size_t n = sp.size();
                            // With an own property in the table, "0" may be there with attributes of its own.
                            if (fastIndex(sp[n - 3], sp[n - 2], arr, index) && !arr->frozen && arr->props.empty()) {
                                note(ElemSite::of(arr));
                                arr->put(index, sp[n - 1]);
                                Value v = std::move(sp[n - 1]);
                                sp.resize(n - 3);
                                sp.push_back(std::move(v));
                                break;
                            }
                        }
                        {
                            const std::size_t n = sp.size();
                            DynObject* holder = asObject(sp[n - 3]);
                            if (holder && holder->cls == ObjClass::TypedArray && sp[n - 2].isDouble()) {
                                const double d = detail::Access::r8(sp[n - 2]);
                                if (d >= 0 && d == std::floor(d)) {
                                    auto* ta = static_cast<DynTypedArray*>(holder);
                                    if (d < double(ta->length) && sp[n - 1].isDouble())
                                        ElemSite::noteTyped(elemSaw, int(ta->element));
                                    else
                                        note(ElemSite::kOther);
                                    typedArraySet(rt, static_cast<DynTypedArray*>(holder), d, sp[n - 1]);
                                    Value v = std::move(sp[n - 1]);
                                    sp.resize(n - 3);
                                    sp.push_back(std::move(v));
                                    break;
                                }
                            }
                        }
                        note(ElemSite::kOther);
                        Value v = pop();
                        Ref<GCObject> key = toPropertyKey(rt, pop());
                        Value base = pop();
                        setProperty(rt, base, key.get(), v);
                        sp.push_back(std::move(v));
                        break;
                    }
                    case Op::DeleteProp: {
                        GCObject* key = code.atoms[operandAt(pc)].get();
                        pc += 4;
                        Value base = pop();
                        sp.emplace_back(deleteProperty(rt, base, key));
                        break;
                    }
                    case Op::DeleteElem: {
                        Ref<GCObject> key = toPropertyKey(rt, pop());
                        Value base = pop();
                        sp.emplace_back(deleteProperty(rt, base, key.get()));
                        break;
                    }

                    case Op::DefField:
                    case Op::DefMethod: {
                        GCObject* key = code.atoms[operandAt(pc)].get();
                        pc += 4;
                        Value v = pop();
                        DynObject* target = asObject(sp.back());
                        if (op == Op::DefMethod) {
                            if (DynFunction* m = asFunction(v))
                                m->homeObject.reset(target);
                            target->putOwn(key, v, PropWritable | PropConfigurable);
                        } else {
                            target->putOwn(key, v);
                        }
                        break;
                    }
                    case Op::DefMethodElem: {
                        Value v = pop();
                        Ref<GCObject> key = toPropertyKey(rt, pop());
                        DynObject* target = asObject(sp.back());
                        if (DynFunction* m = asFunction(v))
                            m->homeObject.reset(target);
                        target->putOwn(key.get(), v, PropWritable | PropConfigurable);
                        break;
                    }
                    case Op::DefElem: {
                        Value v = pop();
                        Ref<GCObject> key = toPropertyKey(rt, pop());
                        DynObject* target = asObject(sp.back());
                        target->putOwn(key.get(), v);
                        break;
                    }
                    case Op::DefGetter:
                    case Op::DefSetter: {
                        GCObject* key = code.atoms[operandAt(pc)].get();
                        pc += 4;
                        Value fn = pop();
                        DynObject* target = asObject(sp.back());
                        if (DynFunction* m = asFunction(fn))
                            m->homeObject.reset(target);
                        Prop* existing = target->findOwn(key);
                        if (!existing || !target->accessorOf(*existing))
                            existing = &target->putOwn(key, Value::undefined(),
                                                       PropEnumerable | PropConfigurable |
                                                           PropAccessor);
                        if (op == Op::DefGetter)
                            existing->value = fn;
                        else
                            target->setSetter(*existing, fn);
                        break;
                    }
                    case Op::ObjectSpread: {
                        Value src = pop();
                        DynObject* target = asObject(sp.back());
                        if (DynObject* from = asObject(src)) {
                            for (const Value& key : ownKeys(rt, from, true, false)) {
                                Ref<GCObject> k = toPropertyKey(rt, key);
                                target->putOwn(k.get(), getProperty(rt, src, k.get()));
                            }
                        } else if (StringCell* s = asString(src)) {
                            for (std::uint32_t i = 0; i < s->text().size(); ++i)
                                target->putOwn(rt.intern(numberToString(i)),
                                               rt.str(s->text().substr(i, 1)));
                        }
                        break;
                    }
                    case Op::ObjectRest: {
                        Value excluded = pop();
                        Value src = pop();
                        DynObject* out = rt.newObject(rt.objectProto.get());
                        Value result = detail::Access::adopt(Type::Object, out);
                        if (DynObject* from = asObject(src)) {
                            DynArray* skip = asArray(excluded);
                            for (const Value& key : ownKeys(rt, from, true, false)) {
                                bool drop = false;
                                if (skip) {
                                    for (std::size_t i = 0; i < skip->size(); ++i)
                                        drop = drop || strictEquals(skip->at(i), key);
                                }
                                if (drop)
                                    continue;
                                Ref<GCObject> k = toPropertyKey(rt, key);
                                out->putOwn(k.get(), getProperty(rt, src, k.get()));
                            }
                        }
                        sp.push_back(std::move(result));
                        break;
                    }
                    case Op::SetProtoOf: {
                        Value proto = pop();
                        if (DynObject* target = asObject(sp.back()))
                            target->setProto(asObject(proto));
                        break;
                    }

                    case Op::GetSuper:
                    case Op::GetSuperElem: {
                        Ref<GCObject> key;
                        if (op == Op::GetSuper) {
                            key.reset(code.atoms[operandAt(pc)].get());
                            pc += 4;
                        } else {
                            key = toPropertyKey(rt, pop());
                        }
                        DynObject* home = frame.fn->homeObject.get();
                        DynObject* base = home ? home->proto.get() : nullptr;
                        if (!base)
                            throwError(rt, ErrorCode::TypeError, "'super' has no base object");
                        sp.push_back(
                            getPropertyOfObject(rt, base, key.get(), frame.thisValue));
                        break;
                    }
                    case Op::SetSuper: {
                        GCObject* key = code.atoms[operandAt(pc)].get();
                        pc += 4;
                        Value v = pop();
                        setProperty(rt, frame.thisValue, key, v);
                        sp.push_back(std::move(v));
                        break;
                    }

                    case Op::NewObject: {
                        const std::uint32_t room = operandAt(pc);
                        pc += 4;
                        sp.push_back(detail::Access::adopt(Type::Object, rt.newObjectSized(rt.objectProto.get(), room)));
                        break;
                    }
                    case Op::NewRegExp: {
                        DynObject* made = newRegExpFromTemplate(rt, asObject(code.consts[operandAt(pc)]));
                        pc += 4;
                        sp.push_back(detail::Access::adopt(Type::Object, made));
                        break;
                    }
                    case Op::NewArray: {
                        DynArray* arr = rt.newArray(operandAt(pc));
                        pc += 4;
                        sp.push_back(detail::Access::adopt(Type::Object, arr));
                        break;
                    }
                    case Op::ArrayAppend: {
                        Value v = pop();
                        asArray(sp.back())->append(std::move(v));
                        break;
                    }
                    case Op::ArraySpread: {
                        Value src = pop();
                        const Vec<Value> items = iterableToVector(rt, src);
                        asArray(sp.back())->appendAll(items.data(), items.size());
                        break;
                    }
                    case Op::Closure: {
                        const std::uint32_t index = operandAt(pc);
                        pc += 4;
                        sp.push_back(makeClosure(rt, code.children[index], &frame));
                        break;
                    }
                    case Op::ClassSetup: {
                        const bool hasSuper = operandAt(pc) != 0;
                        const bool allocatesThis = operandAt(pc) == 3;
                        pc += 4;
                        Value ctorValue = pop();
                        Value superValue = Value::undefined();
                        if (hasSuper)
                            superValue = pop();
                        DynFunction* ctor = asFunction(ctorValue);
                        DynFunction* super = asFunction(superValue);
                        if (hasSuper && !super && !superValue.isNull())
                            throwError(rt, ErrorCode::TypeError,
                                       "a class can only extend a constructor or null");
                        DynObject* protoParent =
                            super ? asObject(getPropertyOfObject(rt, super,
                                                                 rt.atom.prototype.get(),
                                                                 superValue))
                                  : (hasSuper ? nullptr : rt.objectProto.get());
                        DynObject* proto = rt.newObject(protoParent);
                        Value protoValue = detail::Access::adopt(Type::Object, proto);
                        proto->putOwn(rt.atom.constructor.get(), ctorValue,
                                      PropWritable | PropConfigurable);
                        ctor->putOwn(rt.atom.prototype.get(), protoValue, 0);
                        ctor->homeObject.reset(proto);
                        ctor->isClassConstructor = true;
                        ctor->isDerivedConstructor = hasSuper && !allocatesThis;
                        if (super)
                            ctor->setProto(super);
                        sp.push_back(std::move(ctorValue));
                        break;
                    }

                    case Op::Call:
                    case Op::CallSpread: {
                        ContextImpl::ArgBuffer buffer(rt.ctx);
                        popArgs(buffer.args, op == Op::CallSpread);
                        Value self = pop();
                        Value callee = pop();
#if SCRIPT_ENABLE_DYN_JIT
                        if (op == Op::Call) {
                            if (DynFunction* target = asFunction(callee); target && target->fnKind == FnKind::Bytecode && target->code)
                                code.noteCall(pc - 5, target->code);
                        }
#endif
                        sp.push_back(callValue(rt, callee, self, buffer.args));
                        break;
                    }
                    case Op::DirectEval: {
                        const EvalScope& scope = code.evalScopes[operandAt(pc)];
                        pc += 4;
                        ContextImpl::ArgBuffer buffer(rt.ctx);
                        buffer.args.assign(std::make_move_iterator(sp.end() - scope.argc), std::make_move_iterator(sp.end()));
                        sp.resize(sp.size() - scope.argc);
                        Value self = pop();
                        Value callee = pop();
                        // Only the real `eval` sees the caller's names; anything else called `eval` is an ordinary call.
                        if (detail::Access::ptr(callee) == rt.intrinsics[Runtime::Eval].get())
                            sp.push_back(evalString(rt, buffer.args.empty() ? Value::undefined() : buffer.args[0], &frame, &scope));
                        else
                            sp.push_back(callValue(rt, callee, self, buffer.args));
                        break;
                    }
                    case Op::New:
                    case Op::NewSpread: {
                        ContextImpl::ArgBuffer buffer(rt.ctx);
                        popArgs(buffer.args, op == Op::NewSpread);
                        Value callee = pop();
                        sp.push_back(constructValue(rt, callee, buffer.args));
                        break;
                    }
                    case Op::CallSuper:
                    case Op::CallSuperSpread: {
                        ContextImpl::ArgBuffer buffer(rt.ctx);
                        popArgs(buffer.args, op == Op::CallSuperSpread);
                        // `self` is there already, for the base's `__init__` to work on.
                        if (!code.isDerivedConstructor) {
                            DynFunction* base = frame.fn->proto ? asFunction(detail::Access::make(Type::Object, frame.fn->proto.get())) : nullptr;
                            if (!base || base == rt.functionProto.get())
                                throwError(rt, ErrorCode::TypeError, "the class has no base to call");
                            callInternal(rt, base, frame.thisValue, buffer.args, true, base);
                            sp.push_back(Value::undefined());
                            break;
                        }
                        Value made = superCall(rt, frame.fn, frame.newTarget.get(), buffer.args, frame.thisValue);
                        frame.thisValue = made;
                        runFieldInit(rt, frame.fn, made);
                        sp.push_back(std::move(made));
                        break;
                    }
                    case Op::CallSuperVia:
                    case Op::CallSuperViaSpread: {
                        ContextImpl::ArgBuffer buffer(rt.ctx);
                        popArgs(buffer.args, op == Op::CallSuperViaSpread);
                        const Value thisNow = pop();
                        const Value target = pop();
                        const Value ctorValue = pop();
                        DynFunction* ctor = asFunction(ctorValue);
                        Value made = superCall(rt, ctor, asFunction(target), buffer.args, thisNow);
                        runFieldInit(rt, ctor, made);
                        sp.push_back(std::move(made));
                        break;
                    }
                    case Op::CheckThis:
                        if (sp.back().isUndefined())
                            throwError(rt, ErrorCode::ReferenceError, "'this' is not there until super() has been called");
                        break;
                    case Op::DerivedReturn: {
                        const Value self = pop();
                        if (sp.back().isObject())
                            break;
                        if (!sp.back().isUndefined())
                            throwError(rt, ErrorCode::TypeError, "a derived constructor can only return an object or undefined");
                        if (self.isUndefined())
                            throwError(rt, ErrorCode::ReferenceError, "a derived constructor did not call super()");
                        sp.back() = self;
                        break;
                    }
                    case Op::PushNewTarget:
                        sp.push_back(frame.newTarget ? detail::Access::make(Type::Object, frame.newTarget.get()) : Value::undefined());
                        break;
                    case Op::NewPrivateName: {
                        auto* name = rt.newCell<SymbolCell>();
                        name->isPrivate = true;
                        name->description = asString(detail::Access::make(Type::String, code.atoms[operandAt(pc)].get()))->text();
                        pc += 4;
                        sp.push_back(detail::Access::adopt(Type::Symbol, name));
                        break;
                    }
                    case Op::PrivateIn: {
                        const Value name = pop();
                        const Value object = pop();
                        DynObject* o = asObject(object);
                        if (!o)
                            throwError(rt, ErrorCode::TypeError, "the right of 'in' with a private name must be an object");
                        sp.emplace_back(o->findOwn(detail::Access::ptr(name)) != nullptr);
                        break;
                    }
                    case Op::DefPrivate:
                    case Op::DefPrivateMethod: {
                        const Value value = pop();
                        const Value name = pop();
                        DynObject* o = asObject(sp.back());
                        GCObject* key = detail::Access::ptr(name);
                        if (o->findOwn(key))
                            throwError(rt, ErrorCode::TypeError, "this object has that private member already");
                        o->putOwn(key, value, op == Op::DefPrivate ? std::uint8_t(PropWritable) : std::uint8_t(0));
                        break;
                    }
                    case Op::DefPrivateAccessor: {
                        const bool setter = operandAt(pc) != 0;
                        pc += 4;
                        const Value fn = pop();
                        const Value name = pop();
                        DynObject* o = asObject(sp.back());
                        GCObject* key = detail::Access::ptr(name);
                        Prop* existing = o->findOwn(key);
                        if (!existing)
                            existing = &o->putOwn(key, Value::undefined(), PropAccessor);
                        if (setter)
                            o->setSetter(*existing, fn);
                        else
                            existing->value = fn;
                        break;
                    }
                    case Op::DefAccessorElem: {
                        const std::uint32_t flags = operandAt(pc);
                        pc += 4;
                        const Value fn = pop();
                        Ref<GCObject> key = toPropertyKey(rt, pop());
                        DynObject* target = asObject(sp.back());
                        if (DynFunction* m = asFunction(fn))
                            m->homeObject.reset(target);
                        Prop* existing = target->findOwn(key.get());
                        if (!existing || !target->accessorOf(*existing))
                            existing = &target->putOwn(key.get(), Value::undefined(),
                                                       std::uint8_t((flags & 2 ? 0 : PropEnumerable) | PropConfigurable | PropAccessor));
                        if (flags & 1)
                            target->setSetter(*existing, fn);
                        else
                            existing->value = fn;
                        break;
                    }
                    case Op::SetFieldInit: {
                        const Value fn = pop();
                        DynFunction* ctor = asFunction(sp.back());
                        DynFunction* init = asFunction(fn);
                        init->homeObject = ctor->homeObject;
                        ctor->fieldInit.reset(init);
                        break;
                    }
                    case Op::CallStaticInit: {
                        const Value fn = pop();
                        asFunction(fn)->homeObject.reset(asObject(sp.back()));
                        const Value self = sp.back();
                        callInternal(rt, asFunction(fn), self, {}, false, nullptr);
                        break;
                    }
                    case Op::SetHome: {
                        const Value fn = pop();
                        asFunction(fn)->homeObject.reset(asObject(sp.back()));
                        sp.back() = fn;
                        break;
                    }
                    case Op::Return:
                        return pop();
                    case Op::ReturnUndefined:
                        return Value::undefined();

                    case Op::Jump: {
                        const std::uint32_t target = operandAt(pc);
                        if (target < pc) {
                            Value result;
                            if (loopBack(target, result))
                                return result;
                        }
                        pc = target;
                        break;
                    }
                    case Op::JumpIfFalse:
                    case Op::JumpIfTrue: {
                        const std::uint32_t target = operandAt(pc);
                        pc += 4;
                        bool truthy;
                        if (sp.back().type() == Type::Bool) {
                            truthy = detail::Access::boolean(sp.back());
                            sp.pop_back();
                        } else {
                            truthy = toBoolean(pop());
                        }
                        if (truthy != (op == Op::JumpIfTrue))
                            break;
                        if (target < pc) {
                            Value result;
                            if (loopBack(target, result))
                                return result;
                        }
                        pc = target;
                        break;
                    }
                    case Op::JumpIfFalseKeep: {
                        const std::uint32_t target = operandAt(pc);
                        pc += 4;
                        if (!toBoolean(sp.back()))
                            pc = target;
                        break;
                    }
                    case Op::JumpIfTrueKeep: {
                        const std::uint32_t target = operandAt(pc);
                        pc += 4;
                        if (toBoolean(sp.back()))
                            pc = target;
                        break;
                    }
                    case Op::JumpIfNullishKeep: {
                        const std::uint32_t target = operandAt(pc);
                        pc += 4;
                        if (sp.back().isNullish())
                            pc = target;
                        break;
                    }
                    case Op::JumpIfNotNullishKeep: {
                        const std::uint32_t target = operandAt(pc);
                        pc += 4;
                        if (!sp.back().isNullish())
                            pc = target;
                        break;
                    }
                    case Op::JumpSub: {
                        const std::uint32_t target = operandAt(pc);
                        pc += 4;
                        frame.subStack.push_back(pc);
                        pc = target;
                        break;
                    }
                    case Op::EndFinally: {
                        const std::uint32_t back = frame.subStack.back();
                        frame.subStack.pop_back();
                        if (back == kRethrow) {
                            Value pending = frame.pendingException;
                            frame.pendingException = Value::undefined();
                            throwValue(rt, pending);
                        }
                        pc = back;
                        break;
                    }
                    case Op::Throw: {
                        Value v = pop();
                        throwValue(rt, v);
                    }

                    case Op::ForInStart: {
                        Value src = pop();
                        DynArray* keys = rt.newArray();
                        Value keysValue = detail::Access::adopt(Type::Object, keys);
                        if (DynObject* obj = asObject(src)) {
                            GcList seen;
                            for (DynObject* cursor = obj; cursor; cursor = cursor->proto.get()) {
                                for (const Value& k : ownKeys(rt, cursor, true, false)) {
                                    GCObject* atom = rt.intern(asString(k)->text());
                                    if (std::find(seen.begin(), seen.end(), atom) != seen.end())
                                        continue;
                                    seen.push_back(atom);
                                    keys->append(k);
                                }
                            }
                        } else if (StringCell* s = asString(src)) {
                            for (std::uint32_t i = 0; i < s->text().size(); ++i)
                                keys->append(rt.str(numberToString(i)));
                        }
                        sp.push_back(std::move(keysValue));
                        sp.emplace_back(0.0);
                        break;
                    }
                    case Op::ForInNext: {
                        const std::uint32_t target = operandAt(pc);
                        pc += 4;
                        DynArray* keys = asArray(sp[sp.size() - 2]);
                        const std::size_t index = std::size_t(numberOf(sp.back()));
                        if (index >= keys->size()) {
                            pc = target;
                            break;
                        }
                        sp.back() = Value(double(index + 1));
                        sp.push_back(keys->at(index));
                        break;
                    }
                    case Op::IterStart: {
                        Value src = pop();
                        sp.push_back(getIterator(rt, src));
                        break;
                    }
                    case Op::IterNext: {
                        const std::uint32_t target = operandAt(pc);
                        pc += 4;
                        Value item = Value::undefined();
                        if (!iteratorStep(rt, sp.back(), item)) {
                            pc = target;
                            break;
                        }
                        sp.push_back(std::move(item));
                        break;
                    }
                    case Op::IterNextOpt: {
                        Value item = Value::undefined();
                        if (!iteratorStep(rt, sp.back(), item))
                            item = Value::undefined();
                        sp.push_back(std::move(item));
                        break;
                    }
                    case Op::IterRest: {
                        DynArray* rest = rt.newArray();
                        Value restValue = detail::Access::adopt(Type::Object, rest);
                        Value item = Value::undefined();
                        while (iteratorStep(rt, sp.back(), item))
                            rest->append(item);
                        sp.push_back(std::move(restValue));
                        break;
                    }
                    case Op::IterClose: {
                        Value iterator = pop();
                        iteratorClose(rt, iterator);
                        break;
                    }
                    case Op::IterStartLoop: {
                        Value src = pop();
                        sp.push_back(getIterator(rt, src));
                        break;
                    }
                    case Op::IterCloseAt: {
                        const Value iterator = sp[operandAt(pc)];
                        pc += 4;
                        const Value fn = getProperty(rt, iterator, rt.intern("return"));
                        if (!fn.isNullish() && !callValue(rt, fn, iterator, {}).isObject())
                            throwError(rt, ErrorCode::TypeError, "the iterator's return() did not give an object");
                        break;
                    }
                    case Op::IterCloseThrow: {
                        const Value thrown = pop();
                        const Value iterator = pop();
                        try {
                            iteratorClose(rt, iterator);
                        } catch (const Exception&) {
                            // The reason for leaving stands.
                        }
                        throwValue(rt, thrown);
                    }
                    case Op::CoStart: {
                        Value made = startCoroutine(rt, frame, pc);
                        return made;
                    }
                    case Op::Yield: {
                        frame.coroutine->returnPc = operandAt(pc);
                        pc += 4;
                        frame.pc = pc;
                        frame.left = Frame::Left::Yielded;
                        return pop();
                    }
                    case Op::Await: {
                        frame.pc = pc;
                        frame.left = Frame::Left::Awaiting;
                        return pop();
                    }
                    case Op::DelegateStart:
                        frame.coroutine->mode = DynCoroutine::Resume::Next;
                        frame.coroutine->received = Value::undefined();
                        break;
                    case Op::DelegateCall: {
                        const std::uint32_t target = operandAt(pc);
                        pc += 4;
                        DynCoroutine* co = frame.coroutine;
                        co->delegating = false;
                        const Value iterator = sp.back();
                        const Value sent = co->received;
                        if (co->mode == DynCoroutine::Resume::Next) {
                            sp.push_back(invokeMethod(rt, iterator, rt.atom.next.get(), {sent}));
                        } else if (co->mode == DynCoroutine::Resume::Throw) {
                            const Value fn = getProperty(rt, iterator, rt.intern("throw"));
                            if (fn.isNullish()) {
                                iteratorClose(rt, iterator);
                                throwError(rt, ErrorCode::TypeError, "the iterator has no throw method");
                            }
                            sp.push_back(callValue(rt, fn, iterator, {sent}));
                        } else {
                            const Value fn = getProperty(rt, iterator, rt.intern("return"));
                            if (fn.isNullish()) {
                                sp.back() = sent;
                                pc = target;
                                break;
                            }
                            sp.push_back(callValue(rt, fn, iterator, {sent}));
                        }
                        break;
                    }
                    case Op::DelegateCheck: {
                        const std::uint32_t target = operandAt(pc);
                        pc += 4;
                        const Value result = pop();
                        if (!result.isObject())
                            throwError(rt, ErrorCode::TypeError, "the iterator result is not an object");
                        if (toBoolean(getProperty(rt, result, rt.atom.done.get()))) {
                            sp.back() = getProperty(rt, result, rt.atom.value.get());
                            pc = target;
                            break;
                        }
                        // Only once nothing more can throw: what is thrown from here on is the generator's own.
                        Value out = code.isAsync ? getProperty(rt, result, rt.atom.value.get()) : result;
                        frame.coroutine->delegating = true;
                        frame.pc = pc;
                        frame.left = code.isAsync ? Frame::Left::Yielded : Frame::Left::YieldedResult;
                        return out;
                    }
                    case Op::DelegateDone: {
                        const std::uint32_t target = operandAt(pc);
                        pc += 4;
                        if (frame.coroutine->mode == DynCoroutine::Resume::Return)
                            pc = target;
                        break;
                    }
                    case Op::AsyncIterStart: {
                        Value src = pop();
                        const Value method = getProperty(rt, src, rt.sym.asyncIterator.get());
                        if (method.isNullish()) {
                            sp.push_back(asyncFromSyncIterator(rt, getIterator(rt, src)));
                        } else {
                            Value iterator = callValue(rt, method, src, {});
                            if (!iterator.isObject())
                                throwError(rt, ErrorCode::TypeError, "the async iterator is not an object");
                            sp.push_back(std::move(iterator));
                        }
                        break;
                    }
                    case Op::AsyncIterCall: {
                        const Value iterator = sp.back();
                        sp.push_back(invokeMethod(rt, iterator, rt.atom.next.get(), {}));
                        break;
                    }
                    case Op::AsyncIterCheck: {
                        const std::uint32_t target = operandAt(pc);
                        pc += 4;
                        const Value result = pop();
                        if (!result.isObject())
                            throwError(rt, ErrorCode::TypeError, "the iterator result is not an object");
                        if (toBoolean(getProperty(rt, result, rt.atom.done.get()))) {
                            pc = target;
                            break;
                        }
                        sp.push_back(getProperty(rt, result, rt.atom.value.get()));
                        break;
                    }
                    case Op::AsyncIterClose: {
                        const Value iterator = sp[operandAt(pc)];
                        pc += 4;
                        const Value fn = getProperty(rt, iterator, rt.intern("return"));
                        sp.push_back(fn.isNullish() ? Value::undefined() : callValue(rt, fn, iterator, {}));
                        break;
                    }

                    case Op::Add: {
                        {
                            const std::size_t n = sp.size();
                            Value& fa = sp[n - 2];
                            Value& fb = sp[n - 1];
                            if (fa.type() == Type::Double && fb.type() == Type::Double) {
                                const double r = detail::Access::r8(fa) + detail::Access::r8(fb);
                                sp.pop_back();
                                fa = Value(r);
                                break;
                            }
                        }
                        Value b = pop();
                        Value a = pop();
                        if (a.isNumber() && b.isNumber())
                            sp.emplace_back(numberOf(a) + numberOf(b));
                        else
                            sp.push_back(add(rt, a, b));
                        break;
                    }
                    case Op::Sub:
                        numeric(bigint::BinaryOp::Sub, [](double a, double b) { return a - b; });
                        break;
                    case Op::Mul:
                        numeric(bigint::BinaryOp::Mul, [](double a, double b) { return a * b; });
                        break;
                    case Op::Div:
                        numeric(bigint::BinaryOp::Div, [](double a, double b) { return a / b; });
                        break;
                    case Op::Mod:
                        numeric(bigint::BinaryOp::Mod, [](double a, double b) {
                            if (a > 0 && b > 0 && a <= 2147483647.0 && b <= 2147483647.0) {
                                const std::int32_t ia = std::int32_t(a), ib = std::int32_t(b);
                                if (double(ia) == a && double(ib) == b)
                                    return double(ia % ib);
                            }
                            return std::fmod(a, b);
                        });
                        break;
                    case Op::Pow:
                        numeric(bigint::BinaryOp::Pow, [](double a, double b) { return numberPow(a, b); });
                        break;
                    case Op::Neg: {
                        const Value operand = toNumeric(rt, sp.back());
                        if (operand.isBigInt())
                            sp.back() = bigint::negate(rt, bigint::cellOf(operand));
                        else
                            sp.back() = Value(-numberOf(operand));
                        break;
                    }
                    case Op::Pos:
                        sp.back() = Value(toNumber(rt, sp.back()));
                        break;
                    case Op::ToNumber:  // ToNumeric: what `x++` starts with
                        sp.back() = toNumeric(rt, sp.back());
                        break;
                    case Op::ToString:
                        sp.back() = rt.str(toStringValue(rt, sp.back()));
                        break;
                    case Op::ToObject:
                        sp.back() = toObject(rt, sp.back());
                        break;
                    case Op::ToPropertyKey: {
                        Ref<GCObject> key = toPropertyKey(rt, sp.back());
                        sp.back() = detail::Access::make(
                            key->kind == CellKind::Symbol ? Type::Symbol : Type::String,
                            key.get());
                        break;
                    }
                    case Op::Inc:
                    case Op::Dec: {
                        const Value operand = toNumeric(rt, sp.back());
                        if (operand.isBigInt())
                            sp.back() = bigint::increment(rt, bigint::cellOf(operand), op == Op::Inc ? 1 : -1);
                        else
                            sp.back() = Value(numberOf(operand) + (op == Op::Inc ? 1 : -1));
                        break;
                    }
                    case Op::Void:
                        sp.back() = Value::undefined();
                        break;
                    case Op::BitAnd:
                        numeric(bigint::BinaryOp::And, [](double a, double b) { return double(toInt32(a) & toInt32(b)); });
                        break;
                    case Op::BitOr:
                        numeric(bigint::BinaryOp::Or, [](double a, double b) { return double(toInt32(a) | toInt32(b)); });
                        break;
                    case Op::BitXor:
                        numeric(bigint::BinaryOp::Xor, [](double a, double b) { return double(toInt32(a) ^ toInt32(b)); });
                        break;
                    case Op::BitNot: {
                        const Value operand = toNumeric(rt, sp.back());
                        if (operand.isBigInt())
                            sp.back() = bigint::bitNot(rt, bigint::cellOf(operand));
                        else
                            sp.back() = Value(double(~toInt32(numberOf(operand))));
                        break;
                    }
                    case Op::Shl:
                        numeric(bigint::BinaryOp::Shl, [](double a, double b) {
                            return double(std::int32_t(toUint32(a) << (toUint32(b) & 31)));
                        });
                        break;
                    case Op::Shr:
                        numeric(bigint::BinaryOp::Shr, [](double a, double b) {
                            return double(toInt32(a) >> (toUint32(b) & 31));
                        });
                        break;
                    case Op::UShr: {
                        const Value right = pop();
                        const Value left = pop();
                        const Value x = toNumeric(rt, left);
                        const Value y = toNumeric(rt, right);
                        if (x.isBigInt() || y.isBigInt())
                            throwError(rt, ErrorCode::TypeError, "BigInts have no unsigned right shift");
                        sp.emplace_back(double(toUint32(numberOf(x)) >> (toUint32(numberOf(y)) & 31)));
                        break;
                    }
                    case Op::Eq: {
                        Value b = pop();
                        Value a = pop();
                        sp.emplace_back(looseEquals(rt, a, b));
                        break;
                    }
                    case Op::Ne: {
                        Value b = pop();
                        Value a = pop();
                        sp.emplace_back(!looseEquals(rt, a, b));
                        break;
                    }
                    case Op::StrictEq: {
                        Value b = pop();
                        Value a = pop();
                        sp.emplace_back(strictEquals(a, b));
                        break;
                    }
                    case Op::StrictNe: {
                        Value b = pop();
                        Value a = pop();
                        sp.emplace_back(!strictEquals(a, b));
                        break;
                    }
                    case Op::Lt:
                    case Op::Le:
                    case Op::Gt:
                    case Op::Ge: {
                        {
                            const std::size_t n = sp.size();
                            Value& fa = sp[n - 2];
                            Value& fb = sp[n - 1];
                            if (fa.type() == Type::Double && fb.type() == Type::Double) {
                                const double x = detail::Access::r8(fa), y = detail::Access::r8(fb);
                                const bool res = op == Op::Lt ? x < y : op == Op::Le ? x <= y : op == Op::Gt ? x > y : x >= y;
                                sp.pop_back();
                                fa = Value(res);
                                break;
                            }
                        }
                        Value b = pop();
                        Value a = pop();
                        const int r = compare(rt, a, b);
                        bool result = false;
                        if (r != 2) {
                            switch (op) {
                                case Op::Lt: result = r < 0; break;
                                case Op::Le: result = r <= 0; break;
                                case Op::Gt: result = r > 0; break;
                                default: result = r >= 0; break;
                            }
                        }
                        sp.emplace_back(result);
                        break;
                    }
                    case Op::In: {
                        Value container = pop();
                        Ref<GCObject> key = toPropertyKey(rt, pop());
                        sp.emplace_back(hasProperty(rt, container, key.get()));
                        break;
                    }
                    case Op::InstanceOf: {
                        Value ctor = pop();
                        Value obj = pop();
                        sp.emplace_back(instanceOf(rt, obj, ctor));
                        break;
                    }
                    case Op::Not:
                        sp.back() = Value(!toBoolean(sp.back()));
                        break;
                    case Op::TypeOf:
                        sp.back() = rt.str(typeOfName(sp.back()));
                        break;
                    default:
                        throwError(rt, ErrorCode::InternalError,
                                   std::string("unimplemented opcode ") + opName(op));
                }
            }
        } catch (Exception& e) {
            if (!findHandler(frame, e.value()))
                throw;
            pc = frame.pc;
        }
    }
}

}  // namespace

Value resumeInterpreter(Runtime& rt, DynFunction* fn, const FunctionCode& code, const Value& thisValue, std::uint32_t pc,
                        void (*fill)(Frame&, void*), void* state)
{
    Frame& frame = rt.acquireFrame(rt.frames.size());
    frame.begin(fn, &code, thisValue, nullptr);
    rt.frames.push_back(&frame);
    struct PopFrame
    {
        Runtime* rt;
        Frame* frame;
        ~PopFrame()
        {
            rt->frames.pop_back();
            frame->end();
        }
    } popper{&rt, &frame};
    fill(frame, state);
    frame.pc = pc;
    return execute(rt, frame);
}

// ------------------------------------------------------------ promises ---

namespace {
void settledAwait(Runtime& rt, DynCoroutine* co, bool fulfilled, const Value& result);
}

Value makePromise(Runtime& rt)
{
    auto* p = rt.newCell<DynPromise>();
    p->setProto(rt.promiseProto.get());
    return detail::Access::adopt(Type::Object, p);
}

namespace {

void scheduleReactions(Runtime& rt, DynPromise* p)
{
    const bool fulfilled = p->state == DynPromise::State::Fulfilled;
    for (DynPromise::Reaction& r : p->reactions) {
        if (r.coroutine) {
            const Value co = detail::Access::make(Type::Object, r.coroutine.get());
            const Value settled = p->result;
            rt.enqueue(rt.makeFunction(ascii(""),
                                       [co, settled, fulfilled](CallInfo&) -> Value {
                                           settledAwait(*runtimeOf(co), static_cast<DynCoroutine*>(asObject(co)), fulfilled, settled);
                                           return Value::undefined();
                                       },
                                       0),
                       {});
            continue;
        }
        const Value handler = fulfilled ? r.onFulfilled : r.onRejected;
        Value result = p->result;
        const bool isFulfilled = fulfilled;
        if (!r.next) {
            // Another constructor's promise: what the handler gives goes to its resolve, what it throws to its reject.
            const Value resolve = r.capResolve;
            const Value reject = r.capReject;
            rt.enqueue(rt.makeFunction(ascii(""),
                                       [handler, resolve, reject, result, isFulfilled](CallInfo& ci) -> Value {
                                           Runtime& rtc = rtOf(ci);
                                           if (!asFunction(handler)) {
                                               if (asFunction(isFulfilled ? resolve : reject))
                                                   callValue(rtc, isFulfilled ? resolve : reject, Value::undefined(), {result});
                                               return Value::undefined();
                                           }
                                           Value out;
                                           try {
                                               out = callValue(rtc, handler, Value::undefined(), {result});
                                           } catch (Exception& e) {
                                               if (asFunction(reject))
                                                   callValue(rtc, reject, Value::undefined(), {e.value()});
                                               return Value::undefined();
                                           }
                                           if (asFunction(resolve))
                                               callValue(rtc, resolve, Value::undefined(), {out});
                                           return Value::undefined();
                                       },
                                       0),
                       {});
            continue;
        }
        Value next = detail::Access::make(Type::Object, r.next.get());
        rt.enqueue(rt.makeFunction(ascii(""),
                                   [handler, next, result, isFulfilled](CallInfo& ci) -> Value {
                                       Runtime& rtc = *runtimeOf(next);
                                       auto* target = static_cast<DynPromise*>(asObject(next));
                                       (void)ci;
                                       if (!asFunction(handler)) {
                                           if (isFulfilled)
                                               resolvePromise(rtc, target, result);
                                           else
                                               rejectPromise(rtc, target, result);
                                           return Value::undefined();
                                       }
                                       try {
                                           Value out = callValue(rtc, handler, Value::undefined(), {result});
                                           resolvePromise(rtc, target, out);
                                       } catch (Exception& e) {
                                           rejectPromise(rtc, target, e.value());
                                       }
                                       return Value::undefined();
                                   },
                                   0),
                   {});
    }
    p->reactions.clear();
}

}  // namespace

void resolvePromise(Runtime& rt, DynPromise* p, const Value& value)
{
    if (p->state != DynPromise::State::Pending)
        return;
    if (value.isObject() && asObject(value) == p) {
        rejectPromise(rt, p, makeErrorValue(rt, ErrorCode::TypeError, "a promise cannot be resolved with itself"));
        return;
    }
    // Adopting a thenable is what makes promise chaining flatten.
    if (value.isObject()) {
        Value then;
        try {
            then = getProperty(rt, value, rt.atom.then.get());
        } catch (Exception& e) {
            rejectPromise(rt, p, e.value());
            return;
        }
        if (asFunction(then)) {
            const Value self = detail::Access::make(Type::Object, p);
            // One of the two counts, once: what the thenable does after that is ignored.
            auto done = std::make_shared<bool>(false);
            Value onOk = rt.makeFunction(String(), [self, done](CallInfo& ci) -> Value {
                if (!*done) {
                    *done = true;
                    resolvePromise(rtOf(ci), static_cast<DynPromise*>(asObject(self)), ci.arg(0));
                }
                return Value::undefined();
            }, 1);
            Value onErr = rt.makeFunction(String(), [self, done](CallInfo& ci) -> Value {
                if (!*done) {
                    *done = true;
                    rejectPromise(rtOf(ci), static_cast<DynPromise*>(asObject(self)), ci.arg(0));
                }
                return Value::undefined();
            }, 1);
            rt.enqueue(rt.makeFunction(ascii(""),
                                       [then, value, onOk, onErr](CallInfo& ci) -> Value {
                                           Runtime& rtc = rtOf(ci);
                                           try {
                                               callValue(rtc, then, value, {onOk, onErr});
                                           } catch (Exception& e) {
                                               callValue(rtc, onErr, Value::undefined(), {e.value()});
                                           }
                                           return Value::undefined();
                                       },
                                       0),
                       {});
            return;
        }
    }
    p->state = DynPromise::State::Fulfilled;
    p->result = value;
    scheduleReactions(rt, p);
}

void addReaction(Runtime& rt, DynPromise* p, DynPromise::Reaction reaction)
{
    p->handled = true;
    p->reactions.push_back(std::move(reaction));
    if (p->state != DynPromise::State::Pending)
        scheduleReactions(rt, p);
}

void rejectPromise(Runtime& rt, DynPromise* p, const Value& reason)
{
    if (p->state != DynPromise::State::Pending)
        return;
    p->state = DynPromise::State::Rejected;
    p->result = reason;
    if (p->reactions.empty() && !p->handled) {
        // A host that never drains the queue must not accumulate these for ever.
        if (rt.unhandled.size() >= 256)
            rt.unhandled.erase(rt.unhandled.begin());
        rt.unhandled.emplace_back(p);
    }
    scheduleReactions(rt, p);
}

Value makeSettler(Runtime& rt, const Value& promise, bool reject)
{
    return rt.makeFunction(ascii(""), [promise, reject](CallInfo& ci) -> Value {
        auto* target = static_cast<DynPromise*>(asObject(promise));
        Runtime& rtc = *runtimeOf(promise);
        if (reject)
            rejectPromise(rtc, target, ci.arg(0));
        else
            resolvePromise(rtc, target, ci.arg(0));
        return Value::undefined();
    }, 1);
}

// ---------------------------------------------------------- coroutines ---

DynCoroutine::DynCoroutine() : frame(std::make_unique<Frame>())
{
    cls = ObjClass::Generator;
    frame->coroutine = this;
}

DynCoroutine::~DynCoroutine() = default;

void DynCoroutine::children(GcList& out) const
{
    DynObject::children(out);
    const auto add = [&out](const Value& v) {
        if (GCObject* p = detail::Access::ptr(v))
            out.push_back(p);
    };
    add(received);
    for (const Value& v : frame->locals)
        add(v);
    for (const Ref<Box>& box : frame->cells) {
        if (box)
            out.push_back(box.get());
    }
    for (const Value& v : frame->stack)
        add(v);
    add(frame->thisValue);
    add(frame->pendingException);
    if (frame->newTarget)
        out.push_back(frame->newTarget.get());
    if (function)
        out.push_back(function.get());
    if (promise)
        out.push_back(promise.get());
    for (const Request& r : requests) {
        add(r.value);
        if (r.promise)
            out.push_back(r.promise.get());
    }
}

void DynCoroutine::clearReferences()
{
    DynObject::clearReferences();
    frame->end();
    frame->coroutine = this;
    received = Value::undefined();
    function.reset();
    promise.reset();
    requests.clear();
}

Value iterResult(Runtime& rt, const Value& value, bool done)
{
    DynObject* out = rt.newObject(rt.objectProto.get());
    Value handle = detail::Access::adopt(Type::Object, out);
    out->putOwn(rt.atom.value.get(), value);
    out->putOwn(rt.atom.done.get(), Value(done));
    return handle;
}

namespace {

void stepAsync(Runtime& rt, DynCoroutine* co, DynCoroutine::Resume mode, const Value& value);
void drainAsyncGenerator(Runtime& rt, DynCoroutine* co);

/// Run the frame until it next leaves. What it left for is in `left`.
Value runCoroutine(Runtime& rt, DynCoroutine* co, DynCoroutine::Resume mode, const Value& value)
{
    Frame& frame = *co->frame;
    if (co->state != DynCoroutine::State::SuspendedStart) {
        if (co->delegating) {
            co->mode = mode;
            co->received = value;
        } else if (mode == DynCoroutine::Resume::Throw) {
            co->throwOnResume = true;
            co->received = value;
        } else {
            frame.stack.push_back(value);
            if (mode == DynCoroutine::Resume::Return)
                frame.pc = co->returnPc;
        }
    }
    co->state = DynCoroutine::State::Running;
    frame.left = Frame::Left::Finished;
    DepthGuard depth(rt);
    rt.frames.push_back(&frame);
    struct Pop
    {
        Runtime& rt;
        ~Pop() { rt.frames.pop_back(); }
    } pop{rt};
    return execute(rt, frame);
}

void finishCoroutine(DynCoroutine* co)
{
    co->state = DynCoroutine::State::Completed;
    co->frame->end();
    co->frame->coroutine = co;
}

/// The promise an `await` waits on, and what happens when it settles.
void awaitValue(Runtime& rt, DynCoroutine* co, const Value& value)
{
    Value waited;
    try {
        DynObject* object = asObject(value);
        if (object && object->cls == ObjClass::Promise &&
            detail::Access::ptr(getProperty(rt, value, rt.atom.constructor.get())) == rt.promiseCtor.get()) {
            waited = value;
        } else {
            waited = makePromise(rt);
            resolvePromise(rt, static_cast<DynPromise*>(asObject(waited)), value);
        }
    } catch (Exception& e) {
        settledAwait(rt, co, false, e.value());
        return;
    }
    DynPromise::Reaction reaction;
    reaction.coroutine.reset(co);
    addReaction(rt, static_cast<DynPromise*>(asObject(waited)), std::move(reaction));
}

void settleRequest(Runtime& rt, DynCoroutine* co, bool fulfilled, const Value& value)
{
    const Ref<DynPromise> promise = co->requests.front().promise;
    co->requests.erase(co->requests.begin());
    if (fulfilled)
        resolvePromise(rt, promise.get(), value);
    else
        rejectPromise(rt, promise.get(), value);
}

void stepAsync(Runtime& rt, DynCoroutine* co, DynCoroutine::Resume mode, const Value& value)
{
    const Value keep = detail::Access::make(Type::Object, co);
    Value out;
    bool threw = false;
    try {
        out = runCoroutine(rt, co, mode, value);
    } catch (Exception& e) {
        threw = true;
        out = e.value();
    }
    if (!threw && co->frame->left == Frame::Left::Awaiting) {
        awaitValue(rt, co, out);
        return;
    }
    if (co->coKind == DynCoroutine::Kind::AsyncFunction) {
        finishCoroutine(co);
        const Ref<DynPromise> promise = co->promise;
        if (threw)
            rejectPromise(rt, promise.get(), out);
        else
            resolvePromise(rt, promise.get(), out);
        return;
    }
    if (!threw && co->frame->left == Frame::Left::Yielded) {
        co->state = DynCoroutine::State::SuspendedYield;
        settleRequest(rt, co, true, iterResult(rt, out, false));
    } else {
        finishCoroutine(co);
        settleRequest(rt, co, !threw, threw ? out : iterResult(rt, out, true));
    }
    drainAsyncGenerator(rt, co);
}

void drainAsyncGenerator(Runtime& rt, DynCoroutine* co)
{
    while (!co->requests.empty()) {
        if (co->state == DynCoroutine::State::Running || co->state == DynCoroutine::State::AwaitingReturn)
            return;
        const DynCoroutine::Resume mode = co->requests.front().mode;
        const Value value = co->requests.front().value;
        if (co->state == DynCoroutine::State::SuspendedStart && mode != DynCoroutine::Resume::Next)
            finishCoroutine(co);
        if (co->state != DynCoroutine::State::Completed) {
            stepAsync(rt, co, mode, value);
            return;
        }
        if (mode == DynCoroutine::Resume::Next) {
            settleRequest(rt, co, true, iterResult(rt, Value::undefined(), true));
        } else if (mode == DynCoroutine::Resume::Throw) {
            settleRequest(rt, co, false, value);
        } else {
            co->state = DynCoroutine::State::AwaitingReturn;
            awaitValue(rt, co, value);
            return;
        }
    }
}

void settledAwait(Runtime& rt, DynCoroutine* co, bool fulfilled, const Value& result)
{
    if (co->state == DynCoroutine::State::AwaitingReturn) {
        co->state = DynCoroutine::State::Completed;
        settleRequest(rt, co, fulfilled, fulfilled ? iterResult(rt, result, true) : result);
        drainAsyncGenerator(rt, co);
        return;
    }
    stepAsync(rt, co, fulfilled ? DynCoroutine::Resume::Next : DynCoroutine::Resume::Throw, result);
}

Value startCoroutine(Runtime& rt, Frame& frame, std::uint32_t pc)
{
    const FunctionCode& code = *frame.code;
    auto* co = rt.newCell<DynCoroutine>();
    Value handle = detail::Access::adopt(Type::Object, co);
    co->coKind = !code.isGenerator ? DynCoroutine::Kind::AsyncFunction
                                   : code.isAsync ? DynCoroutine::Kind::AsyncGenerator : DynCoroutine::Kind::Generator;
    co->function.reset(frame.fn);
    Frame& own = *co->frame;
    own.fn = frame.fn;
    own.code = frame.code;
    own.pc = pc;
    own.locals.swap(frame.locals);
    own.cells.swap(frame.cells);
    own.stack.swap(frame.stack);
    own.subStack.swap(frame.subStack);
    own.thisValue = std::move(frame.thisValue);
    own.pendingException = std::move(frame.pendingException);
    own.newTarget = std::move(frame.newTarget);
    if (co->coKind == DynCoroutine::Kind::AsyncFunction) {
        co->setProto(rt.objectProto.get());
        Value promise = makePromise(rt);
        co->promise.reset(static_cast<DynPromise*>(asObject(promise)));
        stepAsync(rt, co, DynCoroutine::Resume::Next, Value::undefined());
        return promise;
    }
    // What it inherits is whatever the function's `prototype` is when it is called.
    const bool isAsync = co->coKind == DynCoroutine::Kind::AsyncGenerator;
    DynObject* proto = asObject(getProperty(rt, detail::Access::make(Type::Object, frame.fn), rt.atom.prototype.get()));
    co->setProto(proto ? proto : isAsync ? rt.asyncGeneratorProto.get() : rt.generatorProto.get());
    return handle;
}

}  // namespace

Value generatorResume(Runtime& rt, DynCoroutine* g, DynCoroutine::Resume mode, const Value& value)
{
    if (g->state == DynCoroutine::State::Running)
        throwError(rt, ErrorCode::TypeError, "the generator is already running");
    if (g->state == DynCoroutine::State::SuspendedStart && mode != DynCoroutine::Resume::Next)
        finishCoroutine(g);
    if (g->state == DynCoroutine::State::Completed) {
        if (mode == DynCoroutine::Resume::Throw)
            throwValue(rt, value);
        return iterResult(rt, mode == DynCoroutine::Resume::Return ? value : Value::undefined(), true);
    }
    Value out;
    try {
        out = runCoroutine(rt, g, mode, value);
    } catch (...) {
        finishCoroutine(g);
        throw;
    }
    const Frame::Left left = g->frame->left;
    if (left == Frame::Left::YieldedResult) {
        g->state = DynCoroutine::State::SuspendedYield;
        return out;
    }
    if (left == Frame::Left::Yielded) {
        g->state = DynCoroutine::State::SuspendedYield;
        return iterResult(rt, out, false);
    }
    finishCoroutine(g);
    return iterResult(rt, out, true);
}

Value asyncGeneratorRequest(Runtime& rt, const Value& self, DynCoroutine::Resume mode, const Value& value)
{
    Value promise = makePromise(rt);
    auto* p = static_cast<DynPromise*>(asObject(promise));
    DynObject* object = asObject(self);
    if (!object || object->cls != ObjClass::Generator ||
        static_cast<DynCoroutine*>(object)->coKind != DynCoroutine::Kind::AsyncGenerator) {
        rejectPromise(rt, p, makeErrorValue(rt, ErrorCode::TypeError, "not an async generator"));
        return promise;
    }
    auto* co = static_cast<DynCoroutine*>(object);
    co->requests.push_back(DynCoroutine::Request{mode, value, Ref<DynPromise>(p)});
    drainAsyncGenerator(rt, co);
    return promise;
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
