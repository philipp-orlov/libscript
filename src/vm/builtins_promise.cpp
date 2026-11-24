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
    Handle onFulfilled, onRejected;  ///< functions, or undefined to pass the outcome on
    /// The job that runs it: 0 a `then`; for an `await`, 4 (`onFulfilled` is the generator of an async function that waits,
    /// `derived` the promise of its function) or 6 (`onFulfilled` is the async generator object that waits).
    std::uint8_t job = 0;
    bool set = false;
};

struct PromiseData : NativeState
{
    enum State : std::uint8_t { Pending, Fulfilled, Rejected };
    State state = Pending;
    bool handled = false;
    JSValue result = JSValue::undefined();
    Reaction first;  ///< most promises have one; the ones after it are in `more`
    std::vector<Reaction> more;

    void add(Reaction r)
    {
        r.set = true;
        if (first.set)
            more.push_back(std::move(r));
        else
            first = std::move(r);
    }
    ~PromiseData() override { clear_(); }
    void clear(VM&) noexcept override { clear_(); }
    void clear_() noexcept
    {
        release(result);
        result = JSValue::undefined();
        first = Reaction();
        more.clear();
    }
    void values(Vec<JSValue>& out) const override
    {
        out.push_back(result);
        auto push = [&](const Reaction& r) {
            out.push_back(r.derived.get());
            out.push_back(r.onFulfilled.get());
            out.push_back(r.onRejected.get());
        };
        push(first);
        for (const Reaction& r : more)
            push(r);
    }
};

static_assert(sizeof(PromiseData) <= JSObject::kPromiseStateBytes && alignof(PromiseData) <= 8,
              "a promise's state fits in its cell");

/// A promise's state, in its own cell after its slots in place: no allocation of its own, and no load to find it.
PromiseData* dataOf(JSValue v)
{
    if (isObject(v) && asObject(v)->cls() == ObjClass::Promise)
        return static_cast<PromiseData*>(asObject(v)->embeddedState());
    return nullptr;
}

JSValue newPromiseObject(VM& vm, JSObject* proto = nullptr)
{
    constexpr std::uint8_t aux = 3;
    auto* o = static_cast<JSObject*>(
        vm.allocCell(JSObject::bytesFor(aux) + JSObject::kPromiseStateBytes, Kind::Object, ObjClass::Promise, aux));
    o->h.shape = vm.rootShapeFor(proto ? proto : vm.protos.promise, aux)->id;
    o->slots = nullptr;
    ::new (static_cast<void*>(o->embeddedState())) PromiseData();
    return toJSValue(o);
}

/// An `AggregateError` for the reasons `results` holds, owned.
JSValue makeAggregateError(VM& vm, JSValue results)
{
    Key name = vm.internKey(std::string_view("AggregateError"));
    JSValue* slot = vm.global->findOwn(vm, name);
    detail::decRef(name);
    if (slot && isFunction(*slot)) {
        JSValue args[2] = {results, makeString(vm, std::string_view("All promises were rejected"))};
        JSValue r = vm.construct(*slot, args, 2);
        release(args[1]);
        if (!r.isException())
            return r;
        vm.clearException();
    }
    JSValue err = vm.makeError(ErrorCode::Exception, "All promises were rejected");
    retain(results);
    Key errorsKey = vm.internKey(std::string_view("errors"));
    asObject(err)->putOwn(vm, errorsKey, results, attr::kWritable | attr::kConfigurable);
    detail::decRef(errorsKey);
    return err;
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
    if (!d->first.set)
        return;
    const Reaction first = std::move(d->first);
    std::vector<Reaction> more;
    more.swap(d->more);
    d->first = Reaction();
    auto run = [&](const Reaction& r) {
        const bool fulfilled = state == PromiseData::Fulfilled;
        vm.enqueueJob(r.job + (fulfilled ? 0 : 1), (r.job || fulfilled ? r.onFulfilled : r.onRejected).get(),
                      r.derived.get(), value);
    };
    run(first);
    for (const Reaction& r : more)
        run(r);
}

/// Whether `p` is a promise with nothing on it, its prototype or its constructor that a `then` could notice: `constructor`,
/// `then` and the species are as made.
bool promisePristine(VM& vm, JSValue p)
{
    JSObject* proto = vm.protos.promise;
    if (!dataOf(p) || !proto || asObject(p)->h.shape != vm.rootShapeFor(proto, 3)->id)
        return false;
    if (vm.promiseEpoch == vm.globalEpoch)
        return true;
    std::uint8_t attrs = 0;
    const JSValue* slot = proto->findOwn(vm, vm.atoms.constructor, &attrs);
    if (!slot || (attrs & attr::kAccessor) || slot->bits != vm.promiseCtor.bits)
        return false;
    slot = proto->findOwn(vm, vm.atoms.then, &attrs);
    if (!slot || (attrs & attr::kAccessor) || slot->bits != vm.promiseThen.bits)
        return false;
    JSObject* ctor = asObject(vm.promiseCtor);
    slot = ctor->findOwn(vm, vm.atoms.symSpecies, &attrs);
    if (!slot || slot->bits != vm.promiseSpecies.bits)
        return false;
    if (vm.watchMethod(proto, proto) && vm.watchMethod(ctor, ctor))
        vm.promiseEpoch = vm.globalEpoch;
    return true;
}

