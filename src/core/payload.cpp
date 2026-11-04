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

}  // namespace internal
}  // namespace script
