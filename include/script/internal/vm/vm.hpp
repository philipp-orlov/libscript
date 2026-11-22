// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The new JavaScript core's runtime: what a context owns once something runs on it. The design is in ls-jsc.md; this
// grows with each phase -- the VM stack and the code blocks land here.
#pragma once

#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <string_view>

#include "mem/local_heap.hpp"
#include "script/internal/context.hpp"
#include "script/internal/vm/cell.hpp"
#include "script/internal/vm/shape.hpp"
#include "script/internal/vm/value.hpp"

namespace script {
namespace internal {
namespace vm {

struct JSObject;
struct JSArray;
struct JSFunction;
struct JSTypedArray;
enum class ElemKind : std::uint8_t;
struct Box;
struct GetterSetter;
struct Rare;
struct CodeBlock;
struct Frame;


/// One eight-byte cell of validity: a cache or a piece of code that depends on a prototype chain holds a count of one
/// and reads `valid` before trusting what it learned. Made by `VM::validityOf`; cleared by `VM::invalidateChain`.
struct ValidityCell
{
    std::uint32_t refs;
    std::uint32_t valid;
};

/// An entry of the megamorphic read cache (`VM::megaRead`): objects of shape `shape` read `key` from slot `index` of
/// themselves, or (`holder`) of a prototype while `validity` holds. Holds a count of `key` and of `validity`.
struct MegaEntry
{
    std::uint32_t shape;
    std::uint32_t index;
    Key key;
    JSObject* holder;
    ValidityCell* validity;
    ValidityCell* absent;  ///< a dictionary receiver's (`Dictionary::absent`): it has not gained a key since
    std::uint64_t spare[3];
};

/// A call into script from the runtime: the interpreter installs it; accessors and native callbacks go through it.
/// Returns an owned value, or the exception sentinel with `VM::exception` set.
using CallHook = JSValue (*)(struct VM&, JSValue callee, JSValue thisValue, const JSValue* args, std::uint32_t argc,
                             void* user);

/// A function written in C++, called without making an argument vector. Returns an owned value or the exception sentinel.
using FastNative = JSValue (*)(struct VM&, JSValue thisValue, const JSValue* args, std::uint32_t argc, void* data);

/// Names the runtime itself looks up, interned once and held for the VM's life.
struct Atoms
{
    Key length = nullptr;
    Key prototype = nullptr;
    Key constructor = nullptr;
    Key name = nullptr;
    Key value = nullptr;
    Key done = nullptr;
    Key next = nullptr;
    Key lastIndex = nullptr;
    Key index = nullptr;
    Key input = nullptr;
    Key groups = nullptr;
    Key indices = nullptr;
    Key then = nullptr;
    Key return_ = nullptr;
    Key join = nullptr;
    Key flags = nullptr;
    Key exec = nullptr;
    Key add = nullptr;
    Key set = nullptr;
    // The well-known symbols; each holds a count.
    Key symIterator = nullptr;
    Key symAsyncIterator = nullptr;
    Key symToPrimitive = nullptr;
    Key symToStringTag = nullptr;
    Key symHasInstance = nullptr;
    Key symSpecies = nullptr;
    Key symMatch = nullptr;
    Key symMatchAll = nullptr;
    Key symReplace = nullptr;
    Key symSearch = nullptr;
    Key symSplit = nullptr;
    Key symIsConcatSpreadable = nullptr;
    Key symUnscopables = nullptr;
};

/// The prototypes of what a primitive is read through. Null until something installs them; a property read on a
/// primitive whose prototype is null finds nothing.
struct Intrinsics
{
    JSObject* object = nullptr;
    JSObject* function = nullptr;
    JSObject* array = nullptr;
    JSObject* number = nullptr;
    JSObject* string = nullptr;
    JSObject* boolean = nullptr;
    JSObject* symbol = nullptr;
    JSObject* bigint = nullptr;
    JSObject* generator = nullptr;       ///< what a generator function's `prototype` inherits from
    JSObject* iterator = nullptr;        ///< %IteratorPrototype%
    JSObject* nativeIterator = nullptr;  ///< array, map and set iterators
    JSObject* map = nullptr;
    JSObject* set = nullptr;
    JSObject* weakMap = nullptr;
    JSObject* weakSet = nullptr;
    JSObject* regexp = nullptr;
    JSObject* promise = nullptr;
    JSObject* asyncGenerator = nullptr;
    JSObject* arguments = nullptr;       ///< what the `arguments` of a call inherits from
    JSObject* arrayBuffer = nullptr;
    JSObject* sharedArrayBuffer = nullptr;
    JSObject* generatorFunction = nullptr;       ///< %GeneratorFunction.prototype%: what generator functions inherit from
    JSObject* asyncFunction = nullptr;
    JSObject* asyncGeneratorFunction = nullptr;
    JSObject* iteratorHelper = nullptr;         ///< %IteratorHelperPrototype%
    JSObject* wrapForValidIterator = nullptr;   ///< %WrapForValidIteratorPrototype%
    JSObject* typedArrays[16] = {};  ///< by ElemKind
    JSObject* iteratorFamilies[6] = {};  ///< by `IteratorFamily`; 0 is unused
};

/// One activation on the VM stack. `R` is its register window; register 0 is `this`, then the parameters. A function called
/// from bytecode shares its first registers with the call window of its caller.
struct Frame
{
    static constexpr std::uint8_t kArgsBorrowed = 1, kOwned = 2;
    CodeBlock* code;
    JSFunction* fn;
    const struct Ins* pc;       ///< where to go on in this frame: set when it calls out
    JSValue* R;
    std::uint16_t retDst;       ///< the caller's register that receives the result
    /// Optimized code of a function that changes nothing it did not make (see `BorrowInfo`): `kArgsBorrowed`, its caller
    /// passed arguments it holds without a count; `kOwned`, those it held so have been counted since.
    std::uint8_t borrow;
    std::uint32_t argc;
    bool construct;             ///< `new`: a result that is not an object is replaced by `this`
    bool entry;                 ///< entered from C++: leaving it returns from `run`
    JSValue newTarget;          ///< the constructor `new` was applied to; undefined in a call (not counted: it is a callee)
    JSValue gen = JSValue::undefined();  ///< the generator this frame runs, if it does (not counted: the resumer holds it)
};

/// A queued promise job: what to run when the host runs jobs; each value holds a count.
struct MicroJob
{
    int kind;  ///< 0 a reaction to a fulfilled promise, 1 to a rejected one, 2 a thenable to resolve, 3 a plain call
    JSValue a, b, c;
};

struct VM : EngineRuntime
{
    explicit VM(ContextImpl& context);
    ~VM() override;
    VM(const VM&) = delete;
    VM& operator=(const VM&) = delete;

