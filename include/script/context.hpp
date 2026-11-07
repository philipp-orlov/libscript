// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libscript -- the engine instance: one heap, every language, one lock.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "script/diagnostic.hpp"
#include "script/language.hpp"
#include "script/value.hpp"

namespace script {

namespace internal {
struct ContextImpl;
struct CompiledUnit;
}  // namespace internal

class Context;

// ------------------------------------------------------------ call info ---

/// The arguments of a native (host) function call, whichever language made it.
class CallInfo
{
public:
    Context& context() const noexcept { return *ctx_; }
    /// The receiver of an instance method; null for a static one.
    const Value& thisValue() const noexcept { return *this_; }
    const std::vector<Value>& args() const noexcept { return *args_; }
    std::size_t size() const noexcept { return args_->size(); }
    /// Out-of-range indices read as `undefined`, like a JavaScript call
    /// would see them. The C# compiler checks arity, so this only matters for
    /// calls made straight from C++.
    Value arg(std::size_t i) const
    {
        return i < args_->size() ? (*args_)[i] : Value::undefined();
    }
    /// True when invoked through `new`.
    bool isConstructorCall() const noexcept { return isNew_; }

    /// Convert argument `i`, raising an error naming the function if the
    /// value does not fit. Defined in binding.hpp.
    template <class T>
    T arg(std::size_t i) const;

    [[noreturn]] void fail(ErrorCode code, std::string_view message) const;

private:
    CallInfo() = default;
    Context* ctx_ = nullptr;
    const Value* this_ = nullptr;
    const std::vector<Value>* args_ = nullptr;
    bool isNew_ = false;
    friend struct detail::Access;
};

using NativeFunction = std::function<Value(CallInfo&)>;

// ------------------------------------------------------------- assembly ---

/// A compiled program, in whichever language it was written.
///
/// Compilation and execution are separate steps. `Context::compile` produces
/// an Assembly that is ready to run and `Context::run` runs it, as often as
/// wanted, without parsing or checking anything again. A C# assembly is
/// metadata plus typed bytecode with every name resolved and every call site
/// bound; `serialize` writes that state to bytes another process can
/// `Context::load`. A TypeScript or JavaScript assembly is the compiled
/// program with its closures; it runs, but has no portable image yet.
///
/// An Assembly belongs to the Context that produced it.
class Assembly
{
public:
    Assembly();
    ~Assembly();
    Assembly(Assembly&&) noexcept;
    Assembly& operator=(Assembly&&) noexcept;
    Assembly(const Assembly&);
    Assembly& operator=(const Assembly&);

    explicit operator bool() const noexcept { return impl_ != nullptr; }
    const std::string& name() const noexcept;
    Language language() const noexcept;
    /// `Type.Method` for every method a C# assembly defines; the function
    /// names of a JavaScript program.
    std::vector<std::string> methods() const;
    /// The synthesized or declared entry point, or "" if there is none.
    std::string entryPoint() const;

    /// The portable image of a C# assembly: a header, the string and type
    /// tables, then one blob of bytecode per method. Stable across runs and
    /// machines. Throws `Error(NotSupported)` for the other languages.
    std::vector<std::uint8_t> serialize() const;
    /// Bytes the compiled form occupies, excluding the heap it allocates.
    std::size_t imageSize() const noexcept;

    internal::CompiledUnit* impl() const noexcept { return impl_.get(); }

private:
    explicit Assembly(std::shared_ptr<internal::CompiledUnit> impl);
    std::shared_ptr<internal::CompiledUnit> impl_;
    friend class Context;
};

// ------------------------------------------------------------------ jit ---

/// Native code generation for the statically typed languages. The arm64 and
/// x86-64 back ends report themselves unavailable on any other host, so
/// `Mode::Tiered` is always safe to ask for.
struct JitOptions
{
    enum class Mode
    {
        Off,      ///< interpret everything
        Tiered,   ///< interpret, then compile a method once it gets hot
        Eager,    ///< compile every method when its assembly is loaded
    };

    /// Tiered wherever there is a code generator: the interpreter's answer and native code's are the same, only faster.
    Mode mode = Mode::Tiered;
    /// Calls (or loop back-edges) before a method is handed to the back end.
    std::uint32_t threshold = 200;
    /// Cap on the code cache, in bytes; 0 means unbounded.
    std::size_t codeCacheLimit = 8u << 20;
    /// Write the generated code to the print handler, for debugging.
    bool dumpCode = false;
};

// ---------------------------------------------------------- host members ---

/// One member of a host class, as the binding layer describes it: the C++
/// signature spelled in each language's type system. Filled in by
/// binding.hpp; rarely written by hand.
struct HostMember
{
    enum Kind : std::uint8_t
    {
        Method,
        Constructor,
        Property,  ///< a getter, and a setter when one was given
        Constant,  ///< a static read-only value
    };