void rejectPromise(VM& vm, JSValue promise, JSValue reason) { settle(vm, promise, PromiseData::Rejected, reason); }

/// Whether `o` is a plain object over `Object.prototype` with no `then` of its own or there (an iterator result, a record
/// an async generator yields): what resolves to it is fulfilled with no lookup. The shapes found so are kept while
/// `globalEpoch` stands.
bool noThen(VM& vm, JSObject* o)
{
    if (o->cls() != ObjClass::Plain)
        return false;
    const std::uint32_t shape = o->h.shape;
    if (vm.noThenEpoch == vm.globalEpoch)
        for (std::uint32_t s : vm.noThenShapes)
            if (s == shape)
                return true;
    Shape* sh = o->shapeOf(vm);
    JSObject* op = vm.protos.object;
    if (sh->isDictionary() || sh->proto != op || o->findOwn(vm, vm.atoms.then) || op->findOwn(vm, vm.atoms.then))
        return false;
    if (vm.noThenEpoch != vm.globalEpoch) {
        if (!vm.watchMethod(op, op))
            return true;
        vm.noThenEpoch = vm.globalEpoch;
        for (std::uint32_t& s : vm.noThenShapes)
            s = 0;
    }
    vm.noThenShapes[vm.noThenNext++ % 4] = shape;
    return true;
}