    ContextImpl& ctx;

    void releaseRoots() noexcept override;
    void afterCollect() noexcept override;
    void collectCycles(bool minor) noexcept override;
    void rootsBegin() noexcept override;
    bool freeHeldBack() noexcept override;
    void rootsEnd() noexcept override;
    std::uint64_t ownedBytes() const noexcept override { return ownedBytes_; }
    std::uint64_t ownedBytes_ = 0;
    std::string captureStack() const override;

    // ---- cells ----
    /// The cells of the new family; separate from the context's heap, which holds the old family and the strings. Every
    /// cell is at most `kMaxSmall` bytes, so `ownerOf` finds this VM from any cell.
    mem::LocalHeap heap;

    /// A zeroed header and room after it; `rc` is 1. `bytes` includes the header and is at most `kMaxSmall`. Throws
    /// `Error(OutOfMemory)` before taking anything when the context's limit would be passed.
    void* allocCell(std::size_t bytes, Kind kind, ObjClass cls, std::uint8_t aux = 0);
    /// Out-of-line storage: property slots, elements, tables. 16-byte aligned.
    void* allocPayload(std::size_t bytes);
    void freePayload(void* p, std::size_t bytes) noexcept;
    /// Called by `detail::destroyUnreferenced` when a count reached zero.
    void destroy(CellPtr cell) noexcept;
    /// `Options::Memory::Deferred`: frames hold their values without counts. A count that reaches zero puts the cell in
    /// the zero-count table, and `scanZct` frees what in it no frame holds.
    bool deferred = false;
    /// Free every cell of the zero-count table that no frame holds.
    void scanZct() noexcept;
    /// The same when no frame is left: the registers are nobody's, and are filled again as they are used again.
    void scanAtHost() noexcept
    {
        if (!zctCount() || (runningJobs_ && zctCount() < zctLimit_))  // (at the end of the jobs)
            return;
        if (stackInit > stackTop)
            stackInit = stackTop;
        scanZct();
    }
    /// The `gc` word of a cell in the zero-count table. A collection rewrites the word of every cell it looks at, and
    /// leaves 0 behind; it never runs with cells in the table.
    static constexpr std::uint32_t kInZct = 1;
    /// What a deferred frame does with a count that reached zero: the cell into the table (the interpreter's way, in line).
    [[gnu::always_inline]] void zeroCount(detail::GCObject* cell) noexcept
    {
        if (isVmCell(cell)) [[likely]] {
            CellHeader* h = headerOf(cell);
            if (h->gc != kInZct && zctTop_ < zctEnd_) [[likely]] {
                *zctTop_++ = cell;
                h->gc = kInZct;
                return;
            }
        } else if (!cell->pinned && !cell->inZct && zctOld_.size() < zctOld_.capacity() && zctCount() < zctLimit_) {
            zctOld_.push_back(cell);  // within its capacity: nothing to allocate
            cell->inZct = 1;
            return;
        }
        // On its way into a register, whose frame may not be there yet: into the table in any case, not freed.
        parking_ = true;
        detail::destroyUnreferenced(cell);
        parking_ = false;
    }
    /// `release` of a value a deferred frame let go of.
    [[gnu::always_inline]] void releaseDeferred(JSValue v) noexcept
    {
        if (v.isCell() && --detail::cellCount(v.asCell()) == 0)
            zeroCount(v.asCell());
    }
    /// An old-family cell (a string, symbol or bigint) whose count reached zero: true when it waits in the table.
    bool deferOld(detail::GCObject* cell) noexcept;
    /// How many cells of the new family are alive.
    std::uint64_t liveCells = 0;
    bool tearingDown = false;

