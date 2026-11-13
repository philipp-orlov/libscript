// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/vm/vm.hpp"

#include <cstdio>
#include <cstdlib>

#include "script/internal/vm/census.hpp"
#include "script/internal/vm/object.hpp"
#include "sys/vm.hpp"

namespace script {
namespace internal {
namespace vm {

namespace {

mem::LocalHeap::Options heapOptionsFor(VM* owner)
{
    mem::LocalHeap::Options options = payloadHeapOptions();
    options.owner = owner;
    return options;
}

}  // namespace

VM::VM(ContextImpl& context) : ctx(context), heap(heapOptionsFor(this))
{
    // 2^20 ids, eight bytes each, reserved once so that the table never moves.
    const sys::VmRegion region = sys::vmReserve(std::size_t(kDefaultMaxShapes) * sizeof(Shape*));
    shapeRegionBase_ = region.base;
    shapeRegionBytes_ = region.bytes;
    shapeTable_ = static_cast<Shape**>(region.base);

    atoms.length = internKey("length");
    atoms.prototype = internKey("prototype");
    atoms.constructor = internKey("constructor");
    atoms.name = internKey("name");
}

VM::~VM()
{
    // `releaseRoots` ran before the context's cells went; what is left is the memory the VM itself reserved.
    if (!tearingDown)
        releaseRoots();
    for (void* chunk : validityChunks_)
        freePayload(chunk, kValidityChunkBytes);
    if (shapeRegionBase_)
        sys::vmUnmap(shapeRegionBase_, shapeRegionBytes_);
}

void VM::afterCollect() noexcept {}

std::string VM::captureStack() const
{
    return {};
}

Key VM::internKey(StringView text)
{
    StringCell* s = ctx.intern(text);
    detail::incRef(s);
    return s;
}

Key VM::internKey(std::string_view text)
{
    return internKey(ascii(text));
}

void VM::setIntrinsic(JSObject** slot, JSObject* object) noexcept
{
    if (object)
        detail::incRef(reinterpret_cast<CellPtr>(object));
    if (*slot)
        detail::decRef(reinterpret_cast<CellPtr>(*slot));
    *slot = object;
}

JSValue VM::throwError(ErrorCode code, std::string_view message)
{
    clearException();
    exception = JSValue::cell(ctx.intern(ascii(message)));
    retain(exception);
    exceptionCode = code;
    return JSValue::exception();
}

void VM::clearException() noexcept
{
    release(exception);
    exception = JSValue::undefined();
    exceptionCode = ErrorCode::Ok;
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
