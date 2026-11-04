// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The C library the front end provides: one table describing each function's
// signature, read by the parser to type calls and by the installer to
// declare the native methods, so the two cannot disagree.
#pragma once

#include <cstdint>

namespace script {
namespace internal {

namespace typed {
struct Runtime;
}

namespace c {

/// One `libc` function. `params` spells the parameter types, one character
/// each: `i` int, `u` unsigned, `l` long, `d` double, `c` char, `s` string
/// (`char*`), `A` char array (`char[]`), `b` bool, `V` the variadic tail
/// (everything after it boxed into an `object[]`). `result` uses the same
/// letters, plus `v` for void.
struct LibcFunction
{
    const char* name;
    char result;
    const char* params;
};

/// Every function, terminated by an entry with a null name. A name may appear
/// more than once with different parameters; the parser picks by argument type.
const LibcFunction* libcFunctions() noexcept;

/// A constant such as `INT_MAX` or `M_PI`.
struct LibcConstant
{
    const char* name;
    char kind;  ///< `i` int, `l` long, `d` double
    std::int64_t intValue;
    double realValue;
};
const LibcConstant* libcConstants() noexcept;

/// Create the `libc` static class in a typed runtime, once.
void installLibc(typed::Runtime& rt);

}  // namespace c
}  // namespace internal
}  // namespace script
