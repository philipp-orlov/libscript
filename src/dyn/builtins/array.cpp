// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Array, its prototype methods and the iterator protocol helpers.

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/proxy.hpp"
#include "script/internal/dyn/typedarray.hpp"
#include "script/internal/dyn/numconv.hpp"
#include "script/internal/dyn/vm.hpp"
#include "script/internal/stack_guard.hpp"

namespace script {
namespace internal {
namespace dyn {

namespace {

/// Element `i` of `a` as HasProperty and Get see it: its own, or for a hole, one up the prototype chain.
bool presentElement(Runtime& rt, DynArray* a, std::size_t i, Value& out)
{
    if (i >= a->size())
        return false;
    if (a->hasElement(i)) {
        out = a->at(i);
        return true;
    }
    const Value self = detail::Access::make(Type::Object, a);
    Ref<GCObject> key(rt.intern(numberToString(double(i))));
    if (!hasProperty(rt, self, key.get()))
        return false;
    out = getProperty(rt, self, key.get());
    return true;
}

DynArray* thisArray(CallInfo& ci)
{
    DynArray* arr = asArray(ci.thisValue());
    if (!arr)
        ci.fail(ErrorCode::TypeError, "this method needs an array");
    return arr;
}

/// `this` for a method that changes the array.
DynArray* thisMutableArray(CallInfo& ci)
{
    DynArray* arr = thisArray(ci);
    if (arr->frozen)
        ci.fail(ErrorCode::TypeError, "the array is frozen");
    return arr;
}

std::int64_t clampIndex(double raw, std::size_t length)
{
    if (std::isnan(raw))
        return 0;
    std::int64_t i = toIntegerOrInfinity(raw);
    if (i < 0)
        i += std::int64_t(length);
    return std::max<std::int64_t>(0, std::min<std::int64_t>(i, std::int64_t(length)));
}

}  // namespace

Value makeArrayIterator(Runtime& rt, const Value& target, int mode)
{
    auto* it = rt.newCell<DynArrayIterator>();
    it->cls = ObjClass::ArrayIterator;
    it->setProto(rt.arrayIteratorProto.get());
    it->target = target;
    it->mode = mode;
    return detail::Access::adopt(Type::Object, it);
}

double lengthOfLike(Runtime& rt, const Value& v)
{
    if (DynArray* arr = asArray(v))
        return double(arr->size());
    const double n = toNumber(rt, getProperty(rt, v, rt.atom.length.get()));
    if (std::isnan(n) || n <= 0)
        return 0;
    return std::min(std::trunc(n), 9007199254740991.0);
}

bool arrayIteratorStep(Runtime& rt, DynArrayIterator* it, Value& out)
{
    if (it->target.isUndefined())
        return false;
    double length;
    DynObject* target = asObject(it->target);
    if (target && target->cls == ObjClass::TypedArray) {
        auto* ta = static_cast<DynTypedArray*>(target);
        if (ta->outOfBounds || ta->buffer->detached)
            throwError(rt, ErrorCode::TypeError, "the typed array's buffer is detached or too short for it");
        length = double(ta->length);
    } else {
        length = lengthOfLike(rt, it->target);
    }
    if (it->index >= length) {
        it->target = Value::undefined();
        return false;
    }
    const double i = it->index++;
    if (it->mode == 1) {
        out = Value(i);
        return true;
    }
    const Value keep = it->target;
    const Value item = i < 4294967295.0 ? getIndex(rt, keep, std::uint32_t(i)) : getProperty(rt, keep, rt.intern(numberToString(i)));
    out = it->mode == 2 ? rt.makeArray({Value(i), item}) : item;
    return true;
}

/// A snapshot iterator, used where the collection cannot be indexed.
Value makeIterator(Runtime& rt, Vec<Value> items)
{
    DynObject* it = rt.newObject(rt.arrayIteratorProto.get());
    Value result = detail::Access::adopt(Type::Object, it);
    auto state = std::make_shared<Vec<Value>>(std::move(items));
    auto index = std::make_shared<std::size_t>(0);
    rt.method(it, "next", [state, index](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynObject* out = rt2.newObject(rt2.objectProto.get());
        Value record = detail::Access::adopt(Type::Object, out);
        const bool done = *index >= state->size();
        out->putOwn(rt2.atom.value.get(), done ? Value::undefined() : (*state)[(*index)++]);
        out->putOwn(rt2.atom.done.get(), Value(done));
        return record;
    }, 0);
    return result;
}

namespace {

/// Merge sort: a user comparator may be inconsistent, and std::sort is allowed
/// to run off the end of the range when it is.
template <class T, class Less>
void mergeSort(Vec<T>& v, const Less& less)
{
    if (v.size() < 2)
        return;
    // Each pass moves every value once, from one buffer to the other, so no count changes on the way.
    Vec<T> scratch(v.size());
    for (std::size_t width = 1; width < v.size(); width *= 2) {
        for (std::size_t start = 0; start < v.size(); start += 2 * width) {
            const std::size_t mid = std::min(start + width, v.size());
            const std::size_t end = std::min(start + 2 * width, v.size());
            std::size_t a = start;
            std::size_t b = mid;
            for (std::size_t out = start; out < end; ++out) {
                if (a < mid && (b >= end || !less(v[b], v[a])))
                    scratch[out] = std::move(v[a++]);
                else
                    scratch[out] = std::move(v[b++]);
            }
        }
        v.swap(scratch);
    }
}

/// `function (a, b) { return a - b; }`, as ascending numeric sorts are written, says 1; `b - a` says -1; anything else 0.
/// Those need no call: what the comparator would answer is the difference, worked out here.
int numericComparator(const Value& comparator)
{
    const DynFunction* fn = asFunction(comparator);
    if (!fn || fn->fnKind != FnKind::Bytecode || !fn->code)
        return 0;
    const FunctionCode& code = *fn->code;
    if (code.arity != 2 || code.hasRest || code.argumentsSlot != 0xffffffffu)
        return 0;
    const std::vector<std::uint8_t>& c = code.code;
    if (c.size() < 12 || Op(c[0]) != Op::GetLocal || Op(c[5]) != Op::GetLocal || Op(c[10]) != Op::Sub || Op(c[11]) != Op::Return)
        return 0;
    // What may follow the return is never reached.
    for (std::size_t i = 12; i < c.size(); ++i) {
        if (Op(c[i]) != Op::ReturnUndefined && Op(c[i]) != Op::Nop)
            return 0;
    }
    const std::uint32_t first = readOperand(c, 1);
    const std::uint32_t second = readOperand(c, 6);
    if (first == 0 && second == 1)
        return 1;
    if (first == 1 && second == 0)
        return -1;
    return 0;
}

}  // namespace

void installArray(Runtime& rt)
{
    DynObject* iterProto = rt.newObject(rt.objectProto.get());
    rt.iteratorProto.reset(iterProto, true);
    iterProto->putOwn(rt.sym.iterator.get(),
                      rt.makeFunction(ascii("[Symbol.iterator]"),
                                      [](CallInfo& ci) { return ci.thisValue(); }, 0),
                      PropWritable | PropConfigurable);
    rt.arrayIteratorProto.reset(rt.newObject(iterProto), true);
    rt.arrayIteratorProto->putOwn(rt.sym.toStringTag.get(), rt.str("Array Iterator"), PropConfigurable);
    Value iteratorNext = rt.makeFunction(ascii("next"), [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynObject* o = asObject(ci.thisValue());
        if (!o || o->cls != ObjClass::ArrayIterator)
            ci.fail(ErrorCode::TypeError, "not an Array Iterator");
        Value item = Value::undefined();
        const bool more = arrayIteratorStep(rt2, static_cast<DynArrayIterator*>(o), item);
        return iterResult(rt2, item, !more);
    }, 0);
    rt.arrayIteratorProto->putOwn(rt.atom.next.get(), iteratorNext, PropWritable | PropConfigurable);
    rt.intrinsics[Runtime::ArrayIteratorNext].reset(detail::Access::ptr(iteratorNext));

    auto* protoArray = rt.newCell<DynArray>();
    protoArray->setProto(rt.objectProto.get());
    rt.arrayProto.reset(protoArray, true);
    DynObject* proto = protoArray;

    DynFunction* ctor = rt.makeConstructor("Array", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        if (ci.size() == 1 && ci.arg(0).isNumber()) {
            const double n = numberOf(ci.arg(0));
            if (n < 0 || n != std::floor(n) || n > 4294967295.0)
                ci.fail(ErrorCode::RangeError, "invalid array length");
            if (!rt2.ctx.options.memoryLimit && n > double(kMaxDenseArrayLength))
                ci.fail(ErrorCode::RangeError, "arrays are dense; this length is too large");
            DynArray* a = rt2.newArray(std::size_t(n));
            // Own the array first: resizing is refused with OutOfMemory when it does not fit, and an array nobody
            // owns would never be freed.
            Value result = detail::Access::adopt(Type::Object, a);
            a->resize(std::size_t(n));
            return result;
        }
        return rt2.makeArray(ci.args());
    }, 1, proto);
    rt.arrayCtor.reset(ctor);

