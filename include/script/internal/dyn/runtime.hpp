// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The dynamic engine's runtime: atoms, intrinsics, the global object, the job
// queue and the VM's frame state. The heap itself belongs to the context;
// this borrows it.
#pragma once

#include <initializer_list>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "script/internal/context.hpp"
#include "script/internal/dyn/bytecode.hpp"
#include "script/internal/dyn/object.hpp"

namespace script {
namespace internal {
namespace dyn {

struct Frame;

/// A queued promise reaction. Jobs run to completion, one at a time, in FIFO
/// order, exactly like a browser microtask queue.
struct Job
{
    Value fn;
    std::vector<Value> args;
};

/// The frequently used property names, interned once.
struct Atoms
{
    Ref<StringCell> length, prototype, constructor, name, message, stack, value, done, next, then,
        toString, valueOf, protoName, arguments;
};

/// The well-known symbols the engine itself uses.
struct Symbols
{
    Ref<SymbolCell> iterator, asyncIterator, toPrimitive, toStringTag, hasInstance;
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
        return ctx.newCell<T>(std::forward<Args>(args)...);
    }
    void maybeCollect() { ctx.maybeCollect(); }
    StringCell* intern(StringView text) { return ctx.intern(text); }
    StringCell* intern(const char* asciiText) { return ctx.intern(asciiText); }
    StringCell* newString(StringView text) { return ctx.newString(text); }
    /// A fresh string cell that is not interned: JavaScript makes strings by
    /// the million and compares them by content.
    Value str(StringView text) { return detail::Access::adopt(Type::String, ctx.newString(text)); }
    Value str(const char* asciiText) { return str(ascii(asciiText)); }

    // ---- atoms and symbols ----
    std::unordered_map<String, SymbolCell*> symbolRegistry;
    Atoms atom;
    Symbols sym;

    // ---- intrinsics ----
    Ref<DynObject> globalObject;
    Ref<DynObject> objectProto, functionProto, arrayProto, stringProto, numberProto, booleanProto,
        symbolProto, errorProto, dateProto, mapProto, setProto, promiseProto, iteratorProto,
        arrayIteratorProto;
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

    /// "at fn (file:line)" lines for the frames that are live right now.
    std::string captureStack() const override;
    void releaseRoots() noexcept override;
    void afterCollect() noexcept override;
    void enqueue(Value fn, std::vector<Value> args);
    std::size_t runJobs();

    // ---- allocation helpers ----
    DynObject* newObject(DynObject* proto = nullptr);

    // ---- shapes ----
    /// The shape an empty object with this prototype has. Null when there are too many shapes already.
    Shape* rootShapeFor(DynObject* proto);
    /// The shape after adding this property to `from`; null means it stops having one.
    Shape* shapeAfter(Shape* from, GCObject* key, std::uint8_t attrs);
    /// How many shapes a runtime keeps; past this a new layout simply has no shape, and works by name.
    static constexpr std::size_t kMaxShapes = 1u << 16;
    std::vector<std::unique_ptr<Shape>> shapes;
    Shape* nullProtoRoot = nullptr;
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
