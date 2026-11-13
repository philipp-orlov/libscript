// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Promise, the job queue the host drains with `Context::runJobs`, and async functions (generators that a promise drives).
#include "builtins_util.hpp"

namespace script {
namespace internal {
namespace vm {

void VM::enqueueJob(int kind, JSValue a, JSValue b, JSValue c)
{
    retain(a);
    retain(b);
    retain(c);
    jobs.push_back(MicroJob{kind, a, b, c});
}

namespace bi {

namespace {

struct Reaction
{
    Handle derived;  ///< the promise `then` made, or undefined for an internal reaction
    Handle handler;  ///< a function, or undefined to pass the outcome on
};

struct PromiseData : NativeState
{
    enum State : std::uint8_t { Pending, Fulfilled, Rejected };
    State state = Pending;
    bool handled = false;
    JSValue result = JSValue::undefined();
    std::vector<Reaction> onFulfilled, onRejected;

    ~PromiseData() override { clear_(); }
    void clear(VM&) noexcept override { clear_(); }
    void clear_() noexcept
    {
        release(result);
        result = JSValue::undefined();
        onFulfilled.clear();
        onRejected.clear();
    }
    void values(std::vector<JSValue>& out) const override
    {
        out.push_back(result);
        for (const auto* list : {&onFulfilled, &onRejected})
            for (const Reaction& r : *list) {
                out.push_back(r.derived.get());
                out.push_back(r.handler.get());
            }
    }
};

PromiseData* dataOf(JSValue v)
{
    if (isObject(v) && asObject(v)->cls() == ObjClass::Promise)
        if (Rare* r = asObject(v)->rare())
            return static_cast<PromiseData*>(r->ext);
    return nullptr;
}

JSValue newPromiseObject(VM& vm, JSObject* proto = nullptr)
{
    JSObject* o = vm.newObject(proto ? proto : vm.protos.promise);
    Handle keep = Handle::adopt(toJSValue(o));
    o->h.cls = std::uint8_t(ObjClass::Promise);
    o->rareOf(vm).ext = new PromiseData();
    return keep.take();
}

/// The pending exception as a value of its own; the VM has none pending afterwards.
JSValue takeException(VM& vm)
{
    JSValue e = vm.exception;
    vm.exception = JSValue::undefined();
    vm.exceptionCode = ErrorCode::Ok;
    return e;
}

void settle(VM& vm, JSValue promise, PromiseData::State state, JSValue value)
{
    PromiseData* d = dataOf(promise);
    if (!d || d->state != PromiseData::Pending)
        return;
    retain(value);
    d->result = value;
    d->state = state;
    std::vector<Reaction> reactions;
    reactions.swap(state == PromiseData::Fulfilled ? d->onFulfilled : d->onRejected);
    d->onFulfilled.clear();
    d->onRejected.clear();
    for (const Reaction& r : reactions)
        vm.enqueueJob(state == PromiseData::Fulfilled ? 0 : 1, r.handler.get(), r.derived.get(), value);
}

void rejectPromise(VM& vm, JSValue promise, JSValue reason) { settle(vm, promise, PromiseData::Rejected, reason); }

/// The resolve function's steps: a thenable is followed, anything else fulfils.
void resolvePromise(VM& vm, JSValue promise, JSValue resolution)
{
    if (resolution == promise) {
        JSValue e = vm.makeError(ErrorCode::TypeError, "Chaining cycle detected for promise #<Promise>");
        rejectPromise(vm, promise, e);
        release(e);
        return;
    }
    if (!isObject(resolution)) {
        settle(vm, promise, PromiseData::Fulfilled, resolution);
        return;
    }
    JSValue then = getProperty(vm, resolution, vm.internKey(std::string_view("then")));
    if (then.isException()) {
        JSValue e = takeException(vm);
        rejectPromise(vm, promise, e);
        release(e);
        return;
    }
    Handle thenKeep = Handle::adopt(then);
    if (!isFunction(then)) {
        settle(vm, promise, PromiseData::Fulfilled, resolution);
        return;
    }
    vm.enqueueJob(2, promise, resolution, then);
}

/// A native function that carries two values and an integer.
JSFunction* closureFn(VM& vm, FastNative fn, JSValue a, JSValue b, std::uint32_t n)
{
    JSFunction* f = vm.newNativeFunction(vm.protos.function, fn, nullptr, nullptr, 1);
    Rare& r = f->rareOf(vm);
    r.nativeData = f;
    retain(a);
    retain(b);
    r.boundTarget = a;
    r.boundThis = b;
    r.fnLength = n;
    return f;
}

Rare& carried(void* data) { return *static_cast<JSFunction*>(data)->rare(); }

NATIVE(resolverFn)
{
    UNUSED();
    Rare& r = carried(data);
    // boundThis is `[settled]`, shared by the pair.
    auto* flag = static_cast<JSArray*>(asObject(r.boundThis));
    if (flag->at(0) == JSValue::boolean(true))
        return JSValue::undefined();
    flag->setAt(vm, 0, JSValue::boolean(true));
    if (r.fnLength == 0)
        resolvePromise(vm, r.boundTarget, arg(args, argc, 0));
    else
        rejectPromise(vm, r.boundTarget, arg(args, argc, 0));
    return JSValue::undefined();
}

void makeResolvers(VM& vm, JSValue promise, Handle* resolve, Handle* reject)
{
    JSArray* flag = vm.newArray(vm.protos.array);
    Handle flagKeep = Handle::adopt(toJSValue(flag));
    flag->append(vm, JSValue::boolean(false));
    *resolve = Handle::adopt(toJSValue(closureFn(vm, resolverFn, promise, flagKeep.get(), 0)));
    *reject = Handle::adopt(toJSValue(closureFn(vm, resolverFn, promise, flagKeep.get(), 1)));
}

void performThen(VM& vm, JSValue promise, JSValue onFulfilled, JSValue onRejected, JSValue derived)
{
    PromiseData* d = dataOf(promise);
    Reaction f{Handle::retained(derived), Handle::retained(isFunction(onFulfilled) ? onFulfilled : JSValue::undefined())};
    Reaction r{Handle::retained(derived), Handle::retained(isFunction(onRejected) ? onRejected : JSValue::undefined())};
    d->handled = true;
    if (d->state == PromiseData::Pending) {
        d->onFulfilled.push_back(std::move(f));
        d->onRejected.push_back(std::move(r));
    } else if (d->state == PromiseData::Fulfilled) {
        vm.enqueueJob(0, f.handler.get(), f.derived.get(), d->result);
    } else {
        vm.enqueueJob(1, r.handler.get(), r.derived.get(), d->result);
    }
}

/// `Promise.resolve(v)`: v itself when it is a promise.
JSValue promiseResolve(VM& vm, JSValue v)
{
    if (dataOf(v)) {
        retain(v);
        return v;
    }
    JSValue p = newPromiseObject(vm);
    resolvePromise(vm, p, v);
    return p;
}

// ------------------------------------------------------------------ the jobs ---

void runJob(VM& vm, const MicroJob& job)
{
    switch (job.kind) {
        case 0:
        case 1: {
            // a: handler, b: the derived promise (or undefined), c: the value.
            const bool isReject = job.kind == 1;
            if (job.a.isUndefined()) {
                if (!job.b.isUndefined()) {
                    if (isReject)
                        rejectPromise(vm, job.b, job.c);
                    else
                        resolvePromise(vm, job.b, job.c);
                }
                return;
            }
            JSValue r = vm.call(job.a, JSValue::undefined(), &job.c, 1);
            if (r.isException()) {
                JSValue e = takeException(vm);
                if (!job.b.isUndefined())
                    rejectPromise(vm, job.b, e);
                release(e);
                return;
            }
            if (!job.b.isUndefined())
                resolvePromise(vm, job.b, r);
            release(r);
            return;
        }
        case 2: {
            // a: the promise, b: the thenable, c: its `then`.
            Handle resolve, reject;
            makeResolvers(vm, job.a, &resolve, &reject);
            JSValue callArgs[2] = {resolve.get(), reject.get()};
            JSValue r = vm.call(job.c, job.b, callArgs, 2);
            if (r.isException()) {
                JSValue e = takeException(vm);
                JSValue rejectArgs[1] = {e};
                JSValue rr = vm.call(reject.get(), JSValue::undefined(), rejectArgs, 1);
                release(e);
                release(rr);
                return;
            }
            release(r);
            return;
        }
        default: {
            JSValue r = vm.call(job.a, JSValue::undefined(), nullptr, 0);
            if (r.isException())
                release(takeException(vm));
            else
                release(r);
        }
    }
}

// ------------------------------------------------------------- the language ---

NATIVE(promiseCtor)
{
    UNUSED();
    if (vm.newTarget.isUndefined() || !isObject(self))
        return typeError(vm, "Promise constructor cannot be invoked without 'new'");
    const JSValue executor = arg(args, argc, 0);
    if (!isFunction(executor))
        return typeError(vm, "Promise resolver " + describe(vm, executor) + " is not a function");
    JSObject* o = asObject(self);
    o->h.cls = std::uint8_t(ObjClass::Promise);
    delete o->rareOf(vm).ext;
    o->rareOf(vm).ext = new PromiseData();
    Handle resolve, reject;
    makeResolvers(vm, self, &resolve, &reject);
    JSValue callArgs[2] = {resolve.get(), reject.get()};
    JSValue r = vm.call(executor, JSValue::undefined(), callArgs, 2);
    if (r.isException()) {
        JSValue e = takeException(vm);
        JSValue rr = vm.call(reject.get(), JSValue::undefined(), &e, 1);
        release(e);
        release(rr);
    } else {
        release(r);
    }
    retain(self);
    return self;
}

NATIVE(promiseThen)
{
    UNUSED();
    if (!dataOf(self))
        return typeError(vm, "Promise.prototype.then called on incompatible receiver " + describe(vm, self));
    JSValue derived = newPromiseObject(vm);
    Handle keep = Handle::adopt(derived);
    performThen(vm, self, arg(args, argc, 0), arg(args, argc, 1), derived);
    return keep.take();
}

NATIVE(promiseCatch)
{
    UNUSED();
    Key thenKey = vm.internKey(std::string_view("then"));
    JSValue then = getProperty(vm, self, thenKey);
    detail::decRef(thenKey);
    if (then.isException())
        return then;
    Handle keep = Handle::adopt(then);
    JSValue callArgs[2] = {JSValue::undefined(), arg(args, argc, 0)};
    return vm.call(then, self, callArgs, 2);
}

NATIVE(finallyReturn)
{
    UNUSED();
    Rare& r = carried(data);
    // boundThis: 0 returns boundTarget, 1 throws it.
    if (r.fnLength == 1) {
        release(vm.exception);
        vm.exception = r.boundTarget;
        retain(vm.exception);
        vm.exceptionCode = ErrorCode::Exception;
        return JSValue::exception();
    }
    retain(r.boundTarget);
    return r.boundTarget;
}

NATIVE(finallyStep)
{
    UNUSED();
    Rare& r = carried(data);
    // boundTarget: onFinally; fnLength: 0 after a fulfilment, 1 after a rejection.
    JSValue res = vm.call(r.boundTarget, JSValue::undefined(), nullptr, 0);
    if (res.isException())
        return res;
    Handle resKeep = Handle::adopt(res);
    JSValue p = promiseResolve(vm, res);
    Handle pKeep = Handle::adopt(p);
    JSFunction* thunk = closureFn(vm, finallyReturn, arg(args, argc, 0), JSValue::undefined(), r.fnLength);
    Handle thunkKeep = Handle::adopt(toJSValue(thunk));
    JSValue derived = newPromiseObject(vm);
    Handle derivedKeep = Handle::adopt(derived);
    performThen(vm, p, thunkKeep.get(), JSValue::undefined(), derived);
    return derivedKeep.take();
}

NATIVE(promiseFinally)
{
    UNUSED();
    const JSValue onFinally = arg(args, argc, 0);
    Key thenKey = vm.internKey(std::string_view("then"));
    JSValue then = getProperty(vm, self, thenKey);
    detail::decRef(thenKey);
    if (then.isException())
        return then;
    Handle keep = Handle::adopt(then);
    if (!isFunction(onFinally)) {
        JSValue callArgs[2] = {onFinally, onFinally};
        return vm.call(then, self, callArgs, 2);
    }
    Handle a = Handle::adopt(toJSValue(closureFn(vm, finallyStep, onFinally, JSValue::undefined(), 0)));
    Handle b = Handle::adopt(toJSValue(closureFn(vm, finallyStep, onFinally, JSValue::undefined(), 1)));
    JSValue callArgs[2] = {a.get(), b.get()};
    return vm.call(then, self, callArgs, 2);
}

NATIVE(promiseResolveStatic)
{
    UNUSED();
    return promiseResolve(vm, arg(args, argc, 0));
}

NATIVE(promiseRejectStatic)
{
    UNUSED();
    JSValue p = newPromiseObject(vm);
    rejectPromise(vm, p, arg(args, argc, 0));
    return p;
}

// all, allSettled, any: one record per call, `[results, remaining, promise]`, and a function per element.
NATIVE(combinatorElement)
{
    UNUSED();
    Rare& r = carried(data);
    auto* record = static_cast<JSArray*>(asObject(r.boundTarget));
    auto* flag = static_cast<JSArray*>(asObject(r.boundThis));
    if (flag->at(0) == JSValue::boolean(true))
        return JSValue::undefined();
    flag->setAt(vm, 0, JSValue::boolean(true));
    // fnLength: bit 0 rejection side; bits 1-2 the mode (0 all, 1 allSettled, 2 any); the rest the index.
    const std::uint32_t code = r.fnLength;
    const int mode = int((code >> 1) & 3);
    const std::uint32_t index = code >> 3;
    const bool rejection = (code & 1) != 0;
    auto* results = static_cast<JSArray*>(asObject(record->at(0)));
    JSValue v = arg(args, argc, 0);
    if (mode == 1) {
        JSObject* o = vm.newObject(vm.protos.object);
        Handle keep = Handle::adopt(toJSValue(o));
        o->putOwn(vm, vm.internKey(std::string_view("status")), makeString(vm, std::string_view(rejection ? "rejected" : "fulfilled")),
                  attr::kDefault);
        retain(v);
        o->putOwn(vm, vm.internKey(std::string_view(rejection ? "reason" : "value")), v, attr::kDefault);
        retain(keep.get());
        results->setAt(vm, index, keep.get());
    } else {
        retain(v);
        results->setAt(vm, index, v);
    }
    const std::int32_t remaining = record->at(1).asInt32() - 1;
    record->setAt(vm, 1, JSValue::int32(remaining));
    if (remaining == 0) {
        const JSValue promise = record->at(2);
        if (mode == 2) {
            JSValue err = vm.makeError(ErrorCode::Exception, "All promises were rejected");
            Handle errKeep = Handle::adopt(err);
            asObject(err)->putOwn(vm, vm.internKey(std::string_view("name")), makeString(vm, std::string_view("AggregateError")),
                                  attr::kWritable | attr::kConfigurable);
            retain(toJSValue(results));
            asObject(err)->putOwn(vm, vm.internKey(std::string_view("errors")), toJSValue(results), attr::kWritable | attr::kConfigurable);
            rejectPromise(vm, promise, err);
        } else {
            settle(vm, promise, PromiseData::Fulfilled, toJSValue(results));
        }
    }
    return JSValue::undefined();
}

NATIVE(rejectDerived)
{
    UNUSED();
    rejectPromise(vm, carried(data).boundTarget, arg(args, argc, 0));
    return JSValue::undefined();
}

NATIVE(resolveDerived)
{
    UNUSED();
    resolvePromise(vm, carried(data).boundTarget, arg(args, argc, 0));
    return JSValue::undefined();
}

NATIVE(promiseCombinator)
{
    UNUSED();
    const int mode = int(reinterpret_cast<std::intptr_t>(data));  // 0 all, 1 allSettled, 2 any, 3 race
    JSValue derived = newPromiseObject(vm);
    Handle derivedKeep = Handle::adopt(derived);
    JSArray* results = vm.newArray(vm.protos.array);
    Handle resultsKeep = Handle::adopt(toJSValue(results));
    JSArray* record = vm.newArray(vm.protos.array);
    Handle recordKeep = Handle::adopt(toJSValue(record));
    retain(resultsKeep.get());
    record->append(vm, resultsKeep.get());
    record->append(vm, JSValue::int32(1));  // one for the loop itself, so it cannot finish early
    retain(derived);
    record->append(vm, derived);
    std::uint32_t index = 0;
    JSValue resolveStatic = JSValue::undefined();
    (void)resolveStatic;
    const bool ok = forEachIterated(vm, arg(args, argc, 0), [&](JSValue item) {
        JSValue p = promiseResolve(vm, item);
        Handle pKeep = Handle::adopt(p);
        if (mode == 3) {
            Handle res = Handle::adopt(toJSValue(closureFn(vm, resolveDerived, derived, JSValue::undefined(), 0)));
            Handle rej = Handle::adopt(toJSValue(closureFn(vm, rejectDerived, derived, JSValue::undefined(), 0)));
            JSValue d2 = newPromiseObject(vm);
            Handle d2Keep = Handle::adopt(d2);
            performThen(vm, p, res.get(), rej.get(), d2);
            return Step::Continue;
        }
        results->append(vm, JSValue::undefined());
        record->setAt(vm, 1, JSValue::int32(record->at(1).asInt32() + 1));
        JSArray* flag = vm.newArray(vm.protos.array);
        Handle flagKeep = Handle::adopt(toJSValue(flag));
        flag->append(vm, JSValue::boolean(false));
        const std::uint32_t base = (index << 3) | (std::uint32_t(mode) << 1);
        Handle onFul, onRej;
        if (mode == 0) {
            onFul = Handle::adopt(toJSValue(closureFn(vm, combinatorElement, recordKeep.get(), flagKeep.get(), base)));
            onRej = Handle::adopt(toJSValue(closureFn(vm, rejectDerived, derived, JSValue::undefined(), 0)));
        } else if (mode == 1) {
            onFul = Handle::adopt(toJSValue(closureFn(vm, combinatorElement, recordKeep.get(), flagKeep.get(), base)));
            onRej = Handle::adopt(toJSValue(closureFn(vm, combinatorElement, recordKeep.get(), flagKeep.get(), base | 1)));
        } else {
            onFul = Handle::adopt(toJSValue(closureFn(vm, resolveDerived, derived, JSValue::undefined(), 0)));
            onRej = Handle::adopt(toJSValue(closureFn(vm, combinatorElement, recordKeep.get(), flagKeep.get(), base | 1)));
        }
        JSValue d2 = newPromiseObject(vm);
        Handle d2Keep = Handle::adopt(d2);
        performThen(vm, p, onFul.get(), onRej.get(), d2);
        ++index;
        return Step::Continue;
    });
    if (!ok) {
        JSValue e = takeException(vm);
        rejectPromise(vm, derived, e);
        release(e);
        retain(derived);
        return derived;
    }
    if (mode != 3) {
        const std::int32_t remaining = record->at(1).asInt32() - 1;
        record->setAt(vm, 1, JSValue::int32(remaining));
        if (remaining == 0) {
            if (mode == 2) {
                JSValue err = vm.makeError(ErrorCode::Exception, "All promises were rejected");
                Handle errKeep = Handle::adopt(err);
                asObject(err)->putOwn(vm, vm.internKey(std::string_view("name")), makeString(vm, std::string_view("AggregateError")),
                                      attr::kWritable | attr::kConfigurable);
                retain(resultsKeep.get());
                asObject(err)->putOwn(vm, vm.internKey(std::string_view("errors")), resultsKeep.get(), attr::kWritable | attr::kConfigurable);
                rejectPromise(vm, derived, err);
            } else {
                settle(vm, derived, PromiseData::Fulfilled, resultsKeep.get());
            }
        }
    }
    retain(derived);
    return derived;
}

// -------------------------------------------------------------------- async ---

NATIVE(asyncContinue);

void asyncStep(VM& vm, JSValue generator, JSValue promise, int mode, JSValue sent)
{
    JSValue out;
    bool done;
    if (!vm.resumeRaw(generator, mode, sent, &out, &done)) {
        JSValue e = takeException(vm);
        rejectPromise(vm, promise, e);
        release(e);
        return;
    }
    Handle outKeep = Handle::adopt(out);
    if (done) {
        resolvePromise(vm, promise, out);
        return;
    }
    // An `await`: go on when what was awaited settles.
    JSValue awaited = promiseResolve(vm, out);
    Handle awaitedKeep = Handle::adopt(awaited);
    Handle onFul = Handle::adopt(toJSValue(closureFn(vm, asyncContinue, generator, promise, 0)));
    Handle onRej = Handle::adopt(toJSValue(closureFn(vm, asyncContinue, generator, promise, 1)));
    performThen(vm, awaited, onFul.get(), onRej.get(), JSValue::undefined());
}

NATIVE(asyncContinue)
{
    UNUSED();
    Rare& r = carried(data);
    asyncStep(vm, r.boundTarget, r.boundThis, int(r.fnLength), arg(args, argc, 0));
    return JSValue::undefined();
}


// ---------------------------------------------------------- async generators ---

struct AsyncGenRequest
{
    int mode;  // 0 next, 1 throw, 2 return
    Handle value;
    Handle promise;
};

struct AsyncGenState : NativeState
{
    Handle generator;  ///< the frame, which resumes through `resumeRaw`
    std::deque<AsyncGenRequest> queue;
    bool running = false;