    rt.method(ctor, "isArray", [](CallInfo& ci) { return Value(isArray(rtOf(ci), ci.arg(0))); }, 1);

    rt.method(ctor, "of", [](CallInfo& ci) { return rtOf(ci).makeArray(ci.args()); }, 0);

    rt.method(ctor, "from", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        Vec<Value> items;
        const Value source = ci.arg(0);
        if (source.isObject() && !getProperty(rt2, source, rt2.sym.iterator.get()).isUndefined()) {
            items = iterableToVector(rt2, source);
        } else if (!source.isNullish()) {
            const std::uint32_t n = lengthOf(rt2, source);
            for (std::uint32_t i = 0; i < n; ++i) {
                // `{length: 1e9}` is a legal source; the loop must stop at the memory limit and at an interrupt.
                checkLongLoop(rt2, i, items.size() * sizeof(Value) * 2);
                items.push_back(getIndex(rt2, source, i));
            }
        }
        if (asFunction(ci.arg(1))) {
            for (std::size_t i = 0; i < items.size(); ++i)
                items[i] = callValue(rt2, ci.arg(1), Value::undefined(), {items[i], Value(double(i))});
        }
        return rt2.makeArray(std::move(items));
    }, 2);

    // ---- mutation ----
    // Native code does these two in place, when the callee is still the built-in.
    const auto keepIntrinsic = [&rt, proto](Runtime::Intrinsic which, const char* name) {
        rt.intrinsics[which].reset(detail::Access::ptr(proto->findOwn(rt.intern(name))->value));
    };
    // Every method works on anything with a length: on a copy of its elements as an array, which what changes the array
    // writes back.
    const auto arrayMethod = [&rt, proto](const char* name, bool mutates, auto fn, int arity = 0) {
        rt.method(proto, name, [fn, mutates](CallInfo& ci) -> Value {
            if (asArray(ci.thisValue()))
                return fn(ci);
            Runtime& rt2 = rtOf(ci);
            if (ci.thisValue().isNullish())
                ci.fail(ErrorCode::TypeError, "this method needs an object");
            const Value object = toObject(rt2, ci.thisValue());
            const double raw = toNumber(rt2, getProperty(rt2, object, rt2.atom.length.get()));
            const double length = std::isnan(raw) || raw <= 0 ? 0 : std::floor(raw);
            if (length > double(1u << 24))
                ci.fail(ErrorCode::RangeError, "the array-like is too long");
            DynArray* copy = rt2.newArray(std::size_t(length));
            const Value copyValue = detail::Access::adopt(Type::Object, copy);
            const Value source = ci.thisValue().type() == Type::String ? ci.thisValue() : object;
            for (double i = 0; i < length; ++i) {
                Ref<GCObject> key = toPropertyKey(rt2, Value(i));
                // What the array-like does not have is a hole in the copy, for the methods that skip them.
                if (hasProperty(rt2, source, key.get()))
                    copy->append(getProperty(rt2, source, key.get()));
                else
                    copy->appendHole();
            }
            const CallInfo inner = detail::Access::makeCallInfo(rt2.ctx.owner, &copyValue, &ci.args(), false);
            CallInfo call = inner;
            const Value result = fn(call);
            if (mutates) {
                for (std::size_t i = 0; i < copy->size(); ++i) {
                    Ref<GCObject> key = toPropertyKey(rt2, Value(double(i)));
                    if (copy->hasElement(i))
                        setProperty(rt2, object, key.get(), copy->at(i));
                    else
                        deleteProperty(rt2, object, key.get());
                }
                for (double i = double(copy->size()); i < length; ++i)
                    deleteProperty(rt2, object, toPropertyKey(rt2, Value(i)).get());
                setProperty(rt2, object, rt2.atom.length.get(), Value(double(copy->size())));
            }
            return asObject(result) == copy ? object : result;
        }, arity);
    };

