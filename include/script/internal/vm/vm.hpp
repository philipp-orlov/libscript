// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The new JavaScript core's runtime: what a context owns once something runs on it. The design is in ls-jsc.md; this
// grows with each phase -- the shape table and the VM stack land here.
#pragma once

#include <string>

#include "script/internal/context.hpp"

namespace script {
namespace internal {
namespace vm {

struct VM : EngineRuntime
{
    explicit VM(ContextImpl& context);
    ~VM() override;
    VM(const VM&) = delete;
    VM& operator=(const VM&) = delete;

    ContextImpl& ctx;

    void releaseRoots() noexcept override;
    void afterCollect() noexcept override;
    std::string captureStack() const override;
};

/// The language registration of JavaScript on this core.
const LanguageInfo& javascriptLanguage() noexcept;

}  // namespace vm
}  // namespace internal
}  // namespace script