/// The resolve function's steps: a thenable is followed, anything else fulfils.
void resolvePromise(VM& vm, JSValue promise, JSValue resolution)
{
    if (resolution == promise) {
        JSValue e = vm.makeError(ErrorCode::TypeError, "Chaining cycle detected for promise #<Promise>");
        rejectPromise(vm, promise, e);
        release(e);
        return;
    }
    if (!isObject(resolution) || noThen(vm, asObject(resolution))) {
        settle(vm, promise, PromiseData::Fulfilled, resolution);
        return;
    }
    if (promisePristine(vm, resolution)) {
        vm.enqueueJob(2, promise, resolution, vm.promiseThen);
        return;
    }
    JSValue then = getProperty(vm, resolution, vm.atoms.then);
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
JSFunction* closureFn(VM& vm, FastNative fn, JSValue a, JSValue b, std::uint32_t n, std::uint32_t length = 1)
{
    JSFunction* f = vm.newNativeFunction(vm.protos.function, fn, nullptr, nullptr, length);
    Rare& r = f->rareOf(vm);
    r.nativeData = f;
    retain(a);
    retain(b);
    r.boundTarget = a;
    r.boundThis = b;
    r.fnState = n;
    return f;
}

Rare& carried(void* data) { return *static_cast<JSFunction*>(data)->rare(); }

NATIVE(resolverFn)
{
    UNUSED();
    Rare& r = carried(data);
    // boundThis is a box that says whether one of the pair has run.
    Box* flag = reinterpret_cast<Box*>(r.boundThis.asCell());
    if (flag->value == JSValue::boolean(true))
        return JSValue::undefined();
    flag->value = JSValue::boolean(true);
    if (r.fnState == 0)
        resolvePromise(vm, r.boundTarget, arg(args, argc, 0));
    else
        rejectPromise(vm, r.boundTarget, arg(args, argc, 0));
    return JSValue::undefined();
}

void makeResolvers(VM& vm, JSValue promise, Handle* resolve, Handle* reject)
{
    Handle flagKeep = Handle::adopt(JSValue::cell(reinterpret_cast<CellPtr>(vm.newBox(JSValue::boolean(false)))));
    *resolve = Handle::adopt(toJSValue(closureFn(vm, resolverFn, promise, flagKeep.get(), 0)));
    *reject = Handle::adopt(toJSValue(closureFn(vm, resolverFn, promise, flagKeep.get(), 1)));
}

void performThen(VM& vm, JSValue promise, JSValue onFulfilled, JSValue onRejected, JSValue derived)
{
    PromiseData* d = dataOf(promise);
    const JSValue f = isFunction(onFulfilled) ? onFulfilled : JSValue::undefined();
    const JSValue r = isFunction(onRejected) ? onRejected : JSValue::undefined();
    d->handled = true;
    if (d->state == PromiseData::Pending)
        d->add(Reaction{Handle::retained(derived), Handle::retained(f), Handle::retained(r)});
    else if (d->state == PromiseData::Fulfilled)
        vm.enqueueJob(0, f, derived, d->result);
    else
        vm.enqueueJob(1, r, derived, d->result);
}

/// An `await` of `promise`: the generator goes on when it settles, with no function made for it. `job` is 4 for an
/// async function, 6 for an async generator (see `Reaction::job`).
void performAwait(VM& vm, JSValue promise, JSValue generator, JSValue result, std::uint8_t job = 4)
{
    PromiseData* d = dataOf(promise);
    d->handled = true;
    if (d->state == PromiseData::Pending) {
        d->add(Reaction{Handle::retained(result), Handle::retained(generator), Handle(), job});
    } else {
        vm.enqueueJob(job + (d->state == PromiseData::Fulfilled ? 0 : 1), generator, result, d->result);
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

void asyncResume(VM& vm, JSValue generator, JSValue promise, int mode, JSValue sent);
void asyncGenStep(VM& vm, JSValue outer, int mode, JSValue sent);

void runJob(VM& vm, const MicroJob& job)
{
    switch (job.kind) {
        case 0:
        case 1: {
            // a: handler, b: the derived promise or `[promise, resolve, reject]` (or undefined), c: the value.
            const bool isReject = job.kind == 1;
            auto settleDerived = [&](bool reject, JSValue v) {
                if (job.b.isUndefined())
                    return;
                if (isArray(job.b)) {
                    JSValue f = static_cast<JSArray*>(asObject(job.b))->at(reject ? 2 : 1);
                    JSValue r = vm.call(f, JSValue::undefined(), &v, 1);
                    if (r.isException())
                        release(takeException(vm));
                    else
                        release(r);
                    return;
                }
                if (reject)
                    rejectPromise(vm, job.b, v);
                else
                    resolvePromise(vm, job.b, v);
            };
            if (job.a.isUndefined()) {
                settleDerived(isReject, job.c);
                return;
            }
            JSValue r = vm.call(job.a, JSValue::undefined(), &job.c, 1);
            if (r.isException()) {
                JSValue e = takeException(vm);
                settleDerived(true, e);
                release(e);
                return;
            }
            settleDerived(false, r);
            release(r);
            return;
        }
        case 4:
        case 5:
            // a: the generator of an async function, b: its promise, c: what the awaited promise settled with.
            asyncResume(vm, job.a, job.b, job.kind - 4, job.c);
            return;
        case 6:
        case 7:
            // a: an async generator object, c: what the promise it awaited settled with.
            asyncGenStep(vm, job.a, job.kind - 6, job.c);
            return;
        case 2: {
            // a: the promise, b: the thenable, c: its `then`.
            if (job.c.bits == vm.promiseThen.bits && promisePristine(vm, job.b)) {
                // Its `then` with the two functions that settle `a`: the same as a reaction with no handlers.
                performThen(vm, job.b, JSValue::undefined(), JSValue::undefined(), job.a);
                return;
            }
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

// A promise capability: a promise and the functions that settle it.
struct Capability
{
    Handle promise, resolve, reject;
};

/// The executor `NewPromiseCapability` gives a constructor that is not Promise itself: it keeps the two functions it is
/// given in the array it carries (`[resolve, reject]`).
NATIVE(capabilityExecutor)
{
    UNUSED();
    Rare& r = carried(data);
    auto* holder = static_cast<JSArray*>(asObject(r.boundTarget));
    if (!holder->at(0).isUndefined() || !holder->at(1).isUndefined())
        return typeError(vm, "Promise executor has already been invoked with non-undefined arguments");
    retain(arg(args, argc, 0));
    holder->setAt(vm, 0, arg(args, argc, 0));
    retain(arg(args, argc, 1));
    holder->setAt(vm, 1, arg(args, argc, 1));
    return JSValue::undefined();
}

/// NewPromiseCapability(C); false with an exception pending.
bool newCapability(VM& vm, JSValue c, Capability* out)
{
    if (!isConstructor(c)) {
        vm.throwError(ErrorCode::TypeError, describe(vm, c) + " is not a constructor");
        return false;
    }
    if (c.bits == vm.promiseCtor.bits) {
        out->promise = Handle::adopt(newPromiseObject(vm));
        makeResolvers(vm, out->promise.get(), &out->resolve, &out->reject);
        return true;
    }
    JSArray* holder = vm.newArray(vm.protos.array);
    Handle holderKeep = Handle::adopt(toJSValue(holder));
    holder->append(vm, JSValue::undefined());
    holder->append(vm, JSValue::undefined());
    Handle executor = Handle::adopt(toJSValue(closureFn(vm, capabilityExecutor, holderKeep.get(), JSValue::undefined(), 0, 2)));
    const JSValue executorArg = executor.get();
    JSValue made = vm.construct(c, &executorArg, 1);
    if (made.isException())
        return false;
    out->promise = Handle::adopt(made);
    if (!isFunction(holder->at(0)) || !isFunction(holder->at(1))) {
        vm.throwError(ErrorCode::TypeError, "Promise resolve or reject function is not callable");
        return false;
    }
    out->resolve = Handle::retained(holder->at(0));
    out->reject = Handle::retained(holder->at(1));
    return true;
}

/// The array a reaction carries for a capability that is not one of ours: `[promise, resolve, reject]`.
JSValue capabilityRecord(VM& vm, const Capability& c)
{
    JSArray* record = vm.newArray(vm.protos.array, 0, 3);
    Handle keep = Handle::adopt(toJSValue(record));
    record->append(vm, (retain(c.promise.get()), c.promise.get()));
    record->append(vm, (retain(c.resolve.get()), c.resolve.get()));
    record->append(vm, (retain(c.reject.get()), c.reject.get()));
    return keep.take();
}

NATIVE(promiseThen)
{
    UNUSED();
    if (!dataOf(self))
        return typeError(vm, "Promise.prototype.then called on incompatible receiver " + describe(vm, self));
    Handle ctor;
    if (promisePristine(vm, self))
        ctor = Handle::retained(vm.promiseCtor);
    else
        ctor = Handle::adopt(speciesConstructor(vm, self, vm.promiseCtor));
    if (ctor.get().isException())
        return ctor.get();
    if (ctor.get().bits == vm.promiseCtor.bits) {
        JSValue derived = newPromiseObject(vm);
        Handle keep = Handle::adopt(derived);
        performThen(vm, self, arg(args, argc, 0), arg(args, argc, 1), derived);
        return keep.take();
    }
    Capability cap;
    if (!newCapability(vm, ctor.get(), &cap))
        return JSValue::exception();
    Handle record = Handle::adopt(capabilityRecord(vm, cap));
    performThen(vm, self, arg(args, argc, 0), arg(args, argc, 1), record.get());
    return cap.promise.take();
}

/// `target.then(a, b)`: Invoke.
JSValue invokeThen(VM& vm, JSValue target, JSValue onFulfilled, JSValue onRejected, std::uint32_t argc)
{
    JSValue then = getProperty(vm, target, vm.atoms.then);
    if (then.isException())
        return then;
    Handle keep = Handle::adopt(then);
    if (!isFunction(then))
        return vm.throwError(ErrorCode::TypeError, "Promise.prototype.then is not a function");
    JSValue callArgs[2] = {onFulfilled, onRejected};
    return vm.call(then, target, callArgs, argc);
}

NATIVE(promiseCatch)
{
    UNUSED();
    return invokeThen(vm, self, JSValue::undefined(), arg(args, argc, 0), 2);
}

/// PromiseResolve(C, x), owned; the exception sentinel on a failure.
JSValue promiseResolveWith(VM& vm, JSValue c, JSValue x)
{
    if (dataOf(x)) {
        JSValue xc = getProperty(vm, x, vm.atoms.constructor);
        if (xc.isException())
            return xc;
        const bool same = xc.bits == c.bits;
        release(xc);
        if (same) {
            retain(x);
            return x;
        }
    }
    if (c.bits == vm.promiseCtor.bits) {
        // (Nobody sees the functions that would settle it.)
        Handle promise = Handle::adopt(newPromiseObject(vm));
        resolvePromise(vm, promise.get(), x);
        return promise.take();
    }
    Capability cap;
    if (!newCapability(vm, c, &cap))
        return JSValue::exception();
    JSValue r = vm.call(cap.resolve.get(), JSValue::undefined(), &x, 1);
    if (r.isException())
        return r;
    release(r);
    return cap.promise.take();
}

NATIVE(finallyReturn)
{
    UNUSED();
    Rare& r = carried(data);
    // fnState: 0 returns boundTarget, 1 throws it.
    if (r.fnState == 1) {
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
    // boundTarget: onFinally; boundThis: the constructor; fnState: 0 after a fulfilment, 1 after a rejection.
    JSValue res = vm.call(r.boundTarget, JSValue::undefined(), nullptr, 0);
    if (res.isException())
        return res;
    Handle resKeep = Handle::adopt(res);
    Handle p = Handle::adopt(promiseResolveWith(vm, r.boundThis, res));
    if (p.get().isException())
        return p.get();
    Handle thunk = Handle::adopt(toJSValue(closureFn(vm, finallyReturn, arg(args, argc, 0), JSValue::undefined(), r.fnState, 0)));
    return invokeThen(vm, p.get(), thunk.get(), JSValue::undefined(), 1);
}

NATIVE(promiseFinally)
{
    UNUSED();
    if (!isObject(self))
        return typeError(vm, "Promise.prototype.finally called on a non-object");
    Handle ctor = Handle::adopt(speciesConstructor(vm, self, vm.promiseCtor));
    if (ctor.get().isException())
        return ctor.get();
    const JSValue onFinally = arg(args, argc, 0);
    if (!isFunction(onFinally))
        return invokeThen(vm, self, onFinally, onFinally, 2);
    Handle a = Handle::adopt(toJSValue(closureFn(vm, finallyStep, onFinally, ctor.get(), 0)));
    Handle b = Handle::adopt(toJSValue(closureFn(vm, finallyStep, onFinally, ctor.get(), 1)));
    return invokeThen(vm, self, a.get(), b.get(), 2);
}

NATIVE(promiseResolveStatic)
{
    UNUSED();
    if (!isObject(self))
        return typeError(vm, "PromiseResolve called on non-object");
    return promiseResolveWith(vm, self, arg(args, argc, 0));
}

NATIVE(promiseRejectStatic)
{
    UNUSED();
    if (self.bits == vm.promiseCtor.bits) {
        Handle promise = Handle::adopt(newPromiseObject(vm));
        rejectPromise(vm, promise.get(), arg(args, argc, 0));
        return promise.take();
    }
    Capability cap;
    if (!newCapability(vm, self, &cap))
        return JSValue::exception();
    JSValue reason = arg(args, argc, 0);
    JSValue r = vm.call(cap.reject.get(), JSValue::undefined(), &reason, 1);
    if (r.isException())
        return r;
    release(r);
    return cap.promise.take();
}

NATIVE(promiseWithResolvers)
{
    UNUSED();
    Capability cap;
    if (!newCapability(vm, self, &cap))
        return JSValue::exception();
    JSObject* out = vm.newObject(vm.protos.object);
    Handle keep = Handle::adopt(toJSValue(out));
    value(vm, out, "promise", (retain(cap.promise.get()), cap.promise.get()), attr::kDefault);
    value(vm, out, "resolve", (retain(cap.resolve.get()), cap.resolve.get()), attr::kDefault);
    value(vm, out, "reject", (retain(cap.reject.get()), cap.reject.get()), attr::kDefault);
    return keep.take();
}

NATIVE(promiseTry)
{
    UNUSED();
    if (!isObject(self))
        return typeError(vm, "Promise.try called on a non-object");
    Capability cap;
    if (!newCapability(vm, self, &cap))
        return JSValue::exception();
    JSValue r = isFunction(arg(args, argc, 0)) ? vm.call(args[0], JSValue::undefined(), args + 1, argc - 1)
                                                : vm.throwError(ErrorCode::TypeError, describe(vm, arg(args, argc, 0)) + " is not a function");
    JSValue settled;
    if (r.isException()) {
        Handle reason = Handle::adopt(takeException(vm));
        const JSValue reasonValue = reason.get();
        settled = vm.call(cap.reject.get(), JSValue::undefined(), &reasonValue, 1);
    } else {
        Handle resultKeep = Handle::adopt(r);
        settled = vm.call(cap.resolve.get(), JSValue::undefined(), &r, 1);
    }
    if (settled.isException())
        return settled;
    release(settled);
    return cap.promise.take();
}

NATIVE(promiseCtor)
{
    UNUSED();
    if (vm.newTarget.isUndefined() || !isObject(self))
        return typeError(vm, "Promise constructor cannot be invoked without 'new'");
    const JSValue executor = arg(args, argc, 0);
    if (!isFunction(executor))
        return typeError(vm, "Promise resolver " + describe(vm, executor) + " is not a function");
    // A promise of its own (its state in its cell), inheriting what `this` was made to (a subclass's prototype).
    Handle made = Handle::adopt(newPromiseObject(vm, asObject(self)->proto(vm)));
    const JSValue promise = made.get();
    Handle resolve, reject;
    makeResolvers(vm, promise, &resolve, &reject);
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
    return made.take();
}

// ----------------------------------------------------------------- the combinators ---

/// IteratorClose after a throw: the exception pending stays, and nothing closing raises matters.
void closeAfterThrow(VM& vm, JSValue iterator)
{
    Handle pending = Handle::adopt(takeException(vm));
    JSValue method = getProperty(vm, iterator, vm.atoms.return_);
    if (!method.isException()) {
        Handle keep = Handle::adopt(method);
        if (isFunction(method)) {
            JSValue r = vm.call(method, iterator, nullptr, 0);
            if (!r.isException())
                release(r);
        }
    }
    vm.clearException();
    release(vm.exception);
    vm.exception = pending.take();
    vm.exceptionCode = ErrorCode::Exception;
}

/// What a combinator shares among the functions it hands out: `[values, remaining, capability record]`, a function per
/// element, and one flag per element (or per pair, for allSettled).
NATIVE(combinatorElement)
{
    UNUSED();
    Rare& r = carried(data);
    auto* record = static_cast<JSArray*>(asObject(r.boundTarget));
    auto* flag = static_cast<JSArray*>(asObject(r.boundThis));
    if (flag->at(0) == JSValue::boolean(true))
        return JSValue::undefined();
    flag->setAt(vm, 0, JSValue::boolean(true));
    // fnState: bit 0 the rejection side; bits 1-2 the mode (0 all, 1 allSettled, 2 any); the rest the index.
    const std::uint32_t bits = r.fnState;
    const bool rejection = (bits & 1) != 0;
    const int mode = int((bits >> 1) & 3);
    const std::uint32_t index = bits >> 3;
    auto* values = static_cast<JSArray*>(asObject(record->at(0)));
    JSValue x = arg(args, argc, 0);
    if (mode == 1) {
        JSObject* o = vm.newObject(vm.protos.object);
        Handle keep = Handle::adopt(toJSValue(o));
        value(vm, o, "status", makeString(vm, std::string_view(rejection ? "rejected" : "fulfilled")), attr::kDefault);
        retain(x);
        value(vm, o, rejection ? "reason" : "value", x, attr::kDefault);
        retain(keep.get());
        values->setAt(vm, index, keep.get());
    } else {
        retain(x);
        values->setAt(vm, index, x);
    }
    const double remaining = record->at(1).asNumber() - 1;
    record->setAt(vm, 1, JSValue::boxNumber(remaining));
    if (remaining != 0)
        return JSValue::undefined();
    auto* cap = static_cast<JSArray*>(asObject(record->at(2)));
    if (mode == 2) {
        Handle error = Handle::adopt(makeAggregateError(vm, toJSValue(values)));
        const JSValue e = error.get();
        return vm.call(cap->at(2), JSValue::undefined(), &e, 1);
    }
    const JSValue all = toJSValue(values);
    return vm.call(cap->at(1), JSValue::undefined(), &all, 1);
}

/// Promise.all, allSettled, any and race (data 0 to 3).
NATIVE(promiseCombinator)
{
    UNUSED();
    const int mode = int(reinterpret_cast<std::intptr_t>(data));
    if (!isObject(self))
        return typeError(vm, "Promise combinator called on a non-object");
    Capability cap;
    if (!newCapability(vm, self, &cap))
        return JSValue::exception();
    // Whatever goes wrong from here is the promise's rejection.
    auto rejectWith = [&]() -> JSValue {
        Handle reason = Handle::adopt(takeException(vm));
        const JSValue reasonValue = reason.get();
        JSValue r = vm.call(cap.reject.get(), JSValue::undefined(), &reasonValue, 1);
        if (r.isException())
            return r;
        release(r);
        return cap.promise.take();
    };
    Key resolveKey = vm.internKey(std::string_view("resolve"));
    Handle promiseResolve = Handle::adopt(getProperty(vm, self, resolveKey));
    detail::decRef(resolveKey);
    if (promiseResolve.get().isException())
        return rejectWith();
    if (!isFunction(promiseResolve.get())) {
        vm.throwError(ErrorCode::TypeError, "Promise resolve is not a function");
        return rejectWith();
    }
    Handle iterator, next;
    if (!getIterator(vm, arg(args, argc, 0), &iterator, &next))
        return rejectWith();

    JSArray* values = vm.newArray(vm.protos.array);
    Handle valuesKeep = Handle::adopt(toJSValue(values));
    JSArray* record = vm.newArray(vm.protos.array);
    Handle recordKeep = Handle::adopt(toJSValue(record));
    Handle capRecord = Handle::adopt(capabilityRecord(vm, cap));
    retain(valuesKeep.get());
    record->append(vm, valuesKeep.get());
    record->append(vm, JSValue::int32(1));
    retain(capRecord.get());
    record->append(vm, capRecord.get());

    std::uint32_t index = 0;
    for (;;) {
        JSValue v;
        const int step = iteratorStep(vm, iterator.get(), next.get(), &v);
        if (step < 0)
            return rejectWith();  // the iterator itself failed: it is not closed
        if (step == 0) {
            if (mode == 3)
                return cap.promise.take();
            const double remaining = record->at(1).asNumber() - 1;
            record->setAt(vm, 1, JSValue::boxNumber(remaining));
            if (remaining == 0) {
                JSValue settled;
                if (mode == 2) {
                    Handle err = Handle::adopt(makeAggregateError(vm, valuesKeep.get()));
                    const JSValue e = err.get();
                    settled = vm.call(cap.reject.get(), JSValue::undefined(), &e, 1);
                } else {
                    const JSValue all = valuesKeep.get();
                    settled = vm.call(cap.resolve.get(), JSValue::undefined(), &all, 1);
                }
                if (settled.isException())
                    return rejectWith();
                release(settled);
            }
            return cap.promise.take();
        }
        Handle element = Handle::adopt(v);
        JSValue nextPromise = vm.call(promiseResolve.get(), self, &v, 1);
        if (nextPromise.isException()) {
            closeAfterThrow(vm, iterator.get());
            return rejectWith();
        }
        Handle nextKeep = Handle::adopt(nextPromise);
        JSValue result;
        if (mode == 3) {
            result = invokeThen(vm, nextPromise, cap.resolve.get(), cap.reject.get(), 2);
        } else {
            values->append(vm, JSValue::undefined());
            record->setAt(vm, 1, JSValue::boxNumber(record->at(1).asNumber() + 1));
            JSArray* flag = vm.newArray(vm.protos.array);
            Handle flagKeep = Handle::adopt(toJSValue(flag));
            flag->append(vm, JSValue::boolean(false));
            const std::uint32_t base = (index << 3) | (std::uint32_t(mode) << 1);
            Handle onFulfilled, onRejected;
            if (mode == 2) {
                onFulfilled = Handle::retained(cap.resolve.get());
                onRejected = Handle::adopt(toJSValue(closureFn(vm, combinatorElement, recordKeep.get(), flagKeep.get(), base | 1)));
            } else if (mode == 1) {
                onFulfilled = Handle::adopt(toJSValue(closureFn(vm, combinatorElement, recordKeep.get(), flagKeep.get(), base)));
                onRejected = Handle::adopt(toJSValue(closureFn(vm, combinatorElement, recordKeep.get(), flagKeep.get(), base | 1)));
            } else {
                onFulfilled = Handle::adopt(toJSValue(closureFn(vm, combinatorElement, recordKeep.get(), flagKeep.get(), base)));
                onRejected = Handle::retained(cap.reject.get());
            }
            result = invokeThen(vm, nextPromise, onFulfilled.get(), onRejected.get(), 2);
            ++index;
        }
        if (result.isException()) {
            closeAfterThrow(vm, iterator.get());
            return rejectWith();
        }
        release(result);
    }
}

/// An `await`: go on when what was awaited settles. Something that is no object is fulfilled at once; nothing is made.
void awaitValue(VM& vm, JSValue out, JSValue generator, JSValue promise)
{
    if (!isObject(out)) {
        vm.enqueueJob(4, generator, promise, out);
        return;
    }
    JSValue awaited = promiseResolve(vm, out);
    Handle awaitedKeep = Handle::adopt(awaited);
    performAwait(vm, awaited, generator, promise);
}

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
    if (done)
        resolvePromise(vm, promise, out);
    else
        awaitValue(vm, out, generator, promise);
}

void asyncResume(VM& vm, JSValue generator, JSValue promise, int mode, JSValue sent)
{
    asyncStep(vm, generator, promise, mode, sent);
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
    void values(Vec<JSValue>& out) const override
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
            return r->ext && typeid(*r->ext) == typeid(AsyncGenState) ? static_cast<AsyncGenState*>(r->ext) : nullptr;
    return nullptr;
}

void asyncGenDrain(VM& vm, JSValue outer);

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
        // An `await`: go on when what was awaited settles. Something that is no object is fulfilled at once.
        if (!isObject(out)) {
            vm.enqueueJob(6, outer, JSValue::undefined(), out);
            return;
        }
        JSValue awaited = promiseResolve(vm, out);
        Handle awaitedKeep = Handle::adopt(awaited);
        performAwait(vm, awaited, outer, JSValue::undefined(), 6);
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
    void values(Vec<JSValue>& out) const override
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
    return iterResult(vm, arg(args, argc, 0), r.fnState != 0);
}

/// What a rejected value of `next()` or `return()` does: closes the sync iterator (boundTarget), then rejects with the same
/// reason (AsyncFromSyncIteratorContinuation with closeOnRejection).
NATIVE(wrapCloseOnReject)
{
    UNUSED();
    Rare& r = carried(data);
    const JSValue reason = arg(args, argc, 0);
    iteratorClose(vm, r.boundTarget, true);
    release(vm.exception);
    vm.exception = reason;
    retain(vm.exception);
    vm.exceptionCode = ErrorCode::Exception;
    return JSValue::exception();
}

NATIVE(syncWrapStep)
{
    UNUSED();
    SyncWrap* w = nullptr;
    if (isObject(self) && asObject(self)->cls() == ObjClass::Native)
        if (Rare* r = asObject(self)->rare())
            w = r->ext && typeid(*r->ext) == typeid(SyncWrap) ? static_cast<SyncWrap*>(r->ext) : nullptr;
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
                // No `throw`: the iterator is closed to give it a chance to clean up, and that is a protocol violation.
                if (!iteratorClose(vm, w->iterator.get(), false)) {
                    r = JSValue::exception();
                } else {
                    r = vm.throwError(ErrorCode::TypeError, "The iterator does not provide a 'throw' method");
                }
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
    if (wrapped.isException()) {
        if (!done && mode != 1)
            iteratorClose(vm, w->iterator.get(), true);
        JSValue e = takeException(vm);
        rejectPromise(vm, derived, e);
        release(e);
        return derivedKeep.take();
    }
    Handle wrappedKeep = Handle::adopt(wrapped);
    Handle unwrap = Handle::adopt(toJSValue(closureFn(vm, wrapUnwrap, JSValue::undefined(), JSValue::undefined(), done ? 1 : 0)));
    Handle onRejected;
    if (!done && mode != 1)
        onRejected = Handle::adopt(toJSValue(closureFn(vm, wrapCloseOnReject, w->iterator.get(), JSValue::undefined(), 0)));
    performThen(vm, wrapped, unwrap.get(), onRejected.get(), derived);
    return derivedKeep.take();
}

}  // namespace

JSValue asyncStarted(VM& vm, JSValue generator, JSValue out)
{
    Handle gen = Handle::adopt(generator);
    Handle outKeep = Handle::adopt(out);
    JSValue promise = newPromiseObject(vm);
    Handle promiseKeep = Handle::adopt(promise);
    if (out.isException()) {
        outKeep.take();
        JSValue e = takeException(vm);
        rejectPromise(vm, promise, e);
        release(e);
    } else if (generator.isUndefined()) {
        resolvePromise(vm, promise, out);
    } else {
        awaitValue(vm, out, generator, promise);
    }
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

bool isAsyncGenerator(JSValue v) { return asyncGenOf(v) != nullptr; }

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
        // %AsyncIteratorPrototype%, which `Symbol.asyncIterator` lives on, is what an async generator's prototype inherits from.
        JSObject* asyncIteratorProto = vm.newObject(vm.protos.object);
        symbolMethod(vm, asyncIteratorProto, vm.atoms.symAsyncIterator, "[Symbol.asyncIterator]",
                     [](VM&, JSValue self, const JSValue*, std::uint32_t, void*) -> JSValue {
                         retain(self);
                         return self;
                     }, 0);
        JSObject* agProto = vm.newObject(asyncIteratorProto);
        release(toJSValue(asyncIteratorProto));
        vm.setIntrinsic(&vm.protos.asyncGenerator, agProto);
        release(toJSValue(agProto));
        method(vm, agProto, "next", asyncGenEnqueue, 1, reinterpret_cast<void*>(0));
        if (JSValue* next = agProto->findOwn(vm, vm.atoms.next))
            vm.origAsyncGenNext = *next;
        if (JSValue* self = asyncIteratorProto->findOwn(vm, vm.atoms.symAsyncIterator))
            vm.origAsyncIteratorSelf = *self;
        method(vm, agProto, "throw", asyncGenEnqueue, 1, reinterpret_cast<void*>(1));
        method(vm, agProto, "return", asyncGenEnqueue, 1, reinterpret_cast<void*>(2));
        agProto->putOwn(vm, vm.atoms.symToStringTag, makeString(vm, std::string_view("AsyncGenerator")), attr::kConfigurable);
    }
    JSObject* proto = vm.newObject(vm.protos.object);
    vm.setIntrinsic(&vm.protos.promise, proto);
    release(toJSValue(proto));
    JSFunction* ctor = constructor(vm, "Promise", promiseCtor, 1, proto);
    vm.promiseCtor = toJSValue(ctor);
    retain(vm.promiseCtor);
    method(vm, proto, "then", promiseThen, 2);
    vm.promiseThen = *proto->findOwn(vm, vm.atoms.then);
    retain(vm.promiseThen);
    method(vm, proto, "catch", promiseCatch, 1);
    method(vm, proto, "finally", promiseFinally, 1);
    proto->putOwn(vm, vm.atoms.symToStringTag, makeString(vm, std::string_view("Promise")), attr::kConfigurable);
    method(vm, ctor, "resolve", promiseResolveStatic, 1);
    method(vm, ctor, "reject", promiseRejectStatic, 1);
    method(vm, ctor, "all", promiseCombinator, 1, reinterpret_cast<void*>(0));
    method(vm, ctor, "allSettled", promiseCombinator, 1, reinterpret_cast<void*>(1));
    method(vm, ctor, "any", promiseCombinator, 1, reinterpret_cast<void*>(2));
    method(vm, ctor, "race", promiseCombinator, 1, reinterpret_cast<void*>(3));
    method(vm, ctor, "withResolvers", promiseWithResolvers, 0);
    method(vm, ctor, "try", promiseTry, 1);
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
    struct Running
    {
        bool& flag;
        const bool outer;
        explicit Running(bool& f) noexcept : flag(f), outer(!f) { flag = true; }
        ~Running()
        {
            if (outer)
                flag = false;
        }
    };
    {
    const Running running(runningJobs_);
    while (!jobs.empty()) {
        MicroJob job = jobs.front();
        jobs.pop_front();
        bi::runJob(*this, job);
        release(job.a);
        release(job.b);
        release(job.c);
        ++ran;
    }
    }
    if (deferred && frameTop == frameBase)
        scanAtHost();
    return ran;
}

}  // namespace vm
}  // namespace internal
}  // namespace script