    arrayMethod("push", true, [](CallInfo& ci) {
        DynArray* a = thisMutableArray(ci);
        a->appendAll(ci.args().data(), ci.args().size());
        return Value(double(a->size()));
    }, 1);

    arrayMethod("pop", true, [](CallInfo& ci) {
        DynArray* a = thisMutableArray(ci);
        if (a->empty())
            return Value::undefined();
        Value v = a->at(a->size() - 1);
        a->removeRange(a->size() - 1, a->size());
        a->trimElements();
        return v;
    }, 0);
    keepIntrinsic(Runtime::ArrayPush, "push");
    keepIntrinsic(Runtime::ArrayPop, "pop");

    arrayMethod("shift", true, [](CallInfo& ci) {
        DynArray* a = thisMutableArray(ci);
        if (a->empty())
            return Value::undefined();
        Value v = a->at(0);
        a->removeRange(0, 1);
        a->trimElements();
        return v;
    }, 0);

    arrayMethod("unshift", true, [](CallInfo& ci) {
        DynArray* a = thisMutableArray(ci);
        a->insertRange(0, ci.args().data(), ci.args().size());
        return Value(double(a->size()));
    }, 1);

    arrayMethod("splice", true, [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynArray* a = thisMutableArray(ci);
        const std::size_t n = a->size();
        const std::size_t start = std::size_t(clampIndex(toNumber(rt2, ci.arg(0)), n));
        std::size_t remove = n - start;
        if (ci.size() > 1) {
            const std::int64_t requested = toIntegerOrInfinity(toNumber(rt2, ci.arg(1)));
            remove = std::size_t(std::max<std::int64_t>(
                0, std::min<std::int64_t>(requested, std::int64_t(n - start))));
        }
        Vec<Value> removed;
        removed.reserve(remove);
        for (std::size_t i = start; i < start + remove; ++i)
            removed.push_back(a->at(i));
        Vec<Value> inserted(ci.args().begin() + std::min<std::size_t>(2, ci.size()),
                                    ci.args().end());
        a->removeRange(start, start + remove);
        a->insertRange(start, inserted.data(), inserted.size());
        a->trimElements();
        return rt2.makeArray(std::move(removed));
    }, 2);

