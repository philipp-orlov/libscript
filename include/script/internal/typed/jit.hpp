// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Native code generation.
//
// The engine interprets by default and compiles a method to machine code once
// it gets hot. The pieces are:
//
//   * `JitEngine` -- the tiering policy: counts calls, hands a method over
//     exactly once, and never retries one the back end refused;
//   * `CodeCache` -- page-granular, write-then-protect executable memory;
//   * `JitBackend` -- what a code generator implements, one per architecture;
//   * `planBody` -- the shared analysis both back ends run first: it decides
//     whether a method is in the supported subset and computes the evaluation
//     stack depth at every instruction.
//
// -------------------------------------------------------------- the ABI ---
//
// A compiled method is entered through `NativeCode::entry`:
//
//     void entry(Runtime* rt, const MethodInfo* method, Value* frame,
//                std::uint32_t argc, Value* result);
//
//   * `frame` points at `method->frameSize()` constructed `Value` slots. The
//     receiver, if the method is an instance method, is slot 0; the parameters
//     follow in declaration order; the locals follow those, already zeroed to
//     the defaults of `MethodBody::localTypes`. The caller owns the storage.
//     Because a slot's static type never changes, generated code writes only
//     the payload half of a slot and leaves the tag the caller set.
//   * `argc` is what the caller pushed. The compiler already checked the
//     arity, so this is an assertion, not a dispatch input.
//   * `result` points at a default-constructed `Value` the callee overwrites.
//     Everything is passed and returned through plain pointers and integers,
//     which keeps generated code out of the C++ ABI's rules for returning a
//     class with a non-trivial destructor.
//   * Generated code never throws. To raise, it stores an `ErrorCode` in
//     `Runtime::nativeError` and returns; the trampoline in `invoke`
//     checks that field and raises the exception from C++.
//   * `rt` is the owning context. Every runtime helper -- allocation, casts,
//     string concatenation, virtual dispatch -- is an ordinary function in
//     vm.hpp taking that pointer, so a back end can widen its subset by
//     calling the same code the interpreter does.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "script/internal/common.hpp"
#include "script/internal/typed/metadata.hpp"

namespace script {
namespace internal {
namespace typed {

struct Runtime;

/// One compiled method. Owned by the back end that produced it; the method
/// points at it, the code cache accounts for it.
struct NativeCode
{
    using Entry = void (*)(Runtime* rt, const MethodInfo* method, Value* frame,
                           std::uint32_t argc, Value* result);

    Entry entry = nullptr;
    const MethodInfo* method = nullptr;
    void* codeStart = nullptr;
    std::size_t codeBytes = 0;
    /// Which back end produced this, for `Stats` and for the dump.
    const char* backend = "";

    // ---- the optimizing tier; `entry` stays null and these say how to get in ----
    void* fastEntry = nullptr;
    /// A loop head in the interpreter's frame, and the code that picks up from there.
    struct Osr
    {
        std::uint32_t pc;
        void* entry;
        std::vector<std::uint32_t> ownedSlots;
        std::vector<std::uint32_t> ownedPointers;
    };
    std::vector<Osr> osr;
    /// Return addresses inside this code, sorted, and which source position each stands for.
    struct Site
    {
        std::uint32_t offset;
        std::int32_t site;
    };
    std::vector<Site> sites;
    struct SourceSite
    {
        const void* method;
        std::uint32_t pc;
        std::int32_t parent;
    };
    std::vector<SourceSite> sourceSites;
};

/// Where a `Value`'s tag and payload sit, discovered rather than assumed. A
/// back end that finds a layout it was not built for reports itself
/// unavailable instead of generating nonsense.
ValueLayout valueLayout() noexcept;

/// Called from generated code at every loop back edge. Returns non-zero when
/// execution must stop, having recorded why in `Runtime::nativeError`.
/// Never throws: a C++ exception crossing a generated frame would need unwind
/// information the back ends do not emit.
extern "C" int scriptJitPoll(Runtime* rt) noexcept;

/// Pages that can hold machine code.
///
/// Mapping is page-granular but allocation is not: methods are packed into a
/// block until it fills, because a small method is a few hundred bytes and a
/// page each would waste most of the cache. A block is flipped back to
/// writable while it is being added to and to read+execute afterwards, which
/// is what hardened kernels (and Apple silicon's W^X) require; a context is
/// single-threaded, so nothing is executing out of the block at that moment.
/// A block is unmapped once the last method in it is released.
class CodeCache
{
public:
    CodeCache() = default;
    ~CodeCache();
    CodeCache(const CodeCache&) = delete;
    CodeCache& operator=(const CodeCache&) = delete;

    /// Reserve `bytes` of writable memory. Returns null when the cache is at
    /// its limit, which the caller must treat as "stay interpreted".
    void* reserve(std::size_t bytes);
    /// Flip the block holding `block` to executable and flush the instruction
    /// cache.
    bool commit(void* block, std::size_t bytes);
    void release(void* block, std::size_t bytes) noexcept;

