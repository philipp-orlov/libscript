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

struct Tier2Code;


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
    alignas(8) std::uint32_t refs;
    std::uint32_t valid;
    std::uint32_t holes = 0;  ///< of a prototype chain (`VM::plainHoles`): 1 no element can be met on it, 2 perhaps one
};

/// An element site's array shape whose prototypes have no element, other than a plain array's (`CodeBlock::holeRecs`), with
/// the validity cell that says so, of which it holds a count.
struct HoleRec
{
    std::uint32_t shape = 0;
    std::uint32_t pad = 0;
    ValidityCell* cell = nullptr;
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
    Key toString_ = nullptr;
    Key valueOf_ = nullptr;
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
        // (First of all: with no frame left a free is done at once, and what is above the top may name what it frees.)
        if (stackInit > stackTop)
            stackInit = stackTop;
        if (shadowLen_)
            dropShadow();
        if (!zctCount() && youngTop_ == youngBase_)
            return;
        if (runningJobs_ && zctCount() < zctLimit_) {  // (the table at the end of the jobs)
            // Between jobs: the young cells only, so that what no frame holds is freed at once again.
            collectYoungAtHost();
            return;
        }
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
            if (h->gc == 0 && zctTop_ < zctEnd_) [[likely]] {
                *zctTop_++ = cell;
                h->gc = kInZct;
                return;
            }
            if (youngHeader(h))  // a young cell waits for the scan, in its list
                return;
        } else if (!cell->pinned && !cell->inZct && cell->gcBytes < kLargeCell && zctOld_.size() < zctOld_.capacity() &&
                   zctCount() < zctLimit_) {
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
    Shape* rootShapeFor(JSObject* proto, std::uint8_t aux);
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
    /// The record of element site `index` of `code` (`CodeBlock::holeRecs`), made on first need.
    HoleRec* holeRecAt(CodeBlock* code, std::uint32_t index);
    /// After `plainHoles(a)` held at element site `index` of `code`: the site's record names `a`'s shape and its cell.
    void noteHoleShape(CodeBlock* code, std::uint32_t index, JSArray* a);
    /// `holeStoreCell` and an `Array.prototype` with no elements: whether a hole store into a plain array is plain now.
    bool plainHoleStores();
    /// Whether a hole of `a`, or an index past its end, is met by nothing: no index among its own names, no element and no
    /// proxy above it.
    bool plainHoles(JSArray* a);
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
    /// Moves it: optimized code that relies on it (`epochCodes`) is made to leave where it next would.
    void epochMoved() noexcept;
    std::vector<Tier2Code*> epochCodes;
    /// What is known of the global object's slots, a byte for each of the first `kWatchable`: how often it was written
    /// (`kWatchWrites`, as far as that was seen), that optimized code takes its value for a constant (`kWatched`: the next
    /// write moves `globalEpoch`), or that one did and was wrong (`kNotConstant`).
    static constexpr std::uint32_t kWatchable = 8192;
    static constexpr std::uint8_t kWatchWrites = 3, kWatched = 4, kNotConstant = 8;
    std::unique_ptr<std::uint8_t[]> globalWatch;
    /// The index of one of the global object's slots.
    std::uint32_t globalSlotIndex(const JSValue* slot) const noexcept;
    /// A write of a slot of `o`, about to be done or just done, wherever the runtime does one.
    void slotWritten(const JSObject* o, const JSValue* slot) noexcept
    {
        if (o == global && globalWatch) [[unlikely]]
            globalWritten(slot);
        else if (reinterpret_cast<const CellHeader*>(o)->flags & cellflags::kIsProto) [[unlikely]]
            protoWritten(o);
    }
    void protoWritten(const JSObject* o) noexcept;
    /// The same for a captured variable's box.
    void boxWritten(Box* box) noexcept;
    /// Another function, `f`, has the code `code`.
    void functionMade(CodeBlock* code, JSFunction* f) noexcept;
    /// Optimized code may take the method found on `holder`, through the prototypes from `first` on, for a constant: any
    /// change of those moves `globalEpoch` from now on. False when they have changed too often.
    bool watchMethod(JSObject* first, JSObject* holder);
    /// Moves an object to shapes no inline cache has seen, so that every write of it goes through the runtime.
    bool ownTree(JSObject* o);
    void watchedChanged(Rare& r) noexcept;
    void globalWritten(const JSValue* slot) noexcept;
    /// Optimized code may take the value of the global object's slot `index` for a constant: watched from now on.
    bool watchGlobal(std::uint32_t index) noexcept;
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
    /// The same for the integers below `kIndexKeys` (an object used as a map by ids): 1024 to a chunk, each chunk made when
    /// first asked for. Bounded: at most `kIndexKeys` names stay for the life of the VM.
    static constexpr std::size_t kIndexKeys = 65536;
    Key* indexKeyChunks[kIndexKeys / kSmallIndexKeys] = {};
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
    JSValue origIteratorNext = JSValue::undefined();  ///< the `next` of array iterators, as made
    /// The `next` of generator objects as installed, which iteration may skip the result object for.
    JSValue origGeneratorNext = JSValue::undefined();
    JSValue origIteratorSelf = JSValue::undefined();  ///< %IteratorPrototype%[Symbol.iterator] as made
    std::uint32_t generatorEpoch = ~0u;               ///< where `globalEpoch` was when both were found as made
    /// `%AsyncGeneratorPrototype%.next` and `%AsyncIteratorPrototype%[Symbol.asyncIterator]` as made, and where
    /// `globalEpoch` was when both were found so.
    JSValue origAsyncGenNext = JSValue::undefined();
    JSValue origAsyncIteratorSelf = JSValue::undefined();
    std::uint32_t asyncGeneratorEpoch = ~0u;
    /// Where `globalEpoch` was when `Object.prototype` was found with no `then`, and plain objects' shapes over it found
    /// with none of their own since (`bi::noThen`): such an object is no thenable while it stands.
    std::uint32_t noThenEpoch = ~0u;
    std::uint32_t noThenShapes[4] = {};
    std::uint32_t noThenNext = 0;
    /// The `next` of each family of built-in iterators (array, string, map, set, regexp) as installed, indexed by `IteratorFamily`.
    JSValue origNativeNext[8] = {};
    /// The RegExp constructor and `RegExp.prototype.exec` as made, and the slots a plain RegExp counts on staying as they are.
    JSValue regexpCtor = JSValue::undefined();
    JSValue regexpExec = JSValue::undefined();
    JSValue promiseCtor = JSValue::undefined();  ///< the Promise constructor as made
    JSValue arrayConstructor = JSValue::undefined();    ///< the Array constructor as made
    FastNative functionCallNative = nullptr;  ///< `Function.prototype.call`'s (`unwrapCall`)
    /// What `RegExp.$1`, `RegExp.lastMatch` and the like read: the last successful match of a RegExp made by the constructor.
    JSValue legacyInput = JSValue::undefined();  ///< counted
    std::int32_t legacyBounds[20] = {};
    std::uint8_t legacyGroups = 0;
    bool legacyInvalid = false;                  ///< a subclass instance matched since: the accessors throw
    static constexpr std::uint64_t kMinCellBudget = 16384;
    std::uint64_t cellBudget_ = 8 * kMinCellBudget;  ///< collect cycles when this many cells are alive
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
    /// Where `globalEpoch` was when all of them were found as they were made, and watched; nothing has changed while it stands
    /// there.
    std::uint32_t regexpEpoch = ~0u;
    /// `[Symbol.iterator]` of Set's and Map's prototypes as made, and where `globalEpoch` was when they were found so.
    JSValue tableIterators[2] = {JSValue::undefined(), JSValue::undefined()};
    std::uint32_t tableEpoch[2] = {~0u, ~0u};
    /// The slot of `Array[Symbol.species]` as made, and where `globalEpoch` was when arrays were found to make plain arrays.
    JSValue arraySpecies = JSValue::undefined();
    std::uint32_t arraySpeciesEpoch = ~0u;
    /// `Promise.prototype.then` and the slot of `Promise[Symbol.species]` as made, and where `globalEpoch` was when the language
    /// was found to have them still (`promisePristine`).
    JSValue promiseThen = JSValue::undefined(), promiseSpecies = JSValue::undefined();
    std::uint32_t promiseEpoch = ~0u;
    /// The root shape of a promise of `protos.promise` (three slots), which lives as long as that prototype: found once.
    Shape* promiseRoot = nullptr;
    /// Prototypes found to have no enumerable name, and the shape they had then (compared, not followed).
    struct ForInProto
    {
        const JSObject* object = nullptr;
        std::uint32_t shape = 0;
    };
    static constexpr std::size_t kForInProtos = 8;
    ForInProto forInProtos[kForInProtos];
    std::size_t jsonRoom = 0;  ///< how many characters the next JSON.stringify starts with room for
    /// A regular expression literal's compiled program, by the string constants of its pattern and flags (each entry holds
    /// a count of both): a literal in a loop makes a RegExp each time round with no parse of its flags and no lookup by text.
    struct RegExpLiteral
    {
        JSValue pattern = JSValue::raw(0), flags = JSValue::raw(0);
        std::shared_ptr<void> text;  ///< what its RegExps share (builtins_regexp.cpp)
    };
    RegExpLiteral regexpLiterals[64];
    std::uint32_t arrayRootShape = 0;
    /// The shape a match's array starts with and the one it has with its `index`, `input` and `groups` (0: none made yet).
    std::uint32_t matchRoot = 0, matchShape = 0;
    /// The shape of an iterator result `{value, done}` with its two slots in place (0: none made yet).
    std::uint32_t iterResultShape = 0;
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

    /// Young cells (`deferred` only; docs/vm-core.md, "Young cells"). A young cell has `kYoungGc` set in its `gc` word,
    /// its allocation site above bit 8, and is in the list [youngBase_, youngTop_). It holds no count of what it refers to;
    /// its own count is what old cells and the runtime hold. A scan marks the young cells that frames and counts reach,
    /// makes them old, and frees the others without looking at what they hold. Compiled code adds a cell it makes while
    /// `youngTop_` is below `youngEnd_` and its site makes young cells.
    bool young = false;  ///< on with `deferred`
    static constexpr std::uint32_t kYoungGc = 2, kYoungMarked = 4, kSiteShift = 8;
    static constexpr std::size_t kYoungCells = 4096;  ///< the list's room: a scan when it is full
    CellPtr* youngBase_ = nullptr;
    CellPtr* youngTop_ = nullptr;
    CellPtr* youngEnd_ = nullptr;
    /// Where young cells are made: compiled code's allocations, one site each. A site whose cells nearly all live past the
    /// scan after them makes old ones from then on (`old`): making a cell old costs more than counting as it is filled.
    struct AllocSite
    {
        std::uint32_t made = 0, kept = 0;
        std::uint8_t old = 0;
    };
    static constexpr std::uint32_t kSiteWindow = 4096;  ///< cells of a site looked at before it is judged
    /// The site of instruction `index` of `code`, made on first use (0, which is no site, when there is no room).
    std::uint32_t allocSiteFor(CodeBlock* code, std::uint32_t index);
    AllocSite& allocSite(std::uint32_t id) noexcept { return siteChunks_[id >> 10][id & 1023]; }
    /// A young cell into the list, made at `site`; false when it must be old (the list is full, or the site makes old ones).
    bool addYoung(CellPtr cell, std::uint32_t site) noexcept
    {
        if (youngTop_ == youngEnd_ || !site || allocSite(site).old)
            return false;
        *youngTop_++ = cell;
        headerOf(cell)->gc = kYoungGc | (site << kSiteShift);
        headerOf(cell)->rc = 0;
        return true;
    }
    /// Whether the cell with header `h` is young. Only while the list holds any: a collection, which runs with none, writes
    /// the `gc` word of every cell it looks at.
    bool youngHeader(const CellHeader* h) const noexcept { return youngTop_ != youngBase_ && (h->gc & kYoungGc); }
    /// Whether a new plain object may be young now.
    bool youngAllowed() const noexcept { return young && !immediate_ && !destroying_ && youngTop_ != youngEnd_; }
    /// A young cell made old now: it takes a count of each cell it refers to. What wants to treat it as any other cell
    /// (the runtime's general paths, a class change, a prototype) calls this first.
    void makeOld(CellPtr cell) noexcept;
    void makeOldIfYoung(JSObject* o) noexcept
    {
        if (youngHeader(headerOf(reinterpret_cast<CellPtr>(o)))) [[unlikely]]
            makeOld(reinterpret_cast<CellPtr>(o));
    }

private:
    Vec<CellPtr> youngWork_;
    Vec<AllocSite*> siteChunks_;
    std::uint32_t siteCount_ = 1;  ///< 0 is no site
    /// Mark, make old and free the young cells, as said above. `registers`: the frames' cells are not counted now, and are
    /// looked at as well.
    void collectYoung(bool registers) noexcept;
    void collectYoungAtHost() noexcept;
    /// Every young cell made old (the context's end).
    void allYoungOld() noexcept;
    /// The registers a scan counts: [stackBase, liveTop()).
    JSValue* liveTop() const noexcept;
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
    /// The registers as a scan last counted them: each cell there holds a count of it from then until the next scan, which
    /// counts only the registers that changed (a deep stack that stays costs a compare a register, not a miss a cell).
    /// [shadow_, shadow_ + shadowLen_) mirrors [stackBase, ...).
    JSValue* shadow_ = nullptr;
    std::size_t shadowLen_ = 0, shadowCap_ = 0, shadowCells_ = 0;
    /// Count the registers through the shadow; false, with the shadow empty, when it could not grow (count them with
    /// `countRoots` then).
    bool syncShadow() noexcept;
    /// The shadow lets go of its counts (no frame is left, or the context ends).
    void dropShadow() noexcept;
    /// A count of a frame's cell fewer: at zero, back into the table.
    void uncountRoot(detail::GCObject* c) noexcept;
    /// Cells a scan counted outside the shadow, for the scan only.
    Vec<detail::GCObject*> transient_;
    void dropTransient() noexcept;
    void noteZct() noexcept;
    void noteLarge(std::size_t bytes) noexcept;
    static constexpr std::size_t kLargeCell = std::size_t(32) << 10, kLargeScan = std::size_t(2) << 20;
    std::size_t largeSinceScan_ = 0;  ///< bytes of those noted since the last scan
    void auditCounts() noexcept;
    void freeZct() noexcept;
    bool rootsCounted_ = false;
    bool rootsTransient_ = false;  ///< the collection counted the registers with `countRoots`
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
    /// What a run of frees let go of, summed and counted once at its end.
    struct Freed
    {
        std::size_t bytes = 0, cells = 0;
    };
    /// Frees `cell` and what it alone held.
    void destroyFrom(CellPtr cell, Freed& total) noexcept;
    void countFreed(const Freed& f) noexcept;
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
