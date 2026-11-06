// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The instruction set and the compiled form of one function.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "script/internal/common.hpp"

namespace script {
namespace internal {
namespace dyn {

/// Every instruction is one opcode byte followed by zero or more 32-bit
/// little-endian operands. Fixed-width operands cost a little space and buy a
/// decoder that cannot get out of step with the stream.
enum class Op : std::uint8_t
{
    Nop,

    // ---- stack ----
    PushUndefined,
    PushNull,
    PushTrue,
    PushFalse,
    PushConst,  // k
    PushThis,
    PushCallee,  // the function whose frame is running
    Pop,
    PopN,  // n
    Dup,
    Dup2,
    Swap,
    Rot3,  // a b c -> c a b

    // ---- variables ----
    GetLocal,   // slot
    SetLocal,   // slot
    GetCell,    // slot   (local that was boxed for capture)
    SetCell,    // slot
    NewCell,    // slot   (box a fresh binding)
    RenewCell,  // slot   (per-iteration `let` binding: box a copy)
    GetUpval,   // index
    SetUpval,   // index
    GetGlobal,     // atom
    SetGlobal,     // atom
    DefGlobal,     // atom, attrs
    TypeofGlobal,  // atom  (missing name yields "undefined", never throws)
    DeleteGlobal,  // atom

    // ---- properties ----
    GetProp,     // atom
    SetProp,     // atom
    GetPropDup,  // atom: obj -> func obj   (method calls keep the receiver)
    GetPropBound,  // atom: GetProp, and a function read goes through `__py.bindRead` (Python's bound methods)
    GetElem,
    SetElem,
    GetElemDup,
    DeleteProp,  // atom
    DeleteElem,
    DefField,   // atom  (obj value -> obj)
    DefGetter,  // atom
    DefSetter,  // atom
    DefElem,    // (obj key value -> obj)
    DefMethod,      // atom  (like DefField, plus the home object for `super`)
    DefMethodElem,  // (obj key fn -> obj)
    ObjectRest,     // (obj excludedKeys -> obj)  the `...rest` of a pattern
    ClassSetup,     // hasSuper: wires prototype, [[Prototype]] and class flags
    SetProtoOf,
    GetSuper,  // atom
    SetSuper,  // atom
    GetSuperElem,

    // ---- creation ----
    NewObject,
    NewArray,     // reserve
    ArrayAppend,  // (arr value -> arr)
    ArraySpread,  // (arr iterable -> arr)
    ObjectSpread,
    Closure,  // child index

    // ---- calls ----
    Call,        // argc          stack: func this arg...
    CallSpread,  //               stack: func this argsArray
    New,         // argc          stack: ctor arg...
    NewSpread,
    CallSuper,  // argc
    CallSuperSpread,
    Return,
    ReturnUndefined,

    // ---- control ----
    Jump,             // target
    JumpIfFalse,      // target
    JumpIfTrue,       // target
    // The `Keep` forms test the top of the stack without popping it, which is
    // what short-circuit operators and optional chains need.
    JumpIfFalseKeep,       // target
    JumpIfTrueKeep,        // target
    JumpIfNullishKeep,     // target
    JumpIfNotNullishKeep,  // target
    JumpSub,               // target: run a finally block, then come back
    EndFinally,
    Throw,

    // ---- iteration ----
    ForInStart,  // obj -> keysArray index
    ForInNext,   // target
    IterStart,    // obj -> iterator
    IterNext,     // target: iterator -> iterator value
    IterNextOpt,  // iterator -> iterator value|undefined  (destructuring)
    IterRest,     // iterator -> iterator array-of-remaining
    IterClose,

    // ---- operators ----
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Pow,
    Neg,
    Pos,
    BitAnd,
    BitOr,
    BitXor,
    BitNot,
    Shl,
    Shr,
    UShr,
    Eq,
    Ne,
    StrictEq,
    StrictNe,
    Lt,
    Le,
    Gt,
    Ge,
    In,
    InstanceOf,
    Not,
    TypeOf,
    Inc,
    Dec,
    ToNumber,
    ToString,
    ToObject,
    ToPropertyKey,
    Void,

