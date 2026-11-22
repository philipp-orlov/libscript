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
    /// LoadK's constant: its index in two operands, low half first.
    std::uint32_t kIndex() const noexcept { return std::uint32_t(b) | (std::uint32_t(c) << 16); }
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

namespace elemprof {
inline constexpr std::uint8_t kArray = 1;
inline constexpr std::uint8_t kFloat64 = 2;
inline constexpr std::uint8_t kInt32 = 4;
inline constexpr std::uint8_t kUint8 = 8;
inline constexpr std::uint8_t kOtherTyped = 16;
inline constexpr std::uint8_t kOther = 32;
}  // namespace elemprof

/// `CodeBlock::thisSlots`, counted on first use from the properties the code stores into `this`.
inline std::uint8_t thisSlotsOf(CodeBlock* c) noexcept;

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
/// A call a block runs in place (`selfInline`): instructions [start, end) are the body of the function the `Call` at `site`
/// calls, which is the block's own function.
struct InlineSite
{
    std::uint32_t start;
    std::uint32_t end;
    std::uint32_t site;
    CodeBlock* callee = nullptr;  ///< the function run in place, when it is not the block's own (holds a count)
};

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
    static constexpr std::uint8_t kMegaSite = 1;
    std::uint32_t shape = 0;
    std::uint32_t index = 0;
    std::uint32_t next = 0;
    std::uint16_t pad = 0;     ///< index + 1 of the site's `PolyCache`, 0 for none
    std::uint8_t aux = 0;      ///< the inline slot count of the receiver it learned on, + 1 (0: not known)
    std::uint8_t flags = 0;    ///< kMegaSite: reads here go to the megamorphic table (they outgrow or escape this cache)
    JSObject* holder = nullptr;
    ValidityCell* validity = nullptr;  ///< holds a count
};
/// What a property read or store remembers beyond its one inline cache: up to four more shapes (a site that sees several), and
/// a read's accessors. An entry is an own or inherited data property, or an own or inherited accessor; the inherited ones hold a count of
/// the validity cell of the chain. A site reaches its `PolyCache` by `InlineCache::pad` (index plus one; 0 for none).
struct PolyEntry
{
    /// A read site's kinds, then a store site's: a write to an own data property, and an add (from `shape` to `next`, with
    /// the validity of the chain when one is held). `Absent` is a read site's: the key is nowhere on the chain of objects of
    /// `shape` (undefined), good while `validity` (of the receiver's prototype; none when it has none) is valid.
    enum Kind : std::uint8_t { OwnData, ProtoData, OwnAccessor, ProtoAccessor, Write, Add, Absent };
    std::uint32_t shape = 0;
    std::uint32_t index = 0;
    JSObject* holder = nullptr;
    ValidityCell* validity = nullptr;  ///< holds a count (inherited kinds, an add)
    Kind kind = OwnData;
    std::uint8_t aux = 0;  ///< the inline slot count of the receiver it learned on, + 1 (0: not known or not one)
    std::uint32_t next = 0;  ///< the shape after an add
};
struct PolyCache
{
    PolyEntry e[4];
    std::uint8_t n = 0;
    std::uint8_t next = 0;  ///< the entry replaced when all four are taken
};
/// What a `base[#name]` site found last: an own data property (writable, at a store site) of objects of one shape in the
/// tree, under one private name; or, where a field is defined, the shape it moves objects of one shape to. The name is compared, not counted: the shape holds a count of it while it lives, and a
/// tree shape's id is never given to another.
struct PrivateCache
{
    const void* key = nullptr;
    std::uint32_t shape = 0;
    std::uint32_t index = 0;
    std::uint32_t next = 0;  ///< a definition's (`#x = v` in a class body): the shape after it adds the field at `index`
};
/// What a `base[key]` read found last, where `key` is a string atom or a symbol: a data property (own, or of a prototype while
/// `validity` holds) of objects of one shape. The key is held (a count), compared by address. `misses` counts the times
/// the site could not learn: when it has seen too many keys it stops trying.
struct ElemCache
{
    static constexpr std::uint32_t kWays = 4;
    CellPtr key = nullptr;
    std::uint32_t shape = 0;
    std::uint32_t index = 0;
    JSObject* holder = nullptr;
    ValidityCell* validity = nullptr;  ///< holds a count
    std::uint8_t misses = 0;  ///< of the site, in its first entry
};
/// What a `super.name` read found: the slot of a data property on the chain above `home`, good while `validity` (of
/// `home`'s chain) is valid. `home` is compared, not counted: a dead home object was a prototype, so its cell is cleared.
struct SuperCache
{
    const void* home = nullptr;
    JSValue* slot = nullptr;
    ValidityCell* validity = nullptr;
};
/// The id a property instruction carries in the word after it when it has no cache (too many in one function).
inline constexpr std::uint16_t kNoCache = 0xFFFF;

