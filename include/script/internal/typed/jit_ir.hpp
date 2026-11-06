// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The optimizing tier's intermediate form.
//
// A method on its way to machine code goes through three passes, each small enough to read in one sitting:
//
//   bytecode --lowerMethod--> IrFunction --allocateRegisters--> IrFunction + locations --back end--> machine code
//
// The IR is a linear list of three-address instructions over virtual registers, with labels. It is not SSA: a virtual
// register may be written many times, which is what a local variable is. The evaluation stack is gone by this point,
// because lowering runs it at compile time.
//
// Reference counts are explicit (`IncRef`, `DecRef`), so what lowering proves it does not need never reaches the back
// end. The rules it follows:
//
//   * a callee borrows its arguments: the caller guarantees they outlive the call, and a parameter the callee never
//     assigns costs no count at all;
//   * a return value is owned by the caller;
//   * a reference local holds a count unless it is a parameter that is never assigned.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "script/internal/typed/metadata.hpp"

namespace script {
namespace internal {
namespace typed {

enum class IrOp : std::uint8_t
{
    Label,    // imm: label id
    Mov,      // d = a, in the register class of d (both integer or both floating point)
    MovImm,   // d = imm
    MovRuntime, // d = the runtime pointer
    FMovImm,  // d = the floating point constant whose bits are imm; width says which size
    BitsToFp, // d (fp) = bits of a (int)
    FpToBits, // d (int) = bits of a (fp)

    // Integer arithmetic, at `width` bits. `b` or, when hasImm, `imm`.
    Add, Sub, Mul, And, Or, Xor, Shl, Sar, Shr,
    SDiv, UDiv, SRem, URem,
    Neg, Not,
    Sext8, Sext16, Sext32, Zext8, Zext16, Zext32,

    FAdd, FSub, FMul, FDiv, FNeg, FSqrt, FAbs, FFloor, FCeil, FTrunc, FRoundEven,
    FConvert,    // d (width) = a (the other float size)
    IntToFp,     // d (fp, width) = a (int, srcWidth), signed unless `isUnsigned`
    FpToInt,     // d (int) = a (fp, srcWidth), truncating; infinities give 0, as the interpreter does

    Cset,        // d = a <cond> b|imm, at `width`
    FCset,       // d = a <cond> b, floats at `width`
    Br,          // to label imm
    BrCmp,       // to label `target` if a <cond> b|imm, at `width`
    BrFCmp,      // to label `target` if a <cond> b, floats at `width`
    ExitCmp,     // to exit `target` if a <cond> b|imm, at `width`

    Load,        // d = [a + imm], `width` bytes, sign extended when isSigned
    LoadIdx,     // d = [a + (b << scale) + imm]
    Store,       // [a + imm] = c
    StoreIdx,    // [a + (b << scale) + imm] = c
    StoreTag,    // the tag byte at [a + imm] = cond-free constant `tag`, or Null when c is zero (c < 0: unconditional)
    StoreTagIdx, // the same at [a + (b << scale) + imm]

    IncRef,      // a may be null when isNullable
    DecRef,      // a may be null when isNullable
    IncRefTagged,  // a is a value whose tag is b: a count only if the tag says reference
    DecRefTagged,
    TagOfNullable, // d = tag, or Null when a is zero
    Poll,        // a loop back edge: stop at exit `target` if an interrupt is asked for

    Call,        // calls[imm]
    Ret,         // return a, or nothing when a < 0
    Trap,        // unreachable, a marker for the disassembly

    OsrEntry,    // the point execution joins at when entering from the interpreter: imm is the OSR entry id