    // ---- shapes ----
    /// Shape ids index this table, which is reserved once so that its address never changes.
    Shape* shapeById(std::uint32_t id) const noexcept { return shapeTable_[id & kShapeIndexMask]; }
    /// The table of shapes by id; it never moves, so compiled code can name it.
    Shape* const* shapeTable() const noexcept { return shapeTable_; }
    /// Whether `new` of `f` finds `prototype` as a data property in slot 2 (`Shape::kProtoAt2`).
    bool prototypeAt2(const JSObject* f) const noexcept;
    Shape* newShape();
    void freeShape(Shape* shape) noexcept;
    /// The root of the tree for objects with this prototype (null for none); made on first use, and it holds a count of
    /// the prototype.
    Shape* rootShapeFor(JSObject* proto);
    std::uint32_t maxShapes = kDefaultMaxShapes;
    /// Dictionary shapes inline caches were let hold: their ids are never reused, so there is a budget.
    std::uint32_t cachedDictionaries = 0;
    std::uint32_t shapesAlive() const noexcept { return shapeCount_ - std::uint32_t(freeShapeIds_.size()) - retiredShapes_ - 1; }
    /// Free the shapes of a prototype that is gone and of everything inherited from it. Their ids stay taken: native code and
    /// inline caches may still name one, and must never meet another shape under it.
    void retireShapeTree(Shape* root) noexcept;

    // ---- objects (object.cpp) ----
    /// A plain object with `aux` inline slots, with the shapes of `root`.
    JSObject* newObject(Shape* root, std::uint8_t aux = 3);
    JSObject* newObject(JSObject* proto, std::uint8_t aux = 3);
    JSArray* newArray(JSObject* proto, std::uint32_t length = 0, std::uint32_t capacity = 0);
    Box* newBox(JSValue ownedValue);
    /// An ArrayBuffer of `bytes` zeros; owned, or null with a RangeError pending when it cannot be had.
    /// A zeroed buffer of `bytes`; a resizable one has room for `maxBytes`. Null with an exception pending.
    JSObject* newArrayBuffer(std::size_t bytes, std::size_t maxBytes = 0, bool resizable = false);
    /// A typed array over `buffer` (an ArrayBuffer object, which it counts) from `byteOffset`, `length` elements.
    JSTypedArray* newTypedArray(JSObject* proto, ElemKind kind, JSObject* buffer, std::uint32_t byteOffset, std::uint32_t length);
    GetterSetter* newGetterSetter(JSValue getter, JSValue setter);
    /// A closure over `n` boxes, each of which gets a count, and a count of `code`. `length`, `name` and, for a
    /// constructor, `prototype` appear as properties on first use.
    JSFunction* newFunction(JSObject* proto, CodeBlock* code, Box* const* upvalues, std::uint32_t n);
    CodeBlock* newCodeBlock(Key name, std::uint32_t length, bool constructor);
    /// The live code block with this `uid`, or null.
    CodeBlock* codeByUid(std::uint64_t uid) const noexcept
    {
        auto it = codeByUid_.find(uid);
        return it == codeByUid_.end() ? nullptr : it->second;
    }
    void releaseCode(CodeBlock* code) noexcept;
    JSFunction* newNativeFunction(JSObject* proto, FastNative fn, void* data, Key name, std::uint32_t length);