/// What compiling a stub later needs of the code around it: the names it captured when its closure was made, and where its text
/// starts.
struct LazyInfo
{
    std::vector<std::u16string> upvalNames;  ///< parallel to `CodeBlock::upvals`
    std::vector<std::uint8_t> upvalBits;     ///< 1 const, 2 the name of a function expression, 4 a read needs the hole check
    std::uint32_t line = 0;
    bool isDeclaration = false;
    bool isArrow = false;
    bool strictOuter = false;
};

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
        kNoThis = 1024,   ///< never reads `this` (found when the baseline code is made): a call need not make one
        kRestOnly = 4096,  ///< with `kNeedsArgs`: the register holds the rest parameter's array itself (nothing else uses `arguments`)
        kPrivate = 2048,  ///< names a private member: its element sites keep a `PrivateCache` each
        kLazy = 8192,     ///< a stub: the header and the upvalues are made, `code` is empty until `ensureCompiled` compiles it
    };

    std::uint32_t refs = 1;
    std::uint32_t length = 0;  ///< the `length` property: parameters before the first default
    Key name = nullptr;        ///< holds a count; null for an anonymous function
    bool constructor = false;
    std::uint32_t flags = 0;
    std::uint16_t nparams = 0;
    std::uint32_t nregs = 1;

    /// The function this is the code of, while there has been one only (`made`: none, one, more), so that its variables are
    /// always the same; not counted. `singleAssumed`: optimized code relies on that.
    struct JSFunction* only = nullptr;
    std::uint8_t made = 0;
    bool singleAssumed = false;

    Vec<Ins> code;
    Vec<JSValue> consts;  ///< owned; strings are interned
    Vec<Key> atoms;       ///< property and global names; each holds a count
    Vec<CodeBlock*> children;  ///< each holds a count
    Vec<UpvalDesc> upvals;
    LazyInfo* lazy = nullptr;  ///< owned; while `kLazy` is set
    Vec<Vec<EvalVar>> evalScopes;  ///< what each direct `eval` call in the function sees
    Vec<std::uint8_t> evalHome;    ///< for each of them, whether it is in a method, where `super.x` is allowed
    Vec<Handler> handlers;
    Vec<std::uint32_t> lines;  ///< the source line of each instruction
    Vec<InlineCache> caches;
    Vec<PolyCache> poly;
    Vec<SuperCache> superCaches;
    /// Four entries to a site (`ElemCache::kWays`), made when a read by name first learns; `elemSite[at]` is the index of the
    /// first of the site at instruction `at` plus one, 0 for none yet.
    Vec<ElemCache> elemCaches;
    Vec<std::uint32_t> elemSite;
    Vec<PrivateCache> privCaches;  ///< one per instruction, made with the baseline code of a function that names `#x`
    /// The `uid` of the function the baseline code last called from each `Call` (0 for none): the optimizing compiler's hint
    /// for what to inline. One per instruction once the baseline code is made.
    Vec<std::uint64_t> callProf;
    /// The same for the functions such a call has called, up to four, and whether there were more: where in `callTargets` an
    /// instruction's are (one more than the index; 0: none). Made with the baseline code, and not moved after.
    struct CallTargets
    {
        std::uint64_t uid[4] = {0, 0, 0, 0};
        std::uint64_t more = 0;
    };
    Vec<CallTargets> callTargets;
    Vec<std::uint32_t> callTargetAt;
    std::uint64_t mergedInto = 0;  ///< the uid of the block a function of this one runs instead (`mergeInline`)
    /// Where the baseline code of a `GetGlobal`/`PutGlobal` finds its data property of the global object: the slot's index,
    /// good while `epoch` is the VM's `globalEpoch`; the helper that runs when it is not writes it again. One per
    /// instruction once the baseline code is made.
    struct GlobalRec
    {
        std::uint32_t epoch = 0xFFFFFFFFu;
        std::uint32_t index = 0;
    };
    Vec<GlobalRec> globalRecs;
    std::uint64_t uid = 0;  ///< never reused; `VM::codeByUid` finds the block while it lives
    /// Inline slots for the objects `new` makes for this function: 3, or 8 when it gives `this` more properties than
    /// that (0 until counted; see `thisSlotsOf`).
    std::uint8_t thisSlots = 0;  ///< one per instruction, made with the baseline code of a function that reads `super.x`
    /// What the instructions have made so far (profBit: 1 int32, 2 double, 8 anything else), one byte per instruction, then
    /// one per parameter for what calls passed: the optimizing compiler's type feedback. Empty until the code first runs.
    Vec<std::uint8_t> prof;
    /// What `GetElem` and `PutElem` have indexed (`elemprof` bits), one byte per instruction; made with the baseline code.
    Vec<std::uint8_t> elemProf;
    /// Which typed-array kinds below Float64 (`ElemKind` Int8 to Float32, which `elemprof::kOtherTyped` lumps together) each
    /// of those instructions has indexed: a bit per kind, so the optimizing compiler can take the one a site sees.
    Vec<std::uint8_t> elemKinds;
    /// The optimizing compiler's code for this function (jit.hpp), once the baseline code has run hot; owned.
    struct Tier2Code* tier2 = nullptr;
    std::int32_t tier2Counter = 0;  ///< entries and back edges of the baseline code left before the optimizing compiler is asked
    std::uint32_t optExits = 0;     ///< guards of the optimizing code that failed
    std::uint8_t tier2Tries = 0;
    /// Runs left of optimized-code sites that had learned nothing when it was made, before it makes way for a new compile.
    std::int32_t tier2Budget = 0;
    std::uint8_t tier2Matured = 0;  ///< optimized codes made so far that make way once sites that knew nothing have run
    /// Baseline machine code for this function, once it has run hot enough (jit.hpp); owned.
    struct Tier1Code* tier1 = nullptr;
    std::uint32_t hotness = 0;
    bool tier1Failed = false;  ///< the compiler declined this function: never ask again
    String file;
    /// The program text, shared by every function of it, and where this function's own text lies in it (bytes).
    std::shared_ptr<const std::string> source;
    std::uint32_t srcStart = 0, srcEnd = 0;
    /// The block this one was made from by `selfInline`, which runs the function's calls of itself in place one level deep;
    /// holds a count. Null for a block the compiler made.
    CodeBlock* inlinedFrom = nullptr;
    CodeBlock* selfInlined = nullptr;  ///< the block `selfInline` made from this one, while it lives
    bool selfInlineTried = false;
    /// The block `mergeInline` made this one from, taking the place of it in the function that was running it (holds a count).
    CodeBlock* mergedFrom = nullptr;
    bool mergeTried = false;
    bool constructed = false;  ///< a frame `new` made of it has turned hot
    Vec<InlineSite> inlineSites;
    /// A generator's suspensions (`planSuspends`): for the instruction it resumes at, where in `suspendRegs` its live registers
    /// are: how many, then which. `suspendMax`: the most values a suspended frame keeps; 0 until planned.
    Vec<std::uint32_t> suspendAt;
    Vec<std::uint16_t> suspendRegs;
    std::uint32_t suspendMax = 0;

    CodeBlock() = default;
    ~CodeBlock();
    CodeBlock(const CodeBlock&) = delete;
    CodeBlock& operator=(const CodeBlock&) = delete;
};

