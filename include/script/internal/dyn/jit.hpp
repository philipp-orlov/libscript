// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Native code for the dynamic languages.
//
// The same IR, register allocator and back ends as the typed engine, with a different lowering in front: there is no static
// type to go on, so an operation is an inline test for what it usually sees, with the general case in a helper on a cold path.
// Two things make that fast rather than merely native. A local that only ever holds numbers lives unboxed in a floating point
// register, found by a pass over the function. And a property access reads what the interpreter's inline cache learned, so it
// is a compare against the shape that site has seen and a load.
//
// The calling convention, in the integer argument registers: the function, `this` as a tag and a payload, a pointer to the
// arguments as `Value`s, how many. A `Value` comes back as a payload and a tag. The caller's arguments are borrowed.
#pragma once

#include <cstdint>
#include <string>

#include "script/internal/typed/jit.hpp"
#include "script/internal/typed/jit_ir.hpp"
#include "script/internal/dyn/bytecode.hpp"

namespace script {
namespace internal {
namespace dyn {

struct Runtime;
struct Frame;
struct DynFunction;

/// A `Value` in the two registers it travels in.
struct NativePair
{
    std::uint64_t payload;
    std::uint64_t tag;
};

/// Make `code` native if it can be. Remembers a refusal, so this is cheap to ask again.
bool compileNative(Runtime& rt, const FunctionCode& code);
/// Lower to IR. False, with `why` set, for a function native code does not handle.
bool lowerFunction(Runtime& rt, const FunctionCode& code, typed::IrFunction& out, std::string* why);

/// Call a compiled function from C++.
Value enterNative(Runtime& rt, DynFunction* fn, const FunctionCode& code, const Value& thisValue, const Value* args,
                  std::size_t argc);
/// At a loop head with an empty stack: carry on in native code, if there is an entry for here.
bool enterNativeAtLoop(Runtime& rt, Frame& frame, std::uint32_t pc, Value& result);
/// Carry on with a call in the interpreter from `pc`, the frame filled in by `fill`: where native code that assumed too
/// much hands over.
Value resumeInterpreter(Runtime& rt, DynFunction* fn, const FunctionCode& code, const Value& thisValue, std::uint32_t pc,
                        void (*fill)(Frame&, void*), void* state);
/// "    at ..." lines for a run of native frames.
std::string describeNativeFrames(const Runtime& rt, void* topFrame, const void* topPc, int& shown);

}  // namespace dyn
}  // namespace internal
}  // namespace script