    // ---- validity (validity.cpp) ----
    /// The cell that says whether the chain from `proto` upwards is as it was. No count is added for the caller.
    ValidityCell* validityOf(JSObject* proto);
    /// A valid cell while a store into a hole of a plain array (the usual prototype chain, no properties of its own)
    /// cannot be intercepted: neither `Array.prototype` nor `Object.prototype` has a property named by an index. Null when
    /// one has. `Array.prototype`'s own elements do not move the cell: the caller also checks that its length is 0.
    ValidityCell* holeStoreCell();
    /// `holeStoreCell` and an `Array.prototype` with no elements: whether a hole store into a plain array is plain now.
    bool plainHoleStores();
    void retainValidity(ValidityCell* cell) noexcept { ++cell->refs; }
    /// The megamorphic read cache: one table for every read site, by shape and key, that compiled code probes when the
    /// shapes it was made for do not match (`megaSlot` places an entry). Filled by the runtime after a read it could cache.
    static constexpr unsigned kMegaBits = 11;
    MegaEntry* megaRead = nullptr;
    /// `MegaEntry::index` of a key that no object of the shape has, nor its prototypes (`holder` is the first of them).
    static constexpr std::uint32_t kMegaAbsent = 0xFFFFFFFFu;
    static std::uint32_t megaKeyMix(const void* key) noexcept
    {
        return std::uint32_t((reinterpret_cast<std::uintptr_t>(key) >> 4) * 0x85EBCA6Bu);
    }
    static std::uint32_t megaSlot(std::uint32_t shape, const void* key) noexcept
    {
        return ((shape * 0x9E3779B1u) ^ megaKeyMix(key)) >> (32 - kMegaBits);
    }
    void megaClear() noexcept;
    /// The cell dictionary object `o` clears when it gains a key (made on first use; the dictionary holds a count).
    ValidityCell* absenceOf(JSObject* o);
    /// Dictionary `d` gains a key, or goes: what read through it a prototype's property no longer holds.
    void dictionaryGains(Dictionary& d) noexcept;
    void releaseValidity(ValidityCell* cell) noexcept;
    /// `proto` or something above it changed what it has: every cell that depended on it goes invalid. Does nothing for
    /// an object that is nobody's prototype.
    void invalidateChain(JSObject* proto);

    // ---- errors ----
    /// The value being thrown while a function returns the exception sentinel; owned.
    JSValue exception = JSValue::undefined();
    ErrorCode exceptionCode = ErrorCode::Ok;
    /// An error object of the class `code` names, with `message`; owned.
    JSValue makeError(ErrorCode code, std::string_view message);
    /// Give an error object its `stack`: its name and message, then the calls that lead here.
    void attachStack(JSObject* error);
    /// Make an error and leave it pending; returns the exception sentinel, so `return vm.throwError(...)` reads well.
    JSValue throwError(ErrorCode code, std::string_view message);
    void clearException() noexcept;
    /// The prototypes of the error classes, by `ErrorCode`, null for a code that has none: those use `Error`'s.
    JSObject* errorProtos[48] = {};
    JSObject* errorProto(ErrorCode code) const noexcept;

