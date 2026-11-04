// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Every snippet in this directory is C: a Context whose default language
// says so, plus the helpers the suites share.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "script/script.hpp"

namespace cc {

using script::CallInfo;
using script::CompileError;
using script::Diagnostic;
using script::Error;
using script::ErrorCode;
using script::Exception;
using script::Type;
using script::Value;
template <class T>
using Class = script::Class<T>;

struct Context : script::Context
{
    Context() : Context(Options{}) {}
    explicit Context(Options options) : script::Context(withLanguage(options)) {}

    static Options withLanguage(Options options)
    {
        options.language = script::Language::C;
        return options;
    }
};

/// Run a whole program and keep its context alive, so the Value survives.
inline Value run(const char* source)
{
    static std::vector<std::unique_ptr<Context>> contexts;
    contexts.push_back(std::make_unique<Context>());
    return contexts.back()->eval(source, "<test>");
}

/// Run a program and return everything it printed.
inline std::string output(const char* source)
{
    Context ctx;
    std::string out;
    ctx.setPrintHandler([&out](std::string_view text, bool) { out += text; });
    ctx.eval(source, "<test>");
    return out;
}

}  // namespace cc
