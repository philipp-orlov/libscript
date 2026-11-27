// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "code_region.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#include <sys/mman.h>
#include <unistd.h>
#define SCRIPT_VM_CODE_REGION 1
#endif

namespace script {
namespace internal {
namespace vm {
namespace jit {

#if defined(SCRIPT_VM_CODE_REGION)

namespace {

constexpr std::size_t kGranule = 64;
constexpr std::size_t kReservation = std::size_t(128) << 20;

struct Hole
{
    std::size_t offset, bytes;
};

struct Region
{
    std::mutex lock;
    std::uint8_t* rw = nullptr;
    std::uint8_t* rx = nullptr;
    bool failed = false;
    std::size_t filled = 0;
    std::size_t used = 0;
    std::vector<Hole> holes;  // by offset, never touching

    bool open()
    {
        if (rx)
            return true;
        if (failed)
            return false;
        failed = true;
        int fd = -1;
#if defined(MFD_CLOEXEC)
        fd = memfd_create("script-vm-code", MFD_CLOEXEC);
#endif
        if (fd < 0)
            return false;
        if (ftruncate(fd, off_t(kReservation)) != 0) {
            close(fd);
            return false;
        }
        void* w = mmap(nullptr, kReservation, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        void* x = w == MAP_FAILED ? MAP_FAILED : mmap(nullptr, kReservation, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
        close(fd);
        if (w == MAP_FAILED || x == MAP_FAILED) {
            if (w != MAP_FAILED)
                munmap(w, kReservation);
            return false;
        }
        rw = static_cast<std::uint8_t*>(w);
        rx = static_cast<std::uint8_t*>(x);
        failed = false;
        return true;
    }
};

/// Never destroyed: a context that outlives static destruction (one held in a static of its own) still gives its code back.
Region& region()
{
    static Region* r = new Region;
    return *r;
}

}  // namespace

CodeSpan allocateCode(std::size_t bytes) noexcept
{
    if (bytes == 0)
        return {};
    const std::size_t need = (bytes + kGranule - 1) & ~(kGranule - 1);
    Region& r = region();
    std::lock_guard<std::mutex> guard(r.lock);
    if (!r.open())
        return {};
    for (std::size_t i = 0; i < r.holes.size(); ++i) {
        Hole& h = r.holes[i];
        if (h.bytes < need)
            continue;
        const std::size_t at = h.offset;
        h.offset += need;
        h.bytes -= need;
        if (h.bytes == 0)
            r.holes.erase(r.holes.begin() + std::ptrdiff_t(i));
        r.used += need;
        return {r.rw + at, r.rx + at, need};
    }
    if (r.filled + need > kReservation)
        return {};
    const std::size_t at = r.filled;
    r.filled += need;
    r.used += need;
    return {r.rw + at, r.rx + at, need};
}

void releaseCode(const CodeSpan& span) noexcept
{
    if (!span)
        return;
    Region& r = region();
    std::lock_guard<std::mutex> guard(r.lock);
    const std::size_t offset = std::size_t(span.rx - r.rx);
    r.used -= span.bytes;
    try {
        auto at = std::lower_bound(r.holes.begin(), r.holes.end(), offset,
                                   [](const Hole& h, std::size_t where) { return h.offset < where; });
        Hole joined{offset, span.bytes};
        if (at != r.holes.end() && at->offset == offset + span.bytes) {
            joined.bytes += at->bytes;
            at = r.holes.erase(at);
        }
        if (at != r.holes.begin() && std::prev(at)->offset + std::prev(at)->bytes == offset) {
            --at;
            joined.offset = at->offset;
            joined.bytes += at->bytes;
            at = r.holes.erase(at);
        }
        if (joined.offset + joined.bytes == r.filled) {
            r.filled = joined.offset;
            // Give the pages back to the system; they read as zero when taken again.
            const long page = sysconf(_SC_PAGESIZE);
            const std::size_t from = (joined.offset + std::size_t(page) - 1) & ~(std::size_t(page) - 1);
            if (from < joined.offset + joined.bytes)
                madvise(r.rw + from, joined.offset + joined.bytes - from, MADV_REMOVE);
            return;
        }
        r.holes.insert(at, joined);
    } catch (...) {
        // The space is lost until the process ends.
    }
}

void commitCode(const CodeSpan& span, const void* code, std::size_t bytes) noexcept
{
    std::memcpy(span.rw, code, bytes);
#if defined(__aarch64__)
    // The data cache is written back through the view the bytes went in by, and the instruction cache forgets what it
    // had for the view they run from. CTR_EL0 says which of the two this processor needs (IDC, DIC) and the line sizes.
    std::uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    if (!(ctr & (std::uint64_t(1) << 28))) {
        const std::uintptr_t line = std::uintptr_t(4) << ((ctr >> 16) & 15);
        const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(span.rw) + bytes;
        for (std::uintptr_t p = reinterpret_cast<std::uintptr_t>(span.rw) & ~(line - 1); p < end; p += line)
            __asm__ volatile("dc cvau, %0" : : "r"(p) : "memory");
        __asm__ volatile("dsb ish" : : : "memory");
    }
    if (!(ctr & (std::uint64_t(1) << 29))) {
        const std::uintptr_t line = std::uintptr_t(4) << (ctr & 15);
        const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(span.rx) + bytes;
        for (std::uintptr_t p = reinterpret_cast<std::uintptr_t>(span.rx) & ~(line - 1); p < end; p += line)
            __asm__ volatile("ic ivau, %0" : : "r"(p) : "memory");
        __asm__ volatile("dsb ish" : : : "memory");
    }
    __asm__ volatile("isb" : : : "memory");
#endif
}

void patchCode(std::uint8_t* rx, std::uint32_t word) noexcept
{
    Region& r = region();
    commitCode(CodeSpan{r.rw + (rx - r.rx), rx, 4}, &word, 4);
}

std::size_t codeInUse() noexcept
{
    Region& r = region();
    std::lock_guard<std::mutex> guard(r.lock);
    return r.used;
}

#else

CodeSpan allocateCode(std::size_t) noexcept { return {}; }
void releaseCode(const CodeSpan&) noexcept {}
void commitCode(const CodeSpan&, const void*, std::size_t) noexcept {}
void patchCode(std::uint8_t*, std::uint32_t) noexcept {}
std::size_t codeInUse() noexcept { return 0; }

#endif

}  // namespace jit
}  // namespace vm
}  // namespace internal
}  // namespace script
