// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What native code for the dynamic languages calls. Plain C functions: one that can fail records the exception in the runtime
// and returns, and native code checks `Runtime::nativeError` after it. Values travel as a tag and a payload.
#pragma once

#include <cstdint>
#include <memory>

#include "script/internal/dyn/jit.hpp"
#include "script/internal/dyn/object.hpp"

namespace script {
namespace internal {
namespace dyn {

extern "C" {

void scriptDynRaise(Runtime* rt, std::uint32_t code, std::int32_t index) noexcept;
int scriptDynPoll(Runtime* rt) noexcept;

double scriptDynToNumber(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept;
std::int32_t scriptDynToInt32(double d) noexcept;
std::int32_t scriptDynToBoolean(std::int64_t tag, std::uint64_t payload) noexcept;
double scriptDynFmod(double a, double b) noexcept;
double scriptDynPow(double a, double b) noexcept;
NativePair scriptDynAdd(Runtime* rt, std::int64_t ta, std::uint64_t a, std::int64_t tb, std::uint64_t b) noexcept;
std::int32_t scriptDynCompare(Runtime* rt, std::int64_t op, std::int64_t ta, std::uint64_t a, std::int64_t tb,
                              std::uint64_t b) noexcept;
std::int32_t scriptDynLooseEquals(Runtime* rt, std::int64_t ta, std::uint64_t a, std::int64_t tb, std::uint64_t b) noexcept;
std::int32_t scriptDynStrictEquals(std::int64_t ta, std::uint64_t a, std::int64_t tb, std::uint64_t b) noexcept;
NativePair scriptDynTypeOf(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept;
NativePair scriptDynToStringOp(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept;
NativePair scriptDynToPropertyKeyOp(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept;

/// These four borrow: what they return stays alive until the next one returns, which is how their slow path can join a fast
/// path that only reads.
NativePair scriptDynGetProp(Runtime* rt, const FunctionCode* code, std::int64_t tag, std::uint64_t payload,
                            std::uint32_t operand) noexcept;
NativePair scriptDynGetElem(Runtime* rt, std::int64_t tag, std::uint64_t payload, std::int64_t keyTag,
                            std::uint64_t key) noexcept;
NativePair scriptDynGetGlobal(Runtime* rt, GCObject* key) noexcept;

void scriptDynSetProp(Runtime* rt, const FunctionCode* code, std::int64_t tag, std::uint64_t payload, std::uint32_t operand,
                      std::int64_t valueTag, std::uint64_t value) noexcept;
void scriptDynSetElem(Runtime* rt, std::int64_t tag, std::uint64_t payload, std::int64_t keyTag, std::uint64_t key,
                      std::int64_t valueTag, std::uint64_t value) noexcept;
void scriptDynSetGlobal(Runtime* rt, GCObject* key, std::int64_t tag, std::uint64_t payload) noexcept;

NativePair scriptDynCall(Runtime* rt, std::int64_t fnTag, std::uint64_t fn, std::int64_t thisTag, std::uint64_t self,
                         const Value* args, std::uint64_t argc) noexcept;
NativePair scriptDynNew(Runtime* rt, std::int64_t fnTag, std::uint64_t fn, const Value* args, std::uint64_t argc) noexcept;

GCObject* scriptDynNewObject(Runtime* rt) noexcept;
GCObject* scriptDynNewArray(Runtime* rt, std::uint64_t reserve) noexcept;
void scriptDynArrayAppend(Runtime* rt, GCObject* array, std::int64_t tag, std::uint64_t payload) noexcept;
void scriptDynDefField(Runtime* rt, GCObject* object, GCObject* key, std::int64_t tag, std::uint64_t payload) noexcept;
GCObject* scriptDynNewBox(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept;
void scriptDynStoreBox(GCObject* box, std::int64_t tag, std::uint64_t payload) noexcept;
NativePair scriptDynClosure(Runtime* rt, const std::shared_ptr<FunctionCode>* code, DynFunction* parent,
                            Box* const* boxes) noexcept;
void scriptDynThrow(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept;
NativePair scriptDynGetIterator(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept;
std::int32_t scriptDynIterNext(Runtime* rt, std::int64_t tag, std::uint64_t payload, NativePair* out) noexcept;
void scriptDynIterClose(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept;
NativePair scriptDynGlobalThis(Runtime* rt) noexcept;

}  // extern "C"

// ---- in vm.cpp, with what they need ----
Value getPropertyForNative(Runtime& rt, const FunctionCode& code, const Value& base, std::uint32_t operand);
void setPropertyForNative(Runtime& rt, const FunctionCode& code, const Value& base, std::uint32_t operand, const Value& v);
Value getGlobalForNative(Runtime& rt, GCObject* key);
void setGlobalForNative(Runtime& rt, GCObject* key, const Value& v);
Value makeClosureForNative(Runtime& rt, const std::shared_ptr<const FunctionCode>& code, DynFunction* parent,
                           Box* const* boxes);
[[noreturn]] void throwValueForNative(Runtime& rt, const Value& v);

}  // namespace dyn
}  // namespace internal
}  // namespace script
