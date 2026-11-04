// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Every snippet in this directory is TypeScript: a Context whose default
// language says so, so the suites read as they did when the engine spoke one
// language.
#pragma once

#include "script/script.hpp"

namespace ts {

using script::CallInfo;
using script::CheckOptions;
using script::CompileError;
using script::Diagnostic;
using script::Error;
using script::ErrorCode;
using script::Exception;
using script::NativeFunction;
using script::PropertyFlags;
using script::Type;
using script::Value;
template <class T>
using Class = script::Class<T>;
template <class T>
using Convert = script::Convert<T>;
using script::adapt;
using script::typeKey;

struct Context : script::Context
{
    Context() : Context(Options{}) {}
    explicit Context(Options options) : script::Context(withLanguage(options)) {}

    static Options withLanguage(Options options)
    {
        options.language = script::Language::TypeScript;
        return options;
    }
};

}  // namespace ts