    Kind kind = Method;
    std::string name;
    bool isStatic = true;
    std::string csharpReturn = "void";
    std::vector<std::string> csharpParams;
    std::string typescriptReturn = "void";
    std::vector<std::string> typescriptParams;
};

// -------------------------------------------------------------- context ---

/// An isolated engine instance. Everything reachable from a Context -- values,
/// objects, types, assemblies, the JavaScript global object -- belongs to that
/// Context and must not be mixed with another one. One Context runs every
/// language this build contains, on one heap, so a value made by one language
/// can be handed to another through the host.
///
/// Every public entry point takes the Context's recursive lock, so a Context
/// may be shared between threads; calls are serialized. Contexts share no
/// global state, so the way to get parallelism is one Context per thread.
class Context
{
public:
    struct Options
    {
        std::size_t memoryLimit = 0;         ///< bytes; 0 means unbounded
        std::size_t stackLimit = 512;        ///< maximum call depth
        std::size_t gcThreshold = 4u << 20;  ///< heap growth between cycle collections
        bool threadSafe = true;              ///< take the lock on every entry point
        /// The language of a source whose name has no telling extension.
        /// `Auto` means the first language this build contains, in the order
        /// C#, TypeScript, JavaScript.
        Language language = Language::Auto;
        CompileOptions compile;              ///< how the C# front end compiles
        CheckOptions check;                  ///< how strictly TypeScript is checked
        JitOptions jit;                      ///< native code generation
    };

    struct Stats
    {
        std::uint64_t liveObjects = 0;
        std::uint64_t liveStrings = 0;
        std::uint64_t heapBytes = 0;      ///< bytes charged to the heap right now
        std::uint64_t reservedBytes = 0;  ///< memory the cell pool holds for reuse
        std::uint64_t totalAllocated = 0;
        std::uint64_t gcRuns = 0;
        std::uint64_t gcFreed = 0;          ///< cells reclaimed by cycle collection
        std::uint64_t strings = 0;          ///< interned strings alive right now
        std::uint64_t assemblies = 0;       ///< C# assemblies loaded
        std::uint64_t methodsCompiled = 0;  ///< methods the jit turned into native code
        std::uint64_t nativeCodeBytes = 0;
        std::uint64_t pendingJobs = 0;      ///< queued promise reactions
    };

    Context();
    explicit Context(const Options& options);
    ~Context();
    Context(Context&&) noexcept;
    Context& operator=(Context&&) noexcept;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    /// Hold the Context's lock across several operations so no other thread
    /// interleaves with them. Reentrant.
    class Lock
    {
    public:
        explicit Lock(Context& ctx);
        ~Lock();
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;

    private:
        Context* ctx_;
    };

    // --------------------------------------------------------- compile ---

    /// Compile `source` into a ready-to-run assembly. The language is taken
    /// from `language`, else from the extension of `name`, else from the
    /// options. Throws `Error(SyntaxError)` when the source does not parse
    /// and `CompileError` carrying every diagnostic when it does not compile.
    Assembly compile(std::string_view source, std::string_view name = "<assembly>",
                     Language language = Language::Auto);
    Assembly compileFile(const std::string& path, Language language = Language::Auto);
    /// Compile without throwing on diagnostics. An empty result means the
    /// program is good and `out`, if given, holds the assembly. A syntax
    /// error still throws, because there is no program to report on.
    std::vector<Diagnostic> diagnose(std::string_view source, std::string_view name = "<check>",
                                     Assembly* out = nullptr, Language language = Language::Auto);
    /// Read back an image written by `Assembly::serialize`. Throws
    /// `Error(BadImage)` if the bytes are not an image of this version.
    Assembly load(const void* data, std::size_t bytes);
    /// Compile every method of `assembly` the code generator handles, and
    /// return how many that was. `CompileOptions::native` does this for you;
    /// this is the form to call when the decision comes later.
    std::size_t compileNative(const Assembly& assembly);

    // ------------------------------------------------------------- run ---

    /// Compile and run in one step, returning what the program returned: the
    /// value of `Main`, or the completion value of the last statement.
    Value eval(std::string_view source, std::string_view name = "<eval>",
               Language language = Language::Auto);
    Value evalFile(const std::string& path, Language language = Language::Auto);
    /// Run an assembly's entry point.
    Value run(const Assembly& assembly, const std::vector<Value>& args = {});
    /// Call `Type.Method(args...)` in a C# assembly. The overload is picked
    /// from the arguments.
    Value call(const Assembly& assembly, std::string_view qualifiedMethod,
               const std::vector<Value>& args = {});
    /// Add ambient TypeScript declarations -- `declare function`,
    /// `interface`, `type` -- that every later TypeScript compile starts
    /// with. Declarations accumulate. Bindings made through `Class<T>` and
    /// `hostClass` declare themselves.
    void declare(std::string_view declarations);
    /// The JavaScript global object. Top-level declarations of every
    /// TypeScript and JavaScript program land here.
    Value global() const;

    // ------------------------------------------------------- factories ---

