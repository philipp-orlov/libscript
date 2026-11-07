// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/typed/jit.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "script/internal/typed/bytecode.hpp"
#include "script/internal/typed/jit_runtime.hpp"
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
        if (!rt->ctx.interruptAsked())
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
#if !SCRIPT_ENABLE_JIT
    std::unique_ptr<JitBackend> backend;
#elif defined(SCRIPT_ARCH_ARM64)
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

void* jitLazyEntry() noexcept
{
#if (defined(SCRIPT_ARCH_ARM64) || (defined(SCRIPT_ARCH_X64) && !defined(_WIN32))) && SCRIPT_ENABLE_JIT
    return reinterpret_cast<void*>(&scriptJitLazyStub);
#else
    return nullptr;
#endif
}

void* jitLazyDelegateEntry() noexcept
{
#if (defined(SCRIPT_ARCH_ARM64) || (defined(SCRIPT_ARCH_X64) && !defined(_WIN32))) && SCRIPT_ENABLE_JIT
    return reinterpret_cast<void*>(&scriptJitLazyDelegateStub);
#else
    return nullptr;
#endif
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
