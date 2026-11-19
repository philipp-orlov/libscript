// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Why the new core leaves its fast paths: counters, compiled only in a build with SCRIPT_JIT_ENV_OVERRIDE and printed at
// exit as `vmcensus <name> <count>` when SCRIPT_CENSUS is set. Elsewhere SCRIPT_VM_CENSUS(id) is nothing. The macro has
// its own name because the dynamic engine's `SCRIPT_CENSUS` can be included in the same file.
#pragma once

#include <cstddef>
#include <cstdint>

namespace script {
namespace internal {
namespace vm {

#if defined(SCRIPT_JIT_ENV_OVERRIDE)

enum class Census : std::uint8_t
{
    SlowGetProp,
    SlowPutProp,
    SlowCall,
    SlowElem,
    IcMiss,
    IcMonoToPoly,
    IcToMega,
    ChainInvalidated,
    Tier1Compiled,
    Tier2Compiled,
    Tier2Exit,
    Tier2Invalidated,
    ThrowCount,
    MergedSites,  ///< calls put in place by `mergeInline`
    ScriptCalls,  ///< calls of script functions made by the interpreter (run with SCRIPT_VM_TIER1=0 to count them all)
    CycleMinor,
    CycleOld,
    CycleFull,
    FreeDeferred,
    Count,
};

/// One count per `Census`; always present in such a build, printed at exit only when SCRIPT_CENSUS is set.
std::uint64_t* censusCounts() noexcept;
/// The name of a counter, as printed.
const char* censusName(Census id) noexcept;

#define SCRIPT_VM_CENSUS(id) \
    (void)(++script::internal::vm::censusCounts()[std::size_t(script::internal::vm::Census::id)])

#else

#define SCRIPT_VM_CENSUS(id) (void)0

#endif

}  // namespace vm
}  // namespace internal
}  // namespace script
