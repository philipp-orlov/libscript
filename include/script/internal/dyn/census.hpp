// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Why the runtime is entered: counters, compiled only in a build with SCRIPT_JIT_ENV_OVERRIDE and printed at exit as
// `census <name> <count>` when SCRIPT_CENSUS is set. Elsewhere SCRIPT_CENSUS(id) is nothing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace script {
namespace internal {
namespace dyn {

#if defined(SCRIPT_JIT_ENV_OVERRIDE)

enum class Census : std::uint8_t
{
    // Where `protoEpoch` moves.
    EpochSetProto,
    EpochDropShape,
    EpochAppendKnown,
    EpochPutOwn,
    EpochClearReferences,
    EpochProtoDied,
    // A read that missed its site, and what became of it.
    LearnTotal,
    LearnNoSite,
    LearnNoShape,
    LearnKeyNotString,
    LearnNotObject,
    LearnStaleEpoch,
    LearnAddShape,
    LearnResetFullSite,
    LearnFirst,
    LearnReplace,
    LearnAbsent,
    LearnPresentUnlearnable,
    // Native code calling the runtime.
    GetPropForNative,
    SetPropForNative,
    GetGlobalForNative,
    GetElemTotal,
    GetElemByNumber,
    GetElemStringKey,
    GetElemOtherKey,
    DynCallTotal,
    DynCallBuiltin,
    DynCallBytecode,
    DynCallBound,
    DynCallOther,
    NewThisTotal,
    NewThisRefused,
    NewThisFieldInit,
    Bail,
    SweepCalls,
    SweepFreedNothing,
    Count,
};

/// The table of counts, one per `Census`; always there in such a build, printed at exit only when SCRIPT_CENSUS is set.
std::uint64_t* censusCounts() noexcept;
/// Whether SCRIPT_CENSUS is set: what costs more than an increment is done only then.
bool censusOn() noexcept;
/// One more of `text` in the histogram printed as `names <count> <text>`, the 40 largest.
void censusName(const std::string& text);

#define SCRIPT_CENSUS(id) \
    (void)(++script::internal::dyn::censusCounts()[std::size_t(script::internal::dyn::Census::id)])

#else

#define SCRIPT_CENSUS(id) (void)0

#endif

}  // namespace dyn
}  // namespace internal
}  // namespace script