    void setLimit(std::size_t bytes) noexcept { limit_ = bytes; }
    /// Bytes mapped, which is what the process pays for.
    std::size_t used() const noexcept { return used_; }

private:
    struct Block
    {
        void* base;
        std::size_t bytes;
        std::size_t filled;
        std::size_t live;
    };
    Block* blockFor(void* address) noexcept;

    std::vector<Block> blocks_;
    std::size_t used_ = 0;
    std::size_t limit_ = 0;
};

struct IrFunction;

/// What a back end needs to know about the engine it generates code for: where its error flag and friends are, relative to
/// the pointer pinned in a register for a run of native code, and how to raise and poll.
struct NativeConfig
{
    std::int32_t nativeError = 0;
    std::int32_t pollFlag = 0;
    std::int32_t stackLimit = 0;
    std::int32_t topFrame = 0;
    std::int32_t savedRegs = 0;
    const void* raise = nullptr;  ///< void (*)(void* engine, std::uint32_t code, std::int32_t index)
    const void* poll = nullptr;   ///< int (*)(void* engine)
};

/// A machine code generator. One instance per context, created from the
/// options; `available()` false means every request falls back to the
/// interpreter.
class JitBackend
{
public:
    virtual ~JitBackend() = default;

    /// "arm64", "x86-64", "interpreter".
    virtual const char* name() const noexcept = 0;
    /// False when this build has no generator for the host, or the host lacks
    /// something the generator needs.
    virtual bool available() const noexcept = 0;
    /// Whether the machine converts a double to an int32 the way JavaScript does in one instruction.
    virtual bool hasJsToInt32() const noexcept { return false; }
    /// Whether this method is within the subset the back end handles. A back
    /// end is free to refuse anything; `reason` says why, for the dump.
    virtual bool supports(const MethodInfo& method, std::string* reason) const = 0;
    /// Generate code. Returns null to mean "keep interpreting"; that is not an
    /// error and must not throw. A back end that compiled other methods along
    /// with this one, because it calls them, adds them to `others`.
    virtual NativeCode* compile(Runtime& rt, MethodInfo& method, std::vector<NativeCode*>& others) = 0;
    /// Generate code for one function already in IR, on behalf of any engine. Returns null to mean "keep interpreting".
    virtual NativeCode* compileFunction(IrFunction& fn, const NativeConfig& config)
    {
        (void)fn;
        (void)config;
        return nullptr;
    }
    virtual void release(NativeCode* code) noexcept = 0;
    /// What `compile` refused the last method for, for the dump.
    virtual const std::string& lastRefusal() const noexcept
    {
        static const std::string kNone;
        return kNone;
    }
    /// Disassembly or a hex dump, for `JitOptions::dumpCode`.
    virtual std::string dump(const NativeCode* code) const;

    CodeCache& cache() noexcept { return cache_; }

protected:
    CodeCache cache_;
};

/// The back end for the architecture this build targets, or a stub that is
/// never available.
std::unique_ptr<JitBackend> createArm64Backend();
std::unique_ptr<JitBackend> createX64Backend();
std::unique_ptr<JitBackend> createHostBackend();

/// The tiering policy: what the VM consults on every call.
///
/// Hotness is counted by the interpreter -- one per call -- and crossing the
/// threshold hands the method to the back end exactly once. A method that
/// fails to compile is marked so it is not retried, which keeps an
/// unsupported construct from costing a compile attempt per call.
class JitEngine
{
public:
    JitEngine(std::unique_ptr<JitBackend> backend, const JitOptions& options);
    ~JitEngine();

    bool enabled() const noexcept;
    /// True when a real back end exists, whatever the mode is set to.
    bool hasBackend() const noexcept;
    const char* backendName() const noexcept;

    /// Called by the interpreter when `method` got hot. Returns the entry
    /// point to jump to, or null to keep interpreting.
    NativeCode* onHot(Runtime& rt, MethodInfo& method);
    /// Compile everything in an assembly that the back end supports. Used by
    /// `CompileOptions::native` and `JitOptions::Mode::Eager`.
    std::size_t compileAll(Runtime& rt, AssemblyImpl& assembly);
    /// Drop the code for methods of an assembly that is going away.
    void forget(AssemblyImpl& assembly) noexcept;

    std::uint32_t threshold() const noexcept { return options_.threshold; }
    /// The code holding an address in the code cache, or null.
    const NativeCode* findCode(const void* address) const noexcept;
    std::uint64_t compiledCount() const noexcept { return compiled_; }
    std::uint64_t codeBytes() const noexcept;

private:
    std::unique_ptr<JitBackend> backend_;
    JitOptions options_;
    std::vector<NativeCode*> owned_;
    std::uint64_t compiled_ = 0;
};

}  // namespace typed
}  // namespace internal
}  // namespace script