    // ---- calls (interp.cpp) ----
    /// The call stack: registers of every live frame, in one reserved range.
    JSValue* stackBase = nullptr;
    JSValue* stackEnd = nullptr;
    /// First register nobody uses: what a call from C++ puts its frame at.
    JSValue* stackTop = nullptr;
    std::uint32_t depth = 0;
    /// The live frames, innermost last, in an array that never moves; `frameCapacity` is the call depth the context allows.
    /// Back edges and calls left before the host is asked whether to stop.
    std::uint32_t ticks = 4096;
    /// `ContextImpl::interruptSet`, for compiled code.
    const std::uint32_t* interruptSet = nullptr;
    /// Calls (and loop back edges) of a function before the baseline compiler translates it; 0 keeps everything interpreted.
    std::uint32_t tier1Threshold = 0;
    /// Entries and loop back edges of baseline code before the optimizing compiler is asked; 0 leaves it off.
    std::int32_t tier2Threshold = 0;
    /// How many levels deep a function that calls itself by name runs those calls in place, from when it turns hot
    /// (`selfInline`); 0 for not at all; only with the optimizing compiler on.
    std::uint8_t selfInlining = 0;
    /// Whether a function turning hot takes the code of the functions its calls reached into its own block (`mergeInline`).
    bool mergeInlining = false;
    bool selfInliningSet = false;  ///< set by SCRIPT_VM_SELFINLINE: no deeper copy for very small functions
    /// The lowest machine stack address compiled code may reach before a call is made the slow way (0: no limit).
    std::uintptr_t jitStackFloor = 0;
    Frame* frameBase = nullptr;
    Frame* frameTop = nullptr;    ///< one past the top frame
    Frame* frameLimit = nullptr;  ///< a frame pushed at or past this is one too many (`frameBase + frameCapacity - 2`)
    std::uint32_t frameCapacity = 0;
    std::uint32_t frameCount() const noexcept { return std::uint32_t(frameTop - frameBase); }
    /// Registers below this hold a value (undefined unless a frame wrote another): the stack is filled as it is first used.
    /// Every filler has made sure of 8 registers past what it fills, so this stays 8 short of `stackEnd`, and compiled
    /// code that finds a frame (and one more) below it needs no other test of the stack's end.
    JSValue* stackInit = nullptr;
    void fillStack(JSValue* upTo) noexcept
    {
        for (; stackInit < upTo; ++stackInit)
            *stackInit = JSValue::undefined();
    }
    /// `callee(this, ...args)`. An owned result, or the exception sentinel with `exception` set. Runs script or native
    /// code; may be called from native code that script called.
    JSValue call(JSValue callee, JSValue thisValue, const JSValue* args, std::uint32_t argc);
    JSValue callInternal(JSValue callee, JSValue thisValue, const JSValue* args, std::uint32_t argc, bool construct,
                         JSValue target = JSValue::undefined());
    /// `new callee(...args)`.
    /// `new callee(...args)`; `target` is the new.target when it is not the callee (Reflect.construct).
    JSValue construct(JSValue callee, const JSValue* args, std::uint32_t argc, JSValue target = JSValue::undefined());
    /// Set while a native function runs as a constructor: the function itself; otherwise undefined. A native that
    /// constructs is handed a fresh `this` made from the function's `prototype`.
    JSValue newTarget = JSValue::undefined();
    /// The global object: `globalThis`. Owned.
    JSObject* global = nullptr;
    /// Counts the changes to the global object that can move or remove a property it already has (a delete, new attributes);
    /// compiled code that reads a global from a fixed place checks it.
    std::uint32_t globalEpoch = 0;
    /// Install the global object and the built-in functions and prototypes (builtins.cpp).
    void installBuiltins();

    CallHook callHook = nullptr;
    void* callHookData = nullptr;

