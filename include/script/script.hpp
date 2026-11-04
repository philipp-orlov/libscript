// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libscript -- one embeddable engine, several languages.
//
//   * C#, TypeScript and JavaScript today, each compiled in or out with a
//     CMake option; the front ends are the only part that differs per
//     language, the heap, the value model, the binding layer and the
//     embedding API are shared
//   * compile and run are separate: `compile` produces an assembly and `run`
//     runs it; a C# assembly also serializes to a portable image
//   * a typed bytecode machine with arm64 and x86-64 back ends for C#, and a
//     dynamic bytecode machine for the ECMAScript languages, on one heap
//   * reference counting plus a cycle collector; no stop-the-world pauses
//   * one `Context` == one isolated heap, guarded by its own lock
//   * header-only C++ binding layer: expose functions, classes and objects
//     once, and every language sees them
//
//     script::Context ctx;
//     ctx.hostClass("Env").method("Greet", [](std::string name) {
//         return "hello " + name;
//     });
//     ctx.eval("return Env.Greet(\"world\").ToUpper();", "a.cs");
//     ctx.eval("Env.Greet('world').toUpperCase()", "a.js");
//
#pragma once

#include "script/config.hpp"
#include "script/error.hpp"
#include "script/language.hpp"
#include "script/diagnostic.hpp"
#include "script/value.hpp"
#include "script/context.hpp"
#include "script/binding.hpp"
