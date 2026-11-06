// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/common.hpp"

namespace script {
namespace internal {

mem::SizeClassPool& payloadPool()
{
    // Never destroyed: a `Value` in a static may outlive anything else.
    static mem::SizeClassPool* const pool = [] {
        mem::PoolOptions options;
        options.classes = mem::ClassTable::quarterSteps(64, std::size_t{1} << 30);
        options.smallLimit = std::size_t{16} << 10;
        options.slabBytes = std::size_t{256} << 10;
        options.maxSlabsPerClass = std::size_t{1} << 16;
        options.maxLargeBlocks = std::size_t{1} << 16;
        options.threadCacheBlocks = 32;
        options.verifyBlocks = false;      // every caller is the allocator below; the check costs a bitmap write
        options.name = "script";
        return new mem::SizeClassPool(options);
    }();
    return *pool;
}

namespace payload_cache {

thread_local Cache t_cache;

namespace {

void flush(Cache& c) noexcept
{
    for (std::size_t bin = 0; bin < kBins; ++bin) {
        while (void* p = c.head[bin]) {
            c.head[bin] = *static_cast<void**>(p);
            mem::PoolAllocator<char, &payloadPool>().deallocate(static_cast<char*>(p), bin * kGranule);
        }
        c.count[bin] = 0;
    }
}

struct Flusher
{
    ~Flusher()
    {
        t_cache.dead = true;
        t_cache.registered = false;
        flush(t_cache);
    }
};

}  // namespace

void* take(std::size_t bytes)
{
    const std::size_t bin = (bytes + kGranule - 1) / kGranule;
    Cache& c = t_cache;
    if (void* p = c.head[bin]) {
        c.head[bin] = *static_cast<void**>(p);
        --c.count[bin];
        return p;
    }
    return refill(bin * kGranule);
}

void give(void* p, std::size_t bytes) noexcept
{
    const std::size_t bin = (bytes + kGranule - 1) / kGranule;
    Cache& c = t_cache;
    if (c.registered && c.count[bin] < (bin <= 16 ? 32 : 8)) {
        *static_cast<void**>(p) = c.head[bin];
        c.head[bin] = p;
        ++c.count[bin];
        return;
    }
    spill(p, bin * kGranule);
}

void* refill(std::size_t bytes)
{
    Cache& c = t_cache;
    if (!c.registered && !c.dead) {
        // Only now does the thread get something to run at its end; the cache itself needs no constructor.
        static thread_local Flusher flusher;
        (void)flusher;
        c.registered = true;
    }
    return mem::PoolAllocator<char, &payloadPool>().allocate(bytes);
}

void* takeLarge(std::size_t bytes)
{
    return mem::PoolAllocator<char, &payloadPool>().allocate(bytes);
}

void giveLarge(void* p, std::size_t bytes) noexcept
{
    mem::PoolAllocator<char, &payloadPool>().deallocate(static_cast<char*>(p), bytes);
}

void spill(void* p, std::size_t bytes) noexcept
{
    mem::PoolAllocator<char, &payloadPool>().deallocate(static_cast<char*>(p), bytes);
}

}  // namespace payload_cache

}  // namespace internal
}  // namespace script
