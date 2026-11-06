// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/typed/jit.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "script/internal/typed/bytecode.hpp"
#include "script/internal/typed/runtime.hpp"

#include "sys/vm.hpp"

namespace script {
namespace internal {
namespace typed {

// ----------------------------------------------------------- code cache ---

CodeCache::~CodeCache()
{
    for (Block& block : blocks_)
        sys::vmUnmap(block.base, block.bytes);
}

CodeCache::Block* CodeCache::blockFor(void* address) noexcept
{
    for (Block& block : blocks_) {
        auto* base = static_cast<std::uint8_t*>(block.base);
        if (address >= base && address < base + block.bytes)
            return &block;
    }
    return nullptr;
}

void* CodeCache::reserve(std::size_t bytes)
{
    if (bytes == 0)
        return nullptr;
    const std::size_t aligned = (bytes + 15u) & ~std::size_t(15u);

    // A small method is a few hundred bytes; a page each would waste most of
    // the cache, so blocks are filled before another is mapped.
    for (Block& block : blocks_) {
        if (block.bytes - block.filled < aligned)
            continue;
        if (!sys::vmProtectWritable(block.base, block.bytes))
            continue;
        void* slot = static_cast<std::uint8_t*>(block.base) + block.filled;
        block.filled += aligned;
        block.live += aligned;
        return slot;
    }

    const std::size_t size = sys::roundUpToPage(aligned > 64 * 1024 ? aligned : 64 * 1024);
    if (limit_ && used_ + size > limit_)
        return nullptr;
    void* const base = sys::tryVmMap(size).base;
    if (!base)
        return nullptr;
    blocks_.push_back({base, size, aligned, aligned});
    used_ += size;
    return base;
}

bool CodeCache::commit(void* block, std::size_t bytes)
{
    (void)bytes;
    Block* owner = blockFor(block);
    if (!owner)
        return false;
    return sys::vmProtectExecutable(owner->base, owner->bytes);
}

void CodeCache::release(void* block, std::size_t bytes) noexcept
{
    Block* owner = blockFor(block);
    if (!owner)
        return;
    const std::size_t aligned = (bytes + 15u) & ~std::size_t(15u);
    owner->live -= owner->live < aligned ? owner->live : aligned;
    if (owner->live != 0)
        return;

    // The last method in the block is gone, so the mapping can go with it.
    const std::size_t index = std::size_t(owner - blocks_.data());
    sys::vmUnmap(owner->base, owner->bytes);
    used_ -= owner->bytes;
    blocks_.erase(blocks_.begin() + std::ptrdiff_t(index));
}

// --------------------------------------------------------- value layout ---

ValueLayout valueLayout() noexcept
{
    // Asking where the compiler put the members beats assuming, and costs one
    // construction when a back end is created.
    return detail::Access::layout();
}

// ----------------------------------------------------------------- plan ---

namespace {

/// The tag the result of an instruction carries. Only the instructions the
/// back ends accept appear here; everything else was already refused.
Type pushedType(Op op, const MethodInfo& method, const std::uint8_t* operands,
                const std::vector<Type>& before) noexcept
{
    switch (op) {
        case Op::LdcI8:
        case Op::ConvI8:
        case Op::ConvU8:
        case Op::AddI8:
        case Op::SubI8:
        case Op::MulI8:
        case Op::DivI8:
        case Op::DivU8:
        case Op::RemI8:
        case Op::RemU8:
        case Op::NegI8:
        case Op::AndI8:
        case Op::OrI8:
        case Op::XorI8:
        case Op::NotI8:
        case Op::ShlI8:
        case Op::ShrI8:
        case Op::ShrU8: return Type::Long;
        case Op::LdcR4:
        case Op::ConvR4:
        case Op::AddR4:
        case Op::SubR4:
        case Op::MulR4:
        case Op::DivR4:
        case Op::NegR4: return Type::Float;
        case Op::LdcR8:
        case Op::ConvR8:
        case Op::ConvR8Un:
        case Op::AddR8:
        case Op::SubR8:
        case Op::MulR8:
        case Op::DivR8:
        case Op::NegR8: return Type::Double;
        case Op::ConvChar: return Type::Char;
        case Op::LdTrue:
        case Op::LdFalse:
        case Op::ConvBool:
        case Op::LogicalNot: return Type::Bool;
        case Op::LdLoc: {
            const std::uint32_t slot = readU32(operands);
            const TypeInfo* type = slot < method.body.localTypes.size()
                                       ? method.body.localTypes[slot]
                                       : nullptr;
            return type ? runtimeTag(type->kind) : Type::Int;
        }
        case Op::Dup: return before.empty() ? Type::Int : before.back();
        default: return Type::Int;
    }
}

/// Whether an instruction leaves a value behind. With the stack delta, this
/// gives how many entries it consumed.
bool producesValue(Op op) noexcept
{
    switch (op) {
        case Op::Nop:
        case Op::Pop:
        case Op::StLoc:
        case Op::Br:
        case Op::BrTrue:
        case Op::BrFalse:
        case Op::Ret:
        case Op::RetVoid: return false;
        default: return true;
    }
}

/// The instructions the code generators handle. Everything here works on
/// primitives: no allocation, no reference counting, no call into the runtime,
/// and nothing that can throw except integer division by zero.
bool isSupportedOp(Op op) noexcept
{
    switch (op) {
        case Op::Nop:
        case Op::LdcI4:
        case Op::LdcI8:
        case Op::LdcR4:
        case Op::LdcR8:
        case Op::LdTrue:
        case Op::LdFalse:
        case Op::Pop:
        case Op::Dup:
        case Op::LdLoc:
        case Op::StLoc:
        case Op::Br:
        case Op::BrTrue:
        case Op::BrFalse:
        case Op::Ret:
        case Op::RetVoid:
        case Op::ConvI1:
        case Op::ConvI2:
        case Op::ConvI4:
        case Op::ConvI8:
        case Op::ConvU1:
        case Op::ConvU2:
        case Op::ConvU4:
        case Op::ConvU8:
        case Op::ConvR4:
        case Op::ConvR8:
        case Op::ConvR8Un:
        case Op::ConvChar:
        case Op::ConvBool:
        case Op::AddI4:
        case Op::SubI4:
        case Op::MulI4:
        case Op::DivI4:
        case Op::DivU4:
        case Op::RemI4:
        case Op::RemU4:
        case Op::NegI4:
        case Op::AddI8:
        case Op::SubI8:
        case Op::MulI8:
        case Op::DivI8:
        case Op::DivU8:
        case Op::RemI8:
        case Op::RemU8:
        case Op::NegI8:
        case Op::AddR4:
        case Op::SubR4:
        case Op::MulR4:
        case Op::DivR4:
        case Op::NegR4:
        case Op::AddR8:
        case Op::SubR8:
        case Op::MulR8:
        case Op::DivR8:
        case Op::NegR8:
        case Op::AndI4:
        case Op::OrI4:
        case Op::XorI4:
        case Op::NotI4:
        case Op::ShlI4:
        case Op::ShrI4:
        case Op::ShrU4:
        case Op::AndI8:
        case Op::OrI8:
        case Op::XorI8:
        case Op::NotI8:
        case Op::ShlI8:
        case Op::ShrI8:
        case Op::ShrU8:
        case Op::CeqI4:
        case Op::CneI4:
        case Op::CltI4:
        case Op::CltU4:
        case Op::CleI4:
        case Op::CleU4:
        case Op::CgtI4:
        case Op::CgtU4:
        case Op::CgeI4:
        case Op::CgeU4:
        case Op::CeqI8:
        case Op::CneI8:
        case Op::CltI8:
        case Op::CltU8:
        case Op::CleI8:
        case Op::CleU8:
        case Op::CgtI8:
        case Op::CgtU8:
        case Op::CgeI8:
        case Op::CgeU8:
        case Op::CeqR8:
        case Op::CneR8:
        case Op::CltR8:
        case Op::CleR8:
        case Op::CgtR8:
        case Op::CgeR8:
        case Op::LogicalNot: return true;
        default: return false;
    }
}

}  // namespace

bool planBody(const MethodInfo& method, BodyPlan& plan, std::string* reason)
{
    auto refuse = [reason](const std::string& why) {
        if (reason)
            *reason = why;
        return false;
    };

    const MethodBody& body = method.body;
    if (body.code.empty())
        return refuse("the method has no body");
    if (!body.handlers.empty())
        return refuse("the method has exception handlers");
    if (method.returnType && method.returnType->kind != TypeKind::Void &&
        !isValueKind(method.returnType->kind))
        return refuse("the method returns a reference type");
    for (std::size_t i = 0; i < body.localTypes.size(); ++i) {
        if (i < body.localBoxed.size() && body.localBoxed[i])
            return refuse("a local is captured or passed by reference");
        const TypeInfo* type = body.localTypes[i];
        if (!type || !isValueKind(type->kind))
            return refuse("a local is a reference type");
    }

    // Walk every reachable instruction, carrying the stack depth and the type
    // of each entry with it. A second arrival at a different depth would mean
    // the addresses generated code bakes in are wrong, so that is a refusal
    // rather than a guess.
    const std::size_t size = body.code.size();
    plan.depth.assign(size, -1);
    plan.stack.assign(size, {});
    plan.maxDepth = 0;

    std::vector<std::uint32_t> work;
    work.push_back(0);
    plan.depth[0] = 0;

    while (!work.empty()) {
        std::uint32_t pc = work.back();
        work.pop_back();
        std::vector<Type> types = plan.stack[pc];
        while (pc < size) {
            const Op op = Op(body.code[pc]);
            if (!isSupportedOp(op))
                return refuse(std::string(opName(op)) + " is not compiled yet");
            const int arity = opArity(op);
            const std::uint32_t next = pc + 1 + std::uint32_t(arity) * 4;
            if (next > size)
                return refuse("the body is truncated");

            const std::int32_t depth = plan.depth[pc];
            const std::int32_t after = depth + opStackDelta(op);
            if (after < 0 || std::size_t(depth) != types.size())
                return refuse("the evaluation stack underflows");
            plan.stack[pc] = types;
            plan.maxDepth = std::uint32_t(std::max(depth, after)) > plan.maxDepth
                                ? std::uint32_t(std::max(depth, after))
                                : plan.maxDepth;

            const Type pushed = pushedType(op, method, &body.code[pc] + 1, types);
            const int pushes = producesValue(op) ? 1 : 0;
            const int pops = pushes - opStackDelta(op);
            if (std::size_t(pops) > types.size())
                return refuse("the evaluation stack underflows");
            for (int i = 0; i < pops; ++i)
                types.pop_back();
            for (int i = 0; i < pushes; ++i)
                types.push_back(pushed);

            auto reach = [&](std::uint32_t target, std::int32_t at) {
                if (target >= size)
                    return false;
                if (plan.depth[target] == -1) {
                    plan.depth[target] = at;
                    plan.stack[target] = types;
                    work.push_back(target);
                    return true;
                }
                return plan.depth[target] == at;
            };

            if (op == Op::Ret || op == Op::RetVoid)
                break;
            if (op == Op::Br) {
                if (!reach(readU32(&body.code[pc + 1]), after))
                    return refuse("the evaluation stack is inconsistent at a branch target");
                break;
            }
            if ((op == Op::BrTrue || op == Op::BrFalse) &&
                !reach(readU32(&body.code[pc + 1]), after))
                return refuse("the evaluation stack is inconsistent at a branch target");
            if (next == size)
                break;
            if (plan.depth[next] != -1) {
                if (plan.depth[next] != after)
                    return refuse("the evaluation stack is inconsistent at a join");
                break;
            }
            plan.depth[next] = after;
            plan.stack[next] = types;
            pc = next;
        }
    }
    return true;
}

extern "C" int scriptJitPoll(Runtime* rt) noexcept
{
    if (!rt || !rt->ctx.interrupt)
        return 0;
    // Native code calls this at every loop back edge. The interpreter offers the handler a chance every 4096
    // instructions; asking a host callback -- which usually reads a clock -- once per iteration made a tight loop
    // spend most of its time in the handler.
    constexpr std::uint32_t kPollInterval = 1024;
    if (rt->ctx.interruptCountdown > 0) {
        --rt->ctx.interruptCountdown;
        return 0;
    }
    rt->ctx.interruptCountdown = kPollInterval - 1;
    try {
        if (!rt->ctx.interrupt())
            return 0;
    } catch (...) {
        // A handler that threw is treated as a request to stop: letting the
        // exception cross a generated frame is not an option.
    }
    rt->nativeError = std::uint32_t(ErrorCode::Interrupted);
    return 1;
}

// -------------------------------------------------------------- backend ---

std::string JitBackend::dump(const NativeCode* code) const
{
    if (!code || !code->codeStart)
        return "<no code>";
    std::string out;
    char buf[16];
    const auto* bytes = static_cast<const unsigned char*>(code->codeStart);
    for (std::size_t i = 0; i < code->codeBytes; ++i) {
        std::snprintf(buf, sizeof buf, "%02x ", bytes[i]);
        out += buf;
        if ((i & 15) == 15)
            out += "\n";
    }
    return out + "\n";
}

/// The stand-in used when this build has no generator for the host. It is
/// never asked to compile anything, because `enabled()` is false.
namespace {

class InterpreterOnly : public JitBackend
{
public:
    const char* name() const noexcept override { return "interpreter"; }
    bool available() const noexcept override { return false; }
    bool supports(const MethodInfo&, std::string* reason) const override
    {
        if (reason)
            *reason = "no code generator is compiled in";
        return false;
    }
    NativeCode* compile(Runtime&, MethodInfo&, std::vector<NativeCode*>&) override { return nullptr; }
    void release(NativeCode*) noexcept override {}
};

}  // namespace

std::unique_ptr<JitBackend> createHostBackend()
{
#if defined(SCRIPT_ARCH_ARM64)
    std::unique_ptr<JitBackend> backend = createArm64Backend();
#elif defined(SCRIPT_ARCH_X64)
    std::unique_ptr<JitBackend> backend = createX64Backend();
#else
    std::unique_ptr<JitBackend> backend;
#endif
    if (backend && backend->available())
        return backend;
    return std::make_unique<InterpreterOnly>();
}

// --------------------------------------------------------------- engine ---

JitEngine::JitEngine(std::unique_ptr<JitBackend> backend, const JitOptions& options)
    : backend_(std::move(backend)), options_(options)
{
    if (backend_)
        backend_->cache().setLimit(options_.codeCacheLimit);
}

JitEngine::~JitEngine()
{
    for (NativeCode* code : owned_) {
        if (backend_)
            backend_->release(code);
    }
}

bool JitEngine::enabled() const noexcept
{
    return options_.mode != JitOptions::Mode::Off && hasBackend();
}

bool JitEngine::hasBackend() const noexcept
{
    return backend_ && backend_->available();
}

const char* JitEngine::backendName() const noexcept
{
    return backend_ ? backend_->name() : "interpreter";
}

std::uint64_t JitEngine::codeBytes() const noexcept
{
    return backend_ ? backend_->cache().used() : 0;
}

NativeCode* JitEngine::onHot(Runtime& rt, MethodInfo& method)
{
    if (!hasBackend())
        return nullptr;
    if (NativeCode* existing = method.code.load(std::memory_order_acquire))
        return existing;

    std::string reason;
    if (!backend_->supports(method, &reason)) {
        if (options_.dumpCode)
            rt.ctx.write(fromUtf8("jit: skipping " + method.qualifiedName() + ": " + reason + "\n"),
                     false);
        // Park the counter so the decision is made once and never revisited.
        method.hotness.store(MethodInfo::kNeverCompile, std::memory_order_relaxed);
        return nullptr;
    }

    std::vector<NativeCode*> others;
    NativeCode* code = backend_->compile(rt, method, others);
    if (!code) {
        if (options_.dumpCode && !backend_->lastRefusal().empty())
            rt.ctx.write(fromUtf8("jit: skipping " + method.qualifiedName() + ": " + backend_->lastRefusal() + "\n"), false);
        method.hotness.store(MethodInfo::kNeverCompile, std::memory_order_relaxed);
        return nullptr;
    }
    owned_.push_back(code);
    ++compiled_;
    for (NativeCode* other : others) {
        owned_.push_back(other);
        ++compiled_;
    }
    if (options_.dumpCode)
        rt.ctx.write(fromUtf8("jit: " + method.qualifiedName() + " (" +
                          std::to_string(code->codeBytes) + " bytes)\n" + backend_->dump(code)),
                 false);
    // Publishing last means an interpreter running this method right now keeps
    // running it; the next call takes the native path.
    method.code.store(code, std::memory_order_release);
    return code;
}

const NativeCode* JitEngine::findCode(const void* address) const noexcept
{
    const auto* at = static_cast<const std::uint8_t*>(address);
    for (const NativeCode* code : owned_) {
        const auto* start = static_cast<const std::uint8_t*>(code->codeStart);
        if (at >= start && at < start + code->codeBytes)
            return code;
    }
    return nullptr;
}

std::size_t JitEngine::compileAll(Runtime& rt, AssemblyImpl& assembly)
{
    if (!hasBackend())
        return 0;
    std::size_t count = 0;
    for (auto& method : assembly.methodTable) {
        if (method->isNative() || method->isAbstract() || method->body.code.empty())
            continue;
        if (method->hotness.load(std::memory_order_relaxed) == MethodInfo::kNeverCompile)
            continue;
        if (onHot(rt, *method))
            ++count;
    }
    return count;
}

void JitEngine::forget(AssemblyImpl& assembly) noexcept
{
    for (auto& method : assembly.methodTable) {
        NativeCode* code = method->code.exchange(nullptr, std::memory_order_acq_rel);
        if (!code)
            continue;
        for (std::size_t i = 0; i < owned_.size(); ++i) {
            if (owned_[i] != code)
                continue;
            owned_.erase(owned_.begin() + std::ptrdiff_t(i));
            break;
        }
        if (backend_)
            backend_->release(code);
        method->fastEntry = jitLazyEntry();
        method->delegateEntry = jitLazyDelegateEntry();
    }
}

}  // namespace typed
}  // namespace internal
}  // namespace script
