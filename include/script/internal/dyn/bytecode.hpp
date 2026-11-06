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
namespace typed {
struct NativeCode;
}

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
    /// What kinds of value reads through this site have given: `kSawNumber`, `kSawOther`.
    std::uint8_t saw = 0;
    static constexpr std::uint8_t kSawNumber = 1;
    static constexpr std::uint8_t kSawOther = 2;
    void see(const Value& v) noexcept { saw |= v.type() == Type::Double ? kSawNumber : kSawOther; }
    /// The property is a getter, to call with the receiver; `holder` and `index` say where it is.
    bool getter = false;
    /// Further receiver shapes a plain read or write has met, each with where the property is in that one: on the
    /// receiver, or for a read, on `moreHolder` up its prototype chain (valid while `epoch` is current).
    static constexpr std::uint8_t kMoreShapes = 3;
    std::uint8_t moreCount = 0;
    Shape* moreShape[kMoreShapes] = {};
    std::uint32_t moreIndex[kMoreShapes] = {};
    DynObject* moreHolder[kMoreShapes] = {};

    /// Which of the further shapes `s` is, or -1.
    int moreFind(const Shape* s) const noexcept
    {
        for (std::uint8_t i = 0; i < moreCount; ++i) {
            if (moreShape[i] == s)
                return i;
        }
        return -1;
    }
    /// A plain own property, in one of the shapes beyond the first: its position, or -1.
    std::int64_t moreAt(const Shape* s) const noexcept
    {
        const int i = moreFind(s);
        return i >= 0 && !moreHolder[i] ? std::int64_t(moreIndex[i]) : -1;
    }
    bool plainOwn() const noexcept { return shape && !holder && !addTo && !getter && !anyHolder(); }
    bool plainRead() const noexcept { return shape && !addTo && !getter; }
    bool anyHolder() const noexcept
    {
        if (holder)
            return true;
        for (std::uint8_t i = 0; i < moreCount; ++i) {
            if (moreHolder[i])
                return true;
        }
        return false;
    }
    /// Learn one more shape, while there is room.
    void addShape(Shape* s, std::uint32_t at, DynObject* where = nullptr) noexcept
    {
        if (s == shape || moreFind(s) >= 0 || moreCount == kMoreShapes)
            return;
        moreShape[moreCount] = s;
        moreIndex[moreCount] = at;
        moreHolder[moreCount] = where;
        ++moreCount;
    }
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

    // ---- native code ----
    /// Where native code comes in; null until there is some.
    mutable void* jitEntry = nullptr;
    /// The same for a plain call, which is not how a class constructor is ever entered.
    mutable void* jitCallEntry = nullptr;
    mutable typed::NativeCode* jitCode = nullptr;
    mutable bool jitRefused = false;
    mutable std::uint32_t jitHotness = 0;
    /// Times native code has handed a call back to the interpreter part way through, since it was last compiled.
    mutable std::uint32_t jitBails = 0;
    /// Times native code has been thrown away for bailing too often. Past what `jitMayGuess` allows, nothing is assumed.
    mutable std::uint8_t jitGeneration = 0;
    /// A compile costs what the function's size does, and each teaches one more place not to guess.
    bool jitMayGuess() const noexcept { return jitGeneration < (code.size() < 1024 ? 12u : 3u); }
    /// Parameters, of the first 32, that some interpreted call has been given something other than a number for.
    mutable std::uint32_t argsNotNumber = 0;
    /// Of those, the ones given a number that is not an int32.
    mutable std::uint32_t argsNotInt = 0;
    /// Locals the current native code keeps as bare numbers. A store the interpreter made after a bail may have put
    /// anything there, which entering part way through has to check.
    mutable std::vector<std::uint32_t> jitNumberLocals;
    /// Those of them kept as int32s, which entering part way through converts to.
    mutable std::vector<std::uint32_t> jitIntLocals;
    /// Where native code has bailed: what it is compiled to next assumes nothing there.
    mutable std::vector<std::uint32_t> jitBailedAt;
    /// What each call site has called while interpreted, for native code to put in its place: by the instruction's pc,
    /// and with no target once it has seen two. Weak, as a function can call itself.
    struct CallTarget
    {
        std::uint32_t pc;
        const FunctionCode* seen;
        std::weak_ptr<const FunctionCode> code;
    };
    mutable std::vector<CallTarget> jitCallTargets;
    void noteCall(std::uint32_t pc, const std::shared_ptr<const FunctionCode>& target) const
    {
        for (CallTarget& known : jitCallTargets) {
            if (known.pc == pc) {
                if (known.seen != target.get())
                    known.seen = nullptr;
                return;
            }
        }
        if (jitCallTargets.size() < 64)
            jitCallTargets.push_back({pc, target.get(), target});
    }
    /// What native code takes to stay alive without counting it: the functions its call sites guard they are calling.
    mutable std::vector<Value> jitKeep;
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
