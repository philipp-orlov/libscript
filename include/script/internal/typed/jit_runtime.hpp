// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What native code calls, and where it looks.
//
// Every helper is an ordinary C function. A helper that can fail records the exception in the runtime and returns; a
// C++ exception crossing a generated frame would need unwind information the back ends do not emit. Native code checks
// `Runtime::nativeError` after such a call and leaves through the exit lowering prepared.
#pragma once

#include <cstddef>
#include <cstdint>

#include "script/internal/typed/object.hpp"

namespace script {
namespace internal {
namespace typed {

struct Runtime;

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
inline constexpr std::int64_t kPayloadOffset = 8;
inline constexpr std::int64_t kCellRefCountOffset = offsetof(GCObject, rc);
inline constexpr std::int64_t kInstanceTypeOffset = offsetof(CSInstance, type);
inline constexpr std::int64_t kArrayLengthOffset = offsetof(CSArray, length);
inline constexpr std::int64_t kArrayDataOffset = sizeof(CSArray);
inline constexpr std::int64_t kBoxValueOffset = offsetof(Box, value);
inline constexpr std::int64_t kStringLengthOffset = offsetof(StringCell, length);
inline constexpr std::int64_t kStringDataOffset = sizeof(StringCell);
inline constexpr std::int64_t kDelegateMethodOffset = offsetof(CSDelegate, method);
inline constexpr std::int64_t kDelegateNextOffset = offsetof(CSDelegate, next);
inline constexpr std::int64_t kDelegateTargetOffset = offsetof(CSDelegate, target);
inline constexpr std::int64_t kDelegateCapturedOffset = offsetof(CSDelegate, captured);
inline constexpr std::int64_t kMethodFastEntryOffset = offsetof(MethodInfo, fastEntry);
inline constexpr std::int64_t kMethodDelegateEntryOffset = offsetof(MethodInfo, delegateEntry);
inline constexpr std::int64_t kTypeVtableOffset = offsetof(TypeInfo, vtable);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
static_assert(sizeof(CSInstance) == 64 && sizeof(CSArray) == 64 && sizeof(StringCell) == 64,
              "the small cells should fill the padding of their header");
inline constexpr std::int64_t instanceSlotOffset(std::uint32_t slot) noexcept
{
    return std::int64_t(sizeof(CSInstance)) + std::int64_t(slot) * 16;
}

extern "C" {

// ---- objects; each returns an owned cell, or null with the error recorded ----
GCObject* scriptJitNewInstance(Runtime* rt, const TypeInfo* type) noexcept;
GCObject* scriptJitNewArray(Runtime* rt, const TypeInfo* element, std::int32_t length) noexcept;
GCObject* scriptJitNewBox(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept;
GCObject* scriptJitClone(Runtime* rt, GCObject* source) noexcept;
GCObject* scriptJitNewDelegate(Runtime* rt, const TypeInfo* type, const MethodInfo* target, GCObject* receiver,
                               std::int64_t captures, GCObject* box0, GCObject* box1, GCObject* box2,
                               GCObject* box3) noexcept;

// ---- values ----
/// `*slot = value`, with the count already taken.
void scriptJitStoreTagged(Value* slot, std::int64_t tag, std::uint64_t payload) noexcept;
std::int64_t scriptJitTagOf(GCObject* cell) noexcept;
void scriptJitCast(Runtime* rt, std::int64_t tag, std::uint64_t payload, const TypeInfo* to) noexcept;
/// `(int)someObject`: the bits of the value as `to`.
std::uint64_t scriptJitUnbox(Runtime* rt, std::int64_t tag, std::uint64_t payload, const TypeInfo* to) noexcept;
std::int32_t scriptJitIsInstance(Runtime* rt, std::int64_t tag, std::uint64_t payload, const TypeInfo* to) noexcept;
std::int32_t scriptJitValueEquals(std::int64_t tagA, std::uint64_t a, std::int64_t tagB, std::uint64_t b) noexcept;
const MethodInfo* scriptJitResolveInterface(Runtime* rt, GCObject* receiver, const MethodInfo* declared) noexcept;
std::int32_t scriptJitStringEquals(GCObject* a, GCObject* b) noexcept;
double scriptJitFmod(double a, double b) noexcept;
float scriptJitFmodf(float a, float b) noexcept;
std::int64_t scriptJitAddOvf(Runtime* rt, std::int64_t a, std::int64_t b) noexcept;
std::int64_t scriptJitSubOvf(Runtime* rt, std::int64_t a, std::int64_t b) noexcept;
std::int64_t scriptJitMulOvf(Runtime* rt, std::int64_t a, std::int64_t b) noexcept;

// ---- strings; `tag` is the value's tag, or -1 for "read it off the cell" ----
GCObject* scriptJitConcat(Runtime* rt, std::int64_t count, std::int64_t tag0, std::uint64_t payload0, std::int64_t tag1,
                          std::uint64_t payload1, std::int64_t tag2, std::uint64_t payload2) noexcept;
GCObject* scriptJitToString(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept;
GCObject* scriptJitToStringUnsigned(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept;

// ---- the collections; a value that comes back is its payload bits, and holds a count if it has one ----
std::int32_t scriptJitListCount(Runtime* rt, CSList* list) noexcept;
std::uint64_t scriptJitListGet(Runtime* rt, CSList* list, std::int32_t index) noexcept;
void scriptJitListSet(Runtime* rt, CSList* list, std::int32_t index, std::int64_t tag, std::uint64_t payload) noexcept;
void scriptJitListAdd(Runtime* rt, CSList* list, std::int64_t tag, std::uint64_t payload) noexcept;
std::int32_t scriptJitDictCount(Runtime* rt, CSDict* dict) noexcept;
std::uint64_t scriptJitDictGet(Runtime* rt, CSDict* dict, std::int64_t keyTag, std::uint64_t key) noexcept;
void scriptJitDictSet(Runtime* rt, CSDict* dict, std::int64_t keyTag, std::uint64_t key, std::int64_t valueTag,
                      std::uint64_t value) noexcept;
std::int32_t scriptJitDictContains(Runtime* rt, CSDict* dict, std::int64_t keyTag, std::uint64_t key) noexcept;
void scriptJitBuilderAppendChar(Runtime* rt, CSBuilder* builder, std::uint64_t ch) noexcept;
void scriptJitBuilderAppendString(Runtime* rt, CSBuilder* builder, GCObject* text) noexcept;

// ---- raising ----
void scriptJitThrow(Runtime* rt, GCObject* exception) noexcept;
/// `index` is what a bounds failure was given; anything else ignores it.
void scriptJitRaise(Runtime* rt, std::uint32_t code, std::int32_t index) noexcept;
/// Release `count` possibly null cells: what a method held when an exception went through it.
void scriptJitCleanup(GCObject** cells, std::uint64_t count) noexcept;
/// With an error pending: the exception, holding a count and no longer pending, if a handler for `type` (null for any)
/// may take it. The engine's own limits, an interrupt above all, are no program's to catch.
GCObject* scriptJitCatch(Runtime* rt, const TypeInfo* type) noexcept;
/// With an error pending, on the way into a `finally`: put it aside and return its number, or 0 if it must not stop for
/// a `finally`.
std::int32_t scriptJitFinallyEnter(Runtime* rt) noexcept;
/// `continuation` is what `scriptJitFinallyEnter` gave, negated: make that error pending again.
void scriptJitFinallyRethrow(Runtime* rt, std::int64_t continuation) noexcept;

// ---- the boundary ----
/// Stubs, in assembly: every method's entry starts as one.
void scriptJitLazyStub();
void scriptJitLazyDelegateStub();
/// Keeps every register live across the call: reached from a cold path, where a caller-saved register still holds a
/// value of the hot path.
void scriptJitPollStub();
/// Enter native code from C++. `delegate` is what a lambda reads its captures from.
void scriptJitEnter(Runtime* rt, void* entry, const std::uint64_t* ints, const double* fps,
                    const std::uint64_t* stackArgs, std::uint64_t stackCount, const void* delegate,
                    std::uint64_t* intResults, double* fpResult);

/// What the lazy stubs call. `saved` holds the integer argument registers, then the eight floating point ones, then two
/// words a result goes in; a floating point result goes in the first floating point one. Returns where to jump instead, or
/// null when the call has been made.
void* scriptJitBridge(Runtime* rt, MethodInfo* method, std::uint64_t* saved, std::uint64_t* stackArgs,
                      CSDelegate* delegate, void* frame) noexcept;

}  // extern "C"

}  // namespace typed
}  // namespace internal
}  // namespace script
