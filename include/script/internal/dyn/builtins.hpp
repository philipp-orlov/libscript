// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Standard library installation. Each unit populates one slice of the global
// object; `installBuiltins` runs them in dependency order.
#pragma once

#include "script/internal/dyn/runtime.hpp"

namespace script {
namespace internal {
namespace dyn {

/// The engine instance behind the Context a native function was called on.
inline Runtime& rtOf(CallInfo& ci) { return ci.context().impl()->dyn(); }

void installBuiltins(Runtime& rt);

void installCore(Runtime& rt);         // Object, Function, Symbol, Error, globals
void installArray(Runtime& rt);        // Array and the array iterator
void installString(Runtime& rt);       // String
void installMathJson(Runtime& rt);     // Number, Boolean, Math, JSON, Date
void installCollections(Runtime& rt);  // Map, Set, Promise, console
void installReflection(Runtime& rt);   // descriptors, the rest of Object, Reflect
void installGenerators(Runtime& rt);   // generator and async function objects, async iteration
void installRegExp(Runtime& rt);       // RegExp and the String methods that use it
void installBigInt(Runtime& rt);       // BigInt
void installProxy(Runtime& rt);        // Proxy, installed by installReflection
void installTypedArrays(Runtime& rt);  // ArrayBuffer, the typed arrays, DataView, Atomics

/// A regular expression literal: a checked pattern made once into an object, which each evaluation copies.
Value makeRegExpTemplate(Runtime& rt, StringView pattern, StringView flags);
DynObject* newRegExpFromTemplate(Runtime& rt, DynObject* templateObject);

/// The one-line rendering `console.log` and `Value::inspect` produce.
String inspectValue(Runtime& rt, const Value& v, int depth);

/// An iterator object over a fixed list of values, using the shared iterator
/// prototype so `for..of` and spread accept it.
Value makeIterator(Runtime& rt, Vec<Value> items);

/// For a builtin that loops over a size the program chose (`Array.from({length: n})`, `join`, a repeated string): call
/// it with the iteration number on every pass. Every 1024th pass it checks that the temporaries the loop holds --
/// `tempBytes`, not yet charged to any cell -- still fit under the memory limit, and hands the interrupt handler a
/// chance to stop the loop. The cost on the other passes is one mask and compare.
void checkLongLoopSlow(Runtime& rt, std::size_t tempBytes);
inline void checkLongLoop(Runtime& rt, std::size_t iteration, std::size_t tempBytes)
{
    if ((iteration & 1023) == 0)
        checkLongLoopSlow(rt, tempBytes);
}

bool sameValue(const Value& a, const Value& b);
/// SpeciesConstructor.
Value speciesConstructor(Runtime& rt, const Value& object, const Value& fallback);
/// `next` of what comes back goes over `target` live: 0 values, 1 keys, 2 entries.
Value makeArrayIterator(Runtime& rt, const Value& target, int mode);
bool arrayIteratorStep(Runtime& rt, DynArrayIterator* it, Value& out);
/// LengthOfArrayLike, past what an array can be long.
double lengthOfLike(Runtime& rt, const Value& v);

/// JSON, shared with `Value::toJson` and `Context::parseJson`.
Value jsonParse(Runtime& rt, StringView text);
bool jsonStringify(Runtime& rt, const Value& value, const Value& replacer, StringView indent,
                   String& out);

}  // namespace dyn
}  // namespace internal
}  // namespace script