/// The instructions of a block, one per line, with its constants and children after them.
std::string dump(const CodeBlock& block);

/// How many properties the code gives `this`, as far as its text says (25: more than 24).
inline unsigned thisStoresOf(const CodeBlock* c) noexcept
{
    Key seen[25];
    unsigned n = 0, privates = 0;
    for (std::size_t j = 0; j < c->code.size() && n + privates < 25; ++j) {
        const Ins& in = c->code[j];
        const Op op = in.opcode();
        if (in.a != 0)
            continue;
        if (op == Op::PutProp || op == Op::InitProp) {
            const Key k = c->atoms[in.b];
            bool dup = false;
            for (unsigned m = 0; m < n; ++m)
                dup = dup || seen[m] == k;
            if (!dup)
                seen[n++] = k;
        } else if (op == Op::DefPrivate || op == Op::DefPrivMethod) {
            ++privates;
        }
    }
    return n + privates;
}

inline std::uint8_t thisSlotsOf(CodeBlock* c) noexcept
{
    if (c->thisSlots)
        return c->thisSlots;
    const unsigned n = thisStoresOf(c);
    c->thisSlots = n <= 3 ? 3 : 8;
    return c->thisSlots;
}

/// The inline slots for `this` of `new` of code `c` with prototype root `root`: what the code stores, or what objects of the
/// tree have grown to.
inline std::uint8_t newSlotsOf(CodeBlock* c, const Shape* root) noexcept
{
    const std::uint8_t own = thisSlotsOf(c);
    return root && root->base && root->base->slotHint > own ? root->base->slotHint : own;
}

/// The same for `new f`, where the tree's hint comes from the text at first: of what `f` and the classes it extends give
/// `this`, their fields with it.
std::uint8_t newSlotsOf(VM& vm, struct JSFunction* f, Shape* root) noexcept;

}  // namespace vm
}  // namespace internal
}  // namespace script
