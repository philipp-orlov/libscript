// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// How the interpreter gets into native code and back.
#pragma once

#include <cstdint>
#include <string>

#include "script/internal/common.hpp"

namespace script {
namespace internal {
namespace typed {

struct Runtime;
struct Frame;
struct MethodInfo;
struct NativeCode;

/// Call a method compiled by the optimizing tier, from C++.
Value invokeNativeCode(Runtime& rt, const MethodInfo* method, NativeCode* code, Value* args, std::uint32_t argc);
Value nativeResultValue(const MethodInfo* method, const std::uint64_t* intResults, double fpResult);
/// At a loop head with an empty stack: carry on in native code, if there is an entry for here. True when the method has
/// run to the end there, leaving its return value in `result`.
bool tryOnStackReplacement(Runtime& rt, Frame& frame, std::uint32_t pc, Value& result);
/// "   at ..." lines for a run of native frames.
std::string describeNativeFrames(const Runtime& rt, void* topFrame, const void* topPc);

}  // namespace typed
}  // namespace internal
}  // namespace script
