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

/// The one-line rendering `console.log` and `Value::inspect` produce.
String inspectValue(Runtime& rt, const Value& v, int depth);

/// An iterator object over a fixed list of values, using the shared iterator
/// prototype so `for..of` and spread accept it.
Value makeIterator(Runtime& rt, Vec<Value> items);

/// JSON, shared with `Value::toJson` and `Context::parseJson`.
Value jsonParse(Runtime& rt, StringView text);
bool jsonStringify(Runtime& rt, const Value& value, const Value& replacer, StringView indent,
                   String& out);

}  // namespace dyn
}  // namespace internal
}  // namespace script
