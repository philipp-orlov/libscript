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
    CodeBlock* code;
    JSFunction* fn;
    const struct Ins* pc;       ///< where to go on in this frame: set when it calls out
    JSValue* R;
    std::uint16_t retDst;       ///< the caller's register that receives the result
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
    /// How many cells of the new family are alive.
    std::uint64_t liveCells = 0;
    bool tearingDown = false;

    // ---- shapes ----
    /// Shape ids index this table, which is reserved once so that its address never changes.
    Shape* shapeById(std::uint32_t id) const noexcept { return shapeTable_[id]; }
    Shape* newShape();
    void freeShape(Shape* shape) noexcept;
    /// The root of the tree for objects with this prototype (null for none); made on first use, and it holds a count of
    /// the prototype.
    Shape* rootShapeFor(JSObject* proto);
    std::uint32_t maxShapes = kDefaultMaxShapes;
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
    void releaseCode(CodeBlock* code) noexcept;
    JSFunction* newNativeFunction(JSObject* proto, FastNative fn, void* data, Key name, std::uint32_t length);

    // ---- validity (validity.cpp) ----
    /// The cell that says whether the chain from `proto` upwards is as it was. No count is added for the caller.
    ValidityCell* validityOf(JSObject* proto);
    void retainValidity(ValidityCell* cell) noexcept { ++cell->refs; }
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
    /// Calls (and loop back edges) of a function before the baseline compiler translates it; 0 keeps everything interpreted.
    std::uint32_t tier1Threshold = 0;
    /// Entries and loop back edges of baseline code before the optimizing compiler is asked; 0 leaves it off.
    std::int32_t tier2Threshold = 0;
    /// The lowest machine stack address compiled code may reach before a call is made the slow way (0: no limit).
    std::uintptr_t jitStackFloor = 0;
    Frame* frameBase = nullptr;
    std::uint32_t frameCount = 0;
    std::uint32_t frameCapacity = 0;
    /// Registers below this hold a value (undefined unless a frame wrote another): the stack is filled as it is first used.
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
    /// The property names of the first thousand non-negative integers, made when first asked for; each holds a count.
    static constexpr std::size_t kSmallIndexKeys = 1024;
    Key smallIndexKeys[kSmallIndexKeys] = {};
    Intrinsics protos;
    /// The shape of an ordinary function whose `prototype` is its third property (0 until one is seen).
    std::uint32_t fnProtoShape = 0;
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
    /// The RegExp constructor and `RegExp.prototype.exec` as made, and the slots a plain RegExp counts on staying as they are.
    JSValue regexpCtor = JSValue::undefined();
    JSValue regexpExec = JSValue::undefined();
    JSValue promiseCtor = JSValue::undefined();  ///< the Promise constructor as made
    JSValue arrayConstructor = JSValue::undefined();    ///< the Array constructor as made
    bool overflowing = false;                    ///< raising a stack overflow right now
    std::string ambient;                         ///< TypeScript declarations the embedder added with `Context::declare`
    JSValue typedCtors[16] = {};                 ///< the typed array constructors as made, by ElemKind
    struct RegExpSlot
    {
        JSObject* owner;  ///< holds a count
        Key key;          ///< holds a count
        JSValue value;    ///< holds a count
    };
    std::vector<RegExpSlot> regexpSlots;
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
    bool resumeRaw(JSValue generator, int mode, JSValue sent, JSValue* out, bool* done, JSValue* rawResult = nullptr);
    /// What a generator'"'"'s `return(v)` throws at the `yield` it waits in, so that `finally` blocks run and `catch` blocks do
    /// not; `returnValue` is `v`. Both owned.
    JSValue returnMarker = JSValue::undefined();
    JSValue returnValue = JSValue::undefined();
    /// Set while a `throw` is being delivered to a suspended generator.
    bool resumeThrow = false;
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

public:
    static constexpr std::size_t kValidityChunkBytes = std::size_t(64) << 10;

private:
    void* stackRegionBase_ = nullptr;
    std::size_t stackRegionBytes_ = 0;
    Vec<CodeBlock**> codeRoots_;

public:
    /// A program's code block, which the VM lets go of when it ends if the program has not.
    void addCodeRoot(CodeBlock** slot);
    void removeCodeRoot(CodeBlock** slot) noexcept;

private:
    Vec<void*> validityChunks_;
    ValidityCell* validityFree_ = nullptr;
    ValidityCell* allocValidity();
    void freeValidity(ValidityCell* cell) noexcept;

    void destroyNow(CellPtr cell) noexcept;
    friend struct Shape;
};

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
