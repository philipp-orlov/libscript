// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The dynamic engine's runtime: atoms, intrinsics, the global object, the job
// queue and the VM's frame state. The heap itself belongs to the context;
// this borrows it.
#pragma once

#include <initializer_list>
#include <cstdint>
#include <exception>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "script/internal/context.hpp"
#include "script/internal/typed/jit.hpp"
#include "script/internal/dyn/bytecode.hpp"
#include "script/internal/dyn/object.hpp"

namespace script {
namespace internal {
namespace dyn {

struct Frame;

/// A queued promise reaction. Jobs run to completion, one at a time, in FIFO
/// order, exactly like a browser microtask queue.
/// One job on the queue: a call, or one of a promise's reactions, which are made by the thousand and so carry what
/// they need rather than being made into functions to call.
struct Job
{
    enum class Kind : std::uint8_t
    {
        Call,        ///< `fn(...args)`
        Reaction,    ///< `handler` on `result`, settling the promise `target`
        Capability,  ///< `handler` on `result`, through another constructor's `resolve` and `reject`
        Await,       ///< an awaiting coroutine `handler` goes on with `result`
        Adopt,       ///< the promise `target` takes on the outcome of the promise `fn`, whose `then` is the built-in one
    };
    Value fn;
    std::vector<Value> args;
    Kind kind = Kind::Call;
    bool fulfilled = false;
    Value result = Value::undefined();
    Value target = Value::undefined();
    Value reject = Value::undefined();
};

/// The frequently used property names, interned once.
struct Atoms
{
    Ref<StringCell> length, prototype, constructor, name, message, stack, value, done, next, then,
        toString, valueOf, protoName, arguments, toJSON;
};

/// The well-known symbols the engine itself uses.
struct Symbols
{
    Ref<SymbolCell> iterator, asyncIterator, toPrimitive, toStringTag, hasInstance, match, matchAll, replace, search, split, species, unscopables, isConcatSpreadable;
};

/// A compiled TypeScript or JavaScript program: what `Assembly` wraps for the
/// dynamic languages.
struct Program : CompiledUnit
{
    std::shared_ptr<const FunctionCode> code;

    std::vector<std::string> methods() const override;
    std::string entryPoint() const override { return code ? code->name : std::string(); }
    std::size_t imageSize() const noexcept override;
};

/// What a host class looks like from JavaScript: the object its static
/// members hang off (the constructor function, for a `Class<T>`), and the
/// prototype its instances inherit from.
struct HostBinding
{
    Ref<DynObject> target;
    Ref<DynObject> prototype;
};

class JitWorker;

struct Runtime : EngineRuntime
{
    explicit Runtime(ContextImpl& context);
    ~Runtime() override;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    ContextImpl& ctx;

    // ---- the heap, which is the context's ----
    template <class T, class... Args>
    T* newCell(Args&&... args)
    {
        // By hand, not through `link`: this is how every array and function starts, and its size is no question.
        ctx.maybeCollect();
        ctx.reserve(sizeof(T));
        void* memory = ctx.heap.allocate(sizeof(T));
        T* cell;
        try {
            cell = new (memory) T(std::forward<Args>(args)...);
        } catch (...) {
            ctx.heap.deallocate(memory, sizeof(T));
            throw;
        }
        cell->cellBytes = std::uint32_t(sizeof(T));
        ctx.linkKnown(cell, std::uint32_t(sizeof(T)));
        return cell;
    }
    void maybeCollect() { ctx.maybeCollect(); }
    StringCell* intern(StringView text) { return ctx.intern(text); }
    StringCell* intern(const char* asciiText) { return ctx.intern(asciiText); }
    StringCell* newString(StringView text) { return ctx.newString(text); }
    /// A fresh string cell that is not interned: JavaScript makes strings by
    /// the million and compares them by content.
    Value str(StringView text) { return detail::Access::adopt(Type::String, ctx.newString(text)); }
    Value str(const char* asciiText) { return str(ascii(asciiText)); }

    /// What `typeof` gives, one interned string for each answer, made on first use: `typeof x === "number"` then makes
    /// nothing and compares two pointers.
    std::pair<const char*, Ref<StringCell>> typeNames[8];
    Value typeOfValue(const Value& v);

    // ---- atoms and symbols ----
    std::unordered_map<String, SymbolCell*> symbolRegistry;
    Atoms atom;
    Symbols sym;

