// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libscript -- static diagnostics: what a compiler or type checker reports,
// and the options that decide how strict it is.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "script/error.hpp"
#include "script/language.hpp"

namespace script {

/// One compile error, located in the source it came from. `code` is the
/// language's own number where it has one -- 1002 for a missing semicolon in
/// C#, so existing documentation applies -- and 0 where it does not.
struct Diagnostic
{
    Language language = Language::Auto;
    std::string file;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::uint32_t code = 0;
    std::string message;

    /// `file(line,col): error CS1002: message`, the form the tools print.
    /// The column and the code are left out when the language did not
    /// supply them.
    std::string toString() const;
};

/// How a statically typed program is compiled. Read by the C# front end; the
/// defaults match what `csc` does for a project created today.
struct CompileOptions
{
    bool nullableReferenceTypes = false;  ///< warn on a null flowing into a non-nullable
    bool checkedArithmetic = false;       ///< integer overflow throws instead of wrapping
    bool warningsAsErrors = false;
    bool allowTopLevelStatements = true;  ///< wrap loose statements in a synthesized Main
    /// Emit line numbers into the image so exceptions carry a stack trace.
    bool debugInfo = true;
    /// Fold constants, thread jumps and drop unreachable code before emit.
    bool optimize = true;
    /// Hand every method the code generator will take to it as soon as the
    /// assembly exists, instead of waiting for it to get hot. Costs compile
    /// time up front and makes the first call as fast as the thousandth; with
    /// no back end for the host it does nothing.
    bool native = false;
};

/// How strict the TypeScript checker is. The defaults are `tsc`'s own: a
/// program with no annotations at all -- plain JavaScript -- still
/// type-checks. `strict` turns on the two settings worth turning on for new
/// code. JavaScript sources are never checked.
struct CheckOptions
{
    bool enabled = true;
    bool strictNullChecks = false;
    bool noImplicitAny = false;

    static CheckOptions strict() { return CheckOptions{true, true, true}; }
};

/// Thrown when a program does not compile. Every diagnostic is carried, not
/// just the first, because an error usually comes with its explanation a line
/// or two away.
class CompileError : public Error
{
public:
    explicit CompileError(std::vector<Diagnostic> diagnostics);

    const std::vector<Diagnostic>& diagnostics() const noexcept { return diagnostics_; }

private:
    std::vector<Diagnostic> diagnostics_;
};

}  // namespace script
