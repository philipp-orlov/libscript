// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The instruction set and the encoding of a method body.
//
// The compiler knows every type before a byte is emitted, so the instruction
// set is typed the way CIL's is: there is no polymorphic `add` that inspects
// its operands at run time, there is `AddI4` and `AddR8`. That costs a few
// dozen opcodes and buys two things -- an interpreter whose arithmetic is a
// single machine instruction under the dispatch, and a body a code generator
// can walk straight into registers without inferring anything.
//
// Every instruction is one opcode byte followed by `opArity(op)` 32-bit
// little-endian operands. Fixed-width operands cost a little space and buy a
// decoder that cannot get out of step with the stream -- which is what lets
// the phase-2 back ends scan a body linearly.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "script/internal/common.hpp"

namespace script {
namespace internal {
namespace typed {

enum class Op : std::uint8_t
{
    Nop,

    // ---- constants ----
    LdNull,
    LdTrue,
    LdFalse,
    LdcI4,   // imm
    LdcI8,   // lo, hi
    LdcR4,   // bits
    LdcR8,   // lo, hi
    LdStr,   // index into the assembly's string table

    // ---- stack ----
    Pop,
    Dup,
    Dup2,  ///< duplicate the top two, for `a[i] += x`
    Swap,

    // ---- locals (parameters occupy the first slots) ----
    LdLoc,     // slot
    StLoc,     // slot
    NewBox,    // slot: move the slot's value into a fresh heap box
    LdBoxed,   // slot: read through the box
    StBoxed,   // slot
    LdBoxRef,  // slot: the box itself, for a `ref`/`out` argument or a capture
    LdInd,     // read through a box on the stack (a `ref` parameter)
    StInd,     // write through a box on the stack

    // ---- fields ----
    LdFld,   // slot          stack: obj -> value
    StFld,   // slot          stack: obj value ->
    /// A static field names its owning type as well as its slot: the store it
    /// lives in belongs to the assembly that declared it, which is not
    /// necessarily the one the code is in.
    LdSFld,  // type index, slot
    StSFld,  // type index, slot

    // ---- arrays ----
    NewArr,  // type index    stack: length -> array
    LdElem,  //               stack: array index -> value
    StElem,  //               stack: array index value ->
    LdLen,   //               stack: array -> int

    // ---- objects ----
    NewObj,    // method index (a constructor), argc
    InitObj,   // type index: the default value of a struct
    CloneVal,  // value semantics: copy a struct before storing or passing
    Box,       // type index
    Unbox,     // type index: checked object -> value type
    CastClass, // type index: checked reference conversion, throws
    IsInst,    // type index: `x is T`, pushes bool
    AsInst,    // type index: `x as T`, pushes null on failure

    // ---- calls ----
    Call,      // method index, argc
    CallVirt,  // method index, argc: dispatch through the receiver's vtable
    CallIface, // method index, argc: dispatch by interface slot
    NewDlg,    // type index, method index, capture count
    CallDlg,   // argc         stack: delegate arg... -> result
    Ret,
    RetVoid,

    // ---- control ----
    Br,       // target
    BrTrue,   // target
    BrFalse,  // target
    /// Leave a protected region: run the intervening `finally` blocks, then
    /// jump. The verifier pairs these with the handler table.
    Leave,        // target
    EndFinally,
    Throw,
    Rethrow,

    // ---- conversions (the source is read from the value's tag) ----
    ConvI1,
    ConvI2,
    ConvI4,
    ConvI8,
    ConvU1,
    ConvU2,
    ConvU4,
    ConvU8,
    ConvR4,
    ConvR8,
    ConvR8Un,  ///< unsigned source: `(double)someULong`
    ConvChar,
    ConvBool,  ///< normalize to Bool; used when `object` is unboxed to `bool`

    // ---- arithmetic ----
    AddI4,
    SubI4,
    MulI4,
    DivI4,
    DivU4,
    RemI4,
    RemU4,
    NegI4,
    AddI8,
    SubI8,
    MulI8,
    DivI8,
    DivU8,
    RemI8,
    RemU8,
    NegI8,
    AddR4,
    SubR4,
    MulR4,
    DivR4,
    RemR4,
    NegR4,
    AddR8,
    SubR8,
    MulR8,
    DivR8,
    RemR8,
    NegR8,
    /// `checked { }` and `CompileOptions::checkedArithmetic`: the same
    /// arithmetic, but an overflow throws instead of wrapping.
    AddI4Ovf,
    SubI4Ovf,
    MulI4Ovf,
    AddI8Ovf,
    SubI8Ovf,
    MulI8Ovf,

    // ---- bitwise ----
    AndI4,
    OrI4,
    XorI4,
    NotI4,
    ShlI4,
    ShrI4,
    ShrU4,
    AndI8,
    OrI8,
    XorI8,
    NotI8,
    ShlI8,
    ShrI8,
    ShrU8,

    // ---- comparison (each pushes a bool) ----
    CeqI4,
    CneI4,
    CltI4,
    CltU4,
    CleI4,
    CleU4,
    CgtI4,
    CgtU4,
    CgeI4,
    CgeU4,
    CeqI8,
    CneI8,
    CltI8,
    CltU8,
    CleI8,
    CleU8,
    CgtI8,
    CgtU8,
    CgeI8,
    CgeU8,
    CeqR8,
    CneR8,
    CltR8,
    CleR8,
    CgtR8,
    CgeR8,
    CeqRef,  ///< reference identity
    CneRef,
    CeqStr,  ///< `string ==`: ordinal value equality
    CneStr,
    LogicalNot,
    IsNull,  ///< `x == null` for any reference type, and the `?.` test

    // ---- strings ----
    Concat,  // count: pop that many, push the joined string
    ToStr,   ///< `Object.ToString()` on the top of the stack
    /// `ToStr` for a `uint` or a `ulong`. A value carries no unsignedness (a `uint` is an int slot, a `ulong` a long
    /// one), so printing it with `ToStr` gave 4000000000u as -294967296; this one prints the bits as unsigned.
    ToStrU,

    OpCount,
};

const char* opName(Op op) noexcept;
/// How many 32-bit operands follow the opcode byte.
int opArity(Op op) noexcept;
/// Net effect on the evaluation stack, for the `maxStack` computation.
/// Variable-arity instructions report their fixed part; the emitter adjusts.
int opStackDelta(Op op) noexcept;

// ---------------------------------------------------------------- code ---

/// Append an instruction. Operands are always 32 bits, little-endian, so the
/// same body decodes identically on every machine.
inline void emitOp(std::vector<std::uint8_t>& code, Op op)
{
    code.push_back(std::uint8_t(op));
}
inline void emitU32(std::vector<std::uint8_t>& code, std::uint32_t v)
{
    code.push_back(std::uint8_t(v));
    code.push_back(std::uint8_t(v >> 8));
    code.push_back(std::uint8_t(v >> 16));
    code.push_back(std::uint8_t(v >> 24));
}
inline std::uint32_t readU32(const std::uint8_t* p) noexcept
{
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
           (std::uint32_t(p[3]) << 24);
}
inline void patchU32(std::vector<std::uint8_t>& code, std::size_t at, std::uint32_t v) noexcept
{
    code[at] = std::uint8_t(v);
    code[at + 1] = std::uint8_t(v >> 8);
    code[at + 2] = std::uint8_t(v >> 16);
    code[at + 3] = std::uint8_t(v >> 24);
}

struct MethodInfo;

/// Human-readable listing of a method body -- what `csr --dump` prints and
/// what a back end author reads first.
std::string disassemble(const MethodInfo& method);

}  // namespace typed
}  // namespace internal
}  // namespace script