    arrayMethod("reverse", true, [](CallInfo& ci) {
        DynArray* a = thisArray(ci);
        // One element or none: nothing is written, so a frozen array has nothing to object to.
        if (a->frozen && a->size() > 1)
            ci.fail(ErrorCode::TypeError, "the array is frozen");
        a->reverse();
        return ci.thisValue();
    }, 0);

    arrayMethod("fill", true, [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynArray* a = thisMutableArray(ci);
        const std::size_t n = a->size();
        const std::size_t from =
            ci.size() > 1 ? std::size_t(clampIndex(toNumber(rt2, ci.arg(1)), n)) : 0;
        const std::size_t to =
            ci.size() > 2 ? std::size_t(clampIndex(toNumber(rt2, ci.arg(2)), n)) : n;
        for (std::size_t i = from; i < to; ++i)
            a->put(i, ci.arg(0));
        return ci.thisValue();
    }, 1);

    arrayMethod("sort", true, [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynArray* a = thisMutableArray(ci);
        const Value comparator = ci.arg(0);
        // A comparator is script and may shrink or grow the array while it is being sorted, which would leave the
        // merge reading past the end of the elements. Sort a private copy and put it back when it is done.
        rt2.ctx.reserve(a->size() * sizeof(Value) * 2);
        // Numbers and a comparator, which is what a sort usually is: no values to shuffle, nor undefined to sort last.
        if (asFunction(comparator) && a->numbersOnly) {
            Vec<double> bare(a->numbers(), a->numbers() + a->size());
            bool plain = true;
            for (const double& d : bare) {
                std::uint64_t bits;
                std::memcpy(&bits, &d, 8);
                plain = plain && bits != kUndefinedElement;
            }
            if (plain && numericComparator(comparator) > 0) {
                mergeSort(bare, [](double x, double y) { return x - y < 0; });
            } else if (plain && numericComparator(comparator) < 0) {
                mergeSort(bare, [](double x, double y) { return y - x < 0; });
            } else if (plain) {
                CallbackLoop call(rt2, comparator);
                mergeSort(bare, [&](double x, double y) { return call.numberOfTwoNumbers(x, y) < 0; });
            }
            if (plain) {
                a->assignNumbers(bare.data(), bare.size());
                return ci.thisValue();
            }
        }
        Vec<Value> work;
        work.reserve(a->size());
        // Holes are left out of the sort and come back at the end, after every undefined.
        std::size_t holes = 0;
        for (std::size_t i = 0; i < a->size(); ++i) {
            if (a->hasElement(i))
                work.push_back(a->at(i));
            else
                ++holes;
        }
        if (asFunction(comparator)) {
            CallbackLoop call(rt2, comparator);
            mergeSort(work, [&](const Value& x, const Value& y) {
                if (x.isUndefined())
                    return false;
                if (y.isUndefined())
                    return true;
                const Value order = call(Value::undefined(), {x, y});
                return (order.isDouble() ? detail::Access::r8(order) : toNumber(rt2, order)) < 0;
            });
        } else {
            mergeSort(work, [&](const Value& x, const Value& y) {
                if (x.isUndefined())
                    return false;
                if (y.isUndefined())
                    return true;
                return toStringValue(rt2, x) < toStringValue(rt2, y);
            });
        }
        a->assignAll(work.data(), work.size());
        for (std::size_t i = 0; i < holes; ++i)
            a->appendHole();
        return ci.thisValue();
    }, 1);

