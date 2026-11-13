// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// A context with the new core's runtime, and the little helpers the object-model tests share.
#pragma once

#include <string>

#include "script/internal/context.hpp"
#include "script/internal/vm/convert.hpp"
#include "script/internal/vm/object.hpp"
#include "script/script.hpp"
#include "tests/test_util.hpp"

namespace vmtest {

using namespace script::internal;
using namespace script::internal::vm;

struct Fixture
{
    script::Context ctx;
    VM& vm;

    Fixture() : ctx(options()), vm(ctx.impl()->vm()) {}
    static script::Context::Options options()
    {
        script::Context::Options o;
        o.language = script::Language::JavaScript;
        o.core = script::Context::Options::Core::Vm;
        return o;
    }

    /// An interned name; the fixture's context keeps it alive.
    Key key(const std::string& name) { return vm.internKey(std::string_view(name)); }
    std::uint64_t liveObjects() const { return ctx.impl()->liveObjects; }
    std::uint64_t heapBytes() const { return ctx.impl()->heapBytes; }
};

inline JSValue num(std::int32_t i)
{
    return JSValue::int32(i);
}

}  // namespace vmtest