    FrameAddr,   // d = the address of byte imm of this frame's memory area (see `frameAreaBytes`)
    FpToIntRaw,  // d (64 bits) = a (a double) truncated by the plain machine instruction, whatever it does out of range
    SaveRegs,    // every machine register, by number, to 64 words at `NativeConfig::savedRegs`: integers, then fp from word 32
    FpToInt32Js, // d = ToInt32 of a (a double), by the machine: only for a back end that says `hasJsToInt32`
    /// Everything between these two is laid out after the method: code a hot path branches to and rarely takes. It goes
    /// where it is in the IR for everything but the machine code, so the values live across it stay in their registers.
    ColdBegin,
    ColdEnd,
};

enum class IrCond : std::uint8_t
{
    Eq, Ne, Lt, Le, Gt, Ge, Lo, Ls, Hi, Hs,
    // For floats: every one of these but FNe answers false for NaN.
    FEq, FNe, FLt, FLe, FGt, FGe,
};

constexpr IrCond invert(IrCond c) noexcept
{
    switch (c) {
        case IrCond::Eq: return IrCond::Ne;
        case IrCond::Ne: return IrCond::Eq;
        case IrCond::Lt: return IrCond::Ge;
        case IrCond::Le: return IrCond::Gt;
        case IrCond::Gt: return IrCond::Le;
        case IrCond::Ge: return IrCond::Lt;
        case IrCond::Lo: return IrCond::Hs;
        case IrCond::Ls: return IrCond::Hi;
        case IrCond::Hi: return IrCond::Ls;
        case IrCond::Hs: return IrCond::Lo;
        default: return c;  // a float comparison has no inverse because of NaN
    }
}

struct IrInst
{
    IrOp op = IrOp::Trap;
    IrCond cond = IrCond::Eq;
    std::uint8_t width = 64;  ///< bits for arithmetic, bytes for memory
    std::uint8_t srcWidth = 64;
    std::uint8_t scale = 0;
    Type tag = Type::Null;
    bool hasImm = false;
    bool isSigned = false;
    bool isUnsigned = false;
    bool isNullable = false;
    /// What memory a load or store touches, for telling apart two that share an offset: 0 is anything, and two non-zero
    /// classes that differ never overlap.
    std::uint8_t alias = 0;
    std::int32_t d = -1, a = -1, b = -1, c = -1;
    std::int32_t target = -1;
    std::int64_t imm = 0;
};

enum class IrCallKind : std::uint8_t
{
    Direct,    ///< a known method: `target`
    Virtual,   ///< through the vtable of the first argument, at `target->vtableSlot`
    Interface, ///< resolved at run time, then through the method's entry
    Delegate,  ///< the delegate in `delegate`
    Helper,    ///< a C function of the runtime, `helper`, with the C calling convention
    Indirect,  ///< the address in `delegate`
};

/// Where in the source a machine address came from, for stack traces. `parent` chains inlined frames.
struct IrSite
{
    const void* method = nullptr;  ///< the engine's own idea of a function
    std::uint32_t pc = 0;
    std::int32_t parent = -1;
};

struct IrCall
{
    IrCallKind kind = IrCallKind::Helper;
    const MethodInfo* target = nullptr;
    void* helper = nullptr;
    std::vector<std::int32_t> args;
    std::int32_t result = -1;
    std::int32_t resultTag = -1;  ///< `object` comes back as a payload and a tag
    std::int32_t delegate = -1;
    std::int32_t exit = -1;   ///< where to go when the call raised; -1 when it cannot raise
    std::int32_t site = -1;   ///< index in `sites`
    bool publishFrame = false;  ///< a helper that may raise wants the frame pointer recorded first
    /// In a cold region: registers live across it are kept by the call itself, not by the allocator's choices.
    bool cold = false;
    std::int32_t liveIndex = -1;  ///< with `cold`, into `liveAtRelease`
    /// When not negative, only so many of `args` are passed. The rest are values something needs to find where they are
    /// when the call is made, so they are live up to it and no further: see `location`.
    std::int32_t abiArgs = -1;
    /// What the engine wants to know of those places, once they are known.
    void* readsPlaces = nullptr;
    std::vector<std::int32_t> passed() const
    {
        return abiArgs < 0 ? args : std::vector<std::int32_t>(args.begin(), args.begin() + abiArgs);
    }
};

/// A way out of the method's normal flow. Everything `cleanup` names is released, then `code` is raised (or, when it is
/// `ErrorCode::None`-like zero, the error already pending travels on), and the method returns to its caller.
struct IrExit
{
    /// A field a loop keeps in a register, which has to be back in memory before anyone can look.
    struct WriteBack
    {
        std::int32_t base;
        std::int64_t offset;
        std::int32_t vreg;
        std::uint8_t width;
    };
    std::vector<WriteBack> writeBacks;
    std::vector<std::int32_t> cleanup;
    /// `object` values: (payload, tag).
    std::vector<std::pair<std::int32_t, std::int32_t>> cleanupTagged;
    /// The index a bounds failure says it was given.
    std::int32_t indexVreg = -1;
    /// Where to carry on when a handler in this method may take the exception; -1 means return to the caller.
    std::int32_t handlerLabel = -1;
    std::uint32_t code = 0;
    std::int32_t site = -1;
};

/// Entering from the interpreter at a loop head: each local's value is read from the interpreter's frame.
struct IrOsrEntry
{
    std::uint32_t bytecodePc = 0;
    std::int32_t label = -1;
    struct Slot
    {
        std::int32_t vreg;
        std::uint32_t slot;
        std::uint8_t bytes;  ///< 4 or 8 of the payload
        bool owned;          ///< the count moves from the interpreter's frame to the native one
        std::int32_t tagVreg = -1;  ///< `object`: its tag goes here
    };
    std::vector<Slot> slots;
    /// The dynamic engine: pointers in an array of them (`cells`), read from the second argument of the entry.
    std::vector<Slot> pointers;
};

struct IrFunction
{
    const MethodInfo* method = nullptr;
    std::vector<IrInst> code;
    std::vector<IrCall> calls;
    std::vector<IrExit> exits;
    std::vector<IrSite> sites;
    std::vector<IrOsrEntry> osrEntries;
    /// What an on-stack replacement has to set to zero, having no such thing in the interpreter's frame to read.
    std::vector<std::int32_t> osrZero;
    /// What the third argument of an on-stack entry and those after it arrive in.
    std::vector<std::int32_t> osrArguments;
    std::vector<std::uint8_t> vregIsFp;
    std::int32_t labelCount = 0;

