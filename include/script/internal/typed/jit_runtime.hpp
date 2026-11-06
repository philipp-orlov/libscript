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
/// `*slot = cell`, with the count already taken and the tag read off the cell.
void scriptJitStoreValue(Value* slot, GCObject* cell) noexcept;
void scriptJitCast(Runtime* rt, GCObject* cell, const TypeInfo* to) noexcept;
std::int32_t scriptJitIsInstance(Runtime* rt, GCObject* cell, const TypeInfo* to) noexcept;
const MethodInfo* scriptJitResolveInterface(Runtime* rt, GCObject* receiver, const MethodInfo* declared) noexcept;
std::int32_t scriptJitStringEquals(GCObject* a, GCObject* b) noexcept;
std::int32_t scriptJitRefEquals(GCObject* a, GCObject* b) noexcept;
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

// ---- raising ----
void scriptJitThrow(Runtime* rt, GCObject* exception) noexcept;
void scriptJitRaise(Runtime* rt, std::uint32_t code) noexcept;
/// Release `count` possibly null cells: what a method held when an exception went through it.
void scriptJitCleanup(GCObject** cells, std::uint64_t count) noexcept;

// ---- the boundary ----
/// Stubs, in assembly: every method's entry starts as one.
void scriptJitLazyStub();
void scriptJitLazyDelegateStub();
/// Keeps every register live across the call: reached from a cold path, where a caller-saved register still holds a
/// value of the hot path.
void scriptJitDecRefStub();
void scriptJitPollStub();
/// Enter native code from C++. `delegate` is what a lambda reads its captures from.
void scriptJitEnter(Runtime* rt, void* entry, const std::uint64_t* ints, const double* fps,
                    const std::uint64_t* stackArgs, std::uint64_t stackCount, const void* delegate,
                    std::uint64_t* intResult, double* fpResult);

/// What the lazy stubs call. `saved` holds x0-x7 then d0-d7. Returns where to jump instead, or null when the call has
/// been made and the result left in `saved`.
void* scriptJitBridge(Runtime* rt, MethodInfo* method, std::uint64_t* saved, std::uint64_t* stackArgs,
                      CSDelegate* delegate, void* frame) noexcept;

}  // extern "C"

}  // namespace typed
}  // namespace internal
}  // namespace script