    // ---- queries ----
    arrayMethod("at", false, [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynArray* a = thisArray(ci);
        std::int64_t i = toIntegerOrInfinity(toNumber(rt2, ci.arg(0)));
        if (i < 0)
            i += std::int64_t(a->size());
        if (i < 0 || i >= std::int64_t(a->size()))
            return Value::undefined();
        return a->at(std::size_t(i));
    }, 1);

    arrayMethod("slice", false, [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynArray* a = thisArray(ci);
        const std::size_t n = a->size();
        const std::size_t from =
            ci.size() > 0 ? std::size_t(clampIndex(toNumber(rt2, ci.arg(0)), n)) : 0;
        const std::size_t to =
            ci.size() > 1 && !ci.arg(1).isUndefined()
                ? std::size_t(clampIndex(toNumber(rt2, ci.arg(1)), n))
                : n;
        Vec<Value> out;
        for (std::size_t i = from; i < to; ++i)
            out.push_back(a->at(i));
        return rt2.makeArray(std::move(out));
    }, 2);

    arrayMethod("concat", false, [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynArray* self = thisArray(ci);
        std::size_t total = self->size();
        for (const Value& v : ci.args())
            total += asArray(v) ? asArray(v)->size() : 1;
        rt2.ctx.reserve(total * sizeof(Value));  // `a = a.concat(a)` doubles; refuse it before the copy, not after
        Vec<Value> out;
        out.reserve(total);
        for (std::size_t i = 0; i < self->size(); ++i)
            out.push_back(self->at(i));
        for (const Value& v : ci.args()) {
            if (DynArray* other = asArray(v)) {
                for (std::size_t i = 0; i < other->size(); ++i)
                    out.push_back(other->at(i));
            } else
                out.push_back(v);
        }
        return rt2.makeArray(std::move(out));
    }, 1);

    arrayMethod("join", false, [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynArray* a = thisArray(ci);
        const String sep =
            ci.arg(0).isUndefined() ? ascii(",") : toStringValue(rt2, ci.arg(0));
        String out;
        for (std::size_t i = 0; i < a->size(); ++i) {
            checkLongLoop(rt2, i, 0);
            String piece;
            const Value item = a->at(i);
            if (!item.isNullish())
                piece = toStringValue(rt2, item);
            // The text is built in a temporary the memory limit has not seen yet, and one piece can be megabytes,
            // so the check is made on every pass rather than every thousandth.
            rt2.ctx.reserve((out.size() + sep.size() + piece.size()) * sizeof(Char));
            if (i)
                out += sep;
            out += piece;
        }
        rt2.ctx.reserve(out.size() * sizeof(Char));
        return rt2.str(out);
    }, 1);

    arrayMethod("toString", false, [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        return invokeMethod(rt2, ci.thisValue(), rt2.intern("join"), {});
    }, 0);

