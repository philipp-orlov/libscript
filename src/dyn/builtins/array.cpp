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
void mergeSort(Vec<Value>& v, const std::function<bool(const Value&, const Value&)>& less)
{
    if (v.size() < 2)
        return;
    Vec<Value> scratch(v.size());
    for (std::size_t width = 1; width < v.size(); width *= 2) {
        for (std::size_t start = 0; start < v.size(); start += 2 * width) {
            const std::size_t mid = std::min(start + width, v.size());
            const std::size_t end = std::min(start + 2 * width, v.size());
            std::size_t a = start;
            std::size_t b = mid;
            for (std::size_t out = start; out < end; ++out) {
                if (a < mid && (b >= end || !less(v[b], v[a])))
                    scratch[out] = v[a++];
                else
                    scratch[out] = v[b++];
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
    protoArray->proto.reset(rt.objectProto.get());
    rt.arrayProto.reset(protoArray, true);
    DynObject* proto = protoArray;

    DynFunction* ctor = rt.makeConstructor("Array", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        if (ci.size() == 1 && ci.arg(0).isNumber()) {
            const double n = numberOf(ci.arg(0));
            if (n < 0 || n != std::floor(n) || n > 4294967295.0)
                ci.fail(ErrorCode::RangeError, "invalid array length");
            DynArray* a = rt2.newArray(std::size_t(n));
            a->resize(std::size_t(n));
            return detail::Access::adopt(Type::Object, a);
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
            for (std::uint32_t i = 0; i < n; ++i)
                items.push_back(getIndex(rt2, source, i));
        }
        if (asFunction(ci.arg(1))) {
            for (std::size_t i = 0; i < items.size(); ++i)
                items[i] = callValue(rt2, ci.arg(1), Value::undefined(), {items[i], Value(double(i))});
        }
        return rt2.makeArray(std::move(items));
    }, 2);

    // ---- mutation ----
    rt.method(proto, "push", [](CallInfo& ci) {
        DynArray* a = thisArray(ci);
        a->appendAll(ci.args().data(), ci.args().size());
        return Value(double(a->elements.size()));
    }, 1);

    rt.method(proto, "pop", [](CallInfo& ci) {
        DynArray* a = thisArray(ci);
        if (a->elements.empty())
            return Value::undefined();
        Value v = a->elements.back();
        a->elements.pop_back();
        a->trimElements();
        return v;
    }, 0);

    rt.method(proto, "shift", [](CallInfo& ci) {
        DynArray* a = thisArray(ci);
        if (a->elements.empty())
            return Value::undefined();
        Value v = a->elements.front();
        a->elements.erase(a->elements.begin());
        a->trimElements();
        return v;
    }, 0);

    rt.method(proto, "unshift", [](CallInfo& ci) {
        DynArray* a = thisArray(ci);
        const std::size_t before = a->elements.capacity();
        a->elements.insert(a->elements.begin(), ci.args().begin(), ci.args().end());
        a->chargeIfGrown(a->elements, before);
        return Value(double(a->elements.size()));
    }, 1);

    rt.method(proto, "splice", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynArray* a = thisArray(ci);
        const std::size_t n = a->elements.size();
        const std::size_t start = std::size_t(clampIndex(toNumber(rt2, ci.arg(0)), n));
        std::size_t remove = n - start;
        if (ci.size() > 1) {
            const std::int64_t requested = toIntegerOrInfinity(toNumber(rt2, ci.arg(1)));
            remove = std::size_t(std::max<std::int64_t>(
                0, std::min<std::int64_t>(requested, std::int64_t(n - start))));
        }
        Vec<Value> removed(a->elements.begin() + std::ptrdiff_t(start),
                                   a->elements.begin() + std::ptrdiff_t(start + remove));
        Vec<Value> inserted(ci.args().begin() + std::min<std::size_t>(2, ci.size()),
                                    ci.args().end());
        a->elements.erase(a->elements.begin() + std::ptrdiff_t(start),
                          a->elements.begin() + std::ptrdiff_t(start + remove));
        a->elements.insert(a->elements.begin() + std::ptrdiff_t(start), inserted.begin(),
                           inserted.end());
        a->trimElements();
        return rt2.makeArray(std::move(removed));
    }, 2);

    rt.method(proto, "reverse", [](CallInfo& ci) {
        DynArray* a = thisArray(ci);
        std::reverse(a->elements.begin(), a->elements.end());
        return ci.thisValue();
    }, 0);

    rt.method(proto, "fill", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynArray* a = thisArray(ci);
        const std::size_t n = a->elements.size();
        const std::size_t from =
            ci.size() > 1 ? std::size_t(clampIndex(toNumber(rt2, ci.arg(1)), n)) : 0;
        const std::size_t to =
            ci.size() > 2 ? std::size_t(clampIndex(toNumber(rt2, ci.arg(2)), n)) : n;
        for (std::size_t i = from; i < to; ++i)
            a->elements[i] = ci.arg(0);
        return ci.thisValue();
    }, 1);

    rt.method(proto, "sort", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynArray* a = thisArray(ci);
        const Value comparator = ci.arg(0);
        if (asFunction(comparator)) {
            mergeSort(a->elements, [&](const Value& x, const Value& y) {
                if (x.isUndefined())
                    return false;
                if (y.isUndefined())
                    return true;
                return toNumber(rt2, callValue(rt2, comparator, Value::undefined(), {x, y})) < 0;
            });
        } else {
            mergeSort(a->elements, [&](const Value& x, const Value& y) {
                if (x.isUndefined())
                    return false;
                if (y.isUndefined())
                    return true;
                return toStringValue(rt2, x) < toStringValue(rt2, y);
            });
        }
        return ci.thisValue();
    }, 1);

    // ---- queries ----
    rt.method(proto, "at", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynArray* a = thisArray(ci);
        std::int64_t i = toIntegerOrInfinity(toNumber(rt2, ci.arg(0)));
        if (i < 0)
            i += std::int64_t(a->elements.size());
        if (i < 0 || i >= std::int64_t(a->elements.size()))
            return Value::undefined();
        return a->elements[std::size_t(i)];
    }, 1);

    rt.method(proto, "slice", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynArray* a = thisArray(ci);
        const std::size_t n = a->elements.size();
        const std::size_t from =
            ci.size() > 0 ? std::size_t(clampIndex(toNumber(rt2, ci.arg(0)), n)) : 0;
        const std::size_t to =
            ci.size() > 1 && !ci.arg(1).isUndefined()
                ? std::size_t(clampIndex(toNumber(rt2, ci.arg(1)), n))
                : n;
        Vec<Value> out;
        for (std::size_t i = from; i < to; ++i)
            out.push_back(a->elements[i]);
        return rt2.makeArray(std::move(out));
    }, 2);

    rt.method(proto, "concat", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        Vec<Value> out = thisArray(ci)->elements;
        for (const Value& v : ci.args()) {
            if (DynArray* other = asArray(v))
                out.insert(out.end(), other->elements.begin(), other->elements.end());
            else
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
        for (std::size_t i = 0; i < a->elements.size(); ++i) {
            if (i)
                out += sep;
            if (!a->elements[i].isNullish())
                out += toStringValue(rt2, a->elements[i]);
        }
        return rt2.str(out);
    }, 1);

    rt.method(proto, "toString", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        return invokeMethod(rt2, ci.thisValue(), rt2.intern("join"), {});
    }, 0);

    rt.method(proto, "indexOf", [](CallInfo& ci) {
        DynArray* a = thisArray(ci);
        for (std::size_t i = 0; i < a->elements.size(); ++i) {
            if (strictEquals(a->elements[i], ci.arg(0)))
                return Value(double(i));
        }
        return Value(-1.0);
    }, 1);

    rt.method(proto, "lastIndexOf", [](CallInfo& ci) {
        DynArray* a = thisArray(ci);
        for (std::size_t i = a->elements.size(); i-- > 0;) {
            if (strictEquals(a->elements[i], ci.arg(0)))
                return Value(double(i));
        }
        return Value(-1.0);
    }, 1);

    rt.method(proto, "includes", [](CallInfo& ci) {
        DynArray* a = thisArray(ci);
        for (const Value& v : a->elements) {
            if (sameValueZero(v, ci.arg(0)))
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
        for (std::size_t i = 0; i < a->elements.size(); ++i) {
            const Value item = a->elements[i];
            produced = callValue(rt2, fn, ci.arg(1), {item, Value(double(i)), ci.thisValue()});
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
        const int depth = ci.arg(0).isUndefined() ? 1 : int(toNumber(rt2, ci.arg(0)));
        Vec<Value> out;
        std::function<void(const Vec<Value>&, int)> walk =
            [&](const Vec<Value>& items, int left) {
                for (const Value& v : items) {
                    DynArray* nested = asArray(v);
                    if (nested && left > 0)
                        walk(nested->elements, left - 1);
                    else
                        out.push_back(v);
                }
            };
        walk(thisArray(ci)->elements, depth);
        return rt2.makeArray(std::move(out));
    }, 1);

    rt.method(proto, "flatMap", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynArray* a = thisArray(ci);
        Vec<Value> out;
        for (std::size_t i = 0; i < a->elements.size(); ++i) {
            Value produced = callValue(rt2, ci.arg(0), ci.arg(1),
                                       {a->elements[i], Value(double(i)), ci.thisValue()});
            if (DynArray* nested = asArray(produced))
                out.insert(out.end(), nested->elements.begin(), nested->elements.end());
            else
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
        const std::size_t n = a->elements.size();
        std::size_t at = 0;
        Value acc;
        if (ci.size() > 1) {
            acc = ci.arg(1);
        } else {
            if (n == 0)
                ci.fail(ErrorCode::TypeError, "reduce of an empty array with no initial value");
            acc = a->elements[reversed ? n - 1 : 0];
            at = 1;
        }
        for (; at < n; ++at) {
            const std::size_t i = reversed ? n - 1 - at : at;
            acc = callValue(rt2, fn, Value::undefined(), {acc, a->elements[i], Value(double(i)),
                                               ci.thisValue()});
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