    // ---- shared names and prototypes ----
    Atoms atoms;
    /// The strings `typeof` answers with, made on first use and held for the VM's life: undefined, object, boolean,
    /// number, string, symbol, bigint, function.
    JSValue typeofNames[8] = {};
    /// Set once any object has had a `Symbol.hasInstance` property: until then `instanceof` needs no look for one.
    bool hasInstanceSeen = false;
    /// Set when a proxy is on the chain above something that is a prototype: until then a proxy can only be the first object of
    /// a chain that matters.
    bool proxyAsProto = false;
    /// What `x instanceof F` found last for objects of one shape and functions of one shape whose `prototype` is one object,
    /// good while the validity cell of the chain above the object's prototype holds (it holds a count). Direct-mapped by the
    /// object's shape; compiled code reads it (offsets fixed by `InstanceEntry`).
    struct InstanceEntry
    {
        std::uint32_t shape = 0;   ///< of the object
        std::uint32_t fnShape = 0; ///< of the function
        JSObject* want = nullptr;  ///< the function's `prototype`
        ValidityCell* validity = nullptr;
        std::uint8_t result = 0;
        std::uint8_t pad[7] = {};
    };
    static constexpr std::uint32_t kInstanceEntries = 64;
    InstanceEntry instanceCache[kInstanceEntries];
    /// A prototype that something inherits from: when a proxy is on its chain, the instance cache is off for good.
    void noteProtoChain(JSObject* proto);
    void clearInstanceCache() noexcept
    {
        for (InstanceEntry& e : instanceCache) {
            if (e.validity)
                releaseValidity(e.validity);
            e = InstanceEntry{};
        }
    }
    JSValue typeofName(unsigned which);
    /// The property names of the first thousand non-negative integers, made when first asked for; each holds a count.
    static constexpr std::size_t kSmallIndexKeys = 1024;
    Key smallIndexKeys[kSmallIndexKeys] = {};
    Intrinsics protos;
    /// Install a prototype into one of `protos`'s fields, counting it and letting go of the one it replaces.
    void setIntrinsic(JSObject** slot, JSObject* object) noexcept;
    /// `intern` for the text of a name; the cell is the table's and the caller's (a count is added).
    Key internKey(std::string_view ascii);
    Key internKey(StringView text);
    /// A new symbol, owned. `registry` symbols are what `Symbol.for` makes.
    JSValue newSymbol(StringView description, bool hasDescription);
    std::map<String, Key> symbolRegistry;  ///< each holds a count