    arrayMethod("indexOf", false, [](CallInfo& ci) {
        DynArray* a = thisArray(ci);
        const std::size_t n = a->size();
        for (std::size_t i = 0; i < n; ++i) {
            Value item;
            if (presentElement(rtOf(ci), a, i, item) && strictEquals(item, ci.arg(0)))
                return Value(double(i));
        }
        return Value(-1.0);
    }, 1);

    arrayMethod("lastIndexOf", false, [](CallInfo& ci) {
        DynArray* a = thisArray(ci);
        for (std::size_t i = a->size(); i-- > 0;) {
            Value item;
            if (presentElement(rtOf(ci), a, i, item) && strictEquals(item, ci.arg(0)))
                return Value(double(i));
        }
        return Value(-1.0);
    }, 1);

    arrayMethod("includes", false, [](CallInfo& ci) {
        DynArray* a = thisArray(ci);
        for (std::size_t i = 0; i < a->size(); ++i) {
            if (sameValueZero(a->at(i), ci.arg(0)))
                return Value(true);
        }
        return Value(false);
    }, 1);

    // ---- higher order ----
    // As far as the array reached when the call began: what the callback adds is not visited, and what it takes away is
    // skipped, or for `find` and `findIndex`, visited as undefined.
    auto each = [](CallInfo& ci, auto&& step, bool visitsGone = false) {
        Runtime& rt2 = rtOf(ci);
        DynArray* a = thisArray(ci);
        const Value fn = ci.arg(0);
        if (!asFunction(fn))
            ci.fail(ErrorCode::TypeError, "a callback function is required");
        Value produced;
        CallbackLoop call(rt2, fn);
        // The array itself, as the third argument, is counted once for the whole loop.
        const Value self = ci.arg(1);
        Value args[3] = {Value::undefined(), Value::undefined(), ci.thisValue()};
        const std::size_t n = a->size();
        for (std::size_t i = 0; i < n; ++i) {
            Value item = Value::undefined();
            if (!presentElement(rt2, a, i, item) && !visitsGone)
                continue;
            args[0] = item;
            args[1] = Value(double(i));
            produced = call.withArgs(self, args, 3);
            if (!step(i, args[0], produced))
                break;
        }
        return produced;
    };

    arrayMethod("forEach", false, [each](CallInfo& ci) {
        each(ci, [](std::size_t, const Value&, Value&) { return true; });
        return Value::undefined();
    }, 1);

    // What these make goes straight into the array they answer with.
    arrayMethod("map", false, [each](CallInfo& ci) {
        const std::size_t n = thisArray(ci)->size();
        DynArray* out = rtOf(ci).newArray(n);
        Value result = detail::Access::adopt(Type::Object, out);
        // As long as the array was, and a hole where it had one.
        out->resize(n);
        each(ci, [out](std::size_t i, const Value&, Value& produced) {
            out->put(i, produced);
            return true;
        });
        return result;
    }, 1);

    arrayMethod("filter", false, [each](CallInfo& ci) {
        DynArray* out = rtOf(ci).newArray(0);
        Value result = detail::Access::adopt(Type::Object, out);
        each(ci, [out](std::size_t, const Value& item, Value& produced) {
            if (produced.type() == Type::Bool ? boolOf(produced) : toBoolean(produced))
                out->append(item);
            return true;
        });
        return result;
    }, 1);

    arrayMethod("find", false, [each](CallInfo& ci) {
        Value found = Value::undefined();
        each(ci, [&found](std::size_t, const Value& item, Value& produced) {
            if (!toBoolean(produced))
                return true;
            found = item;
            return false;
        }, true);
        return found;
    }, 1);

    arrayMethod("findIndex", false, [each](CallInfo& ci) {
        double found = -1;
        each(ci, [&found](std::size_t i, const Value&, Value& produced) {
            if (!toBoolean(produced))
                return true;
            found = double(i);
            return false;
        }, true);
        return Value(found);
    }, 1);

    arrayMethod("some", false, [each](CallInfo& ci) {
        bool any = false;
        each(ci, [&any](std::size_t, const Value&, Value& produced) {
            any = toBoolean(produced);
            return !any;
        });
        return Value(any);
    }, 1);