    // ---- intrinsics ----
    Ref<DynObject> globalObject;
    /// Changes whenever the global object's table moves, loses a property or changes one's attributes: while it stays,
    /// a global is where native code last saw it, and the same kind of property.
    std::size_t jsonSizeSeen = 0;
    std::uint64_t globalEpoch = 0;
    /// Where native code that is bailing leaves its registers, for as long as it takes to read them.
    std::uint64_t bailRegs[64] = {};
    /// The built-ins native code does in place, held so the ones it compares a callee against stay alive.
    enum Intrinsic : int
    {
        MathSqrt,
        MathAbs,
        MathFloor,
        MathCeil,
        MathTrunc,
        // Numbers in, a number out, and nothing else happens: called directly on bare doubles.
        MathSin,
        MathCos,
        MathTan,
        MathAtan,
        MathExp,
        MathLog,
        MathRound,
        ParseInt,
        MathMax,
        MathMin,
        MathPow,
        MathAtan2,
        ArrayPush,
        ArrayPop,
        ArrayForEach,
        ArrayMap,
        ArrayFilter,
        ArrayReduce,
        ArrayValues,
        ArrayIteratorNext,
        GeneratorNext,
        StringCharCodeAt,
        StringFromCharCode,
        FunctionApply,
        FunctionCall,
        MapGet,
        MapSet,
        MapHas,
        SetAdd,
        SetHas,
        Eval,
        PromiseThen,
        IntrinsicCount
    };
    Ref<GCObject> intrinsics[IntrinsicCount];
    /// `protoEpoch` when no `Symbol.hasInstance` was found from `Function.prototype` up, so `instanceof` on a plain
    /// function can skip looking; anything else when that is not known.
    std::uint64_t hasInstanceAbsentAt = ~std::uint64_t(0);
    /// One-character strings below 128, made on first use.
    Ref<GCObject> asciiChars[128];
    Ref<DynObject> objectProto, functionProto, arrayProto, stringProto, numberProto, booleanProto,
        symbolProto, errorProto, dateProto, mapProto, setProto, weakMapProto, weakSetProto, argumentsProto, promiseProto, iteratorProto, generatorProto, generatorFunctionProto, asyncFunctionProto,
        asyncIteratorProto, asyncGeneratorProto, asyncGeneratorFunctionProto,
        arrayIteratorProto, regExpProto, regExpStringIteratorProto, bigintProto;
    /// See builtins/regexp.cpp.
    std::shared_ptr<struct RegExpState> regexp;
    std::shared_ptr<struct TypedState> typed;
    std::shared_ptr<struct IteratorStateHolder> iterators;
    Ref<DynFunction> objectCtor, arrayCtor, functionCtor, promiseCtor;
    /// Prototype per JavaScript error constructor, indexed by `ErrorCode`.
    std::unordered_map<int, Ref<DynObject>> errorProtos;

    // ---- host classes ----
    std::unordered_map<const HostClassInfo*, HostBinding> hostBindings;
    /// Create the host classes and members the binding layer has declared
    /// since the last call. Runs before every compile and before a host
    /// wrapper is made, so bindings declared in any order are all visible.
    void syncHostClasses();
    /// The prototype and constructor `Class<T>` gave a C++ type.
    Value hostPrototype(const void* typeKey);
    Value hostConstructor(const void* typeKey);
    /// The prototype a host wrapper (`NativeCell`) reads its members from:
    /// the class's, or Object.prototype for an unbound type.
    DynObject* hostPrototypeFor(const void* typeKey);
    /// The ambient TypeScript declarations for every host class, derived from
    /// the C++ signatures the binding layer recorded.
    std::string hostDeclarations() const;
    /// Ambient declarations the embedder added with `Context::declare`.
    std::string ambient;

    // ---- vm ----
    std::vector<Frame*> frames;
    /// Coroutine frames native code was entered in at a loop, innermost last: a bail in such code goes back to its frame.
    std::vector<Frame*> nativeCoroutines;
    /// Set by a bail that went back to the innermost of those, for the entry to see.
    bool coroutineHandedBack = false;
    /// One reusable frame per call depth; see `Frame`.
    std::vector<std::unique_ptr<Frame>> framePool;

    Frame& acquireFrame(std::size_t depth);

    std::deque<Job> jobs;
    std::vector<Ref<DynPromise>> unhandled;
    /// Every program compiled in this runtime that may still be held by an
    /// `Assembly`. Teardown empties their constants, so a handle that outlives
    /// the context holds nothing that points into the dead heap.
    std::vector<std::weak_ptr<Program>> programs;
    void adoptProgram(const std::shared_ptr<Program>& program);
    /// What `eval` and `new Function` compiled, emptied at teardown as programs are.
    std::vector<std::weak_ptr<const FunctionCode>> evalCodes;
    /// What the interpreter spent time in, for SCRIPT_INTERP_PROFILE.
    std::vector<const FunctionCode*> interpretedCodes;
    /// Calls the interpreter has begun since it last asked whether to stop: code that recurses instead of looping.
    std::uint32_t interpTicks = 0;
    void adoptEvalCode(const std::shared_ptr<const FunctionCode>& code);

    /// "at fn (file:line)" lines for the frames that are live right now.
    std::string captureStack() const override;
    void releaseRoots() noexcept override;
    void afterCollect() noexcept override;
    void enqueue(Value fn, std::vector<Value> args);
    std::size_t runJobs();