    OpCount,
};

const char* opName(Op op) noexcept;

/// Where a closure finds a captured variable: either a boxed local of the
/// enclosing frame, or one of the enclosing closure's own upvalues.
struct UpvalDesc
{
    bool fromParentLocal = true;
    std::uint32_t index = 0;
};

enum class HandlerKind : std::uint8_t
{
    Catch,
    Finally,
};

/// A protected region. `stackDepth` is the value-stack height to unwind to
/// before entering the handler.
struct Handler
{
    std::uint32_t start = 0;
    std::uint32_t end = 0;
    std::uint32_t target = 0;
    std::uint32_t stackDepth = 0;
    HandlerKind kind = HandlerKind::Catch;
};

struct Shape;
struct DynObject;

/// What one property access in the code has seen: an inline cache, for the last shape only.
struct PropertySite
{
    Shape* shape = nullptr;     ///< the receiver's shape this is about; null for nothing learned
    DynObject* holder = nullptr;  ///< where the property is when not on the receiver
    Shape* addTo = nullptr;     ///< a store that adds the property: the shape it leads to
    std::uint64_t epoch = 0;    ///< `protoEpoch` when `holder` or `addTo` was worked out
    std::uint32_t index = 0;    ///< position in the property table
    std::uint32_t misses = 0;
};

/// A property operand: the atom in the low bits, the site above them. A function with more sites than fit shares the last
/// one as "no site".
inline constexpr std::uint32_t kSiteShift = 20;
inline constexpr std::uint32_t kAtomMask = (1u << kSiteShift) - 1;
inline constexpr std::uint32_t kNoSite = 0xFFFu;

/// One compiled function. Immutable once the compiler is done, shared by every
/// closure made from it.
struct FunctionCode
{
    std::string name;
    String wideName;  ///< what every closure over this code answers to
    void setName(std::string text)
    {
        wideName = fromUtf8(text);
        name = std::move(text);
    }
    std::shared_ptr<const std::string> sourceName;
    int arity = 0;
    bool isArrow = false;
    bool hasRest = false;
    bool isClassConstructor = false;
    bool isDerivedConstructor = false;

    std::uint32_t numLocals = 0;
    std::uint32_t maxStack = 8;
    /// Slot holding the `arguments` array, or `kNoSlot` when unused.
    std::uint32_t argumentsSlot = 0xffffffffu;
    bool argumentsBoxed = false;
    /// Non-strict JavaScript: a plain call's `this` reads as the global object.
    bool sloppyThis = false;

    static constexpr std::uint32_t kNoSlot = 0xffffffffu;

    std::vector<std::uint8_t> code;
    /// What each property access has learned; made the first time the function touches a property. The operand of a
    /// `GetProp`, `GetPropDup` or `SetProp` packs its atom and its site: see `kSiteShift`.
    mutable Vec<PropertySite> sites;
    std::uint32_t siteCount = 0;
    std::vector<Value> consts;         ///< PushConst operands
    std::vector<Ref<GCObject>> atoms;  ///< interned property names and symbols
    std::vector<std::shared_ptr<FunctionCode>> children;
    std::vector<UpvalDesc> upvals;
    std::vector<Handler> handlers;
    /// Sorted by pc; the line of an instruction is the last entry at or below it.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> lines;
    /// Parameter slots that must be boxed on entry because a closure captures them.
    std::vector<std::uint32_t> boxedParams;

    std::uint32_t lineAt(std::uint32_t pc) const noexcept;
};

inline std::uint32_t readOperand(const std::vector<std::uint8_t>& code, std::size_t at) noexcept
{
    return std::uint32_t(code[at]) | (std::uint32_t(code[at + 1]) << 8) |
           (std::uint32_t(code[at + 2]) << 16) | (std::uint32_t(code[at + 3]) << 24);
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
