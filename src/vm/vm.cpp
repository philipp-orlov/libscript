// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/vm/vm.hpp"

#include <cstdio>
#include <cstdlib>

#include "script/internal/vm/census.hpp"

namespace script {
namespace internal {
namespace vm {

VM::VM(ContextImpl& context) : ctx(context) {}

VM::~VM() = default;

void VM::releaseRoots() noexcept {}

void VM::afterCollect() noexcept {}

std::string VM::captureStack() const
{
    return {};
}

#if defined(SCRIPT_JIT_ENV_OVERRIDE)

namespace {

std::uint64_t censusTable[std::size_t(Census::Count)] = {};

const char* const kCensusNames[] = {
    "SlowGetProp", "SlowPutProp", "SlowCall", "SlowElem", "IcMiss", "IcMonoToPoly", "IcToMega", "ChainInvalidated",
    "Tier1Compiled", "Tier2Compiled", "Tier2Exit", "Tier2Invalidated", "ThrowCount", "CycleMinor", "CycleOld",
    "CycleFull", "FreeDeferred",
};
static_assert(sizeof(kCensusNames) / sizeof(kCensusNames[0]) == std::size_t(Census::Count), "a name per counter");

const bool censusEnabled = [] {
    if (!std::getenv("SCRIPT_CENSUS"))
        return false;
    std::atexit([] {
        for (std::size_t i = 0; i < std::size_t(Census::Count); ++i)
            if (censusTable[i])
                std::fprintf(stderr, "vmcensus %-24s %14llu\n", kCensusNames[i],
                             static_cast<unsigned long long>(censusTable[i]));
    });
    return true;
}();

}  // namespace

std::uint64_t* censusCounts() noexcept
{
    (void)censusEnabled;
    return censusTable;
}

const char* censusName(Census id) noexcept
{
    return kCensusNames[std::size_t(id)];
}

#endif

}  // namespace vm
}  // namespace internal
}  // namespace script
