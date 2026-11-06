// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The interpreter loop and the runtime helpers the compiled code calls.
//
// Every helper here takes the context as its first argument and nothing else
// from the interpreter's state, which is what lets a native back end call the
// same functions the interpreter does.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "script/internal/typed/runtime.hpp"
#include "script/internal/typed/metadata.hpp"
#include "script/internal/typed/object.hpp"

namespace script {
namespace internal {
namespace typed {

/// One activation. Frames live on the C++ stack; the context keeps raw
/// pointers to them so a stack trace can be captured.
///
/// The buffers are recycled: frames nest strictly, so the context keeps one
/// spare per depth and a call reuses the capacity the previous call at that
/// depth left behind. That takes two allocations per C# call down to none in
/// steady state.
struct Frame
{
    const MethodInfo* method = nullptr;
    std::uint32_t pc = 0;
    /// Where the instruction being executed starts. `pc` has already moved on
    /// by the time one of them faults, and the handler search needs the
    /// instruction's own offset.
    std::uint32_t opPc = 0;
    /// `this` (for an instance method), then the parameters, then the locals.
    Vec<Value> slots;
    Vec<Value> stack;
    /// Return addresses for `finally` blocks a `Leave` is running through.
    Vec<std::uint32_t> pending;
    Value exception;
    std::uint32_t line = 0;
    /// True while an exception is passing through a `finally`, which is what
    /// tells `EndFinally` to carry on unwinding instead of resuming.
    bool unwinding = false;

    /// This frame stands for a run of native frames, which `captureStack` finds by walking from `nativeInnerFrame`. It
    /// remembers what the runtime's top was when the run began.
    bool nativeRun = false;
    /// On-stack replacement took this frame's handlers with it.
    bool handledNatively = false;
    void* savedTopFrame = nullptr;
    const void* savedTopPc = nullptr;

    void begin(const MethodInfo* target);
    /// Release everything the call referenced, keeping the buffers themselves.
    void end() noexcept;
};

// ------------------------------------------------------- error raising ---

[[noreturn]] void throwError(Runtime& rt, ErrorCode code, const std::string& message);
/// Raise an exception object the program created.
[[noreturn]] void throwValue(Runtime& rt, const Value& exception);
/// Turn any C++ exception coming out of native code into a `script::Exception`.
[[noreturn]] void rethrowAsScript(Runtime& rt);

// ------------------------------------------------------ abstract ops ---

String toStringValue(Runtime& rt, const Value& v);
/// `a + b` where at least one side is a string.
Value concatStrings(Runtime& rt, const Value* parts, std::size_t count);
/// `Object.Equals`: value equality for primitives and strings, identity for
/// everything else.
bool objectEquals(const Value& a, const Value& b) noexcept;

/// The runtime type of a value, for casts and for `GetType`.
const TypeInfo* typeOf(Runtime& rt, const Value& v) noexcept;
bool isInstanceOf(Runtime& rt, const Value& v, const TypeInfo* type) noexcept;
/// `(T)value`, throwing `InvalidCastException` when it does not hold.
Value castTo(Runtime& rt, const Value& v, const TypeInfo* type);
/// Copy a struct, which is what assignment and argument passing do to one.
Value cloneValue(Runtime& rt, const Value& v);
/// The zero of a type: 0, false, '\0' or null.
Value defaultValue(const TypeInfo* type) noexcept;

// ------------------------------------------------------------- objects ---

Value newInstanceOf(Runtime& rt, const TypeInfo* type);
Value newArrayOf(Runtime& rt, const TypeInfo* elementType, std::int32_t length);
Value getElement(Runtime& rt, const Value& array, std::int32_t index);
void setElement(Runtime& rt, const Value& array, std::int32_t index, const Value& v);

// --------------------------------------------------------------- calls ---

/// Resolve a virtual call against the receiver's runtime type.
const MethodInfo* resolveVirtual(Runtime& rt, const Value& receiver, const MethodInfo* declared);
/// Resolve an interface call the same way, with a cache: the pair
/// (concrete type, interface method) never changes.
const MethodInfo* resolveInterface(Runtime& rt, const Value& receiver,
                                   const MethodInfo* declared);

/// Call a method with `args` already in call order. The receiver, if any, is
/// `args[0]`. This is the single entry point every caller uses: the
/// interpreter, the embedding API and any back end.
Value invoke(Runtime& rt, const MethodInfo* method, Value* args, std::uint32_t argc);
Value invokeDelegate(Runtime& rt, const Value& delegateValue, Value* args,
                     std::uint32_t argc);
/// Construct: allocate, run the constructor, return the instance.
Value construct(Runtime& rt, const MethodInfo* ctor, Value* args, std::uint32_t argc);

/// Run one method body. `frame` holds `method->frameSize()` slots, already
/// filled with the receiver and the arguments.
Value execute(Runtime& rt, const MethodInfo& method, Value* frame, std::uint32_t argc);

}  // namespace typed
}  // namespace internal
}  // namespace script