    arrayMethod("every", false, [each](CallInfo& ci) {
        bool all = true;
        each(ci, [&all](std::size_t, const Value&, Value& produced) {
            all = toBoolean(produced);
            return all;
        });
        return Value(all);
    }, 1);

    arrayMethod("flat", false, [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        // `flat(Infinity)` is the usual spelling of "all the way"; converting an infinity to int is undefined, so the
        // depth is clamped as a number first.
        const double requested = ci.arg(0).isUndefined() ? 1.0 : toNumber(rt2, ci.arg(0));
        const std::size_t depth =
            std::isnan(requested) || requested < 1 ? 0 : (requested > 1e9 ? std::size_t(1e9) : std::size_t(requested));
        Vec<Value> out;
        std::size_t level = 0;
        std::function<void(DynArray*, std::size_t)> walk =
            [&](DynArray* items, std::size_t left) {
                NestingScope nest(level);
                if (nest.tooDeep(10000))
                    throwError(rt2, ErrorCode::RangeError, "array is nested too deeply");
                for (std::size_t i = 0; i < items->size(); ++i) {
                    const Value v = items->at(i);
                    DynArray* nested = asArray(v);
                    if (nested && left > 0)
                        walk(nested, left - 1);
                    else {
                        checkLongLoop(rt2, out.size(), out.size() * sizeof(Value) * 2);
                        out.push_back(v);
                    }
                }
            };
        walk(thisArray(ci), depth);
        return rt2.makeArray(std::move(out));
    }, 1);

    arrayMethod("flatMap", false, [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynArray* a = thisArray(ci);
        Vec<Value> out;
        for (std::size_t i = 0; i < a->size(); ++i) {
            Value produced = callValue(rt2, ci.arg(0), ci.arg(1),
                                       {a->at(i), Value(double(i)), ci.thisValue()});
            if (DynArray* nested = asArray(produced)) {
                for (std::size_t k = 0; k < nested->size(); ++k)
                    out.push_back(nested->at(k));
            } else
                out.push_back(produced);
        }
        return rt2.makeArray(std::move(out));
    }, 1);

    auto reduce = [](CallInfo& ci, bool reversed) {
        Runtime& rt2 = rtOf(ci);
        DynArray* a = thisArray(ci);
        const Value fn = ci.arg(0);
        if (!asFunction(fn))
            ci.fail(ErrorCode::TypeError, "a callback function is required");
        const std::size_t n = a->size();
        std::size_t at = 0;
        Value acc;
        if (ci.size() > 1) {
            acc = ci.arg(1);
        } else {
            // The first element there is, holes skipped.
            while (at < n && !presentElement(rt2, a, reversed ? n - 1 - at : at, acc))
                ++at;
            if (at >= n)
                ci.fail(ErrorCode::TypeError, "reduce of an empty array with no initial value");
            ++at;
        }
        CallbackLoop call(rt2, fn);
        Value args[4] = {std::move(acc), Value::undefined(), Value::undefined(), ci.thisValue()};
        for (; at < n; ++at) {
            const std::size_t i = reversed ? n - 1 - at : at;
            // The callback may have shortened the array; an element that is gone is skipped, as in JavaScript.
            if (!presentElement(rt2, a, i, args[1]))
                continue;
            args[2] = Value(double(i));
            args[0] = call.withArgs(Value::undefined(), args, 4);
        }
        return args[0];
    };
    arrayMethod("reduce", false, [reduce](CallInfo& ci) { return reduce(ci, false); }, 1);
    arrayMethod("reduceRight", false, [reduce](CallInfo& ci) { return reduce(ci, true); }, 1);
    keepIntrinsic(Runtime::ArrayForEach, "forEach");
    keepIntrinsic(Runtime::ArrayMap, "map");
    keepIntrinsic(Runtime::ArrayFilter, "filter");
    keepIntrinsic(Runtime::ArrayReduce, "reduce");

