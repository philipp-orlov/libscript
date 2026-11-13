// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The JavaScript every Python program relies on: the built-in functions, the exception classes, the Python spellings of
// the list, string, dict and set methods, and the `math` and `random` modules. Both cores run it.
#pragma once

namespace script {
namespace internal {
namespace python {

/// The text of the prelude, with `__py` (the native helpers) expected on the global object.
const char* prelude() noexcept;

}  // namespace python
}  // namespace internal
}  // namespace script