    // ---- generators (interp.cpp) ----
    /// What a generator function's call makes: an object holding the registers of the call. Owned.
    /// Compile the body of a stub (`CodeBlock::kLazy`) if `c` is one; false, with the exception thrown, when that fails.
    bool ensureCompiled(CodeBlock* c);
    JSValue makeGenerator(JSFunction* fn, JSValue thisValue, const JSValue* args, std::uint32_t argc);
    /// `next`, `throw` and `return` (mode 0, 1, 2) of a generator: an owned iterator result, or the exception sentinel.
    JSValue resumeGenerator(JSValue generator, int mode, JSValue sent);
    /// Compile `source` as a program and run it on the global object: `eval` and `new Function`. An owned result, or the
    /// exception sentinel (a syntax error is a pending SyntaxError).
    JSValue evalSource(std::string_view source, std::string_view name);
    /// A direct `eval` of `source` from the function running now, which sees that function's variables.
    JSValue evalDirect(JSValue source);
    /// The script side of each host class (`Class<T>`): its constructor (or the object of a static class) and its prototype,
    /// each holding a count. Keyed by the class record.
    std::unordered_map<const void*, std::pair<JSObject*, JSObject*>> hostBindings;
    /// The original `Array.prototype[Symbol.iterator]` and `%ArrayIteratorPrototype%.next`: while they are in place an
    /// array is iterated by index. Not counted (the prototypes hold them).
    JSValue origArrayValues = JSValue::undefined();
    JSValue origIteratorNext = JSValue::undefined();
    /// The `next` of generator objects as installed, which iteration may skip the result object for.
    JSValue origGeneratorNext = JSValue::undefined();
    /// The `next` of each family of built-in iterators (array, string, map, set, regexp) as installed, indexed by `IteratorFamily`.
    JSValue origNativeNext[8] = {};
    /// The RegExp constructor and `RegExp.prototype.exec` as made, and the slots a plain RegExp counts on staying as they are.
    JSValue regexpCtor = JSValue::undefined();
    JSValue regexpExec = JSValue::undefined();
    JSValue promiseCtor = JSValue::undefined();  ///< the Promise constructor as made
    JSValue arrayConstructor = JSValue::undefined();    ///< the Array constructor as made
    /// What `RegExp.$1`, `RegExp.lastMatch` and the like read: the last successful match of a RegExp made by the constructor.
    JSValue legacyInput = JSValue::undefined();  ///< counted
    std::int32_t legacyBounds[20] = {};
    std::uint8_t legacyGroups = 0;
    bool legacyInvalid = false;                  ///< a subclass instance matched since: the accessors throw
    static constexpr std::uint64_t kMinCellBudget = 2048;
    std::uint64_t cellBudget_ = 64 * kMinCellBudget;  ///< collect cycles when this many cells are alive
    std::uint64_t cellsAfterFull_ = 0;           ///< cells alive after the last whole collection
    std::uint32_t evalEnvSerial = 0;             ///< numbers the hidden variables that hold eval objects, so no two share a name
    bool overflowing = false;                    ///< raising a stack overflow right now
    std::string ambient;                         ///< TypeScript declarations the embedder added with `Context::declare`
    JSValue typedCtors[16] = {};                 ///< the typed array constructors as made, by ElemKind
    /// `makeString`'s strings of one character below 256 (and, last, the empty one), each counted; undefined until made.
    JSValue smallStrings[257];
    struct RegExpSlot
    {
        JSObject* owner;  ///< holds a count
        Key key;          ///< holds a count
        JSValue value;    ///< holds a count
        /// Where the value is while the owner keeps the shape it had then (`kNoShape` for a dictionary): shapes other
        /// than dictionaries are never freed, so the same shape id is the same layout.
        std::uint32_t shape;
        std::uint32_t index;
    };
    static constexpr std::uint32_t kNoShape = ~std::uint32_t(0);
    std::vector<RegExpSlot> regexpSlots;
    /// A regular expression literal's compiled program, by the string constants of its pattern and flags (each entry holds
    /// a count of both): a literal in a loop makes a RegExp each time round with no parse of its flags and no lookup by text.
    struct RegExpLiteral
    {
        JSValue pattern = JSValue::raw(0), flags = JSValue::raw(0);
        std::shared_ptr<const void> program;
        unsigned bits = 0;
    };
    RegExpLiteral regexpLiterals[64];
    std::uint32_t arrayRootShape = 0;
    /// Set by `SetEvalScope` just before a call that may be a direct `eval`.
    const CodeBlock* evalCode = nullptr;
    std::uint32_t evalIndex = 0;
    std::uint32_t evalFrames = 0;
    /// Room for the boxes a closure with many captures gathers; the interpreter cannot hold a vector across its jumps.
    std::vector<Box*> boxScratch;
    // ---- promises (builtins_promise.cpp) ----
    std::deque<MicroJob> jobs;
    void enqueueJob(int kind, JSValue a, JSValue b, JSValue c);
    /// Run queued jobs, and those they queue, until none are left; how many ran.
    std::size_t runJobs();
    /// `resumeGenerator` without the result object: `*out` is the value (owned), `*done` whether it returned.
    /// `rawResult`, when given, receives the iterator result a `yield*` passes on as it is (owned), and `*out` is then unset.
    JSValue startAsync(JSFunction* f, JSValue thisValue, const JSValue* args, std::uint32_t argc);
    bool resumeRaw(JSValue generator, int mode, JSValue sent, JSValue* out, bool* done, JSValue* rawResult = nullptr);
    /// What a generator'"'"'s `return(v)` throws at the `yield` it waits in, so that `finally` blocks run and `catch` blocks do
    /// not; `returnValue` is `v`. Both owned.
    JSValue returnMarker = JSValue::undefined();
    JSValue returnValue = JSValue::undefined();
    /// Set while a `throw` is being delivered to a suspended generator.
    bool resumeThrow = false;
    /// The generator (owned) that an async function became when it came to its first `await`, for `startAsync`.
    JSValue becameGenerator = JSValue::undefined();
    /// Whether the last suspension of a generator frame was an `await` rather than a `yield`.
    bool suspendedOnAwait = false;

private:
    friend struct JSObject;
    Shape** shapeTable_ = nullptr;
    std::uint32_t shapeCount_ = 1;  ///< id 0 is "no shape"
    std::uint32_t retiredShapes_ = 0;
    std::uint32_t shapeCommitted_ = 0;
    Vec<std::uint32_t> freeShapeIds_;
    Shape* nullRoot_ = nullptr;
    void* shapeRegionBase_ = nullptr;
    std::size_t shapeRegionBytes_ = 0;
    void destroyShape(Shape* shape) noexcept;

