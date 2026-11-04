// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Every snippet in this directory is Python: a Context whose default language
// says so.
#pragma once

#include "script/script.hpp"

namespace py {

using script::CallInfo;
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
        options.language = script::Language::Python;
        return options;
    }
};

}  // namespace py
