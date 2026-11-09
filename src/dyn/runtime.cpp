// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The dynamic engine's runtime: intrinsics, allocation helpers, host class
// materialisation, the job queue and the opcode name table.
#include "script/internal/dyn/runtime.hpp"
#if SCRIPT_ENABLE_DYN_JIT
#include "script/internal/dyn/jit.hpp"
#endif

#include <algorithm>
#include <cstdio>

#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/vm.hpp"

namespace script {
namespace internal {
namespace dyn {

// ------------------------------------------------------------ bytecode ---

/// How many 32-bit operands follow an opcode.
int operandsOf(Op op) noexcept
{
    switch (op) {
        case Op::DefGlobal: return 2;
        case Op::PushConst:
        case Op::PopN:
        case Op::GetLocal:
        case Op::SetLocal:
        case Op::GetCell:
        case Op::SetCell:
        case Op::NewCell:
        case Op::RenewCell:
        case Op::GetUpval:
        case Op::SetUpval:
        case Op::GetGlobal:
        case Op::SetGlobal:
        case Op::TypeofGlobal:
        case Op::DeleteGlobal:
        case Op::ThrowConstAssign:
        case Op::GetElem:
        case Op::SetElem:
        case Op::GetElemDup:
        case Op::GetProp:
        case Op::SetProp:
        case Op::GetPropDup:
        case Op::GetPropBound:
        case Op::DeleteProp:
        case Op::DefField:
        case Op::DefGetter:
        case Op::DefSetter:
        case Op::DefMethod:
        case Op::ClassSetup:
        case Op::GetSuper:
        case Op::SetSuper:
        case Op::NewArray:
        case Op::NewObject:
        case Op::NewRegExp:
        case Op::Closure:
        case Op::Call:
        case Op::New:
        case Op::CallSuper:
        case Op::Jump:
        case Op::JumpIfFalse:
        case Op::JumpIfTrue:
        case Op::JumpIfFalseKeep:
        case Op::JumpIfTrueKeep:
        case Op::JumpIfNullishKeep:
        case Op::JumpIfNotNullishKeep:
        case Op::JumpSub:
        case Op::ForInNext:
        case Op::IterNext:
        case Op::IterCloseAt:
        case Op::NewPrivateName:
        case Op::DefPrivateAccessor:
        case Op::DefPrivate:
        case Op::DefAccessorElem:
        case Op::CallSuperVia:
        case Op::Yield:
        case Op::DelegateCall:
        case Op::DelegateCheck:
        case Op::DelegateDone:
        case Op::AsyncIterCheck:
        case Op::AsyncIterClose: return 1;
        default: return 0;
    }
}

const char* opName(Op op) noexcept
{
    static const char* const names[] = {
        "Nop", "PushUndefined", "PushNull", "PushTrue", "PushFalse", "PushConst", "PushThis",
        "PushCallee",
        "Pop", "PopN", "Dup", "Dup2", "Swap", "Rot3",
        "GetLocal", "SetLocal", "GetCell", "SetCell", "NewCell", "RenewCell", "GetUpval",
        "SetUpval", "GetGlobal", "SetGlobal", "DefGlobal", "TypeofGlobal", "DeleteGlobal",
        "GetProp", "SetProp", "GetPropDup", "GetPropBound", "GetElem", "SetElem", "GetElemDup", "DeleteProp",
        "DeleteElem", "DefField", "DefGetter", "DefSetter", "DefElem", "DefMethod",
        "DefMethodElem", "ObjectRest", "ClassSetup", "SetProtoOf", "GetSuper", "SetSuper", "SetSuperElem",
        "GetSuperElem",
        "NewObject", "NewArray", "ArrayAppend", "ArraySpread", "ObjectSpread", "Closure", "DirectEval", "NewRegExp",
        "Call", "CallSpread", "New", "NewSpread", "CallSuper", "CallSuperSpread", "Return",
        "ReturnUndefined",
        "Jump", "JumpIfFalse", "JumpIfTrue", "JumpIfFalseKeep", "JumpIfTrueKeep",
        "JumpIfNullishKeep", "JumpIfNotNullishKeep", "JumpSub", "EndFinally", "Throw", "ThrowConstAssign",
        "ForInStart", "ForInNext", "IterStart", "IterNext", "IterNextOpt", "IterRest",
        "IterClose", "IterStartLoop", "IterCloseAt", "IterCloseThrow", "NewPrivateName", "PrivateIn", "DefPrivate", "DefPrivateMethod", "DefPrivateAccessor", "DefAccessorElem", "CheckThis", "DerivedReturn", "PushNewTarget", "CallSuperVia", "CallSuperViaSpread", "SetFieldInit", "CallStaticInit", "SetHome", "CoStart", "Yield", "Await", "DelegateStart", "DelegateCall", "DelegateCheck", "DelegateDone", "AsyncIterStart", "AsyncIterCall", "AsyncIterCheck", "AsyncIterClose",
        "Add", "Sub", "Mul", "Div", "Mod", "Pow", "Neg", "Pos", "BitAnd", "BitOr", "BitXor",
        "BitNot", "Shl", "Shr", "UShr", "Eq", "Ne", "StrictEq", "StrictNe", "Lt", "Le", "Gt",
        "Ge", "In", "InstanceOf", "Not", "TypeOf", "Inc", "Dec", "ToNumber", "ToString",
        "ToObject", "ToPropertyKey", "Void", "QCompareJump", "QGetLocal2", "QGetLocalConst", "QSetLocalPop", "QGetLocalProp", "QThisProp", "QIncLocal", "QDecLocal",
    };
    static_assert(sizeof(names) / sizeof(names[0]) == std::size_t(Op::OpCount),
                  "every opcode needs a name");
    const std::size_t index = std::size_t(op);
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : "?";
}

std::uint32_t FunctionCode::lineAt(std::uint32_t pc) const noexcept
{
    std::uint32_t line = 0;
    for (const auto& entry : lines) {
        if (entry.first > pc)
            break;
        line = entry.second;
    }
    return line;
}

// -------------------------------------------------------------- program ---

namespace {

void collectNames(const FunctionCode& code, std::vector<std::string>& out)
{
    for (const auto& child : code.children) {
        if (!child->name.empty())
            out.push_back(child->name);
        collectNames(*child, out);
    }
}

std::size_t codeBytes(const FunctionCode& code) noexcept
{
    std::size_t bytes = sizeof(FunctionCode) + code.code.size() + code.consts.size() * sizeof(Value) +
                        code.atoms.size() * sizeof(void*) + code.handlers.size() * sizeof(Handler) +
                        code.lines.size() * sizeof(std::pair<std::uint32_t, std::uint32_t>);
    for (const auto& child : code.children)
        bytes += codeBytes(*child);
    return bytes;
}

}  // namespace

std::vector<std::string> Program::methods() const
{
    std::vector<std::string> names;
    if (code)
        collectNames(*code, names);
    return names;
}

std::size_t Program::imageSize() const noexcept
{
    return code ? codeBytes(*code) : 0;
}

/// Run a compiled program in the global scope: the `run` hook every dynamic
/// language shares.
Value runProgramUnit(ContextImpl& ctx, CompiledUnit& unit, const std::vector<Value>&)
{
    Runtime& rt = ctx.dyn();
    rt.syncHostClasses();
    return runProgram(rt, static_cast<Program&>(unit).code);
}

// -------------------------------------------------------------- runtime ---

static_assert(sizeof(DynFunction) <= mem::LocalHeap::kMaxSmall,
              "the largest cell should still come from a pooled size class");

Runtime::Runtime(ContextImpl& context) : ctx(context)
{
#if SCRIPT_ENABLE_DYN_JIT
    jitBackend = typed::createHostBackend();
    if (jitBackend)
        jitBackend->cache().setLimit(ctx.options.jit.codeCacheLimit);
#endif
    objectProto.reset(newCell<DynObject>(), true);

    // Function.prototype is itself callable (it returns undefined) but is not a constructor. It has to be a real
    // DynFunction: `asFunction` casts on `cls`, so a plain object tagged as a function would be read past its end.
    {
        auto* callable = newCell<DynFunction>();
        callable->setProto(objectProto.get());
        callable->fnKind = FnKind::Native;
        callable->native = [](CallInfo&) { return Value::undefined(); };
        functionProto.reset(callable, true);
    }
    globalObject.reset(newObject(objectProto.get()), true);

    atom.length.reset(intern("length"));
    atom.prototype.reset(intern("prototype"));
    atom.constructor.reset(intern("constructor"));
    atom.name.reset(intern("name"));
    atom.message.reset(intern("message"));
    atom.stack.reset(intern("stack"));
    atom.value.reset(intern("value"));
    atom.done.reset(intern("done"));
    atom.next.reset(intern("next"));
    atom.then.reset(intern("then"));
    atom.toString.reset(intern("toString"));
    atom.valueOf.reset(intern("valueOf"));
    atom.protoName.reset(intern("__proto__"));
    atom.arguments.reset(intern("arguments"));
    atom.toJSON.reset(intern("toJSON"));

    auto wellKnown = [this](const char* description) {
        auto* s = newCell<SymbolCell>();
        s->description = ascii(description);
        return s;
    };
    sym.iterator.reset(wellKnown("Symbol.iterator"), true);
    sym.asyncIterator.reset(wellKnown("Symbol.asyncIterator"), true);
    sym.toPrimitive.reset(wellKnown("Symbol.toPrimitive"), true);
    sym.toStringTag.reset(wellKnown("Symbol.toStringTag"), true);
    sym.hasInstance.reset(wellKnown("Symbol.hasInstance"), true);
    sym.match.reset(wellKnown("Symbol.match"), true);
    sym.matchAll.reset(wellKnown("Symbol.matchAll"), true);
    sym.replace.reset(wellKnown("Symbol.replace"), true);
    sym.search.reset(wellKnown("Symbol.search"), true);
    sym.split.reset(wellKnown("Symbol.split"), true);
    sym.species.reset(wellKnown("Symbol.species"), true);
    sym.unscopables.reset(wellKnown("Symbol.unscopables"), true);
    sym.isConcatSpreadable.reset(wellKnown("Symbol.isConcatSpreadable"), true);

    installBuiltins(*this);
    for (DynObject* builtin : {objectProto.get(), functionProto.get(), arrayProto.get()})
        builtin->pin();
    // What was built before this runtime could hand out shapes gets them now, so sites can learn reads from Math, JSON,
    // the constructors and their prototypes.
    for (const Prop& p : globalObject->props) {
        DynObject* o = globalObject->accessorOf(p) ? nullptr : asObject(p.value);
        if (!o)
            continue;
        giveShape(o);
        if (const Prop* proto = o->findOwn(atom.prototype.get()); proto && !o->accessorOf(*proto))
            if (DynObject* po = asObject(proto->value))
                giveShape(po);
    }
}

void Runtime::giveShape(DynObject* o)
{
    if (o->shape)
        return;
    Shape* s = rootShapeFor(o->proto.get());
    for (std::size_t i = 0; i < o->props.size(); ++i) {
        if (!s)
            return;
        s = shapeAfter(s, o->keyAt(i), o->attrsAt(i));
    }
    if (!s)
        return;
    o->shape = s;
    if (o->rare) {
        o->rare->keys.clear();
        o->rare->keys.shrink_to_fit();
        o->reindex();
    }
}

Runtime::~Runtime() = default;

namespace {

/// Empty a compiled function's references into the heap, and its children's.
void releaseCode(const FunctionCode& code) noexcept
{
    // The code is immutable to the VM; teardown is the one place that
    // changes it, after the last instruction has run.
    auto& mutableCode = const_cast<FunctionCode&>(code);
    mutableCode.consts.clear();
    mutableCode.atoms.clear();
    mutableCode.jitKeep.clear();
    for (const auto& child : mutableCode.children)
        releaseCode(*child);
}

}  // namespace

void Runtime::adoptEvalCode(const std::shared_ptr<const FunctionCode>& code)
{
    if (evalCodes.size() >= 64)
        evalCodes.erase(std::remove_if(evalCodes.begin(), evalCodes.end(), [](const std::weak_ptr<const FunctionCode>& c) { return c.expired(); }),
                        evalCodes.end());
    evalCodes.push_back(code);
}

void Runtime::adoptProgram(const std::shared_ptr<Program>& program)
{
    // Forget the ones already gone while we are here, so the list stays the
    // size of the live set rather than growing with every compile.
    programs.erase(std::remove_if(programs.begin(), programs.end(),
                                  [](const std::weak_ptr<Program>& p) { return p.expired(); }),
                   programs.end());
    programs.push_back(program);
}

void Runtime::releaseRoots() noexcept
{
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (!interpretedCodes.empty()) {
        std::sort(interpretedCodes.begin(), interpretedCodes.end(), [](const FunctionCode* a, const FunctionCode* b) { return a->interpreted > b->interpreted; });
        for (std::size_t i = 0; i < interpretedCodes.size() && i < 12; ++i)
            std::fprintf(stderr, "interpreted %8llu x4096  %s  native:%s refused:%d gen:%u bails at %zu places\n", (unsigned long long)interpretedCodes[i]->interpreted,
                         interpretedCodes[i]->name.c_str(), interpretedCodes[i]->jitEntry ? "yes" : "no", int(interpretedCodes[i]->jitRefused), unsigned(interpretedCodes[i]->jitGeneration),
                         interpretedCodes[i]->jitBailedAt.size());
    }
#endif
    for (const std::weak_ptr<Program>& weak : programs) {
        if (std::shared_ptr<Program> program = weak.lock()) {
            if (program->code)
                releaseCode(*program->code);
        }
    }
    programs.clear();
    for (const std::weak_ptr<const FunctionCode>& weak : evalCodes) {
        if (std::shared_ptr<const FunctionCode> code = weak.lock())
            releaseCode(*code);
    }
    evalCodes.clear();
    for (Ref<GCObject>& fn : intrinsics)
        fn.reset();
    for (Ref<GCObject>& c : asciiChars)
        c.reset();
    nativeKeepAlive = Value::undefined();
    nativePending = nullptr;
    finallyPending.clear();
    regexp.reset();
    typed.reset();
    iterators.reset();
#if SCRIPT_ENABLE_JIT_THREAD
    jitWorker.reset();
#endif
    if (jitBackend) {
        for (typed::NativeCode* code : jitCodes)
            jitBackend->release(code);
    }
    jitCodes.clear();
    jitHeldCodes.clear();
    jitBackend.reset();
    shapes.clear();
    nullProtoRoot = nullptr;
    jobs.clear();
    unhandled.clear();
    hostBindings.clear();
    errorProtos.clear();
    objectCtor.reset();
    arrayCtor.reset();
    functionCtor.reset();
    promiseCtor.reset();
    for (Ref<DynObject>* p : {&globalObject, &objectProto, &functionProto, &arrayProto,
                              &stringProto, &numberProto, &booleanProto, &symbolProto,
                              &errorProto, &dateProto, &mapProto, &setProto, &weakMapProto, &weakSetProto, &argumentsProto, &promiseProto, &generatorProto, &generatorFunctionProto, &asyncFunctionProto, &asyncIteratorProto,
                              &asyncGeneratorProto, &asyncGeneratorFunctionProto,
                              &iteratorProto, &arrayIteratorProto, &regExpProto, &regExpStringIteratorProto, &bigintProto})
        p->reset();
    for (Ref<GCObject>& name : smallIndexNames)
        name.reset();
    atom = Atoms{};
    sym = Symbols{};
    frames.clear();
    framePool.clear();
    for (auto& entry : typeNames)
        entry = {nullptr, Ref<StringCell>()};
    // The registry's own counts.
    std::vector<SymbolCell*> registered;
    for (const auto& [name, symbol] : symbolRegistry)
        registered.push_back(symbol);
    symbolRegistry.clear();
    for (SymbolCell* symbol : registered) {
        symbol->registered = false;
        detail::decRef(symbol);
    }
}

/// A registered symbol whose only reference is the registry itself is not
/// held by any live object, so it can go.
void Runtime::afterCollect() noexcept
{
    for (auto it = symbolRegistry.begin(); it != symbolRegistry.end();) {
        SymbolCell* symbol = it->second;
        if (symbol->rc > 1) {
            ++it;
            continue;
        }
        symbol->registered = false;
        it = symbolRegistry.erase(it);
        detail::decRef(symbol);
    }
    if (symbolRegistry.bucket_count() > 64 &&
        symbolRegistry.size() * 4 < symbolRegistry.bucket_count())
        symbolRegistry.rehash(0);
    // A call-depth spike should not keep its frame buffers for ever.
    if (framePool.size() > frames.size() + 8)
        framePool.resize(frames.size() + 8);
}

// ----------------------------------------------------------- allocation ---

Shape* Runtime::rootShapeFor(DynObject* proto)
{
    Shape*& root = proto ? proto->rareOf().childRoot : nullProtoRoot;
    if (!root && shapes.size() < kMaxShapes) {
        shapes.push_back(std::make_unique<Shape>());
        root = shapes.back().get();
        root->root = root;
    }
    return root;
}

Shape* Runtime::shapeAfter(Shape* from, GCObject* key, std::uint8_t attrs)
{
    if (Shape* known = from->after(key, attrs))
        return known;
    const bool named = key->kind == CellKind::String && static_cast<StringCell*>(key)->inSource();
    if (from->count >= Shape::kMaxProperties || (!named && from->count >= Shape::kMaxMadeNames) ||
        (!named && from->transitions.size() >= Shape::kMaxTransitions) ||
        shapes.size() >= kMaxShapes)
        return nullptr;
    shapes.push_back(std::make_unique<Shape>());
    Shape* next = shapes.back().get();
    next->key.reset(key);
    // A shape lasts as long as the runtime, so its name does; objects needn't count it.
    key->pin();
    next->attrs = attrs;
    next->plainAttrs = from->plainAttrs && attrs == PropDefault;
    next->count = from->count + 1;
    next->root = from->root;
    next->parent = from;
    next->keyBits = from->keyBits | Shape::keyBit(key);
    next->root->expect = std::max(next->root->expect, next->count);
    from->addTransition(key, attrs, next);
    return next;
}

DynObject* Runtime::newObject(DynObject* proto)
{
    auto* o = newCell<DynObject>();
    o->setProto(proto ? proto : objectProto.get());
    return o;
}

static_assert(sizeof(DynObject) % alignof(Prop) == 0, "the properties in the cell follow the object directly");
static_assert(sizeof(DynObject) % 8 == 0, "native code writes a new object a word at a time");

DynObject* Runtime::newObjectSized(DynObject* proto, std::uint32_t room)
{
    // `newCell` and `setProto` by hand: this is how every object literal and every `new` starts, and a fresh object is
    // nobody's prototype yet, so neither the virtual size nor the epoch is needed.
    if (!proto)
        proto = objectProto.get();
    ctx.maybeCollect();
    // The first properties live in the cell itself: one allocation for the object and its table, while they fit.
    const std::size_t bytes = sizeof(DynObject) + std::size_t(room) * sizeof(Prop);
    ctx.reserve(bytes);
    auto* o = new (ctx.heap.allocate(bytes)) DynObject();
    o->cellBytes = std::uint32_t(bytes);
    if (room) {
        o->propsInCell = true;
        o->props.adopt(reinterpret_cast<Prop*>(o + 1), room);
    }
    ctx.linkKnown(o, std::uint32_t(bytes));
    if (!proto->isProto)
        proto->isProto = true;
    Shape* root = proto->rare && proto->rare->childRoot ? proto->rare->childRoot : rootShapeFor(proto);
    // A prototype of this many objects is here to stay, and counting it is a cost of each of them.
    constexpr std::uint32_t kPinAfter = 1024;
    if (!proto->pinned && root && ++root->births > kPinAfter)
        proto->pin();
    o->proto.reset(proto);
    o->shape = root;
    return o;
}

void Runtime::objectImage(DynObject* proto, Shape* root, std::uint32_t room, std::uint64_t* words)
{
    const std::size_t bytes = sizeof(DynObject) + std::size_t(room) * sizeof(Prop);
    DynObject image;
    image.cellBytes = std::uint32_t(bytes);
    image.gcBytes = std::uint32_t(bytes);
    image.rt = &ctx;
    image.shape = root;
    if (room) {
        image.propsInCell = true;
        image.props.adopt(reinterpret_cast<Prop*>(&image + 1), room);
    }
    std::memcpy(words, static_cast<const void*>(&image), sizeof(DynObject));
    // A prototype named here is pinned, so it is not counted.
    std::memcpy(words + offsetof(DynObject, proto) / 8, &proto, sizeof proto);
    // The caller knows where the table will be.
    words[offsetof(DynObject, props) / 8] = 0;
}

bool Runtime::arrayImage(std::size_t reserve, std::uint64_t* words, std::size_t& bytes)
{
    DynObject* proto = arrayProto.get();
    Shape* root = proto && proto->pinned ? rootShapeFor(proto) : nullptr;
    if (!root || reserve > kArrayInCell)
        return false;
    const std::size_t room = reserve * sizeof(Value);
    bytes = sizeof(DynArray) + room;
    DynArray image;
    image.cellBytes = std::uint32_t(bytes);
    image.gcBytes = std::uint32_t(bytes);
    image.rt = &ctx;
    image.shape = root;
    if (room)
        image.adoptRoom(room);
    std::memcpy(words, static_cast<const void*>(&image), sizeof(DynArray));
    std::memcpy(words + offsetof(DynObject, proto) / 8, &proto, sizeof proto);
    // The caller knows where the elements will be.
    words[offsetof(DynArray, store) / 8] = 0;
    return true;
}

DynArray* Runtime::newArray(std::size_t reserve)
{
    const std::size_t room = reserve <= kArrayInCell ? reserve * sizeof(Value) : 0;
    const std::size_t apart = room ? 0 : std::min<std::size_t>(reserve, 1u << 20);
    ctx.reserve(apart * sizeof(double));  // before the cell exists, so a refusal leaves nothing behind
    ctx.maybeCollect();
    const std::size_t bytes = sizeof(DynArray) + room;
    ctx.reserve(bytes);
    auto* a = new (ctx.heap.allocate(bytes)) DynArray();
    a->cellBytes = std::uint32_t(bytes);
    if (room)
        a->adoptRoom(room);
    ctx.linkKnown(a, std::uint32_t(bytes));
    a->setProto(arrayProto ? arrayProto.get() : objectProto.get());
    if (apart)
        a->reserveElements(apart);
    return a;
}

DynFunction* Runtime::newFunction()
{
    auto* f = newCell<DynFunction>();
    f->setProto(functionProto ? functionProto.get() : objectProto.get());
    return f;
}

Value Runtime::makeArray(Vec<Value> items)
{
    DynArray* a = newArray();
    Value result = detail::Access::adopt(Type::Object, a);
    a->assignAll(items.data(), items.size());
    return result;
}

Value Runtime::makeArray(const std::vector<Value>& items)
{
    return makeArray(Vec<Value>(items.begin(), items.end()));
}

Value Runtime::typeOfValue(const Value& v)
{
    const char* name = typeOfName(v);
    for (auto& [known, cell] : typeNames) {
        if (known == name)
            return detail::Access::make(Type::String, cell.get());
        if (!known) {
            known = name;
            cell.reset(intern(name));
            return detail::Access::make(Type::String, cell.get());
        }
    }
    return str(name);
}

Value Runtime::makeFunction(StringView name, NativeFunction fn, int arity)
{
    DynFunction* f = newFunction();
    f->fnKind = FnKind::Native;
    f->native = std::move(fn);
    f->name = String(name);
    f->arity = arity;
    // Made by the thousand when promises are (a resolve and a reject for each): room for both properties at once, and
    // a short name, the empty one above all, shared rather than a string of its own each time.
    f->props.reserve(2);
    f->putOwn(atom.length.get(), Value(double(arity)), PropConfigurable);
    f->putOwn(atom.name.get(), name.size() <= ContextImpl::kInternedLength ? detail::Access::make(Type::String, intern(name)) : str(name),
              PropConfigurable);
    return detail::Access::adopt(Type::Object, f);
}

void Runtime::method(DynObject* target, const char* name, NativeFunction fn, int arity)
{
    target->putOwn(intern(name), makeFunction(ascii(name), std::move(fn), arity),
                   PropWritable | PropConfigurable);
}

void Runtime::getter(DynObject* target, const char* name, NativeFunction fn)
{
    target->putOwn(intern(name), makeFunction(ascii("get ") + ascii(name), std::move(fn), 0),
                   PropConfigurable | PropAccessor);
}

void Runtime::constant(DynObject* target, const char* name, Value v)
{
    target->putOwn(intern(name), std::move(v), PropWritable | PropConfigurable);
}

DynFunction* Runtime::makeConstructor(const char* name, NativeFunction fn, int arity,
                                      DynObject* proto)
{
    Value ctorValue = makeFunction(ascii(name), std::move(fn), arity);
    DynFunction* ctor = asFunction(ctorValue);
    ctor->isNativeConstructor = true;
    ctor->putOwn(atom.prototype.get(), detail::Access::make(Type::Object, proto), 0);
    proto->putOwn(atom.constructor.get(), ctorValue, PropWritable | PropConfigurable);
    globalObject->putOwn(intern(name), ctorValue, PropWritable | PropConfigurable);
    return ctor;
}

// --------------------------------------------------------- host classes ---

void attachNativeSlot(DynObject& obj, const void* typeKey, void* instance,
                      Context::Finalizer finalizer, void* owner)
{
    std::unique_ptr<NativeSlot>& slot = obj.rareOf().nativeSlot;
    slot = std::make_unique<NativeSlot>();
    slot->typeKey = typeKey;
    slot->data = instance;
    slot->owner = owner;
    slot->finalizer = finalizer;
}

namespace {

/// A native function that also answers `new`: the binding layer's
/// constructor attaches the instance to `this`.
void installMember(Runtime& rt, HostBinding& binding, const HostClassInfo::Member& member)
{
    const HostMember& spec = member.spec;
    const int arity = int(spec.csharpParams.size());
    DynObject* target = spec.isStatic || spec.kind == HostMember::Constant
                            ? binding.target.get()
                            : binding.prototype.get();
    StringCell* key = rt.intern(fromUtf8(spec.name));
    switch (spec.kind) {
        case HostMember::Constructor: {
            // The constructor function was made when the class was; give it
            // its body now.
            DynFunction* ctor = static_cast<DynFunction*>(binding.target.get());
            ctor->native = member.fn;
            ctor->arity = arity;
            ctor->putOwn(rt.atom.length.get(), Value(double(arity)), PropConfigurable);
            return;
        }
        case HostMember::Method:
            target->putOwn(key, rt.makeFunction(fromUtf8(spec.name), member.fn, arity),
                           PropWritable | PropConfigurable);
            return;
        case HostMember::Property:
        case HostMember::Constant: {
            Prop& p = target->putOwn(key, rt.makeFunction(ascii("get ") + fromUtf8(spec.name),
                                                          member.fn, 0),
                                     PropConfigurable | PropEnumerable | PropAccessor);
            if (member.setter)
                target->setSetter(p, rt.makeFunction(ascii("set ") + fromUtf8(spec.name), member.setter, 1));
            return;
        }
    }
}

}  // namespace

void Runtime::syncHostClasses()
{
    for (auto& owned : ctx.hostClasses) {
        HostClassInfo& info = *owned;
        HostBinding& binding = hostBindings[&info];
        if (!binding.target) {
            const String name = fromUtf8(info.name);
            if (info.typeKey) {
                // A class: a constructor function whose body the Constructor
                // member supplies, and a prototype its wrappers inherit from.
                DynObject* proto = newObject(objectProto.get());
                binding.prototype.reset(proto);
                Value ctorValue = makeFunction(name,
                                               [](CallInfo& ci) -> Value {
                                                   ci.fail(ErrorCode::TypeError,
                                                           "this class has no constructor");
                                               },
                                               0);
                DynFunction* ctor = asFunction(ctorValue);
                ctor->isClassConstructor = true;
                ctor->isNativeConstructor = true;
                ctor->putOwn(atom.prototype.get(), detail::Access::make(Type::Object, proto), 0);
                proto->putOwn(atom.constructor.get(), ctorValue, PropWritable | PropConfigurable);
                binding.target.reset(ctor);
            } else {
                // A static class: a plain object holding functions.
                binding.target.reset(newObject(objectProto.get()));
            }
            globalObject->putOwn(intern(name), detail::Access::make(Type::Object, binding.target.get()),
                                 PropWritable | PropConfigurable);
        }
        std::size_t& done = info.installed[1];
        for (; done < info.members.size(); ++done)
            installMember(*this, binding, info.members[done]);
    }
}

namespace {

HostBinding* bindingFor(Runtime& rt, const void* typeKey)
{
    const HostClassInfo* info = rt.ctx.hostClassForKey(typeKey);
    if (!info)
        return nullptr;
    const auto it = rt.hostBindings.find(info);
    return it == rt.hostBindings.end() ? nullptr : &it->second;
}

}  // namespace

Value Runtime::hostPrototype(const void* typeKey)
{
    HostBinding* binding = bindingFor(*this, typeKey);
    if (!binding || !binding->prototype)
        throw Error(ErrorCode::TypeError,
                    "this C++ type has no script class; bind it with Class<T> first");
    return detail::Access::make(Type::Object, binding->prototype.get());
}

Value Runtime::hostConstructor(const void* typeKey)
{
    HostBinding* binding = bindingFor(*this, typeKey);
    if (!binding || !binding->target)
        throw Error(ErrorCode::TypeError,
                    "this C++ type has no script class; bind it with Class<T> first");
    return detail::Access::make(Type::Object, binding->target.get());
}

DynObject* Runtime::hostPrototypeFor(const void* typeKey)
{
    HostBinding* binding = bindingFor(*this, typeKey);
    if (!binding) {
        syncHostClasses();
        binding = bindingFor(*this, typeKey);
    }
    return binding && binding->prototype ? binding->prototype.get() : objectProto.get();
}

std::string Runtime::hostDeclarations() const
{
    std::string out;
    for (const auto& owned : ctx.hostClasses) {
        const HostClassInfo& info = *owned;
        auto params = [](const HostMember& m) {
            std::string list;
            for (std::size_t i = 0; i < m.typescriptParams.size(); ++i)
                list += (i ? ", a" : "a") + std::to_string(i) + ": " + m.typescriptParams[i];
            return list;
        };
        if (info.typeKey) {
            out += "declare class " + info.name + " {\n";
            for (const HostClassInfo::Member& member : info.members) {
                const HostMember& m = member.spec;
                const char* stat = m.isStatic ? "static " : "";
                switch (m.kind) {
                    case HostMember::Constructor:
                        out += "  constructor(" + params(m) + ");\n";
                        break;
                    case HostMember::Method:
                        out += std::string("  ") + stat + m.name + "(" + params(m) + "): " +
                               m.typescriptReturn + ";\n";
                        break;
                    case HostMember::Property:
                        out += std::string("  ") + stat + (member.setter ? "" : "readonly ") +
                               m.name + ": " + m.typescriptReturn + ";\n";
                        break;
                    case HostMember::Constant:
                        out += "  static readonly " + m.name + ": " + m.typescriptReturn + ";\n";
                        break;
                }
            }
            out += "}\n";
        } else {
            out += "declare const " + info.name + ": {\n";
            for (const HostClassInfo::Member& member : info.members) {
                const HostMember& m = member.spec;
                if (m.kind == HostMember::Method)
                    out += "  " + m.name + "(" + params(m) + "): " + m.typescriptReturn + ";\n";
                else
                    out += "  readonly " + m.name + ": " + m.typescriptReturn + ";\n";
            }
            out += "};\n";
        }
    }
    return out;
}

// ---------------------------------------------------------------- stacks ---

Frame& Runtime::acquireFrame(std::size_t depth)
{
    while (framePool.size() <= depth)
        framePool.push_back(std::make_unique<Frame>());
    return *framePool[depth];
}

std::string Runtime::captureStack() const
{
    std::string out;
    int shown = 0;
    void* topFrame = nativeTopFrame;
    const void* topPc = nativeTopPc;
    for (std::size_t i = frames.size(); i-- > 0 && shown < 16; ++shown) {
        const Frame* f = frames[i];
#if SCRIPT_ENABLE_DYN_JIT
        if (f && f->nativeRun) {
            out += describeNativeFrames(*this, topFrame, topPc, shown);
            topFrame = f->savedTopFrame;
            topPc = f->savedTopPc;
            continue;
        }
#endif
        if (!f || !f->code)
            continue;
        const std::string& name = f->code->name;
        out += "    at ";
        out += name.empty() ? "<anonymous>" : name;
        out += " (";
        out += f->code->sourceName ? *f->code->sourceName : std::string("<unknown>");
        out += ":" + std::to_string(f->code->lineAt(f->pc)) + ")\n";
    }
    return out;
}

// ------------------------------------------------------------------ jobs ---

void Runtime::enqueue(Value fn, std::vector<Value> args)
{
    Job job;
    job.fn = std::move(fn);
    job.args = std::move(args);
    jobs.push_back(std::move(job));
}

std::size_t Runtime::runJobs()
{
    std::size_t ran = 0;
    while (!jobs.empty()) {
        Job job = std::move(jobs.front());
        jobs.pop_front();
        ++ran;
        try {
            if (job.kind == Job::Kind::Call)
                callValue(*this, job.fn, Value::undefined(), job.args);
            else
                runPromiseJob(*this, job);
        } catch (const Exception& e) {
            if (ctx.unhandledRejection)
                ctx.unhandledRejection(e.value());
        }
    }
    for (const Ref<DynPromise>& p : unhandled) {
        if (p && p->state == DynPromise::State::Rejected && !p->handled && ctx.unhandledRejection)
            ctx.unhandledRejection(p->result);
    }
    unhandled.clear();
    return ran;
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