    // ---- allocation helpers ----
    /// A shape for an object made without one, when its properties fit.
    void giveShape(DynObject* o);
    /// `newObject` with room for this many properties already there.
    /// What a new object of this prototype and root shape, with this much room, starts out as, a word at a time, for native
    /// code that makes one itself. All but where its table is, which depends on where it is.
    void objectImage(DynObject* proto, Shape* root, std::uint32_t room, std::uint64_t* words);
    /// A few elements are kept in the cell, with room for them to be values: one allocation for a small array.
    static constexpr std::size_t kArrayInCell = 8;
    /// The same for a new array with room for `reserve` values in its cell, of `bytes` in all: false when it is not one
    /// native code can make.
    bool arrayImage(std::size_t reserve, std::uint64_t* words, std::size_t& bytes);
    DynObject* newObjectSized(DynObject* proto, std::uint32_t room);
    DynObject* newObject(DynObject* proto = nullptr);

    /// A function's `arguments` is an array whose prototype is `argumentsProto`: elements like an array's, but to the
    /// program an Arguments object, which `Array.isArray` says is no array.
    bool isArgumentsObject(const DynObject* o) const noexcept { return o && argumentsProto && o->proto.get() == argumentsProto.get(); }

    // ---- native code; see dyn/jit.hpp ----
    std::unique_ptr<typed::JitBackend> jitBackend;
#if SCRIPT_ENABLE_JIT_THREAD
    /// Declared after the back end it uses, so that it stops first.
    std::unique_ptr<JitWorker> jitWorker;
#endif
    std::vector<typed::NativeCode*> jitCodes;
    /// Code no function enters any more, freed by `sweepRetiredNative` once nothing on the stack is in it.
    std::vector<typed::NativeCode*> jitRetired;
    std::size_t jitRetiredBytes = 0;
    /// The code of functions native code has a copy of, known by their code alone: kept while the runtime lives, since
    /// that copy's bails name it. Held here rather than by the code that has the copy, where two functions copied into
    /// each other would keep each other.
    std::unordered_map<const FunctionCode*, std::shared_ptr<const FunctionCode>> jitHeldCodes;
    std::uint64_t jitCompiled = 0;
    std::uint32_t nativeError = 0;
    std::exception_ptr nativePending;
    /// What native code's `finally` blocks have put aside to raise again when they end, innermost last.
    std::vector<std::exception_ptr> finallyPending;
    std::uint32_t pollFlag = 0;
    std::uintptr_t nativeStackLimit = 0;
    void* nativeTopFrame = nullptr;
    const void* nativeTopPc = nullptr;
    /// What a slow path that returns a borrowed value keeps alive until the next one.
    Value nativeKeepAlive = Value::undefined();

    // ---- shapes ----
    /// The shape an empty object with this prototype has. Null when there are too many shapes already.
    Shape* rootShapeFor(DynObject* proto);
    /// The shape after adding this property to `from`; null means it stops having one.
    Shape* shapeAfter(Shape* from, GCObject* key, std::uint8_t attrs);
    /// How many shapes a runtime keeps; past this a new layout simply has no shape, and works by name.
    static constexpr std::size_t kMaxShapes = 1u << 16;
    std::vector<std::unique_ptr<Shape>> shapes;
    Shape* nullProtoRoot = nullptr;
    /// `{ value, done }` on Object.prototype: the shape it starts from, with `value`, and with both. Worked out on first
    /// use by `iterResult`, which makes these by the million in generator code.
    /// One more than the `protoEpoch` at which `next` was found at `generatorNextIndex` in the generators' prototype;
    /// zero for never.
    std::uint64_t generatorNextEpoch = 0;
    /// One more than the `protoEpoch` at which Array.prototype and Object.prototype were last seen to hold no element.
    std::uint64_t noProtoElementsEpoch = 0;
    std::uint32_t generatorNextIndex = 0;
    Shape* iterRootShape = nullptr;
    Shape* iterValueShape = nullptr;
    Shape* iterDoneShape = nullptr;
    DynArray* newArray(std::size_t reserve = 0);
    DynFunction* newFunction();
    Value makeArray(Vec<Value> items);
    Value makeArray(const std::vector<Value>& items);
    Value makeArray(std::initializer_list<Value> items) { return makeArray(Vec<Value>(items)); }
    Value makeFunction(StringView name, NativeFunction fn, int arity);
    /// Install `name` on `target` as a non-enumerable native method.
    void method(DynObject* target, const char* name, NativeFunction fn, int arity = 0);
    void getter(DynObject* target, const char* name, NativeFunction fn);
    void constant(DynObject* target, const char* name, Value v);
    /// Create a constructor with a fresh prototype wired to it.
    DynFunction* makeConstructor(const char* name, NativeFunction fn, int arity, DynObject* proto);
};

/// Give `obj` the wrapper state of a C++ instance.
void attachNativeSlot(DynObject& obj, const void* typeKey, void* instance,
                      Context::Finalizer finalizer, void* owner);

/// The dynamic runtime a value belongs to, or null for a primitive or a value
/// of the other engine.
inline Runtime* runtimeOf(const Value& v) noexcept
{
    GCObject* p = detail::Access::ptr(v);
    return p && p->rt ? p->rt->dynIfAny() : nullptr;
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