    Value str(std::string_view utf8);
    Value symbol(std::string_view description);
    /// A fresh JavaScript object or array.
    Value object();
    Value array(std::size_t reserve = 0);
    Value array(const std::vector<Value>& items);
    /// A JavaScript function that runs C++.
    Value function(std::string_view name, NativeFunction fn, int arity = 0);
    /// Wrap any C++ callable as a JavaScript function, converting arguments
    /// and results automatically. Defined in binding.hpp.
    template <class F>
    Value fn(std::string_view name, F&& f);
    /// `new TypeName(args...)` for a type a C# assembly declares.
    Value newObject(const Assembly& assembly, std::string_view typeName,
                    const std::vector<Value>& args = {});
    /// A fresh C# array of `length` elements, zero/null initialized. The
    /// element type is a tag for the primitives, or spelled the way C# spells
    /// it: "int", "string", "Geometry.Point", "int[]".
    Value newArray(Type elementType, std::int32_t length);
    Value newArray(std::string_view elementType, std::int32_t length);

    /// Convert any bound C++ type to a Value. Defined in binding.hpp.
    template <class T>
    Value value(T&& v);
    /// Convert a Value back to C++. Defined in binding.hpp.
    template <class T>
    T as(const Value& v);

    /// An exception object of the kind the error code names, in the language
    /// currently running -- or, outside any script, the one used last.
    Value makeException(ErrorCode kind, std::string_view message);
    [[noreturn]] void fail(ErrorCode kind, std::string_view message);

    Value parseJson(std::string_view text);

    // --------------------------------------------------------- bindings ---

    class HostClass;
    /// Declare a static class every language can call into. The signature of
    /// every member is derived from its C++ signature, so the C# compiler and
    /// the TypeScript checker type-check host calls like any other.
    ///
    ///     ctx.hostClass("Env").method("Now", []{ return now(); });
    ///     ctx.eval("return Env.Now();", "<eval>", Language::CSharp);
    ///     ctx.eval("Env.Now()", "<eval>", Language::JavaScript);
    HostClass hostClass(std::string_view name);

    /// Add one member to a host class. The binding layer derives the member's
    /// description from the C++ signature, so this is rarely called directly.
    /// `setter` is only read for a `Property`. Members are recorded once and
    /// installed into each engine the first time that engine needs them.
    void declareHostMember(void* hostType, const HostMember& member, NativeFunction fn,
                           NativeFunction setter = nullptr);

    /// Called with `owner` when the wrapper object is collected.
    using Finalizer = void (*)(void* owner);
    /// Wrap `instance` as an object of the host type `typeKey`. `finalizer`
    /// runs when the wrapper is collected and receives `owner`, which defaults
    /// to `instance`; pass a null finalizer for a non-owning reference. Every
    /// language reads the wrapper. Used by `Class<T>`; rarely called directly.
    Value native(const void* typeKey, void* instance, Finalizer finalizer, void* owner = nullptr);
    /// Install wrapper state on a JavaScript object that already exists --
    /// what a `new`-called native constructor does with its `this`.
    void attachNative(const Value& object, const void* typeKey, void* instance,
                      Finalizer finalizer, void* owner = nullptr);
    /// The JavaScript prototype and constructor `Class<T>` installed for a
    /// C++ type.
    Value classPrototype(const void* typeKey);
    Value classConstructor(const void* typeKey);
    /// Tie a host class to the C++ type it stands for, so a wrapper made with
    /// `native` reports that class as its C# runtime type. Called by `Class<T>`.
    void bindClassKey(void* hostType, const void* typeKey);
    bool isClassRegistered(const void* typeKey) const;

    // ------------------------------------------------------ housekeeping ---

    /// Run queued promise reactions until the queue drains. Returns the number
    /// of jobs executed. Call this after `eval` if a script uses promises.
    std::size_t runJobs();
    /// Free unreachable cycles. Everything acyclic is reclaimed immediately by
    /// reference counting and does not need this.
    void collectGarbage();
    Stats stats() const;
    /// The active code generator, e.g. "interpreter" or "arm64".
    const char* jitBackend() const noexcept;

    /// Called periodically while a program runs; return true to abort with
    /// `ErrorCode::Interrupted`. Use it for timeouts and cancellation. It runs in the middle of the program, so it may read
    /// `stats()` and otherwise leave the context alone: entering it for anything else also stops the program with
    /// `Interrupted`, whatever the handler returns.
    void setInterruptHandler(std::function<bool()> handler);
    /// Where `Console.Write`, `console.log` and friends go. Defaults to
    /// stdout/stderr.
    void setPrintHandler(std::function<void(std::string_view text, bool isError)> handler);
    /// Feeds `Console.ReadLine`. Return false at end of input.
    void setReadLineHandler(std::function<bool(std::string&)> handler);
    /// Reports a rejected promise that nothing ever handled.
    void setUnhandledRejectionHandler(std::function<void(const Value&)> handler);

    internal::ContextImpl* impl() const noexcept { return impl_.get(); }

private:
    std::unique_ptr<internal::ContextImpl> impl_;
};

/// Library version, e.g. "0.1.0".
const char* version() noexcept;

}  // namespace script
