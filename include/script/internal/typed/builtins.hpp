// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Shared plumbing for the core library implementations in src/builtins.
#pragma once

#include "script/internal/typed/runtime.hpp"
#include "script/internal/typed/object.hpp"
#include "script/internal/typed/types.hpp"
#include "script/internal/typed/vm.hpp"

namespace script {
namespace internal {
namespace typed {

inline Runtime& runtimeOf(CallInfo& call)
{
    return call.context().impl()->typed();
}

/// The characters of a string argument. A null string reads as empty, which is
/// what most of `System.String` does with one.
inline StringView textOf(const Value& v) noexcept
{
    auto* cell = static_cast<StringCell*>(detail::Access::ptr(v));
    return cell ? cell->text() : StringView();
}

inline CSList* asList(const Value& v) noexcept
{
    auto* cell = detail::Access::ptr(v);
    return cell && cell->kind == CellKind::List ? static_cast<CSList*>(cell) : nullptr;
}

inline CSDict* asDict(const Value& v) noexcept
{
    auto* cell = detail::Access::ptr(v);
    return cell && cell->kind == CellKind::Dictionary ? static_cast<CSDict*>(cell) : nullptr;
}

inline CSBuilder* asBuilder(const Value& v) noexcept
{
    auto* cell = detail::Access::ptr(v);
    return cell && cell->kind == CellKind::Builder ? static_cast<CSBuilder*>(cell) : nullptr;
}

inline CSArray* asArray(const Value& v) noexcept
{
    auto* cell = detail::Access::ptr(v);
    return cell && cell->kind == CellKind::Array ? static_cast<CSArray*>(cell) : nullptr;
}

/// Write through a `ref`/`out` argument, which arrives as the box the caller's
/// variable lives in.
inline void storeRef(const Value& box, Value v)
{
    if (auto* cell = detail::Access::ptr(box)) {
        if (cell->kind == CellKind::Box)
            static_cast<Box*>(cell)->value = std::move(v);
    }
}

/// Raise from inside a native method. The interpreter turns it into a C#
/// exception the program can catch.
[[noreturn]] inline void fail(CallInfo& call, ErrorCode code, const std::string& message)
{
    throwError(runtimeOf(call), code, message);
}

}  // namespace typed
}  // namespace internal
}  // namespace script
