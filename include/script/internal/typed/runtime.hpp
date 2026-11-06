// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The typed engine's runtime: the type universe, the loaded assemblies, the
// code generator and the VM's frame state. The heap itself belongs to the
// context; this borrows it.
#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "script/internal/context.hpp"
#include "script/internal/typed/jit.hpp"
#include "script/internal/typed/metadata.hpp"
#include "script/internal/typed/object.hpp"
#include "script/internal/typed/types.hpp"

namespace script {
namespace internal {
namespace typed {

struct Frame;

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
    template <class T, class... Args>
    T* newCellSized(std::size_t extra, Args&&... args)
    {
        return ctx.newCellSized<T>(extra, std::forward<Args>(args)...);
    }
    void maybeCollect() { ctx.maybeCollect(); }
    void recharge(GCObject* cell) noexcept { ctx.recharge(cell); }
    StringCell* intern(StringView text) { return ctx.intern(text); }
    StringCell* newString(StringView text) { return ctx.newString(text); }
    Value str(StringView text) { return ctx.str(text); }
    Value str(const char* asciiText) { return ctx.str(asciiText); }
    Value makeString(StringView text) { return ctx.str(text); }
    Box* newBox(Value v) { return ctx.newBox(std::move(v)); }
    void write(StringView text, bool isError) { ctx.write(text, isError); }

    // ---- types and assemblies ----
    std::unique_ptr<TypeTable> types;
    std::vector<std::shared_ptr<AssemblyImpl>> assemblies;
    /// The next `beforeCompile` that collects, by number of units loaded.
    std::size_t collectAtUnits = kCollectEveryUnits;
    static constexpr std::size_t kCollectEveryUnits = 256;
    /// Set while initializers run: that loop walks `assemblies` by index.
    bool initializing = false;
    /// Static classes the host declared, by name.
    std::unordered_map<String, TypeInfo*> hostTypes;
    std::vector<std::unique_ptr<TypeInfo>> hostOwned;
    std::vector<std::unique_ptr<MethodInfo>> hostMethods;
    std::unordered_map<const void*, TypeInfo*> nativeClasses;
    /// Interface dispatch: (concrete type, interface method) -> implementation.
    std::unordered_map<std::uint64_t, MethodInfo*> ifaceCache;

    /// Create the host classes and members the binding layer has declared
    /// since the last call. Runs before every compile and before a host
    /// wrapper is made, so bindings declared in any order are all visible.
    void syncHostClasses();

    /// Run the static field initializers of every assembly that has not had
    /// them run yet, in load order. A program spread over several compilation
    /// units reaches the statics of a unit it never entered, so this cannot
    /// wait for the assembly that declares them to be the one being called.
    void ensureInitialized();

    /// Call before compiling. A host that evaluates top-level code in a loop
    /// allocates next to nothing on the heap, so no budget would ever start the
    /// collection that unloads what those evaluations left behind.
    void beforeCompile();
    /// Drop the units that nothing refers to any more; see `declaresNames`.
    void unloadUnusedUnits() noexcept;

    // ---- vm ----
    std::vector<Frame*> frames;
    /// One reusable frame per call depth; a call at depth n reuses the buffers
    /// the previous call at that depth left behind.
    std::vector<std::unique_ptr<Frame>> framePool;

    Frame& acquireFrame(std::size_t depth);

    std::unique_ptr<JitEngine> jit;
    /// Where generated code records the exception it wants raised. It cannot
    /// throw itself -- a C++ exception crossing a generated frame would need
    /// unwind information the back ends do not emit -- so the trampoline in
    /// `invoke` checks this on the way out. Zero means nothing happened.
    std::uint32_t nativeError = 0;
    /// The exception behind `nativeError`, when one was made: the trampoline raises it unchanged.
    std::exception_ptr nativePending;
    /// Exceptions waiting for the `finally` they are passing through to end.
    std::vector<std::exception_ptr> finallyPending;
    /// Nonzero when a back edge has to ask `scriptJitPoll`. Without an interrupt handler a loop only reads this.
    std::uint32_t pollFlag = 0;
    /// The stack address below which native code raises `StackOverflow` instead of making a frame.
    std::uintptr_t nativeStackLimit = 0;
    /// The innermost native frame that has told anyone where it is, for a stack trace; and where in it.
    void* nativeTopFrame = nullptr;
    const void* nativeTopPc = nullptr;

    /// "   at Type.Method(file:line)" for the frames that are live right now.
    std::string captureStack() const override;
    void releaseRoots() noexcept override;
    void afterCollect() noexcept override;

    // ---- allocation helpers ----
    CSInstance* newInstance(const TypeInfo* type);
    CSArray* newArray(const TypeInfo* elementType, std::uint32_t length);
    /// An exception object of the type a code maps to, with its message set.
    Value makeException(ErrorCode code, StringView message);
};

/// The typed runtime a value belongs to, or null for a primitive or a value
/// of the other engine.
inline Runtime* runtimeOf(const Value& v) noexcept
{
    GCObject* p = detail::Access::ptr(v);
    return p && p->rt ? p->rt->typedIfAny() : nullptr;
}

}  // namespace typed
}  // namespace internal
}  // namespace script
