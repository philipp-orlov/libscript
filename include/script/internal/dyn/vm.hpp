// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The interpreter loop and the abstract operations shared with the builtins.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "script/internal/dyn/runtime.hpp"
#include "script/internal/dyn/object.hpp"

namespace script {
namespace internal {
namespace dyn {

/// One activation of a bytecode function. Frames live on the C++ stack; the
/// context keeps raw pointers to them so a stack trace can be captured.
///
/// The buffers are recycled: frames nest strictly, so the context keeps one
/// spare per depth and a call reuses the capacity the previous call at that
/// depth left behind. That takes four allocations per JavaScript call down to
/// none in steady state.
struct Frame
{
    DynFunction* fn = nullptr;
    const FunctionCode* code = nullptr;
    std::uint32_t pc = 0;
    Vec<Value> locals;
    Vec<Ref<Box>> cells;
    Vec<Value> stack;
    Vec<std::uint32_t> subStack;  ///< `finally` return addresses
    Value thisValue = Value::undefined();
    Value pendingException = Value::undefined();
    Ref<DynFunction> newTarget;

    void begin(DynFunction* function, const FunctionCode* body, Value self, DynFunction* target);
    /// Release everything the call referenced, keeping the buffers themselves.
    void end() noexcept;
};

// ------------------------------------------------------ abstract ops ---

[[noreturn]] void throwError(Runtime& rt, ErrorCode code, const std::string& message);
Value makeErrorValue(Runtime& rt, ErrorCode code, const std::string& message);
/// Turn any C++ exception coming out of native code into a TS `Exception`.
[[noreturn]] void rethrowAsScript(Runtime& rt);

bool toBoolean(const Value& v) noexcept;
/// `typeof v`, as the language spells it.
const char* typeOfName(const Value& v) noexcept;
Value toPrimitive(Runtime& rt, const Value& v, int hint);  ///< 0 default, 1 number, 2 string
double toNumber(Runtime& rt, const Value& v);
String toStringValue(Runtime& rt, const Value& v);
std::int32_t toInt32(double d) noexcept;
std::uint32_t toUint32(double d) noexcept;
std::int64_t toIntegerOrInfinity(double d) noexcept;
/// Boxes a primitive; objects are returned unchanged. Property names are
/// interned and, once interned, live for as long as the Context does.
Value toObject(Runtime& rt, const Value& v);
/// Interned string or symbol, ready to be used as a property key.
Ref<GCObject> toPropertyKey(Runtime& rt, const Value& v);

/// True when the key is a canonical array index, which it then writes to `out`.
bool arrayIndexOf(const StringView& key, std::uint32_t& out) noexcept;

/// The prototype a host wrapper cell (`NativeCell`) reads through, or null
/// when `v` is not one.
DynObject* hostProtoOf(Runtime& rt, const Value& v);
Value getProperty(Runtime& rt, const Value& base, GCObject* key);
Value getPropertyOfObject(Runtime& rt, DynObject* obj, GCObject* key, const Value& receiver);
void setProperty(Runtime& rt, const Value& base, GCObject* key, const Value& v);
bool hasProperty(Runtime& rt, const Value& base, GCObject* key);
bool deleteProperty(Runtime& rt, const Value& base, GCObject* key);
/// Own enumerable string keys, integer-like keys first, in insertion order.
Vec<Value> ownKeys(Runtime& rt, DynObject* obj, bool enumerableOnly, bool includeSymbols);

Value getIndex(Runtime& rt, const Value& base, std::uint32_t i);
void setIndex(Runtime& rt, const Value& base, std::uint32_t i, const Value& v);
std::uint32_t lengthOf(Runtime& rt, const Value& v);

bool looseEquals(Runtime& rt, const Value& a, const Value& b);
bool instanceOf(Runtime& rt, const Value& obj, const Value& ctor);
Value add(Runtime& rt, const Value& a, const Value& b);
/// Relational comparison; `result` is -1, 0, 1 or 2 for "undefined".
int compare(Runtime& rt, const Value& a, const Value& b);

// -------------------------------------------------------------- calls ---

Value callValue(Runtime& rt, const Value& callee, const Value& thisValue,
                const std::vector<Value>& args);
Value constructValue(Runtime& rt, const Value& callee, const std::vector<Value>& args);
/// Call `object[name](args...)`.
Value invokeMethod(Runtime& rt, const Value& object, GCObject* name,
                   const std::vector<Value>& args);

/// Build a closure over `code` capturing from `parent` (which may be null for
/// a top-level program). Every ordinary function gets a `prototype` object;
/// a program, which is never constructed, is made without one.
Value makeClosure(Runtime& rt, const std::shared_ptr<const FunctionCode>& code, Frame* parent,
                  bool withPrototype = true);

/// Run a compiled program in the global scope.
Value runProgram(Runtime& rt, const std::shared_ptr<const FunctionCode>& code);

// ---------------------------------------------------------- iteration ---

/// `obj[Symbol.iterator]()`, or a TypeError if there is none.
Value getIterator(Runtime& rt, const Value& obj);
/// Advance; returns false when the iterator is done, otherwise writes `out`.
bool iteratorStep(Runtime& rt, const Value& iterator, Value& out);
void iteratorClose(Runtime& rt, const Value& iterator);
/// Drain an iterable into a vector -- what spread and rest elements do.
Vec<Value> iterableToVector(Runtime& rt, const Value& iterable);

// ---------------------------------------------------------- promises ---

Value makePromise(Runtime& rt);
void resolvePromise(Runtime& rt, DynPromise* p, const Value& value);
void rejectPromise(Runtime& rt, DynPromise* p, const Value& reason);
/// A one-argument native function that settles `promise` when called -- the
/// `resolve`/`reject` pair handed to executors and `then` callbacks.
Value makeSettler(Runtime& rt, const Value& promise, bool reject);

}  // namespace dyn
}  // namespace internal
}  // namespace script