    arrayMethod("findLast", false, [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynArray* a = thisArray(ci);
        if (!asFunction(ci.arg(0)))
            ci.fail(ErrorCode::TypeError, "a callback function is required");
        for (std::size_t i = a->size(); i-- > 0;) {
            const Value item = i < a->size() ? a->at(i) : Value::undefined();
            if (toBoolean(callValue(rt2, ci.arg(0), ci.arg(1), {item, Value(double(i)), ci.thisValue()})))
                return item;
        }
        return Value::undefined();
    }, 1);
    arrayMethod("findLastIndex", false, [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynArray* a = thisArray(ci);
        if (!asFunction(ci.arg(0)))
            ci.fail(ErrorCode::TypeError, "a callback function is required");
        for (std::size_t i = a->size(); i-- > 0;) {
            const Value item = i < a->size() ? a->at(i) : Value::undefined();
            if (toBoolean(callValue(rt2, ci.arg(0), ci.arg(1), {item, Value(double(i)), ci.thisValue()})))
                return Value(double(i));
        }
        return Value(-1.0);
    }, 1);
    // What comes back from these is a new array: the same method is asked of a copy.
    const auto copied = [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynArray* a = thisArray(ci);
        std::vector<Value> items;
        // As long as it was to begin with, each element read as Get reads it: a getter may change the rest.
        const std::size_t n = a->size();
        for (std::size_t i = 0; i < n; ++i) {
            Value item = Value::undefined();
            if (!presentElement(rt2, a, i, item) && i >= a->size())
                item = Value::undefined();
            items.push_back(item);
        }
        return rt2.makeArray(items);
    };
    arrayMethod("toReversed", false, [copied](CallInfo& ci) -> Value {
        const Value out = copied(ci);
        return invokeMethod(rtOf(ci), out, rtOf(ci).intern("reverse"), {});
    }, 0);
    arrayMethod("toSorted", false, [copied](CallInfo& ci) -> Value {
        if (!ci.arg(0).isUndefined() && !asFunction(ci.arg(0)))
            ci.fail(ErrorCode::TypeError, "the comparator must be a function");
        const Value out = copied(ci);
        return invokeMethod(rtOf(ci), out, rtOf(ci).intern("sort"), {ci.arg(0)});
    }, 1);
    arrayMethod("toSpliced", false, [copied](CallInfo& ci) -> Value {
        const Value out = copied(ci);
        invokeMethod(rtOf(ci), out, rtOf(ci).intern("splice"), ci.args());
        return out;
    }, 2);
    arrayMethod("with", false, [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynArray* a = thisArray(ci);
        const std::size_t n = a->size();
        std::int64_t at = toIntegerOrInfinity(toNumber(rt2, ci.arg(0)));
        if (at < 0)
            at += std::int64_t(n);
        if (at < 0 || at >= std::int64_t(n))
            ci.fail(ErrorCode::RangeError, "the index is out of range");
        std::vector<Value> items;
        // The element it replaces is never read.
        for (std::size_t i = 0; i < n; ++i) {
            Value item = Value::undefined();
            if (std::int64_t(i) == at)
                item = ci.arg(1);
            else
                presentElement(rt2, a, i, item);
            items.push_back(item);
        }
        return rt2.makeArray(items);
    }, 2);
    arrayMethod("copyWithin", true, [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynArray* a = thisMutableArray(ci);
        const std::size_t n = a->size();
        const auto to = std::size_t(clampIndex(toNumber(rt2, ci.arg(0)), n));
        const auto from = std::size_t(clampIndex(toNumber(rt2, ci.arg(1)), n));
        const auto end = ci.arg(2).isUndefined() ? n : std::size_t(clampIndex(toNumber(rt2, ci.arg(2)), n));
        std::vector<Value> items;
        for (std::size_t i = from; i < end; ++i)
            items.push_back(a->at(i));
        for (std::size_t i = 0; i < items.size() && to + i < n; ++i)
            a->put(to + i, items[i]);
        return ci.thisValue();
    }, 2);

    // ---- iteration ----
    arrayMethod("values", false, [](CallInfo& ci) { return makeArrayIterator(rtOf(ci), toObject(rtOf(ci), ci.thisValue()), 0); }, 0);
    arrayMethod("keys", false, [](CallInfo& ci) { return makeArrayIterator(rtOf(ci), toObject(rtOf(ci), ci.thisValue()), 1); }, 0);
    arrayMethod("entries", false, [](CallInfo& ci) { return makeArrayIterator(rtOf(ci), toObject(rtOf(ci), ci.thisValue()), 2); }, 0);
    proto->putOwn(rt.sym.iterator.get(), proto->findOwn(rt.intern("values"))->value,
                  PropWritable | PropConfigurable);
    keepIntrinsic(Runtime::ArrayValues, "values");
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
