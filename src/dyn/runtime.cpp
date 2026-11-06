// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The dynamic engine's runtime: intrinsics, allocation helpers, host class
// materialisation, the job queue and the opcode name table.
#include "script/internal/dyn/runtime.hpp"
#if SCRIPT_ENABLE_DYN_JIT
#include "script/internal/dyn/jit.hpp"
#endif

#include <algorithm>

#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/vm.hpp"

namespace script {
namespace internal {
namespace dyn {

// ------------------------------------------------------------ bytecode ---

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
        "DefMethodElem", "ObjectRest", "ClassSetup", "SetProtoOf", "GetSuper", "SetSuper",
        "GetSuperElem",
        "NewObject", "NewArray", "ArrayAppend", "ArraySpread", "ObjectSpread", "Closure",
        "Call", "CallSpread", "New", "NewSpread", "CallSuper", "CallSuperSpread", "Return",
        "ReturnUndefined",
        "Jump", "JumpIfFalse", "JumpIfTrue", "JumpIfFalseKeep", "JumpIfTrueKeep",
        "JumpIfNullishKeep", "JumpIfNotNullishKeep", "JumpSub", "EndFinally", "Throw",
        "ForInStart", "ForInNext", "IterStart", "IterNext", "IterNextOpt", "IterRest",
        "IterClose",
        "Add", "Sub", "Mul", "Div", "Mod", "Pow", "Neg", "Pos", "BitAnd", "BitOr", "BitXor",
        "BitNot", "Shl", "Shr", "UShr", "Eq", "Ne", "StrictEq", "StrictNe", "Lt", "Le", "Gt",
        "Ge", "In", "InstanceOf", "Not", "TypeOf", "Inc", "Dec", "ToNumber", "ToString",
        "ToObject", "ToPropertyKey", "Void",
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

    installBuiltins(*this);
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
    for (const auto& child : mutableCode.children)
        releaseCode(*child);
}

}  // namespace

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
    for (const std::weak_ptr<Program>& weak : programs) {
        if (std::shared_ptr<Program> program = weak.lock()) {
            if (program->code)
                releaseCode(*program->code);
        }
    }
    programs.clear();
    nativeKeepAlive = Value::undefined();
    nativePending = nullptr;
    if (jitBackend) {
        for (typed::NativeCode* code : jitCodes)
            jitBackend->release(code);
    }
    jitCodes.clear();
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
                              &errorProto, &dateProto, &mapProto, &setProto, &promiseProto,
                              &iteratorProto, &arrayIteratorProto})
        p->reset();
    atom = Atoms{};
    sym = Symbols{};
    frames.clear();
    framePool.clear();
    symbolRegistry.clear();
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
    Shape*& root = proto ? proto->childRoot : nullProtoRoot;
    if (!root && shapes.size() < kMaxShapes) {
        shapes.push_back(std::make_unique<Shape>());
        root = shapes.back().get();
    }
    return root;
}

Shape* Runtime::shapeAfter(Shape* from, GCObject* key, std::uint8_t attrs)
{
    for (const Shape::Transition& t : from->transitions) {
        if (t.key == key && t.attrs == attrs)
            return t.to;
    }
    if (from->count >= Shape::kMaxProperties || from->transitions.size() >= Shape::kMaxTransitions ||
        shapes.size() >= kMaxShapes)
        return nullptr;
    shapes.push_back(std::make_unique<Shape>());
    Shape* next = shapes.back().get();
    next->key.reset(key);
    next->attrs = attrs;
    next->count = from->count + 1;
    from->transitions.push_back({key, attrs, next});
    return next;
}

DynObject* Runtime::newObject(DynObject* proto)
{
    auto* o = newCell<DynObject>();
    o->setProto(proto ? proto : objectProto.get());
    return o;
}

DynArray* Runtime::newArray(std::size_t reserve)
{
    const std::size_t room = std::min<std::size_t>(reserve, 1u << 20);
    ctx.reserve(room * sizeof(Value));  // before the cell exists, so a refusal leaves nothing behind
    auto* a = newCell<DynArray>();
    a->setProto(arrayProto ? arrayProto.get() : objectProto.get());
    if (reserve) {
        a->elements.reserve(room);
        a->chargeGrowth(a->elements.capacity() * sizeof(Value));
    }
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
    a->elements = std::move(items);
    a->chargeGrowth(a->elements.capacity() * sizeof(Value));
    return detail::Access::adopt(Type::Object, a);
}

Value Runtime::makeArray(const std::vector<Value>& items)
{
    return makeArray(Vec<Value>(items.begin(), items.end()));
}

Value Runtime::makeFunction(StringView name, NativeFunction fn, int arity)
{
    DynFunction* f = newFunction();
    f->fnKind = FnKind::Native;
    f->native = std::move(fn);
    f->name = String(name);
    f->arity = arity;
    f->putOwn(atom.length.get(), Value(double(arity)), PropConfigurable);
    f->putOwn(atom.name.get(), str(name), PropConfigurable);
    return detail::Access::adopt(Type::Object, f);
}

void Runtime::method(DynObject* target, const char* name, NativeFunction fn, int arity)
{
    target->putOwn(intern(name), makeFunction(ascii(name), std::move(fn), arity),
                   PropWritable | PropConfigurable);
}

void Runtime::getter(DynObject* target, const char* name, NativeFunction fn)
{
    target->putOwn(intern(name), makeFunction(ascii(name), std::move(fn), 0),
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
    ctor->putOwn(atom.prototype.get(), detail::Access::make(Type::Object, proto), 0);
    proto->putOwn(atom.constructor.get(), ctorValue, PropWritable | PropConfigurable);
    globalObject->putOwn(intern(name), ctorValue, PropWritable | PropConfigurable);
    return ctor;
}

// --------------------------------------------------------- host classes ---

void attachNativeSlot(DynObject& obj, const void* typeKey, void* instance,
                      Context::Finalizer finalizer, void* owner)
{
    obj.nativeSlot = std::make_unique<NativeSlot>();
    obj.nativeSlot->typeKey = typeKey;
    obj.nativeSlot->data = instance;
    obj.nativeSlot->owner = owner;
    obj.nativeSlot->finalizer = finalizer;
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
                p.setter = rt.makeFunction(ascii("set ") + fromUtf8(spec.name), member.setter, 1);
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
    jobs.push_back(Job{std::move(fn), std::move(args)});
}

std::size_t Runtime::runJobs()
{
    std::size_t ran = 0;
    while (!jobs.empty()) {
        Job job = std::move(jobs.front());
        jobs.pop_front();
        ++ran;
        try {
            callValue(*this, job.fn, Value::undefined(), job.args);
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
