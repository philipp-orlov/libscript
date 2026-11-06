// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The typed engine's runtime: construction, teardown, the allocation helpers
// that need metadata.
#include "script/internal/typed/runtime.hpp"

#include <algorithm>
#include <cstdio>

#include "script/internal/typed/vm.hpp"

namespace script {
namespace internal {
namespace typed {

Runtime::Runtime(ContextImpl& context) : ctx(context)
{
    types = std::make_unique<TypeTable>(*this);
    jit = std::make_unique<JitEngine>(createHostBackend(), ctx.options.jit);
}

Runtime::~Runtime() = default;

void Runtime::releaseRoots() noexcept
{
    for (auto& assembly : assemblies)
        assembly->release();
    assemblies.clear();
    hostMethods.clear();
    hostOwned.clear();
    hostTypes.clear();
    nativeClasses.clear();
    ifaceCache.clear();
    types.reset();
    jit.reset();
    frames.clear();
    framePool.clear();
}

void Runtime::afterCollect() noexcept
{
    unloadUnusedUnits();
    // A call-depth spike should not keep its frame buffers for ever.
    if (framePool.size() > frames.size() + 8)
        framePool.resize(frames.size() + 8);
}

void Runtime::ensureInitialized()
{
    struct Guard
    {
        bool& flag;
        bool saved;
        explicit Guard(bool& f) : flag(f), saved(f) { flag = true; }
        ~Guard() { flag = saved; }
    } guard(initializing);
    for (std::size_t i = 0; i < assemblies.size(); ++i) {
        const std::shared_ptr<AssemblyImpl> image = assemblies[i];
        if (image->initialized)
            continue;
        image->initialized = true;
        for (MethodInfo* initializer : image->initializers)
            invoke(*this, initializer, nullptr, 0);
    }
}

void Runtime::beforeCompile()
{
    if (assemblies.size() < collectAtUnits || ctx.tearingDown || ctx.destroying)
        return;
    ctx.collectCycles();
    // From what stayed: units that declare types do not make this any more frequent.
    collectAtUnits = std::max(kCollectEveryUnits, assemblies.size() * 2);
}

void Runtime::unloadUnusedUnits() noexcept
{
    if (initializing)
        return;
    bool any = false;
    for (const auto& unit : assemblies) {
        // A handle the host holds, or a call into it that is still running, counts through `use_count`.
        unit->unusedCandidate = !unit->declaresNames && unit.use_count() == 1;
        any = any || unit->unusedCandidate;
    }
    if (!any)
        return;

    const auto keepType = [](const TypeInfo* type) {
        if (type && type->assembly)
            type->assembly->unusedCandidate = false;
    };
    const auto keepMethod = [](const MethodInfo* method) {
        if (method && method->assembly)
            method->assembly->unusedCandidate = false;
    };
    for (const Frame* frame : frames)
        keepMethod(frame->method);
    for (const GCObject* cell = ctx.heapHead; cell; cell = cell->gcNext) {
        keepType(typeOfCell(cell));
        if (cell->kind == CellKind::Delegate)
            keepMethod(static_cast<const CSDelegate*>(cell)->method);
    }
    // A later unit may still have named one of them, by the name of its class.
    for (const auto& unit : assemblies) {
        if (unit->unusedCandidate)
            continue;
        for (const TypeInfo* type : unit->typeRefs)
            keepType(type);
        for (const MethodInfo* method : unit->methodRefs)
            keepMethod(method);
    }

    std::size_t kept = 0;
    for (std::size_t i = 0; i < assemblies.size(); ++i) {
        if (!assemblies[i]->unusedCandidate) {
            if (kept != i)
                assemblies[kept] = std::move(assemblies[i]);
            ++kept;
            continue;
        }
        if (jit)
            jit->forget(*assemblies[i]);
        assemblies[i].reset();
    }
    assemblies.resize(kept);
}

// ---------------------------------------------------------- host classes ---

namespace {

const TypeInfo* resolveSpelling(Runtime& rt, std::string_view spelling)
{
    const String name = fromUtf8(spelling);
    if (const TypeInfo* found = rt.types->find(name))
        return found;
    const auto host = rt.hostTypes.find(name);
    if (host != rt.hostTypes.end())
        return host->second;
    if (spelling.size() > 2 && spelling.substr(spelling.size() - 2) == "[]")
        return rt.types->arrayOf(resolveSpelling(rt, spelling.substr(0, spelling.size() - 2)));
    return rt.types->core().objectType;
}

void installHostMember(Runtime& rt, TypeInfo& type, HostClassInfo::Member& member)
{
    const HostMember& spec = member.spec;
    const TypeInfo* result = spec.csharpReturn == "void" ? rt.types->core().voidType
                                                         : resolveSpelling(rt, spec.csharpReturn);
    switch (spec.kind) {
        case HostMember::Method:
        case HostMember::Constructor: {
            auto method = std::make_unique<MethodInfo>();
            method->name = fromUtf8(spec.name);
            method->owner = &type;
            method->returnType = result;
            method->access = Access::Public;
            method->flags = MethodNative | (spec.isStatic ? MethodStatic : 0) |
                            (spec.kind == HostMember::Constructor ? MethodConstructor : 0);
            for (std::size_t i = 0; i < spec.csharpParams.size(); ++i) {
                ParamInfo param;
                param.name = fromUtf8("arg" + std::to_string(i));
                param.type = resolveSpelling(rt, spec.csharpParams[i]);
                method->params.push_back(std::move(param));
            }
            method->native = member.fn;
            type.methods.push_back(method.get());
            rt.hostMethods.push_back(std::move(method));
            return;
        }
        case HostMember::Property:
        case HostMember::Constant:
            rt.types->addProperty(&type, spec.name.c_str(), result, member.fn, member.setter,
                                  spec.isStatic || spec.kind == HostMember::Constant);
            return;
    }
}

}  // namespace

void Runtime::syncHostClasses()
{
    for (auto& owned : ctx.hostClasses) {
        HostClassInfo& info = *owned;
        const String fullName = fromUtf8(info.name);
        TypeInfo* type = nullptr;
        const auto existing = hostTypes.find(fullName);
        if (existing != hostTypes.end()) {
            type = existing->second;
        } else {
            auto made = std::make_unique<TypeInfo>();
            type = made.get();
            type->kind = TypeKind::Class;
            type->nativeKind = NativeKind::Host;
            type->fullName = fullName;
            const std::size_t dot = fullName.rfind(u'.');
            type->name = dot == String::npos ? fullName : fullName.substr(dot + 1);
            type->base = types->core().objectType;
            type->isSealed = true;
            hostOwned.push_back(std::move(made));
            hostTypes[fullName] = type;
        }
        if (info.typeKey && type->typeKey != info.typeKey) {
            type->typeKey = info.typeKey;
            nativeClasses[info.typeKey] = type;
        }
        std::size_t& done = info.installed[0];
        for (; done < info.members.size(); ++done)
            installHostMember(*this, *type, info.members[done]);
    }
}

// ---------------------------------------------------------------- cells ---

namespace {

/// A primitive field reads as its default from the start, with the right tag.
void computeSlotTags(const TypeInfo& type)
{
    type.slotTags.assign(type.instanceSlots, Type::Null);
    for (const TypeInfo* t = &type; t; t = t->base) {
        for (const FieldInfo& field : t->fields) {
            if (field.isStatic || field.slot >= type.instanceSlots || !field.type)
                continue;
            const TypeKind kind = field.type->kind;
            if (isValueKind(kind) || kind == TypeKind::Enum)
                type.slotTags[field.slot] = runtimeTag(kind);
        }
    }
}

}  // namespace

CSInstance* Runtime::newInstance(const TypeInfo* type)
{
    const std::uint32_t slots = type ? type->instanceSlots : 0;
    auto* cell = newCellSized<CSInstance>(slots * sizeof(Value));
    cell->type = type;
    cell->initSlots(slots);
    if (type && slots) {
        if (type->slotTags.size() != slots)
            computeSlotTags(*type);
        Value* first = cell->slots();
        for (std::uint32_t i = 0; i < slots; ++i)
            detail::Access::retag(first[i], type->slotTags[i]);
    }
    recharge(cell);
    return cell;
}

CSArray* Runtime::newArray(const TypeInfo* elementType, std::uint32_t length)
{
    const ElemStore store = elemStoreOf(elementType);
    auto* cell = newCellSized<CSArray>(std::size_t(length) * elemStoreWidth(store));
    cell->elemType = elementType;
    cell->length = length;
    cell->store = store;
    cell->elemKind = elementType ? elementType->kind : TypeKind::Object;
    if (cell->elemKind == TypeKind::Enum && elementType->underlying)
        cell->elemKind = elementType->underlying->kind;
    cell->initElements();
    recharge(cell);
    return cell;
}

Value Runtime::makeException(ErrorCode code, StringView message)
{
    const TypeInfo* type = types->exceptionFor(code);
    CSInstance* instance = newInstance(type);
    Value result = detail::Access::adopt(Type::Object, instance);
    if (const FieldInfo* field = type->findField(ascii("_message")))
        instance->at(field->slot) = str(message);
    if (const FieldInfo* field = type->findField(ascii("_stack")))
        instance->at(field->slot) = str(fromUtf8(ctx.captureStack()));
    return result;
}

}  // namespace typed
}  // namespace internal
}  // namespace script
