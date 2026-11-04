// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libscript -- error codes and the exception types the C++ API raises.
#pragma once

#include <stdexcept>
#include <string>

namespace script {

/// Why an operation failed. The codes mirror the exception classes a script
/// can raise in any of the languages -- `System.NullReferenceException` on the
/// C# side, `TypeError` on the JavaScript side -- so an exception thrown by a
/// program keeps its identity when it crosses into C++.
enum class ErrorCode
{
    Ok = 0,
    Exception,           ///< a plain `System.Exception` or `Error`
    SyntaxError,         ///< the source did not parse
    CompileError,        ///< the program did not compile or type-check
    // ---- ECMAScript ----
    TypeError,           ///< wrong type, not callable, not constructible
    RangeError,          ///< value outside the permitted range
    ReferenceError,      ///< unresolvable identifier
    EvalError,
    UriError,
    // ---- .NET ----
    NullReference,       ///< `System.NullReferenceException`
    InvalidCast,         ///< `System.InvalidCastException`
    IndexOutOfRange,     ///< `System.IndexOutOfRangeException`
    ArgumentException,   ///< `System.ArgumentException`
    ArgumentNull,        ///< `System.ArgumentNullException`
    ArgumentOutOfRange,  ///< `System.ArgumentOutOfRangeException`
    DivideByZero,        ///< `System.DivideByZeroException`
    Overflow,            ///< `System.OverflowException`
    InvalidOperation,    ///< `System.InvalidOperationException`
    NotSupported,        ///< `System.NotSupportedException`, and an API the build lacks
    KeyNotFound,         ///< `System.Collections.Generic.KeyNotFoundException`
    FormatException,     ///< `System.FormatException`
    // ---- the engine ----
    InternalError,       ///< the engine hit one of its own limits
    StackOverflow,       ///< call depth exceeded Context::Options::stackLimit
    OutOfMemory,         ///< heap exceeded Context::Options::memoryLimit
    Interrupted,         ///< the interrupt handler asked the VM to stop
    BadImage,            ///< a serialized assembly did not load
};

const char* toString(ErrorCode) noexcept;
/// The .NET type name an error code corresponds to, e.g. "System.OverflowException".
const char* exceptionTypeName(ErrorCode) noexcept;

/// Base of everything libscript throws. Failures that never reach script
/// (bad arguments to the embedding API, engine limits) are raised as `Error`;
/// failures a program could catch arrive as `Exception`, which carries the
/// value that was thrown.
class Error : public std::runtime_error
{
public:
    Error(ErrorCode code, const std::string& what) : std::runtime_error(what), code_(code) {}
    explicit Error(ErrorCode code) : std::runtime_error(toString(code)), code_(code) {}

    ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

}  // namespace script
