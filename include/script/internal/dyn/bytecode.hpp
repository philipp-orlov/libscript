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
    DirectEval,  // eval scope: stack func this arg... as Call
    NewRegExp,  // constant: the literal's first object, which every evaluation copies

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
    IterStartLoop,    // obj -> iterator, of a `for-of`: a handler closes it on the way out
    IterCloseAt,      // depth: close the iterator at that place in the stack, keeping everything
    IterCloseThrow,   // iterator exception -> : close it, whatever that does, and throw the exception

    // ---- classes ----
    NewPrivateName,      // atom: -> a private name, new each time the class is evaluated
    PrivateIn,           // object name -> boolean
    DefPrivate,          // object name value -> object
    DefPrivateMethod,    // object name function -> object
    DefPrivateAccessor,  // setter?: object name function -> object
    DefAccessorElem,     // flags (1 setter, 2 not enumerable): object key function -> object
    CheckThis,           // this -> this, or a ReferenceError ahead of `super()`
    DerivedReturn,       // value this -> what a derived constructor returns
    PushNewTarget,
    CallSuperVia,        // argc: constructor newTarget this arg... -> result, from an arrow function
    CallSuperViaSpread,  // constructor newTarget this array -> result
    SetFieldInit,        // constructor function -> constructor
    CallStaticInit,      // constructor function -> constructor
    SetHome,             // home function -> function

    // ---- generators and async functions ----
    CoStart,          // what has run so far was the call: the rest runs as a coroutine, resumed from here
    Yield,            // target: value -> sent; resumed by `return`, goes to the target with the value
    Await,            // value -> settled value, or throws
    DelegateStart,    // the iterator of a `yield*` is on the stack, and its first call is `next(undefined)`
    DelegateCall,     // target: iterator -> iterator result; a `return` it has no method for goes to the target
    DelegateCheck,    // target: iterator result -> value and to the target when done; otherwise yields
    DelegateDone,     // target: to the target if it was a `return` that ended the delegation
    AsyncIterStart,   // obj -> async iterator
    AsyncIterCall,    // iterator -> iterator result
    AsyncIterCheck,   // target: iterator result -> iterator value; to the target when done
    AsyncIterClose,   // depth: push what `return()` gave, or undefined

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
/// What a direct `eval` can see from where it is: each name in scope, and where it is to the function making the call.
struct EvalScope
{
    struct Name
    {
        String name;
        bool local = false;  ///< one of the caller's cells; otherwise one of its upvalues
        bool isConst = false;
        std::uint32_t index = 0;
    };
    std::vector<Name> names;
    std::uint32_t argc = 0;
    bool global = false;  ///< called from the top level of a program
    bool strict = false;
};

struct UpvalDesc
{
    bool fromParentLocal = true;
    std::uint32_t index = 0;
};

enum class HandlerKind : std::uint8_t
{
    Catch,
    Finally,
    /// A `for-of` body. Works as `Catch` does, to code that closes the iterator; native code, which only runs such a loop
    /// over what needs no closing, has nothing to do for it.
    IterClose,
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
struct DynArray;

/// What the element accesses at one place in the code have met, as a byte of `FunctionCode::elemSaw`.
struct ElemSite
{
    static constexpr std::uint8_t kNumbers = 1;  ///< an element of an array of bare numbers
    static constexpr std::uint8_t kValues = 2;   ///< an element of an array of values
    static constexpr std::uint8_t kOther = 4;    ///< anything else: no array, no such element, a key that is no index
    static std::uint8_t of(const DynArray* array) noexcept;
};

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
    /// receiver, or for a read, on `moreHolder` up its prototype chain (valid while `epoch` is current). For a write, a
    /// shape that lacks the property has `moreAddTo`, the shape adding it leads to, valid likewise. A site does only the
    /// one or the other, so only that member of the union is ever used.
    static constexpr std::uint8_t kMoreShapes = 3;
    std::uint8_t moreCount = 0;
    Shape* moreShape[kMoreShapes] = {};
    std::uint32_t moreIndex[kMoreShapes] = {};
    union
    {
        DynObject* moreHolder[kMoreShapes] = {};
        Shape* moreAddTo[kMoreShapes];
    };

    /// Which of the further shapes `s` is, or -1.
    int moreFind(const Shape* s) const noexcept
    {
        for (std::uint8_t i = 0; i < moreCount; ++i) {
            if (moreShape[i] == s)
                return i;
        }
        return -1;
    }
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
    /// Whether a store here adds the property for any shape it knows, and so depends on `epoch`.
    bool anyAdds() const noexcept
    {
        if (addTo)
            return true;
        for (std::uint8_t i = 0; i < moreCount; ++i) {
            if (moreAddTo[i])
                return true;
        }
        return false;
    }
    /// Learn one more shape a store meets, while there is room.
    void addStore(Shape* s, std::uint32_t at, Shape* to) noexcept
    {
        if (s == shape || moreFind(s) >= 0 || moreCount == kMoreShapes)
            return;
        moreShape[moreCount] = s;
        moreIndex[moreCount] = at;
        moreAddTo[moreCount] = to;
        ++moreCount;
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
struct FunctionCode;

/// One local, cell or stack entry of a bail: how to make the interpreter's value of it from what native code holds.
struct DeoptValue
{
    enum Form : std::uint8_t
    {
        Bits,      ///< the payload as it is
        FpBits,    ///< the payload is the bits of a double in a floating point place
        Int32,     ///< a number held as an int32
        Int64,     ///< a number held as an integer in 64 bits
        Constant,  ///< `bits`
        Arguments, ///< an `arguments` array to make: from the array at `payloadAt`, of the length at place `bits`
    };
    Form form = Bits;
    std::uint8_t tag = 0;   ///< when `tagAt` is negative
    std::uint8_t owned = 0;
    std::int32_t tagAt = -1;      ///< index in `DeoptInfo::places`
    std::int32_t payloadAt = -1;  ///< the same
    std::uint64_t bits = 0;
};

/// What a bail needs to know, which it used to write out in code at every place it could happen.
struct DeoptInfo
{
    const FunctionCode* code = nullptr;
    const FunctionCode* outer = nullptr;
    std::uint32_t pc = 0;
    std::uint32_t stack = 0;
    std::int32_t fnAt = -1, thisTagAt = -1, thisAt = -1;
    std::vector<DeoptValue> values;
    /// The register, or when negative spill slot `-place - 1`, of each value: filled in when compiled.
    std::vector<std::int32_t> places;
    std::vector<std::uint8_t> placeIsFp;
    /// From the saved registers to the spill slots.
    std::uint32_t spillOffset = 0;
};

struct FunctionCode
{
    /// One for each `GetElem`, `GetElemDup` and `SetElem`, whose operand says which.
    mutable Vec<std::uint8_t> elemSaw;
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
    bool isGenerator = false;
    bool isAsync = false;
    bool isMethod = false;  ///< written as a method or accessor: `new` means nothing for it

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
    /// The current native code takes `this` to be an object, which entering it part way through has to check.
    mutable bool jitThisIsObject = false;
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
    /// What native code compiled from this has to bail with.
    mutable std::vector<std::unique_ptr<DeoptInfo>> jitDeopts;
    /// What native code takes to stay alive without counting it: the functions its call sites guard they are calling.
    mutable std::vector<Value> jitKeep;
    std::vector<Value> consts;         ///< PushConst operands
    std::vector<Ref<GCObject>> atoms;  ///< interned property names and symbols
    std::vector<std::shared_ptr<FunctionCode>> children;
    std::vector<EvalScope> evalScopes;  ///< DirectEval operands
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
