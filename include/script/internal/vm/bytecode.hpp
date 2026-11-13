// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The new core's bytecode and the code block that holds it.
//
// Registers, not a stack: an instruction names the registers it reads and the one it writes. A frame is a window of
// registers on the VM stack; register 0 is `this`, 1..nparams the parameters, then locals and temporaries. Every
// register of a live frame holds an owned value (a count, if it is a cell), so a frame can be left at any instruction
// by releasing its registers.
//
// This is the fixed-width form: four 16-bit fields. The plan's variable-width narrow/wide encoding (C-1) is a size
// optimisation on top of the same operand kinds and is left for the interpreter-tuning phase.
#pragma once

#include <cstdint>
#include <string>

#include "script/internal/vm/vm.hpp"

namespace script {
namespace internal {
namespace vm {

enum class Op : std::uint16_t
{
#define OP(name, kinds) name,
#include "script/internal/vm/opcodes.def"
#undef OP
    Count
};

struct Ins
{
    std::uint16_t op;
    std::uint16_t a;
    std::uint16_t b;
    std::uint16_t c;

    Op opcode() const noexcept { return Op(op); }
    /// A jump's offset, in instructions from the one after this.
    std::int32_t jump() const noexcept { return std::int32_t(std::uint32_t(b) | (std::uint32_t(c) << 16)); }
    /// The offset of a fused compare-and-jump, which has two registers and so only 16 bits for the distance.
    std::int16_t jump16() const noexcept { return std::int16_t(c); }
    std::int16_t imm() const noexcept { return std::int16_t(b); }
    static Ins make(Op op, std::uint16_t a = 0, std::uint16_t b = 0, std::uint16_t c = 0) noexcept
    {
        return Ins{std::uint16_t(op), a, b, c};
    }
    void setJump(std::int32_t offset) noexcept
    {
        b = std::uint16_t(std::uint32_t(offset) & 0xFFFF);
        c = std::uint16_t(std::uint32_t(offset) >> 16);
    }
};
static_assert(sizeof(Ins) == 8, "an instruction is eight bytes");

const char* opName(Op op) noexcept;
/// The operand kinds of `op`, three characters (see opcodes.def).
const char* opKinds(Op op) noexcept;

/// Where a closure finds a variable of an enclosing function: a register of the parent that holds the variable's box, or
/// an upvalue of the parent.
struct UpvalDesc
{
    std::uint16_t index;
    bool fromParentLocal;
};

/// A protected range of instructions [start, end) and where an exception in it goes: `reg` receives the thrown value.
struct Handler
{
    std::uint32_t start;
    std::uint32_t end;
    std::uint32_t target;
    std::uint16_t reg;
};

/// What a property instruction learned, kept as data beside the code (ls-jsc.md, 4.6). `shape` is the receiver's shape id
/// (never 0, so an empty cache never hits). Four kinds, told apart by the other fields:
///   own property:        holder and validity null; the value is in slot `index` of the receiver
///   prototype property:  `holder` is the object that has it, `validity` says the chain is as it was
///   add a property:      `next` is the shape after the store (an object literal or a store that creates a property)
struct InlineCache
{
    std::uint32_t shape = 0;
    std::uint32_t index = 0;
    std::uint32_t next = 0;
    std::uint32_t pad = 0;
    JSObject* holder = nullptr;
    ValidityCell* validity = nullptr;  ///< holds a count
};
/// The id a property instruction carries in the word after it when it has no cache (too many in one function).
inline constexpr std::uint16_t kNoCache = 0xFFFF;

struct CodeBlock
{
    enum Flags : std::uint32_t
    {
        kStrict = 1,
        kArrow = 2,
        kProgram = 4,
        kUsesThis = 8,
    };

    std::uint32_t refs = 1;
    std::uint32_t length = 0;  ///< the `length` property: parameters before the first default
    Key name = nullptr;        ///< holds a count; null for an anonymous function
    bool constructor = false;
    std::uint32_t flags = 0;
    std::uint16_t nparams = 0;
    std::uint32_t nregs = 1;

    Vec<Ins> code;
    Vec<JSValue> consts;  ///< owned; strings are interned
    Vec<Key> atoms;       ///< property and global names; each holds a count
    Vec<CodeBlock*> children;  ///< each holds a count
    Vec<UpvalDesc> upvals;
    Vec<Handler> handlers;
    Vec<std::uint32_t> lines;  ///< the source line of each instruction
    Vec<InlineCache> caches;   
    String file;

    CodeBlock() = default;
    CodeBlock(const CodeBlock&) = delete;
    CodeBlock& operator=(const CodeBlock&) = delete;
};

/// The instructions of a block, one per line, with its constants and children after them.
std::string dump(const CodeBlock& block);

}  // namespace vm
}  // namespace internal
}  // namespace script
