// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What the public `Value` API does with an object of the new core. A `Value` of type Object may wrap a cell of either
// family; `owns` says which, and the rest are the operations `Value` forwards when it is ours.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "script/internal/vm/object.hpp"

namespace script {
namespace internal {
namespace vm {
namespace host {

/// Whether `v` is an object of the new core.
inline bool owns(const Value& v) noexcept
{
    const detail::GCObject* cell = detail::Access::ptr(v);
    return cell && isVmCell(cell);
}
ContextImpl& contextOf(const Value& v) noexcept;

/// Raise what is pending in the VM as the `Exception` a host sees; the pending value is taken.
[[noreturn]] void raisePending(VM& vm);

bool isArray(const Value& v) noexcept;
bool isFunction(const Value& v) noexcept;
bool isError(const Value& v) noexcept;
std::string typeName(const Value& v);
double toDouble(const Value& v);
std::string toString(const Value& v);
std::string inspect(const Value& v);

Value get(const Value& v, std::string_view key);
Value get(const Value& v, std::uint32_t index);
Value get(const Value& v, const Value& key);
void set(const Value& v, std::string_view key, const Value& value);
void set(const Value& v, std::uint32_t index, const Value& value);
void set(const Value& v, const Value& key, const Value& value);
bool has(const Value& v, std::string_view key);
bool hasOwn(const Value& v, std::string_view key);
bool remove(const Value& v, std::string_view key);
std::vector<std::string> keys(const Value& v);
std::uint32_t length(const Value& v);
void push(const Value& v, const Value& value);
void defineProperty(const Value& v, std::string_view key, const Value& value, PropertyFlags flags);
Value prototype(const Value& v);
void setPrototype(const Value& v, const Value& proto);
Value call(const Value& fn, const Value& thisValue, const std::vector<Value>& args);
Value invoke(const Value& v, std::string_view method, const std::vector<Value>& args);
Value construct(const Value& fn, const std::vector<Value>& args);

}  // namespace host
}  // namespace vm
}  // namespace internal
}  // namespace script