    /// The ABI of the method itself: one virtual register per incoming value, in order.
    std::vector<std::int32_t> params;
    /// `captureSlots` of a lambda: read from the delegate at entry.
    std::vector<std::int32_t> captures;
    bool returnsFp = false;
    bool returnsValue = false;
    bool hasLoops = false;
    std::uint32_t maxCleanup = 0;
    std::uint32_t frameAreaBytes = 0;
    std::uint32_t inlinedMethods = 0;

    // ---- what `allocateRegisters` adds ----
    /// By virtual register: >= 0 is a machine register number (fp registers are numbered in their own space), < 0 is
    /// spill slot `-loc - 1`. Unused registers read kUnused.
    static constexpr std::int32_t kUnused = 0x7fffffff;
    std::vector<std::int32_t> location;
    std::uint32_t spillSlots = 0;
    std::uint64_t usedIntRegs = 0;
    std::uint64_t usedFpRegs = 0;
    bool makesCalls = false;
    /// For a `DecRef` whose `target` is the index here: the caller-saved registers live across it, which its cold path
    /// has to keep.
    struct LiveRegs
    {
        std::uint64_t ints = 0;
        std::uint64_t fps = 0;
    };
    std::vector<LiveRegs> liveAtRelease;
    /// Words a cold call may need to keep the live registers in.
    std::uint32_t coldSaveWords = 0;

    std::int32_t newVreg(bool fp)
    {
        vregIsFp.push_back(fp ? 1 : 0);
        return std::int32_t(vregIsFp.size()) - 1;
    }
};

/// What a method looks like to the calling convention, whichever back end implements it.
inline bool isFpKind(TypeKind k) noexcept
{
    return k == TypeKind::Float || k == TypeKind::Double;
}

struct Runtime;

/// Whether the optimizing tier can handle `method` and, when it can, its IR. `reason` receives what ruled it out.
/// `osr` adds an entry at every loop head with an empty evaluation stack.
bool lowerMethod(Runtime& rt, const MethodInfo& method, IrFunction& out, bool osr, std::string* reason);

/// Move what does not change in a loop out of it.
void optimizeLoops(IrFunction& fn);

/// Which machine registers the allocator may use. Numbers are the back end's.
struct RegisterSet
{
    std::vector<std::int32_t> intCallerSaved;
    std::vector<std::int32_t> intCalleeSaved;
    std::vector<std::int32_t> fpCallerSaved;
    std::vector<std::int32_t> fpCalleeSaved;
    /// The argument registers in order, and the one a result comes back in; -1 where the allocator cannot use it.
    std::vector<std::int32_t> intArgs;
    std::vector<std::int32_t> fpArgs;
    std::int32_t intResult = 0;
};

/// Assign a location to every virtual register, by linear scan over the code.
void allocateRegisters(IrFunction& fn, const RegisterSet& regs);

/// Human-readable IR, for `JitOptions::dumpCode`.
std::string dumpIr(const IrFunction& fn);

}  // namespace typed
}  // namespace internal
}  // namespace script
