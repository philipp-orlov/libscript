// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Array, its prototype methods and the iterator protocol helpers.

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/numconv.hpp"
#include "script/internal/dyn/vm.hpp"
#include "script/internal/stack_guard.hpp"

namespace script {
namespace internal {
namespace dyn {

namespace {

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

/// Iterates live, so growing the array during a `for..of` is observable, as it
/// is in a real engine.
Value makeIndexIterator(Runtime& rt, Value target, int mode)  // 0 values, 1 keys, 2 entries
{
    DynObject* it = rt.newObject(rt.arrayIteratorProto.get());
    Value result = detail::Access::adopt(Type::Object, it);
    auto index = std::make_shared<std::uint32_t>(0);
    rt.method(it, "next", [target, index, mode](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynObject* out = rt2.newObject(rt2.objectProto.get());
        Value record = detail::Access::adopt(Type::Object, out);
        const std::uint32_t length = lengthOf(rt2, target);
        if (*index >= length) {
            out->putOwn(rt2.atom.value.get(), Value::undefined());
            out->putOwn(rt2.atom.done.get(), Value(true));
            return record;
        }
        const std::uint32_t i = (*index)++;
        Value item;
        if (mode == 1)
            item = Value(double(i));
        else if (mode == 2)
            item = rt2.makeArray({Value(double(i)), getIndex(rt2, target, i)});
        else
            item = getIndex(rt2, target, i);
        out->putOwn(rt2.atom.value.get(), item);
        out->putOwn(rt2.atom.done.get(), Value(false));
        return record;
    }, 0);
    return result;
}

}  // namespace

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
template <class Less>
void mergeSort(Vec<Value>& v, const Less& less)
{
    if (v.size() < 2)
        return;
    // Each pass moves every value once, from one buffer to the other, so no count changes on the way.
    Vec<Value> scratch(v.size());
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

    rt.method(ctor, "isArray", [](CallInfo& ci) { return Value(asArray(ci.arg(0)) != nullptr); }, 1);

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
    rt.method(proto, "push", [](CallInfo& ci) {
        DynArray* a = thisMutableArray(ci);
        a->appendAll(ci.args().data(), ci.args().size());
        return Value(double(a->size()));
    }, 1);

    rt.method(proto, "pop", [](CallInfo& ci) {
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

    rt.method(proto, "shift", [](CallInfo& ci) {
        DynArray* a = thisMutableArray(ci);
        if (a->empty())
            return Value::undefined();
        Value v = a->at(0);
        a->removeRange(0, 1);
        a->trimElements();
        return v;
    }, 0);

    rt.method(proto, "unshift", [](CallInfo& ci) {
        DynArray* a = thisMutableArray(ci);
        a->insertRange(0, ci.args().data(), ci.args().size());
        return Value(double(a->size()));
    }, 1);

    rt.method(proto, "splice", [](CallInfo& ci) {
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

    rt.method(proto, "reverse", [](CallInfo& ci) {
        DynArray* a = thisArray(ci);
        // One element or none: nothing is written, so a frozen array has nothing to object to.
        if (a->frozen && a->size() > 1)
            ci.fail(ErrorCode::TypeError, "the array is frozen");
        a->reverse();
        return ci.thisValue();
    }, 0);

    rt.method(proto, "fill", [](CallInfo& ci) {
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

    rt.method(proto, "sort", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynArray* a = thisMutableArray(ci);
        const Value comparator = ci.arg(0);
        // A comparator is script and may shrink or grow the array while it is being sorted, which would leave the
        // merge reading past the end of the elements. Sort a private copy and put it back when it is done.
        rt2.ctx.reserve(a->size() * sizeof(Value) * 2);
        Vec<Value> work;
        work.reserve(a->size());
        for (std::size_t i = 0; i < a->size(); ++i)
            work.push_back(a->at(i));
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
        return ci.thisValue();
    }, 1);

    // ---- queries ----
    rt.method(proto, "at", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynArray* a = thisArray(ci);
        std::int64_t i = toIntegerOrInfinity(toNumber(rt2, ci.arg(0)));
        if (i < 0)
            i += std::int64_t(a->size());
        if (i < 0 || i >= std::int64_t(a->size()))
            return Value::undefined();
        return a->at(std::size_t(i));
    }, 1);

    rt.method(proto, "slice", [](CallInfo& ci) {
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

    rt.method(proto, "concat", [](CallInfo& ci) {
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

    rt.method(proto, "join", [](CallInfo& ci) {
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

    rt.method(proto, "toString", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        return invokeMethod(rt2, ci.thisValue(), rt2.intern("join"), {});
    }, 0);

    rt.method(proto, "indexOf", [](CallInfo& ci) {
        DynArray* a = thisArray(ci);
        for (std::size_t i = 0; i < a->size(); ++i) {
            if (strictEquals(a->at(i), ci.arg(0)))
                return Value(double(i));
        }
        return Value(-1.0);
    }, 1);

    rt.method(proto, "lastIndexOf", [](CallInfo& ci) {
        DynArray* a = thisArray(ci);
        for (std::size_t i = a->size(); i-- > 0;) {
            if (strictEquals(a->at(i), ci.arg(0)))
                return Value(double(i));
        }
        return Value(-1.0);
    }, 1);

    rt.method(proto, "includes", [](CallInfo& ci) {
        DynArray* a = thisArray(ci);
        for (std::size_t i = 0; i < a->size(); ++i) {
            if (sameValueZero(a->at(i), ci.arg(0)))
                return Value(true);
        }
        return Value(false);
    }, 1);

    // ---- higher order ----
    auto each = [](CallInfo& ci, const std::function<bool(std::size_t, const Value&, Value&)>& step) {
        Runtime& rt2 = rtOf(ci);
        DynArray* a = thisArray(ci);
        const Value fn = ci.arg(0);
        if (!asFunction(fn))
            ci.fail(ErrorCode::TypeError, "a callback function is required");
        Value produced;
        CallbackLoop call(rt2, fn);
        for (std::size_t i = 0; i < a->size(); ++i) {
            const Value item = a->at(i);
            produced = call(ci.arg(1), {item, Value(double(i)), ci.thisValue()});
            if (!step(i, item, produced))
                break;
        }
        return produced;
    };

    rt.method(proto, "forEach", [each](CallInfo& ci) {
        each(ci, [](std::size_t, const Value&, Value&) { return true; });
        return Value::undefined();
    }, 1);

    rt.method(proto, "map", [each](CallInfo& ci) {
        Vec<Value> out;
        each(ci, [&out](std::size_t, const Value&, Value& produced) {
            out.push_back(produced);
            return true;
        });
        return rtOf(ci).makeArray(std::move(out));
    }, 1);

    rt.method(proto, "filter", [each](CallInfo& ci) {
        Vec<Value> out;
        each(ci, [&out](std::size_t, const Value& item, Value& produced) {
            if (toBoolean(produced))
                out.push_back(item);
            return true;
        });
        return rtOf(ci).makeArray(std::move(out));
    }, 1);

    rt.method(proto, "find", [each](CallInfo& ci) {
        Value found;
        each(ci, [&found](std::size_t, const Value& item, Value& produced) {
            if (!toBoolean(produced))
                return true;
            found = item;
            return false;
        });
        return found;
    }, 1);

    rt.method(proto, "findIndex", [each](CallInfo& ci) {
        double found = -1;
        each(ci, [&found](std::size_t i, const Value&, Value& produced) {
            if (!toBoolean(produced))
                return true;
            found = double(i);
            return false;
        });
        return Value(found);
    }, 1);

    rt.method(proto, "some", [each](CallInfo& ci) {
        bool any = false;
        each(ci, [&any](std::size_t, const Value&, Value& produced) {
            any = toBoolean(produced);
            return !any;
        });
        return Value(any);
    }, 1);

    rt.method(proto, "every", [each](CallInfo& ci) {
        bool all = true;
        each(ci, [&all](std::size_t, const Value&, Value& produced) {
            all = toBoolean(produced);
            return all;
        });
        return Value(all);
    }, 1);

    rt.method(proto, "flat", [](CallInfo& ci) {
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

    rt.method(proto, "flatMap", [](CallInfo& ci) {
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
            if (n == 0)
                ci.fail(ErrorCode::TypeError, "reduce of an empty array with no initial value");
            acc = a->at(reversed ? n - 1 : 0);
            at = 1;
        }
        CallbackLoop call(rt2, fn);
        for (; at < n; ++at) {
            const std::size_t i = reversed ? n - 1 - at : at;
            // The callback may have shortened the array; an element that is gone is skipped, as in JavaScript.
            if (i >= a->size())
                continue;
            acc = call(Value::undefined(), {acc, a->at(i), Value(double(i)), ci.thisValue()});
        }
        return acc;
    };
    rt.method(proto, "reduce", [reduce](CallInfo& ci) { return reduce(ci, false); }, 1);
    rt.method(proto, "reduceRight", [reduce](CallInfo& ci) { return reduce(ci, true); }, 1);

    // ---- iteration ----
    rt.method(proto, "values",
              [](CallInfo& ci) { return makeIndexIterator(rtOf(ci), ci.thisValue(), 0); }, 0);
    rt.method(proto, "keys",
              [](CallInfo& ci) { return makeIndexIterator(rtOf(ci), ci.thisValue(), 1); }, 0);
    rt.method(proto, "entries",
              [](CallInfo& ci) { return makeIndexIterator(rtOf(ci), ci.thisValue(), 2); }, 0);
    proto->putOwn(rt.sym.iterator.get(), proto->findOwn(rt.intern("values"))->value,
                  PropWritable | PropConfigurable);
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
