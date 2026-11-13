// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/dyn/object.hpp"
#include "script/internal/vm/vm.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "script/internal/vm/census.hpp"
#include "script/internal/vm/bytecode.hpp"
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

    const std::size_t stackBytes = std::size_t(32) << 20;
    const sys::VmRegion stack = sys::vmReserve(stackBytes);
    sys::vmCommit(stack.base, stack.bytes);
    stackRegionBase_ = stack.base;
    stackRegionBytes_ = stack.bytes;
    stackBase = stackTop = static_cast<JSValue*>(stack.base);
    stackEnd = stackBase + stack.bytes / sizeof(JSValue);
    // A frame is pushed without ever growing the vector, so pointers into it stay good.
    frameCapacity = std::uint32_t(ctx.options.stackLimit) + 16;
    frameBase = static_cast<Frame*>(allocPayload(sizeof(Frame) * frameCapacity));
    stackInit = stackBase;
    switch (ctx.options.jit.mode) {
    case JitOptions::Mode::Off: tier1Threshold = 0; break;
    case JitOptions::Mode::Eager: tier1Threshold = 1; break;
    default: tier1Threshold = ctx.options.jit.threshold ? ctx.options.jit.threshold : 1; break;
    }
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (const char* t = std::getenv("SCRIPT_VM_TIER1"))
        tier1Threshold = std::uint32_t(std::atoi(t));
    if (const char* t = std::getenv("SCRIPT_VM_TIER2"))
        tier2Threshold = std::atoi(t);
#endif

    atoms.length = internKey("length");
    atoms.prototype = internKey("prototype");
    atoms.constructor = internKey("constructor");
    atoms.name = internKey("name");
    atoms.value = internKey("value");
    atoms.done = internKey("done");
    atoms.next = internKey("next");
    atoms.lastIndex = internKey("lastIndex");
    atoms.index = internKey("index");
    atoms.input = internKey("input");
    atoms.groups = internKey("groups");
    atoms.indices = internKey("indices");
    {
        struct Well
        {
            Key Atoms::*slot;
            const char* name;
        };
        for (const Well& w : {Well{&Atoms::symIterator, "Symbol.iterator"}, Well{&Atoms::symAsyncIterator, "Symbol.asyncIterator"},
                              Well{&Atoms::symToPrimitive, "Symbol.toPrimitive"}, Well{&Atoms::symToStringTag, "Symbol.toStringTag"},
                              Well{&Atoms::symHasInstance, "Symbol.hasInstance"}, Well{&Atoms::symSpecies, "Symbol.species"},
                              Well{&Atoms::symMatch, "Symbol.match"}, Well{&Atoms::symMatchAll, "Symbol.matchAll"},
                              Well{&Atoms::symReplace, "Symbol.replace"}, Well{&Atoms::symSearch, "Symbol.search"},
                              Well{&Atoms::symSplit, "Symbol.split"}})
            atoms.*(w.slot) = [&] {
                const std::string_view ascii(w.name);
                const String text(ascii.begin(), ascii.end());
                return static_cast<Key>(newSymbol(StringView(text), true).asCell());
            }();
    }
    callHook = [](VM& vm, JSValue callee, JSValue thisValue, const JSValue* args, std::uint32_t argc, void*) {
        return vm.call(callee, thisValue, args, argc);
    };
    installBuiltins();
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
    if (stackRegionBase_)
        sys::vmUnmap(stackRegionBase_, stackRegionBytes_);
    if (frameBase)
        freePayload(frameBase, sizeof(Frame) * frameCapacity);
}

void VM::afterCollect() noexcept {}

void VM::addCodeRoot(CodeBlock** slot)
{
    codeRoots_.push_back(slot);
}

void VM::removeCodeRoot(CodeBlock** slot) noexcept
{
    codeRoots_.erase(std::remove(codeRoots_.begin(), codeRoots_.end(), slot), codeRoots_.end());
}

std::string VM::captureStack() const
{
    return {};
}

JSValue VM::newSymbol(StringView description, bool hasDescription)
{
    auto* sym = ctx.newCell<dyn::SymbolCell>();
    sym->hasDescription = hasDescription;
    sym->description = String(description);
    return JSValue::cell(sym);
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
