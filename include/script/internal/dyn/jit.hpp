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

#include "script/config.hpp"

#include <cstdint>
#include <string>
#if SCRIPT_ENABLE_JIT_THREAD
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#endif

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
/// Stop entering a function's native code, which is freed once no frame is in it.
void retireNative(Runtime& rt, const FunctionCode& code) noexcept;
void sweepRetiredNative(Runtime& rt) noexcept;
bool compileNative(Runtime& rt, const FunctionCode& code);
/// Lower to IR. False, with `why` set, for a function native code does not handle.
bool lowerFunction(Runtime& rt, const FunctionCode& code, typed::IrFunction& out, std::string* why);

#if SCRIPT_ENABLE_JIT_THREAD
/// A function handed to the worker: its IR going in, its machine code coming out. Only `done` is shared while it is out;
/// the rest is the worker's until `done`, and the runtime's after.
struct JitJob
{
    typed::IrFunction fn;
    typed::NativeConfig config;
    typed::BuiltCode built;
    bool ok = false;
    std::atomic<bool> done{false};
};

/// A runtime's one other thread, started by its first large compile: the back end runs there over a function's IR while
/// the interpreter goes on with the function. What it finishes is put in place by the runtime's own thread, the next time
/// the function is called or goes round a loop.
class JitWorker
{
public:
    explicit JitWorker(const typed::JitBackend& backend);
    ~JitWorker();
    JitWorker(const JitWorker&) = delete;
    JitWorker& operator=(const JitWorker&) = delete;
    void submit(std::shared_ptr<JitJob> job);

private:
    void run();

    const typed::JitBackend& backend_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::shared_ptr<JitJob>> queue_;
    bool stop_ = false;
    std::thread thread_;
};
#endif

/// Call a compiled function from C++.
Value enterNative(Runtime& rt, DynFunction* fn, const FunctionCode& code, const Value& thisValue, const Value* args,
                  std::size_t argc);
/// Whether the loop whose head is at `head` can suspend the coroutine it is in on its way round: a `yield` or an
/// `await` in it.
bool loopSuspends(const FunctionCode& code, std::uint32_t head);
/// At a loop head with an empty stack: carry on in native code, if there is an entry for here. 0: there is none; 1: the
/// call is done and `result` is what it returned; 2: a coroutine's code handed back, and the interpreter goes on in the
/// frame from `frame.pc`, its stack filled in.
int enterNativeAtLoop(Runtime& rt, Frame& frame, std::uint32_t pc, Value& result);
/// Carry on with a call in the interpreter from `pc`, the frame filled in by `fill`: where native code that assumed too
/// much hands over.
Value resumeInterpreter(Runtime& rt, DynFunction* fn, const FunctionCode& code, const Value& thisValue, std::uint32_t pc,
                        void (*fill)(Frame&, void*), void* state);
/// "    at ..." lines for a run of native frames.
std::string describeNativeFrames(const Runtime& rt, void* topFrame, const void* topPc, int& shown);

}  // namespace dyn
}  // namespace internal
}  // namespace script