    void clear(VM&) noexcept override
    {
        generator = Handle();
        queue.clear();
    }
    void values(std::vector<JSValue>& out) const override
    {
        out.push_back(generator.get());
        for (const AsyncGenRequest& r : queue) {
            out.push_back(r.value.get());
            out.push_back(r.promise.get());
        }
    }
};

AsyncGenState* asyncGenOf(JSValue v)
{
    if (isObject(v) && asObject(v)->cls() == ObjClass::Native)
        if (Rare* r = asObject(v)->rare())
            return dynamic_cast<AsyncGenState*>(r->ext);
    return nullptr;
}

void asyncGenDrain(VM& vm, JSValue outer);
NATIVE(asyncGenContinue);

void asyncGenStep(VM& vm, JSValue outer, int mode, JSValue sent)
{
    AsyncGenState* st = asyncGenOf(outer);
    JSValue out;
    bool done;
    const bool ok = vm.resumeRaw(st->generator.get(), mode, sent, &out, &done);
    const JSValue front = st->queue.front().promise.get();
    if (!ok) {
        JSValue e = takeException(vm);
        Handle ek = Handle::adopt(e);
        rejectPromise(vm, front, e);
        st->queue.pop_front();
        st->running = false;
        asyncGenDrain(vm, outer);
        return;
    }
    Handle outKeep = Handle::adopt(out);
    if (!done && vm.suspendedOnAwait) {
        // An `await`: go on when what was awaited settles.
        JSValue awaited = promiseResolve(vm, out);
        Handle awaitedKeep = Handle::adopt(awaited);
        Handle onFul = Handle::adopt(toJSValue(closureFn(vm, asyncGenContinue, outer, JSValue::undefined(), 0)));
        Handle onRej = Handle::adopt(toJSValue(closureFn(vm, asyncGenContinue, outer, JSValue::undefined(), 1)));
        performThen(vm, awaited, onFul.get(), onRej.get(), JSValue::undefined());
        return;
    }
    // A `yield` or the return: this request is answered.
    JSValue result = iterResult(vm, out, done);
    retain(out);
    Handle resultKeep = Handle::adopt(result);
    resolvePromise(vm, front, result);
    st->queue.pop_front();
    st->running = false;
    asyncGenDrain(vm, outer);
}

void asyncGenDrain(VM& vm, JSValue outer)
{
    AsyncGenState* st = asyncGenOf(outer);
    if (!st || st->running || st->queue.empty())
        return;
    st->running = true;
    AsyncGenRequest& req = st->queue.front();
    asyncGenStep(vm, outer, req.mode, req.value.get());
}

NATIVE(asyncGenContinue)
{
    UNUSED();
    Rare& r = carried(data);
    asyncGenStep(vm, r.boundTarget, int(r.fnLength), arg(args, argc, 0));
    return JSValue::undefined();
}

NATIVE(asyncGenEnqueue)
{
    UNUSED();
    const int mode = int(reinterpret_cast<std::intptr_t>(data));
    JSValue promise = newPromiseObject(vm);
    Handle promiseKeep = Handle::adopt(promise);
    AsyncGenState* st = asyncGenOf(self);
    if (!st) {
        JSValue e = vm.makeError(ErrorCode::TypeError, "next method called on an incompatible receiver");
        rejectPromise(vm, promise, e);
        release(e);
        return promiseKeep.take();
    }
    Handle keep = Handle::retained(self);
    st->queue.push_back(AsyncGenRequest{mode, Handle::retained(arg(args, argc, 0)), promiseKeep});
    asyncGenDrain(vm, self);
    return promiseKeep.take();
}

// ------------------------------------------------- the sync iterator in async clothes ---

struct SyncWrap : NativeState
{
    Handle iterator, next;
    void clear(VM&) noexcept override
    {
        iterator = Handle();
        next = Handle();
    }
    void values(std::vector<JSValue>& out) const override
    {
        out.push_back(iterator.get());
        out.push_back(next.get());
    }
};

NATIVE(wrapUnwrap)
{
    UNUSED();
    Rare& r = carried(data);
    retain(arg(args, argc, 0));
    return iterResult(vm, arg(args, argc, 0), r.fnLength != 0);
}

NATIVE(syncWrapStep)
{
    UNUSED();
    SyncWrap* w = nullptr;
    if (isObject(self) && asObject(self)->cls() == ObjClass::Native)
        if (Rare* r = asObject(self)->rare())
            w = dynamic_cast<SyncWrap*>(r->ext);
    JSValue derived = newPromiseObject(vm);
    Handle derivedKeep = Handle::adopt(derived);
    if (!w) {
        JSValue e = vm.makeError(ErrorCode::TypeError, "next method called on an incompatible receiver");
        rejectPromise(vm, derived, e);
        release(e);
        return derivedKeep.take();
    }
    const int mode = int(reinterpret_cast<std::intptr_t>(data));  // 0 next, 1 throw, 2 return
    JSValue r;
    if (mode == 0) {
        r = vm.call(w->next.get(), w->iterator.get(), args, argc > 0 ? 1 : 0);
    } else {
        Key k = vm.internKey(std::string_view(mode == 1 ? "throw" : "return"));
        JSValue f = getProperty(vm, w->iterator.get(), k);
        detail::decRef(k);
        if (f.isException()) {
            r = f;
        } else if (f.isNullish()) {
            release(f);
            if (mode == 1) {
                release(vm.exception);
                vm.exception = arg(args, argc, 0);
                retain(vm.exception);
                vm.exceptionCode = ErrorCode::Exception;
                r = JSValue::exception();
            } else {
                retain(arg(args, argc, 0));
                r = iterResult(vm, arg(args, argc, 0), true);
            }
        } else {
            Handle fk = Handle::adopt(f);
            r = vm.call(f, w->iterator.get(), args, argc > 0 ? 1 : 0);
        }
    }
    if (r.isException()) {
        JSValue e = takeException(vm);
        rejectPromise(vm, derived, e);
        release(e);
        return derivedKeep.take();
    }
    Handle rKeep = Handle::adopt(r);
    if (!isObject(r)) {
        JSValue e = vm.makeError(ErrorCode::TypeError, "Iterator result " + describe(vm, r) + " is not an object");
        rejectPromise(vm, derived, e);
        release(e);
        return derivedKeep.take();
    }
    JSValue d = getProperty(vm, r, vm.atoms.done);
    if (d.isException()) {
        JSValue e = takeException(vm);
        rejectPromise(vm, derived, e);
        release(e);
        return derivedKeep.take();
    }
    const bool done = toBoolean(d);
    release(d);
    JSValue v = getProperty(vm, r, vm.atoms.value);
    if (v.isException()) {
        JSValue e = takeException(vm);
        rejectPromise(vm, derived, e);
        release(e);
        return derivedKeep.take();
    }
    Handle vKeep = Handle::adopt(v);
    JSValue wrapped = promiseResolve(vm, v);
    Handle wrappedKeep = Handle::adopt(wrapped);
    Handle unwrap = Handle::adopt(toJSValue(closureFn(vm, wrapUnwrap, JSValue::undefined(), JSValue::undefined(), done ? 1 : 0)));
    performThen(vm, wrapped, unwrap.get(), JSValue::undefined(), derived);
    return derivedKeep.take();
}

}  // namespace

JSValue asyncStart(VM& vm, JSValue generator)
{
    Handle gen = Handle::adopt(generator);
    JSValue promise = newPromiseObject(vm);
    Handle promiseKeep = Handle::adopt(promise);
    asyncStep(vm, gen.get(), promise, 0, JSValue::undefined());
    return promiseKeep.take();
}

JSValue asyncGeneratorStart(VM& vm, JSValue generator, JSObject* proto)
{
    Handle gen = Handle::adopt(generator);
    JSObject* outer = vm.newObject(proto ? proto : vm.protos.asyncGenerator);
    Handle outerKeep = Handle::adopt(toJSValue(outer));
    outer->h.cls = std::uint8_t(ObjClass::Native);
    auto* st = new AsyncGenState();
    st->generator = gen;
    outer->rareOf(vm).ext = st;
    return outerKeep.take();
}

JSValue asyncFromSyncIterator(VM& vm, JSValue iterator, JSValue next)
{
    static_assert(true, "");
    JSObject* o = vm.newObject(vm.protos.object);
    Handle keep = Handle::adopt(toJSValue(o));
    o->h.cls = std::uint8_t(ObjClass::Native);
    auto* w = new SyncWrap();
    w->iterator = Handle::retained(iterator);
    w->next = Handle::retained(next);
    o->rareOf(vm).ext = w;
    method(vm, o, "next", syncWrapStep, 1, reinterpret_cast<void*>(0));
    method(vm, o, "throw", syncWrapStep, 1, reinterpret_cast<void*>(1));
    method(vm, o, "return", syncWrapStep, 1, reinterpret_cast<void*>(2));
    return keep.take();
}

void installPromise(VM& vm)
{
    {
        JSObject* agProto = vm.newObject(vm.protos.object);
        vm.setIntrinsic(&vm.protos.asyncGenerator, agProto);
        release(toJSValue(agProto));
        method(vm, agProto, "next", asyncGenEnqueue, 1, reinterpret_cast<void*>(0));
        method(vm, agProto, "throw", asyncGenEnqueue, 1, reinterpret_cast<void*>(1));
        method(vm, agProto, "return", asyncGenEnqueue, 1, reinterpret_cast<void*>(2));
        symbolMethod(vm, agProto, vm.atoms.symAsyncIterator, "[Symbol.asyncIterator]",
                     [](VM&, JSValue self, const JSValue*, std::uint32_t, void*) -> JSValue {
                         retain(self);
                         return self;
                     }, 0);
        agProto->putOwn(vm, vm.atoms.symToStringTag, makeString(vm, std::string_view("AsyncGenerator")), attr::kConfigurable);
    }
    JSObject* proto = vm.newObject(vm.protos.object);
    vm.setIntrinsic(&vm.protos.promise, proto);
    release(toJSValue(proto));
    JSFunction* ctor = constructor(vm, "Promise", promiseCtor, 1, proto);
    method(vm, proto, "then", promiseThen, 2);
    method(vm, proto, "catch", promiseCatch, 1);
    method(vm, proto, "finally", promiseFinally, 1);
    proto->putOwn(vm, vm.atoms.symToStringTag, makeString(vm, std::string_view("Promise")), attr::kConfigurable);
    method(vm, ctor, "resolve", promiseResolveStatic, 1);
    method(vm, ctor, "reject", promiseRejectStatic, 1);
    method(vm, ctor, "all", promiseCombinator, 1, reinterpret_cast<void*>(0));
    method(vm, ctor, "allSettled", promiseCombinator, 1, reinterpret_cast<void*>(1));
    method(vm, ctor, "any", promiseCombinator, 1, reinterpret_cast<void*>(2));
    method(vm, ctor, "race", promiseCombinator, 1, reinterpret_cast<void*>(3));
    method(vm, vm.global, "queueMicrotask", [](VM& vm2, JSValue, const JSValue* args, std::uint32_t argc, void*) -> JSValue {
        if (!isFunction(arg(args, argc, 0)))
            return typeError(vm2, "The callback must be a function");
        vm2.enqueueJob(3, args[0], JSValue::undefined(), JSValue::undefined());
        return JSValue::undefined();
    }, 1);
}

}  // namespace bi

std::size_t VM::runJobs()
{
    std::size_t ran = 0;
    while (!jobs.empty()) {
        MicroJob job = jobs.front();
        jobs.pop_front();
        bi::runJob(*this, job);
        release(job.a);
        release(job.b);
        release(job.c);
        ++ran;
    }
    return ran;
}

}  // namespace vm
}  // namespace internal
}  // namespace script