    Vec<CellPtr> pending_;
    bool destroying_ = false;
    /// `Options::freeBudget`: cells whose count reached zero, left for later. While there are any, the cell budget is 0,
    /// so every allocation (compiled code's too) goes through `allocCell`, which frees some of them first.
    Vec<CellPtr> deferredFree_;
    std::uint64_t savedCellBudget_ = 0;
    void freeDeferred(std::size_t max) noexcept;
public:
    /// The zero-count table (`deferred`): cells of this family in [zctBase_, zctTop_), each marked by `gc == kInZct`.
    /// Compiled code adds a cell it makes while `zctTop_` is below `zctEnd_`, the table's limit; past that, an
    /// allocation goes through `allocCell`, which scans. Room past the limit up to `zctCap_` is for the runtime.
    CellPtr* zctBase_ = nullptr;
    CellPtr* zctTop_ = nullptr;
    CellPtr* zctEnd_ = nullptr;

private:
    CellPtr* zctCap_ = nullptr;
    /// Old-family cells of the table, which may be in it more than once.
    Vec<detail::GCObject*> zctOld_;
    std::size_t zctLimit_ = 0;
    std::size_t zctCount() const noexcept { return std::size_t(zctTop_ - zctBase_) + zctOld_.size(); }
    /// Into the table, marked; out of memory, left to a collection (a cell with no count).
    void zctPush(CellPtr cell) noexcept;
    /// The limit after a scan: what stayed, weighed against what a scan costs.
    void setZctLimit() noexcept;
    /// Above 0, a count that reaches zero frees at once: a scan or a collection runs, with the frames' cells counted.
    std::uint32_t immediate_ = 0;
    /// One count more (`delta` 1) or fewer (-1) for every cell a frame's register holds.
    void countRoots(int delta) noexcept;
    void noteZct() noexcept;
    void auditCounts() noexcept;
    void freeZct() noexcept;
    bool rootsCounted_ = false;
    bool parking_ = false;
    bool runningJobs_ = false;
    std::size_t scanned_ = 0;  ///< registers the last scan went over
    void restoreCellBudget() noexcept;
    static constexpr std::size_t kZctMin = 256;
    std::size_t zctMin_ = kZctMin;

public:
    static constexpr std::size_t kValidityChunkBytes = std::size_t(64) << 10;

private:
    void* stackRegionBase_ = nullptr;
    std::size_t stackRegionBytes_ = 0;
    Vec<CodeBlock**> codeRoots_;
    std::unordered_map<std::uint64_t, CodeBlock*> codeByUid_;
    std::uint64_t nextCodeUid_ = 1;

public:
    /// A program's code block, which the VM lets go of when it ends if the program has not.
    void addCodeRoot(CodeBlock** slot);
    void removeCodeRoot(CodeBlock** slot) noexcept;

private:
    Vec<void*> validityChunks_;
    ValidityCell* validityFree_ = nullptr;
    ValidityCell* holeCell_ = nullptr;
    bool holePlain_ = false;
    ValidityCell* allocValidity();
    void freeValidity(ValidityCell* cell) noexcept;
    void releaseShapeId(std::uint32_t id) noexcept;

    void destroyNow(CellPtr cell) noexcept;
    friend struct Shape;
};

// A frame register written by the runtime in either mode (`VM::deferred`: frames hold values without counts).
/// = `value`, whose count the caller has taken.
inline void setRegister(VM& vm, JSValue& slot, JSValue value) noexcept
{
    if (vm.deferred) {
        slot = value;
        vm.releaseDeferred(value);
        return;
    }
    const JSValue old = slot;
    slot = value;
    release(old);
}
/// = `value`, which the caller holds.
inline void copyRegister(VM& vm, JSValue& slot, JSValue value) noexcept
{
    if (!vm.deferred)
        retain(value);
    const JSValue old = slot;
    slot = value;
    if (!vm.deferred)
        release(old);
}
/// Undefined, what it held let go of.
inline void clearRegister(VM& vm, JSValue& slot) noexcept
{
    const JSValue old = slot;
    slot = JSValue::undefined();
    if (!vm.deferred)
        release(old);
}

/// The language registration of JavaScript on this core.
const LanguageInfo& javascriptLanguage() noexcept;

/// The VM a cell of the new family belongs to.
inline VM& vmOf(const void* cell) noexcept
{
    return *static_cast<VM*>(mem::LocalHeap::ownerOf(cell));
}

/// The bytes a cell of the new family occupies, as it was allocated.
std::size_t cellBytesOf(const detail::GCObject* cell) noexcept;
/// Every cell the cell holds a count of, appended to `out`.
/// With `tracing`, what a weak table holds is left out: the collector follows it by its keys.
void children(const detail::GCObject* cell, detail::GcList& out, bool tracing = false);
/// Bytes the cell and its out-of-line storage hold.
std::size_t heapSizeOf(const detail::GCObject* cell) noexcept;

}  // namespace vm
}  // namespace internal
}  // namespace script
