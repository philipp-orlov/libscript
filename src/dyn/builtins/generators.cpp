// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The objects behind `function*`, `async function` and `async function*`, and the iterator that lets `for await` take a
// sync iterable. How they run is in vm.cpp.
#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/vm.hpp"

namespace script {
namespace internal {
namespace dyn {

namespace {

DynCoroutine* thisGenerator(CallInfo& ci)
{
    DynObject* object = asObject(ci.thisValue());
    if (!object || object->cls != ObjClass::Generator ||
        static_cast<DynCoroutine*>(object)->coKind != DynCoroutine::Kind::Generator)
        ci.fail(ErrorCode::TypeError, "this method needs a generator");
    return static_cast<DynCoroutine*>(object);
}

/// `name` for what `Object.getPrototypeOf(function* () {}).constructor` is: there, as the language says, and refusing to
/// compile anything, as `Function` does here.
void kindOfFunction(Runtime& rt, DynObject* proto, const char* name)
{
    Value ctor = rt.makeFunction(fromUtf8(name),
                                 [](CallInfo& ci) -> Value {
                                     ci.fail(ErrorCode::TypeError, "the Function constructor is disabled; the engine compiles only what it is given");
                                     return Value::undefined();
                                 },
                                 1);
    asObject(ctor)->putOwn(rt.atom.prototype.get(), detail::Access::make(Type::Object, proto), 0);
    proto->putOwn(rt.atom.constructor.get(), ctor, PropConfigurable);
    proto->putOwn(rt.sym.toStringTag.get(), rt.str(fromUtf8(name)), PropConfigurable);
}

/// What came of a sync iterator's step, as an async iterator's answer: its value awaited, then wrapped back up.
void continueFromSync(Runtime& rt, const Value& result, DynPromise* outer)
{
    if (!result.isObject()) {
        rejectPromise(rt, outer, makeErrorValue(rt, ErrorCode::TypeError, "the iterator result is not an object"));
        return;
    }
    const bool done = toBoolean(getProperty(rt, result, rt.atom.done.get()));
    const Value value = getProperty(rt, result, rt.atom.value.get());
    Value waited = value;
    DynObject* object = asObject(value);
    if (!object || object->cls != ObjClass::Promise) {
        waited = makePromise(rt);
        resolvePromise(rt, static_cast<DynPromise*>(asObject(waited)), value);
    }
    DynPromise::Reaction reaction;
    reaction.onFulfilled = rt.makeFunction(ascii(""),
                                           [done](CallInfo& ci) { return iterResult(rtOf(ci), ci.arg(0), done); }, 1);
    reaction.next.reset(outer);
    addReaction(rt, static_cast<DynPromise*>(asObject(waited)), std::move(reaction));
}

}  // namespace

Value asyncFromSyncIterator(Runtime& rt, const Value& iterator)
{
    DynObject* wrapper = rt.newObject(rt.asyncIteratorProto.get());
    Value handle = detail::Access::adopt(Type::Object, wrapper);
    const auto answer = [](Runtime& rt2, auto&& step) {
        Value promise = makePromise(rt2);
        auto* outer = static_cast<DynPromise*>(asObject(promise));
        try {
            step(outer);
        } catch (Exception& e) {
            rejectPromise(rt2, outer, e.value());
        }
        return promise;
    };
    wrapper->putOwn(rt.atom.next.get(), rt.makeFunction(ascii("next"), [iterator, answer](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const Value sent = ci.arg(0);
        const bool given = ci.size() > 0;
        return answer(rt2, [&](DynPromise* outer) {
            continueFromSync(rt2, invokeMethod(rt2, iterator, rt2.atom.next.get(), given ? std::vector<Value>{sent} : std::vector<Value>{}), outer);
        });
    }, 1), PropWritable | PropConfigurable);
    wrapper->putOwn(rt.intern("return"), rt.makeFunction(ascii("return"), [iterator, answer](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const Value sent = ci.arg(0);
        const bool given = ci.size() > 0;
        return answer(rt2, [&](DynPromise* outer) {
            const Value fn = getProperty(rt2, iterator, rt2.intern("return"));
            if (fn.isNullish()) {
                resolvePromise(rt2, outer, iterResult(rt2, sent, true));
                return;
            }
            continueFromSync(rt2, callValue(rt2, fn, iterator, given ? std::vector<Value>{sent} : std::vector<Value>{}), outer);
        });
    }, 1), PropWritable | PropConfigurable);
    wrapper->putOwn(rt.intern("throw"), rt.makeFunction(ascii("throw"), [iterator, answer](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const Value sent = ci.arg(0);
        return answer(rt2, [&](DynPromise* outer) {
            const Value fn = getProperty(rt2, iterator, rt2.intern("throw"));
            if (fn.isNullish()) {
                iteratorClose(rt2, iterator);
                rejectPromise(rt2, outer, makeErrorValue(rt2, ErrorCode::TypeError, "the iterator has no throw method"));
                return;
            }
            continueFromSync(rt2, callValue(rt2, fn, iterator, {sent}), outer);
        });
    }, 1), PropWritable | PropConfigurable);
    return handle;
}

void installGenerators(Runtime& rt)
{
    DynObject* generatorFunction = rt.newObject(rt.functionProto.get());
    rt.generatorFunctionProto.reset(generatorFunction, true);
    DynObject* generator = rt.newObject(rt.iteratorProto.get());
    rt.generatorProto.reset(generator, true);
    kindOfFunction(rt, generatorFunction, "GeneratorFunction");
    generatorFunction->putOwn(rt.atom.prototype.get(), detail::Access::make(Type::Object, generator), PropConfigurable);
    generator->putOwn(rt.atom.constructor.get(), detail::Access::make(Type::Object, generatorFunction), PropConfigurable);
    generator->putOwn(rt.sym.toStringTag.get(), rt.str(ascii("Generator")), PropConfigurable);
    rt.method(generator, "next", [](CallInfo& ci) { return generatorResume(rtOf(ci), thisGenerator(ci), DynCoroutine::Resume::Next, ci.arg(0)); }, 1);
    rt.intrinsics[Runtime::GeneratorNext].reset(detail::Access::ptr(generator->findOwn(rt.atom.next.get())->value));
    rt.method(generator, "return", [](CallInfo& ci) { return generatorResume(rtOf(ci), thisGenerator(ci), DynCoroutine::Resume::Return, ci.arg(0)); }, 1);
    rt.method(generator, "throw", [](CallInfo& ci) { return generatorResume(rtOf(ci), thisGenerator(ci), DynCoroutine::Resume::Throw, ci.arg(0)); }, 1);

    DynObject* asyncFunction = rt.newObject(rt.functionProto.get());
    rt.asyncFunctionProto.reset(asyncFunction, true);
    kindOfFunction(rt, asyncFunction, "AsyncFunction");

    DynObject* asyncIterator = rt.newObject(rt.objectProto.get());
    rt.asyncIteratorProto.reset(asyncIterator, true);
    asyncIterator->putOwn(rt.sym.asyncIterator.get(),
                          rt.makeFunction(ascii("[Symbol.asyncIterator]"), [](CallInfo& ci) { return ci.thisValue(); }, 0),
                          PropWritable | PropConfigurable);

    DynObject* asyncGeneratorFunction = rt.newObject(rt.functionProto.get());
    rt.asyncGeneratorFunctionProto.reset(asyncGeneratorFunction, true);
    DynObject* asyncGenerator = rt.newObject(asyncIterator);
    rt.asyncGeneratorProto.reset(asyncGenerator, true);
    kindOfFunction(rt, asyncGeneratorFunction, "AsyncGeneratorFunction");
    asyncGeneratorFunction->putOwn(rt.atom.prototype.get(), detail::Access::make(Type::Object, asyncGenerator), PropConfigurable);
    asyncGenerator->putOwn(rt.atom.constructor.get(), detail::Access::make(Type::Object, asyncGeneratorFunction), PropConfigurable);
    asyncGenerator->putOwn(rt.sym.toStringTag.get(), rt.str(ascii("AsyncGenerator")), PropConfigurable);
    rt.method(asyncGenerator, "next", [](CallInfo& ci) { return asyncGeneratorRequest(rtOf(ci), ci.thisValue(), DynCoroutine::Resume::Next, ci.arg(0)); }, 1);
    rt.method(asyncGenerator, "return", [](CallInfo& ci) { return asyncGeneratorRequest(rtOf(ci), ci.thisValue(), DynCoroutine::Resume::Return, ci.arg(0)); }, 1);
    rt.method(asyncGenerator, "throw", [](CallInfo& ci) { return asyncGeneratorRequest(rtOf(ci), ci.thisValue(), DynCoroutine::Resume::Throw, ci.arg(0)); }, 1);
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
