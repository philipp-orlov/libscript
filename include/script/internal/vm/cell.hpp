// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What every cell of the new core starts with: sixteen bytes, the count at offset 8 where `script::Value` expects it.
//
// An old cell (`detail::GCObject`) starts with a virtual table pointer, whose byte 7 is zero for an address below 2^56.
// A new cell has `0x80 | Kind` there, which is how shared code tells the families apart. There are no virtual functions
// on a new cell: destroying one, listing its children and sizing it are `switch`es on its kind in src/vm/cell.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

#include "script/internal/vm/value.hpp"

namespace script {
namespace internal {
namespace vm {

enum class Kind : std::uint8_t
{
    Object = 1,  ///< any JavaScript object; `cls` says which
    Box,         ///< a captured variable
    GetterSetter,
};

/// What an object is. Only the first three have a struct in this phase; the rest are named so a class has one number.
enum class ObjClass : std::uint8_t
{
    Plain = 0,
    Array,
    Function,
    Error,
    Boolean,
    Number,
    String,
    Symbol,
    BigInt,
    Date,
    RegExp,
    Map,
    Set,
    WeakMap,
    WeakSet,
    Promise,
    ArrayBuffer,
    TypedArray,
    DataView,
    Proxy,
    Iterator,
    Arguments,
    Generator,
    Global,
    Native,
    RegExpCell,  ///< a RegExp whose state is in its cell (`JSObject::embeddedState`); a subclass instance is `RegExp`
};

namespace cellflags {
inline constexpr std::uint8_t kImmortal = 1;      ///< never freed: the count starts at 0x40000000
inline constexpr std::uint8_t kYoung = 2;         ///< made since the last collection
inline constexpr std::uint8_t kIsProto = 4;       ///< some shape tree's prototype
inline constexpr std::uint8_t kHasRare = 8;       ///< `slots[-2]` holds a `Rare*`
inline constexpr std::uint8_t kNotExtensible = 16;
inline constexpr std::uint8_t kFrozen = 32;
inline constexpr std::uint8_t kBuffered = 64;     ///< in the old-candidate buffer
inline constexpr std::uint8_t kAcyclic = 128;
}  // namespace cellflags

inline constexpr std::uint32_t kImmortalCount = 0x40000000u;

struct CellHeader
{
    std::uint32_t shape;  ///<  0  ShapeId for objects, 0 otherwise. Native code compares this with an immediate.
    std::uint8_t cls;     ///<  4  ObjClass for objects; a subtype for others
    std::uint8_t flags;   ///<  5  `cellflags`
    std::uint8_t aux;     ///<  6  objects: the number of inline slots
    std::uint8_t kind;    ///<  7  0x80 | Kind. Never zero.
    std::uint32_t rc;     ///<  8  the count
    std::uint32_t gc;     ///< 12  cycle collector scratch: bits 0..29 trial count, bits 30..31 colour
};
static_assert(sizeof(CellHeader) == 16, "a cell header is sixteen bytes");
static_assert(offsetof(CellHeader, rc) == detail::kCellCountOffset, "the count sits where script::Value counts");

/// Whether a cell belongs to the new family.
inline bool isVmCell(const detail::GCObject* cell) noexcept
{
    return (reinterpret_cast<const std::uint8_t*>(cell)[7] & 0x80) != 0;
}
inline CellHeader* headerOf(CellPtr cell) noexcept
{
    return reinterpret_cast<CellHeader*>(cell);
}
inline const CellHeader* headerOf(const detail::GCObject* cell) noexcept
{
    return reinterpret_cast<const CellHeader*>(cell);
}
inline Kind kindOf(const detail::GCObject* cell) noexcept
{
    return Kind(headerOf(cell)->kind & 0x7F);
}

}  // namespace vm
}  // namespace internal
}  // namespace script
