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

#include <memory>
#include <string>

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

inline std::uint8_t profBit(JSValue v) noexcept
{
    return v.isInt32() ? 1 : v.isDouble() ? 2 : 8;
}
/// The operand kinds of `op`, three characters (see opcodes.def).
const char* opKinds(Op op) noexcept;

/// Where a closure finds a variable of an enclosing function: a register of the parent that holds the variable's box, or
/// an upvalue of the parent.
struct UpvalDesc
{
    std::uint16_t index;
    bool fromParentLocal;
};

/// A variable a direct `eval` can see: `atom` names it; its box is a register of the calling function (`local`) or one of
/// the function's upvalues.
struct EvalVar
{
    std::uint32_t atom;
    bool local;
    std::uint16_t index;
};

/// A protected range of instructions [start, end) and where an exception in it goes: `reg` receives the thrown value.
struct Handler
{
    std::uint32_t start;
    std::uint32_t end;
    std::uint32_t target;
    std::uint16_t reg;
    bool isCatch = false;  ///< a `catch` clause: a generator's `return` passes over it
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
    std::uint16_t pad = 0;     ///< index + 1 of the site's `PolyCache`, 0 for none
    std::uint8_t aux = 0;      ///< the inline slot count of the receiver it learned on, + 1 (0: not known)
    std::uint8_t spare = 0;
    JSObject* holder = nullptr;
    ValidityCell* validity = nullptr;  ///< holds a count
};
/// What a property read remembers beyond its one inline cache: up to four more shapes (a read site that sees several), and
/// accessors. An entry is an own or inherited data property, or an own or inherited accessor; the inherited ones hold a count of
/// the validity cell of the chain. A site reaches its `PolyCache` by `InlineCache::pad` (index plus one; 0 for none).
struct PolyEntry
{
    enum Kind : std::uint8_t { OwnData, ProtoData, OwnAccessor, ProtoAccessor };
    std::uint32_t shape = 0;
    std::uint32_t index = 0;
    JSObject* holder = nullptr;
    ValidityCell* validity = nullptr;  ///< holds a count (inherited kinds)
    Kind kind = OwnData;
};
struct PolyCache
{
    PolyEntry e[4];
    std::uint8_t n = 0;
    std::uint8_t next = 0;  ///< the entry replaced when all four are taken
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
        kClassConstructor = 32,  ///< calling it without `new` is an error
        kDerived = 64,           ///< a constructor of a class with `extends`
        kAsyncGen = 512,         ///< an async generator
        kAsync = 256,            ///< an async function: a generator that a promise drives
        kGenerator = 128,        ///< a call makes a generator object instead of running
        kNeedsArgs = 16,  ///< the call keeps every argument in an array, in the register after the parameters
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
    Vec<Vec<EvalVar>> evalScopes;  ///< what each direct `eval` call in the function sees
    Vec<std::uint8_t> evalHome;    ///< for each of them, whether it is in a method, where `super.x` is allowed
    Vec<Handler> handlers;
    Vec<std::uint32_t> lines;  ///< the source line of each instruction
    Vec<InlineCache> caches;
    Vec<PolyCache> poly;
    /// What the instructions have made so far (profBit: 1 int32, 2 double, 8 anything else), one byte per instruction, then
    /// one per parameter for what calls passed: the optimizing compiler's type feedback. Empty until the code first runs.
    Vec<std::uint8_t> prof;
    /// The optimizing compiler's code for this function (jit.hpp), once the baseline code has run hot; owned.
    struct Tier2Code* tier2 = nullptr;
    std::int32_t tier2Counter = 0;  ///< entries and back edges of the baseline code left before the optimizing compiler is asked
    std::uint32_t optExits = 0;     ///< guards of the optimizing code that failed
    std::uint8_t tier2Tries = 0;
    /// Baseline machine code for this function, once it has run hot enough (jit.hpp); owned.
    struct Tier1Code* tier1 = nullptr;
    std::uint32_t hotness = 0;
    bool tier1Failed = false;  ///< the compiler declined this function: never ask again
    String file;
    /// The program text, shared by every function of it, and where this function's own text lies in it (bytes).
    std::shared_ptr<const std::string> source;
    std::uint32_t srcStart = 0, srcEnd = 0;

    CodeBlock() = default;
    ~CodeBlock();
    CodeBlock(const CodeBlock&) = delete;
    CodeBlock& operator=(const CodeBlock&) = delete;
};

/// The instructions of a block, one per line, with its constants and children after them.
std::string dump(const CodeBlock& block);

}  // namespace vm
}  // namespace internal
}  // namespace script
