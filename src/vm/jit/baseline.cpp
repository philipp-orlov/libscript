// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The baseline compiler (x86-64 / Linux). Frame state is the interpreter's at every instruction boundary; see jit.hpp.
//
// Fixed registers while compiled code runs (callee-saved, so calls into the runtime keep them):
//   rbx  the frame's registers (R)       r12  the VM       r13  the Frame
#include "script/internal/vm/jit.hpp"

#include <cstddef>
#include <exception>
#include <cstdlib>
#include <cstring>

#include "script/internal/stack_guard.hpp"
#include "script/internal/vm/bytecode.hpp"
#include "script/internal/vm/census.hpp"

#if defined(__x86_64__) && defined(__linux__)
#include "asm_x64.hpp"
#include "code_region.hpp"
#define SCRIPT_VM_BASELINE 1
#endif

namespace script {
namespace internal {
namespace vm {

#if defined(SCRIPT_VM_BASELINE)

namespace {

using namespace jit;

using Stub = std::uint64_t (*)(VM*, Frame*, void*);

Stub entryStub()
{
    static Stub stub = [] {
        X64 a;
        a.push(RBP);
        a.push(RBX);
        a.push(R12);
        a.push(R13);
        a.push(R14);
        a.push(R15);
        a.aluI(5, RSP, 8);
        a.mov(R12, RDI);
        a.mov(R13, RSI);
        a.mov(RBX, mem(RSI, std::int32_t(offsetof(Frame, R))));
        a.jmp(RDX);
        CodeSpan span = allocateCode(a.buf.size());
        if (!span)
            return Stub(nullptr);
        std::memcpy(span.rw, a.buf.data(), a.buf.size());
        return reinterpret_cast<Stub>(span.rx);
    }();
    return stub;
}

/// Where compiled code goes on after a handler or a jump the interpreter took: the top frame's pc.
extern "C" void* scriptVmJitTarget(VM* vm)
{
    Frame* fr = vm->frameBase + vm->frameCount - 1;
    return fr->code->tier1->addressFor(std::uint32_t(fr->pc - fr->code->code.data()));
}

/// A C++ exception cannot cross compiled frames (they carry no unwind information): it is parked here, the compiled
/// code returns to `enterTier1` as it would for any result, and that throws it again.
thread_local std::exception_ptr pendingException;
constexpr std::uint64_t kStepThrow = 0x32;

extern "C" std::uint64_t scriptVmJitStep(VM* vm, std::uint32_t index)
{
    try {
        return stepInstruction(*vm, index);
    } catch (...) {
        pendingException = std::current_exception();
        return kStepThrow;
    }
}

}  // namespace

bool tier1Available() noexcept
{
    return entryStub() != nullptr;
}

void compileTier1(VM& vm, CodeBlock* code)
{
    code->tier1Failed = true;
    if (code->flags & (CodeBlock::kGenerator | CodeBlock::kAsync | CodeBlock::kAsyncGen))
        return;
    if (code->code.size() > 20000 || !entryStub())
        return;
    const std::uint32_t n = std::uint32_t(code->code.size());

    X64 a;
    Label step, tail, dispatch, exitL;
    std::vector<std::uint32_t> offsets(n + 1, ~0u);

    for (std::uint32_t i = 0; i < n; ++i) {
        if (code->code[i].opcode() == Op::Cache)
            continue;
        offsets[i] = std::uint32_t(a.here());
        a.mov(RDI, R12);
        a.movImm(RSI, i);
        a.call(step);
        a.cmpI(RAX, std::int32_t(kStepFall));
        a.jcc(CNE, tail);
    }
    a.int3();  // the code never falls off its end

    a.bind(step);
    a.movImm(RAX, reinterpret_cast<std::uint64_t>(&scriptVmJitStep));
    a.jmp(RAX);
    a.bind(tail);
    a.cmpI(RAX, std::int32_t(kStepJump));
    a.jcc(CE, dispatch);
    a.bind(exitL);
    a.aluI(0, RSP, 8);
    a.pop(R15);
    a.pop(R14);
    a.pop(R13);
    a.pop(R12);
    a.pop(RBX);
    a.pop(RBP);
    a.ret();
    a.bind(dispatch);
    a.mov(RDI, R12);
    a.callAbs(reinterpret_cast<const void*>(&scriptVmJitTarget));
    a.jmp(RAX);

    CodeSpan span = allocateCode(a.buf.size());
    if (!span)
        return;
    std::memcpy(span.rw, a.buf.data(), a.buf.size());
    auto* t = new Tier1Code;
    t->rx = span.rx;
    t->bytes = span.bytes;
    t->labels = std::move(offsets);
    code->tier1 = t;
    code->tier1Failed = false;
    SCRIPT_VM_CENSUS(Tier1Compiled);
    (void)vm;
}

void releaseTier1(Tier1Code* code) noexcept
{
    if (!code)
        return;
    CodeSpan span;
    span.rx = code->rx;
    span.bytes = code->bytes;
    releaseCode(span);
    delete code;
}

JSValue enterTier1(VM& vm, Tier1Code* code, std::uint32_t index)
{
    Frame* fr = vm.frameBase + vm.frameCount - 1;
    const std::uint64_t bits = entryStub()(&vm, fr, code->addressFor(index));
    if (bits == kStepThrow) [[unlikely]] {
        std::exception_ptr e = std::move(pendingException);
        pendingException = nullptr;
        std::rethrow_exception(e);
    }
    return JSValue::raw(bits);
}

#else

bool tier1Available() noexcept { return false; }
void compileTier1(VM&, CodeBlock* code) { code->tier1Failed = true; }
void releaseTier1(Tier1Code*) noexcept {}
JSValue enterTier1(VM&, Tier1Code*, std::uint32_t) { return JSValue::exception(); }

#endif

CodeBlock::~CodeBlock()
{
    if (tier1)
        releaseTier1(tier1);
}

}  // namespace vm
}  // namespace internal
}  // namespace script
