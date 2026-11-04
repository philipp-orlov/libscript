// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/typed/checker.hpp"

#include <algorithm>
#include <cmath>

#include "script/internal/stack_guard.hpp"
#include "script/internal/typed/object.hpp"
#include "script/internal/typed/vm.hpp"

namespace script {
namespace internal {
namespace typed {

namespace {

Access accessOf(std::uint16_t modifiers) noexcept
{
    if (modifiers & ModPublic)
        return Access::Public;
    if (modifiers & ModProtected)
        return Access::Protected;
    if (modifiers & ModInternal)
        return Access::Internal;
    return Access::Private;
}

bool isStringKind(const TypeInfo* t) noexcept
{
    return t && t->kind == TypeKind::String;
}

}  // namespace

Checker::Checker(Runtime& runtime, Arena& arena, const CompileOptions& options,
                 AssemblyImpl& assembly)
    : rt_(runtime), arena_(arena), types_(*runtime.types), options_(options), assembly_(assembly)
{}

void Checker::error(std::uint32_t line, std::uint32_t column, std::uint32_t code,
                    const std::string& message)
{
    // One error per line is plenty: a cascade after the first is noise.
    for (const Diagnostic& existing : diagnostics_) {
        if (existing.line == line && existing.column == column)
            return;
    }
    Diagnostic d;
    d.language = Language::CSharp;
    d.file = sourceName_;
    d.line = line;
    d.column = column;
    d.code = code;
    d.message = message;
    diagnostics_.push_back(std::move(d));
}

void Checker::errorAt(const Expr* expr, std::uint32_t code, const std::string& message)
{
    error(expr ? expr->line : 0, expr ? expr->column : 0, code, message);
}

TypeInfo* Checker::newType(TypeKind kind, const String& fullName)
{
    auto owned = std::make_unique<TypeInfo>();
    TypeInfo* type = owned.get();
    type->kind = kind;
    type->fullName = fullName;
    const std::size_t dot = fullName.rfind(u'.');
    type->name = dot == String::npos ? fullName : fullName.substr(dot + 1);
    type->assembly = &assembly_;
    assembly_.types.push_back(std::move(owned));
    assembly_.typeIndex[fullName] = type;
    types_.adopt(type);
    return type;
}

MethodInfo* Checker::newMethod(TypeInfo* owner, const String& name, const TypeInfo* returnType,
                               std::uint16_t flags)
{
    auto owned = std::make_unique<MethodInfo>();
    MethodInfo* method = owned.get();
    method->name = name;
    method->owner = owner;
    method->returnType = returnType;
    method->flags = flags;
    method->assembly = &assembly_;
    method->index = std::uint32_t(assembly_.methodTable.size());
    assembly_.methodTable.push_back(std::move(owned));
    owner->methods.push_back(method);
    return method;
}

// ------------------------------------------------------------- driver ---

bool Checker::check(CompilationUnit* unit, std::vector<Diagnostic>& diagnostics)
{
    sourceName_ = unit->sourceName;
    assembly_.sourceName = std::make_shared<const std::string>(unit->sourceName);
    usings_ = unit->usings;
    // The namespaces every program gets for free, plus the ones this unit
    // declares types in: a type can always be named from inside its own
    // namespace without qualification.
    for (const char* implicit : {"System", "System.Collections.Generic", "System.Text"})
        usings_.push_back(ascii(implicit));
    for (TypeDecl* decl : unit->types) {
        const std::size_t dot = decl->fullName.rfind(u'.');
        if (dot != String::npos)
            usings_.push_back(decl->fullName.substr(0, dot));
    }

    // A record is shorthand for members; expand before anything is declared.
    for (TypeDecl* decl : unit->types)
        synthesizeRecord(decl);

    if (!unit->topLevel.empty() || !unit->topLevelMethods.empty())
        synthesizeEntryPoint(unit);
    declareTypes(unit);
    resolveBases(unit);
    for (TypeDecl* decl : decls_)
        layoutType(decl);
    for (TypeDecl* decl : decls_)
        declareMembers(decl);
    for (TypeDecl* decl : decls_) {
        if (decl->info)
            buildVTable(decl->info);
    }
    for (TypeDecl* decl : decls_)
        verifyInterfaces(decl);
    checkBodies(unit);

    // Instantiated types are real types with real bodies; the emitter walks
    // the unit, so they have to be in it. Appended at the end, after every
    // pass that iterates the unit has finished.
    for (TypeDecl* decl : instantiatedDecls_)
        unit->types.push_back(decl);

    diagnostics = diagnostics_;
    return diagnostics_.empty();
}

void Checker::declareTypes(CompilationUnit* unit)
{
    // A generic declaration is a template, not a type. Register it and move
    // on; `Box<int>` builds the type when something asks for one.
    for (TypeDecl* decl : unit->types) {
        if (decl->typeParams.empty())
            continue;
        const String key = templateKey(decl->name, decl->typeParams.size());
        if (generics_.count(key) || assembly_.findType(decl->fullName)) {
            error(decl->line, 1, 101,
                  "the namespace already contains a definition for '" + toUtf8(decl->name) + "'");
            continue;
        }
        generics_[key] = GenericType{decl, decl->typeParams.size()};
    }

    for (TypeDecl* decl : unit->types) {
        if (!decl->typeParams.empty())
            continue;
        if (assembly_.findType(decl->fullName)) {
            error(decl->line, 1, 101,
                  "the namespace already contains a definition for '" + toUtf8(decl->name) + "'");
            continue;
        }
        TypeKind kind = TypeKind::Class;
        switch (decl->kind) {
            case DeclKind::Class: kind = TypeKind::Class; break;
            case DeclKind::Struct: kind = TypeKind::Struct; break;
            case DeclKind::Interface: kind = TypeKind::Interface; break;
            case DeclKind::Enum: kind = TypeKind::Enum; break;
            case DeclKind::Delegate: kind = TypeKind::Delegate; break;
        }
        TypeInfo* type = newType(kind, decl->fullName);
        type->access = accessOf(decl->modifiers);
        type->isAbstract = (decl->modifiers & ModAbstract) != 0;
        type->isSealed = (decl->modifiers & ModSealed) != 0 || kind == TypeKind::Struct ||
                         kind == TypeKind::Enum || kind == TypeKind::Delegate;
        type->isStaticClass = (decl->modifiers & ModStatic) != 0;
        decl->info = type;
        decls_.push_back(decl);
    }
}

void Checker::resolveBases(CompilationUnit* unit)
{
    (void)unit;
    for (TypeDecl* decl : decls_) {
        if (!decl->info)
            continue;
        decl->info->base = types_.core().objectType;
        resolveBaseTypes(decl);
    }
}

void Checker::resolveBaseTypes(TypeDecl* decl)
{
    const CoreTypes& core = types_.core();
    TypeInfo* type = decl->info;
    if (!type)
        return;

    if (decl->kind == DeclKind::Enum) {
        type->underlying = core.intType;
        if (!decl->baseTypes.empty()) {
            const TypeInfo* underlying = resolve(decl->baseTypes[0]);
            if (underlying && isIntegral(underlying->kind))
                type->underlying = underlying;
        }
        return;
    }
    if (decl->kind == DeclKind::Delegate) {
        const TypeInfo* result = resolve(decl->returnTypeRef);
        std::vector<ParamInfo> params;
        for (ParamDecl& p : decl->params) {
            ParamInfo info;
            info.name = p.name;
            info.type = resolve(p.typeRef);
            info.mode = p.mode;
            params.push_back(std::move(info));
        }
        MethodInfo* invoke = newMethod(type, ascii("Invoke"), result, 0);
        invoke->params = std::move(params);
        type->invoke = invoke;
        return;
    }

    for (std::size_t i = 0; i < decl->baseTypes.size(); ++i) {
        const TypeInfo* base = resolve(decl->baseTypes[i]);
        if (!base)
            continue;
        if (base->kind == TypeKind::Interface) {
            type->interfaces.push_back(base);
        } else if (i == 0 && decl->kind == DeclKind::Class) {
            if (base->isSealed) {
                error(decl->line, 1, 509,
                      "cannot derive from sealed type '" + describe(base) + "'");
                continue;
            }
            type->base = base;
        } else {
            error(decl->baseTypes[i]->line, decl->baseTypes[i]->column, 1722,
                  "'" + describe(base) + "' is not an interface");
        }
    }
}

void Checker::layoutType(TypeDecl* decl)
{
    TypeInfo* type = decl->info;
    if (!type)
        return;

    if (decl->kind == DeclKind::Enum) {
        std::int64_t next = 0;
        for (EnumMemberDecl& member : decl->enumMembers) {
            std::int64_t value = next;
            if (member.value) {
                Expr* init = member.value;
                checkExpr(init);
                if (init->isConstant)
                    value = init->constant.toLong();
                else
                    error(member.line, 1, 133, "the expression being assigned must be constant");
            }
            FieldInfo field;
            field.name = member.name;
            field.type = type;
            field.owner = type;
            field.isStatic = true;
            field.isConst = true;
            field.access = Access::Public;
            field.constValue = Value(std::int32_t(value));
            type->statics.push_back(std::move(field));
            next = value + 1;
        }
        return;
    }
    if (decl->kind == DeclKind::Delegate)
        return;

    // The base has to know its size before this type can start counting.
    for (TypeDecl* other : decls_) {
        if (other != decl && other->info == type->base && other->info->instanceSlots == 0)
            layoutType(other);
    }
    type->instanceSlots = type->base ? type->base->instanceSlots : 0;

    for (FieldDecl& decl_field : decl->fields) {
        const TypeInfo* fieldType = resolve(decl_field.typeRef);
        FieldInfo field;
        field.name = decl_field.name;
        field.type = fieldType;
        field.owner = type;
        field.access = accessOf(decl_field.modifiers);
        field.isReadOnly = (decl_field.modifiers & ModReadonly) != 0;
        field.isConst = (decl_field.modifiers & ModConst) != 0;
        field.isStatic = (decl_field.modifiers & ModStatic) != 0 || field.isConst;

        if (field.isConst) {
            if (decl_field.init) {
                Expr* init = decl_field.init;
                checkExpr(init);
                if (init->isConstant)
                    field.constValue = init->constant;
                else
                    error(decl_field.line, 1, 133,
                          "the expression being assigned to '" + toUtf8(field.name) +
                              "' must be constant");
            }
            field.slot = std::uint32_t(assembly_.statics.size());
            assembly_.statics.push_back(field.constValue);
            type->statics.push_back(std::move(field));
            continue;
        }
        if (field.isStatic) {
            field.slot = std::uint32_t(assembly_.statics.size());
            assembly_.statics.push_back(defaultValue(fieldType));
            type->statics.push_back(std::move(field));
            continue;
        }
        field.slot = type->instanceSlots++;
        type->fields.push_back(std::move(field));
    }

    // An auto-property needs storage; give it a field nothing can name.
    for (PropertyDecl& prop : decl->props) {
        if (!prop.autoProperty)
            continue;
        const TypeInfo* propType = resolve(prop.typeRef);
        FieldInfo field;
        field.name = u"<" + prop.name + u">k__BackingField";
        field.type = propType;
        field.owner = type;
        field.access = Access::Private;
        field.isStatic = (prop.modifiers & ModStatic) != 0;
        if (field.isStatic) {
            field.slot = std::uint32_t(assembly_.statics.size());
            assembly_.statics.push_back(defaultValue(propType));
            prop.backingField = field.name;
            type->statics.push_back(std::move(field));
        } else {
            field.slot = type->instanceSlots++;
            prop.backingField = field.name;
            type->fields.push_back(std::move(field));
        }
    }
}

void Checker::declareMethod(TypeDecl* decl, MethodDecl* method)
{
    TypeInfo* type = decl->info;
    if (!type)
        return;
    std::uint16_t flags = 0;
    if (method->modifiers & ModStatic)
        flags |= MethodStatic;
    if (method->modifiers & ModVirtual)
        flags |= MethodVirtual;
    if (method->modifiers & ModOverride)
        flags |= MethodOverride;
    if (method->modifiers & ModAbstract)
        flags |= MethodAbstract;
    if (method->modifiers & ModSealed)
        flags |= MethodSealed;
    if (method->isConstructor)
        flags |= MethodConstructor;
    if (decl->kind == DeclKind::Interface && !method->body && !method->expressionBody)
        flags |= MethodVirtual | MethodAbstract;
    else if (decl->kind == DeclKind::Interface)
        flags |= MethodVirtual;  // a default interface method has a body
    if (method->name.size() > 4 &&
        (method->name.compare(0, 4, u"get_") == 0 || method->name.compare(0, 4, u"set_") == 0))
        flags |= MethodAccessor;

    const TypeInfo* returnType =
        method->isConstructor || !method->returnTypeRef ? types_.core().voidType
                                                        : resolve(method->returnTypeRef);
    MethodInfo* info = newMethod(type, method->name, returnType, flags);
    info->access = accessOf(method->modifiers);
    for (ParamDecl& p : method->params) {
        ParamInfo param;
        param.name = p.name;
        param.type = resolve(p.typeRef);
        param.mode = p.mode;
        if (p.mode == ParamMode::Params && param.type && param.type->kind != TypeKind::Array) {
            error(p.line, 1, 225, "a params parameter must be an array");
            param.mode = ParamMode::Value;
        }
        if (p.defaultValue) {
            Expr* value = p.defaultValue;
            checkExpr(value);
            if (value->isConstant) {
                param.hasDefault = true;
                param.defaultValue = value->constant;
            } else {
                error(p.line, 1, 1736, "the default value must be a compile-time constant");
            }
        }
        info->params.push_back(std::move(param));
    }
    method->info = info;

    if ((flags & MethodStatic) && method->name == u"Main" && !assembly_.entry) {
        info->flags |= MethodEntry;
        assembly_.entry = info;
    }
}

void Checker::declareMembers(TypeDecl* decl)
{
    TypeInfo* type = decl->info;
    if (!type || decl->kind == DeclKind::Enum || decl->kind == DeclKind::Delegate)
        return;
    // Property accessors become ordinary methods, so everything downstream --
    // overload resolution, the emitter, the VM -- only knows about methods.
    std::vector<MethodDecl*> accessors;
    for (PropertyDecl& prop : decl->props) {
        const TypeInfo* propType = resolve(prop.typeRef);
        PropertyInfo info;
        info.name = prop.name;
        info.type = propType;
        info.owner = type;
        info.access = accessOf(prop.modifiers);
        info.isStatic = (prop.modifiers & ModStatic) != 0;
        info.isInitOnly = prop.isInitOnly;
        info.isRequired = prop.isRequired;

        const FieldInfo* backing =
            prop.autoProperty ? type->findField(prop.backingField) : nullptr;

        if (prop.hasGetter) {
            MethodDecl* getter = arena_.make<MethodDecl>();
            getter->line = prop.line;
            getter->name = u"get_" + prop.name;
            getter->modifiers = prop.modifiers;
            getter->returnTypeRef = prop.typeRef;
            getter->body = prop.getterBody;
            getter->expressionBody = prop.getterExpr;
            getter->owner = decl;
            accessors.push_back(getter);
            prop.getterMethod = getter;
        }
        if (prop.hasSetter) {
            MethodDecl* setter = arena_.make<MethodDecl>();
            setter->line = prop.line;
            setter->name = u"set_" + prop.name;
            setter->modifiers = prop.modifiers;
            setter->returnTypeRef = nullptr;
            setter->body = prop.setterBody;
            setter->owner = decl;
            ParamDecl value;
            value.name = ascii("value");
            value.typeRef = prop.typeRef;
            value.line = prop.line;
            setter->params.push_back(std::move(value));
            accessors.push_back(setter);
            prop.setterMethod = setter;
        }
        (void)backing;
        type->props.push_back(std::move(info));
    }
    for (MethodDecl* accessor : accessors)
        decl->methods.push_back(accessor);

    bool hasConstructor = false;
    for (MethodDecl* method : decl->methods) {
        // A generic method is a template: the call site instantiates it.
        if (!method->typeParams.empty()) {
            genericMethods_[type].push_back(method);
            hasConstructor = hasConstructor || method->isConstructor;
            continue;
        }
        declareMethod(decl, method);
        hasConstructor = hasConstructor || method->isConstructor;
    }

    // A class with no constructor gets the parameterless one.
    if (!hasConstructor && decl->kind != DeclKind::Interface) {
        MethodDecl* ctor = arena_.make<MethodDecl>();
        ctor->line = decl->line;
        ctor->name = ascii(".ctor");
        ctor->modifiers = ModPublic;
        ctor->isConstructor = true;
        ctor->owner = decl;
        ctor->body = arena_.make<BlockStmt>();
        decl->methods.push_back(ctor);
        MethodInfo* info = newMethod(type, ctor->name, types_.core().voidType, MethodConstructor);
        info->access = Access::Public;
        ctor->info = info;
    }

    // Wire up the property accessors now that their methods exist.
    for (std::size_t i = 0; i < decl->props.size(); ++i) {
        PropertyDecl& prop = decl->props[i];
        PropertyInfo& info = type->props[i];
        if (prop.getterMethod)
            info.getter = prop.getterMethod->info;
        if (prop.setterMethod)
            info.setter = prop.setterMethod->info;
        if (prop.autoProperty) {
            const FieldInfo* backing = type->findField(prop.backingField);
            if (backing) {
                if (info.getter)
                    info.getter->accessorSlot = backing->slot;
                if (info.setter)
                    info.setter->accessorSlot = backing->slot;
            }
        }
    }

    // Static field and auto-property initializers run once, before Main.
    bool needsInitializer = false;
    for (const FieldDecl& field : decl->fields) {
        if (field.init && ((field.modifiers & ModStatic) != 0) && !(field.modifiers & ModConst))
            needsInitializer = true;
    }
    for (const PropertyDecl& prop : decl->props) {
        if (prop.init && (prop.modifiers & ModStatic))
            needsInitializer = true;
    }
    if (needsInitializer) {
        MethodDecl* cctor = arena_.make<MethodDecl>();
        cctor->line = decl->line;
        cctor->name = ascii(".cctor");
        cctor->modifiers = ModStatic | ModPrivate;
        cctor->owner = decl;
        cctor->body = arena_.make<BlockStmt>();
        decl->methods.push_back(cctor);
        MethodInfo* info =
            newMethod(type, cctor->name, types_.core().voidType, MethodStatic);
        cctor->info = info;
        assembly_.initializers.push_back(info);
    }
}

void Checker::buildVTable(TypeInfo* type)
{
    if (!type->vtable.empty() || type->kind == TypeKind::Enum || type->kind == TypeKind::Delegate)
        return;
    if (type->base && type->base->assembly == &assembly_)
        buildVTable(const_cast<TypeInfo*>(type->base));
    if (type->base)
        type->vtable = type->base->vtable;

    for (MethodInfo* method : type->methods) {
        if (method->isConstructor() || !method->isVirtual())
            continue;
        if (method->flags & MethodOverride) {
            bool replaced = false;
            for (std::size_t i = 0; i < type->vtable.size(); ++i) {
                MethodInfo* base = type->vtable[i];
                if (base->name != method->name || base->params.size() != method->params.size())
                    continue;
                bool same = true;
                for (std::size_t p = 0; p < method->params.size(); ++p)
                    same = same && base->params[p].type == method->params[p].type;
                if (!same)
                    continue;
                type->vtable[i] = method;
                method->vtableSlot = std::int32_t(i);
                replaced = true;
                break;
            }
            if (!replaced)
                error(0, 0, 115,
                      "'" + method->qualifiedName() + "' found no suitable method to override");
            continue;
        }
        method->vtableSlot = std::int32_t(type->vtable.size());
        type->vtable.push_back(method);
    }
}

void Checker::verifyInterfaces(TypeDecl* decl)
{
    TypeInfo* type = decl->info;
    if (!type || type->kind != TypeKind::Class || type->isAbstract)
        return;

    for (const TypeInfo* t = type; t; t = t->base) {
        for (const TypeInfo* iface : t->interfaces) {
            if (!iface)
                continue;
            for (const MethodInfo* wanted : iface->methods) {
                std::vector<MethodInfo*> candidates;
                type->collectMethods(wanted->name, candidates);
                bool found = false;
                for (const MethodInfo* candidate : candidates) {
                    if (candidate->owner == iface ||
                        candidate->params.size() != wanted->params.size() ||
                        candidate->returnType != wanted->returnType)
                        continue;
                    bool same = true;
                    for (std::size_t i = 0; i < wanted->params.size(); ++i)
                        same = same && candidate->params[i].type == wanted->params[i].type;
                    if (same) {
                        found = true;
                        break;
                    }
                }
                if (!found)
                    error(decl->line, 1, 535,
                          "'" + describe(type) + "' does not implement interface member '" +
                              describe(iface) + "." + toUtf8(wanted->name) + "'");
            }
        }
    }
}

MethodDecl* Checker::synthesizeEntryPoint(CompilationUnit* unit)
{
    TypeDecl* decl = arena_.make<TypeDecl>();
    decl->kind = DeclKind::Class;
    decl->name = ascii("Program");
    decl->fullName = decl->name;
    decl->modifiers = ModPublic | ModStatic;
    decl->line = 1;

    // Top-level code returns `object` so that `return 1 + 1;` can hand a value
    // back to the embedder without the program having to say what it is.
    TypeRef* returnType = arena_.make<TypeRef>();
    returnType->predefined = Tok::Object;
    returnType->line = 1;

    MethodDecl* main = arena_.make<MethodDecl>();
    main->name = ascii("Main");
    main->modifiers = ModStatic | ModPublic;
    main->returnTypeRef = returnType;
    main->owner = decl;
    main->line = 1;
    BlockStmt* body = arena_.make<BlockStmt>();
    body->statements = unit->topLevel;
    main->body = body;
    decl->methods.push_back(main);
    for (MethodDecl* method : unit->topLevelMethods) {
        method->owner = decl;
        decl->methods.push_back(method);
    }

    // Declared like any other type, so its members go through the same passes.
    unit->types.push_back(decl);
    return main;
}

// -------------------------------------------------------------- types ---

const TypeInfo* Checker::lookupType(StringView name) const
{
    if (const TypeInfo* type = assembly_.findType(name))
        return type;
    if (const TypeInfo* type = types_.find(name))
        return type;
    const auto it = rt_.hostTypes.find(String(name));
    if (it != rt_.hostTypes.end())
        return it->second;
    for (const String& ns : usings_) {
        const String qualified = ns + u"." + String(name);
        if (const TypeInfo* type = assembly_.findType(qualified))
            return type;
        if (const TypeInfo* type = types_.find(qualified))
            return type;
    }
    return nullptr;
}

const TypeInfo* Checker::resolve(TypeRef* ref, bool reportErrors)
{
    if (!ref)
        return types_.core().voidType;
    if (ref->resolved)
        return ref->resolved;

    const CoreTypes& core = types_.core();
    const TypeInfo* type = nullptr;

    if (ref->isTuple) {
        std::vector<const TypeInfo*> elements;
        for (TypeRef* arg : ref->typeArgs)
            elements.push_back(resolve(arg, reportErrors));
        type = elements.size() >= 2 ? types_.tupleOf(elements, ref->tupleNames) : core.errorType;
    }

    switch (ref->predefined) {
        case Tok::Bool: type = core.boolType; break;
        case Tok::SByte: type = core.sbyteType; break;
        case Tok::Byte: type = core.byteType; break;
        case Tok::Short: type = core.shortType; break;
        case Tok::UShort: type = core.ushortType; break;
        case Tok::Int: type = core.intType; break;
        case Tok::UInt: type = core.uintType; break;
        case Tok::Long: type = core.longType; break;
        case Tok::ULong: type = core.ulongType; break;
        case Tok::Char: type = core.charType; break;
        case Tok::Float: type = core.floatType; break;
        case Tok::Double: type = core.doubleType; break;
        case Tok::StringKw: type = core.stringType; break;
        case Tok::Object: type = core.objectType; break;
        case Tok::Void: type = core.voidType; break;
        default: break;
    }

    if (!type && ref->isVar)
        return nullptr;  // inferred by the initializer

    if (!type) {
        String name;
        for (std::size_t i = 0; i < ref->path.size(); ++i) {
            if (i)
                name.push_back(u'.');
            name += ref->path[i];
        }
        // A type parameter of the instantiation being checked wins over
        // anything declared: inside `Box<int>`, `T` is `int`.
        if (ref->path.size() == 1 && ref->typeArgs.empty()) {
            if (const TypeInfo* bound = lookupTypeParam(ref->path[0]))
                type = bound;
        }
        if (type) {
            // fall through to the array and nullable suffixes
        } else if (!ref->typeArgs.empty()) {
            std::vector<const TypeInfo*> args;
            for (TypeRef* arg : ref->typeArgs)
                args.push_back(resolve(arg, reportErrors));
            const String simple = ref->path.back();
            const auto generic = generics_.find(templateKey(simple, args.size()));
            if (simple == u"List" && args.size() == 1) {
                type = types_.listOf(args[0]);
            } else if (simple == u"Dictionary" && args.size() == 2) {
                type = types_.dictionaryOf(args[0], args[1]);
            } else if (simple == u"KeyValuePair" && args.size() == 2) {
                type = types_.keyValuePairOf(args[0], args[1]);
            } else if (simple == u"Func" && !args.empty()) {
                const TypeInfo* result = args.back();
                args.pop_back();
                type = types_.funcOf(args, result);
            } else if (simple == u"Action") {
                type = types_.actionOf(args);
            } else if (simple == u"IEnumerable" && args.size() == 1) {
                type = types_.enumerableOf(args[0]);
            } else if (generic != generics_.end()) {
                type = instantiate(generic->second, args, ref->line, ref->column, reportErrors);
            } else if (reportErrors) {
                error(ref->line, ref->column, 246,
                      "the generic type '" + toUtf8(simple) + "' could not be found");
            }
        } else {
            type = lookupType(name);
            if (!type && reportErrors) {
                // A generic name used without arguments is a common slip.
                bool isGeneric = false;
                for (const auto& entry : generics_) {
                    if (entry.second.decl->name == ref->path.back())
                        isGeneric = true;
                }
                if (isGeneric)
                    error(ref->line, ref->column, 305,
                          "the type '" + toUtf8(ref->path.back()) +
                              "' is generic and needs type arguments");
                else
                    error(ref->line, ref->column, 246,
                          "the type or namespace name '" + toUtf8(name) + "' could not be found");
            }
        }
    }

    if (!type)
        type = core.errorType;
    for (std::uint32_t i = 0; i < ref->arrayRank; ++i)
        type = types_.arrayOf(type);
    if (ref->nullable && type->isValueType() && reportErrors && type != core.errorType)
        error(ref->line, ref->column, 8370, "nullable value types are not supported");
    ref->resolved = type;
    return type;
}

// ----------------------------------------------------------- generics ---

String Checker::templateKey(StringView name, std::size_t arity)
{
    String key(name);
    key.push_back(u'`');
    key += ascii(std::to_string(arity).c_str());
    return key;
}

String Checker::instantiatedName(StringView base, const std::vector<const TypeInfo*>& args) const
{
    String name(base);
    name.push_back(u'<');
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i)
            name.push_back(u',');
        name += args[i] ? args[i]->fullName : ascii("?");
    }
    name.push_back(u'>');
    return name;
}

const TypeInfo* Checker::lookupTypeParam(StringView name) const
{
    for (auto scope = typeParamScopes_.rbegin(); scope != typeParamScopes_.rend(); ++scope) {
        const auto it = scope->find(String(name));
        if (it != scope->end())
            return it->second;
    }
    return nullptr;
}

bool Checker::checkConstraints(const std::vector<TypeConstraint>& constraints,
                               const std::vector<String>& params,
                               const std::vector<const TypeInfo*>& args, std::uint32_t line,
                               std::uint32_t column, bool reportErrors)
{
    bool ok = true;
    for (const TypeConstraint& constraint : constraints) {
        std::size_t index = params.size();
        for (std::size_t i = 0; i < params.size(); ++i) {
            if (params[i] == constraint.name) {
                index = i;
                break;
            }
        }
        if (index >= args.size())
            continue;
        const TypeInfo* arg = args[index];
        if (!arg)
            continue;
        const auto complain = [&](const std::string& why) {
            ok = false;
            if (reportErrors)
                error(line, column, 315,
                      "the type '" + describe(arg) + "' cannot be used as '" +
                          toUtf8(constraint.name) + "': " + why);
        };
        if (constraint.hasClass && arg->isValueType())
            complain("it must be a reference type");
        if (constraint.hasStruct && !arg->isValueType())
            complain("it must be a value type");
        if (constraint.hasNew) {
            bool hasDefaultCtor = arg->isValueType();
            for (const MethodInfo* m : arg->methods) {
                if (m->name == u".ctor" && m->params.empty())
                    hasDefaultCtor = true;
            }
            if (!hasDefaultCtor)
                complain("it has no public parameterless constructor");
        }
        for (TypeRef* required : constraint.types) {
            const TypeInfo* bound = resolve(required, reportErrors);
            if (!bound || bound == types_.core().errorType)
                continue;
            if (!types_.implicitlyConverts(arg, bound))
                complain("it does not derive from '" + describe(bound) + "'");
        }
    }
    return ok;
}

void Checker::declareInstantiated(TypeDecl* decl)
{
    typeParamScopes_.emplace_back();
    for (std::size_t i = 0; i < decl->typeParams.size() && i < decl->typeArgs.size(); ++i)
        typeParamScopes_.back()[decl->typeParams[i]] = decl->typeArgs[i];

    const CoreTypes& core = types_.core();
    TypeInfo* type = decl->info;
    type->base = core.objectType;
    resolveBaseTypes(decl);
    layoutType(decl);
    declareMembers(decl);
    if (decl->info)
        buildVTable(decl->info);
    verifyInterfaces(decl);

    typeParamScopes_.pop_back();
}

const TypeInfo* Checker::instantiate(const GenericType& generic,
                                     const std::vector<const TypeInfo*>& args, std::uint32_t line,
                                     std::uint32_t column, bool reportErrors)
{
    const CoreTypes& core = types_.core();
    for (const TypeInfo* arg : args) {
        if (!arg || arg == core.errorType)
            return core.errorType;
    }
    const String name = instantiatedName(generic.decl->fullName, args);
    const auto cached = instantiations_.find(name);
    if (cached != instantiations_.end())
        return cached->second;

    // `class Rec<T> { Rec<Rec<T>> next; }` describes an infinite family. A
    // reifying compiler has to stop somewhere, and saying so beats hanging.
    if (instantiationDepth_ > 24) {
        if (reportErrors)
            error(line, column, 307,
                  "the generic type '" + toUtf8(generic.decl->name) +
                      "' is nested too deeply; the instantiation does not terminate");
        return core.errorType;
    }

    checkConstraints(generic.decl->constraints, generic.decl->typeParams, args, line, column,
                     reportErrors);

    TypeDecl* decl = cloneTypeDecl(arena_, generic.decl);
    decl->fullName = name;
    decl->name = instantiatedName(generic.decl->name, args);
    decl->typeArgs = args;

    TypeKind kind = TypeKind::Class;
    switch (decl->kind) {
        case DeclKind::Class: kind = TypeKind::Class; break;
        case DeclKind::Struct: kind = TypeKind::Struct; break;
        case DeclKind::Interface: kind = TypeKind::Interface; break;
        case DeclKind::Enum: kind = TypeKind::Enum; break;
        case DeclKind::Delegate: kind = TypeKind::Delegate; break;
    }
    TypeInfo* type = newType(kind, decl->fullName);
    type->name = decl->name;
    type->access = accessOf(decl->modifiers);
    type->isAbstract = (decl->modifiers & ModAbstract) != 0;
    type->isSealed = (decl->modifiers & ModSealed) != 0 || kind == TypeKind::Struct ||
                     kind == TypeKind::Enum || kind == TypeKind::Delegate;
    type->isStaticClass = (decl->modifiers & ModStatic) != 0;
    type->typeArgs = args;
    decl->info = type;

    // Registered before the members are declared, so a type that mentions
    // itself finds this entry instead of instantiating again.
    instantiations_[name] = type;
    decls_.push_back(decl);
    instantiatedDecls_.push_back(decl);

    ++instantiationDepth_;
    declareInstantiated(decl);
    --instantiationDepth_;

    pendingInstantiations_.push_back(decl);
    return type;
}

bool Checker::unify(const TypeRef* param, const TypeInfo* actual,
                    const std::vector<String>& typeParams,
                    std::vector<const TypeInfo*>& bindings) const
{
    if (!param || !actual)
        return false;
    // `T` on its own binds directly; `T[]` binds through the element type.
    if (param->predefined == Tok::Eof && param->path.size() == 1 && param->typeArgs.empty()) {
        for (std::size_t i = 0; i < typeParams.size(); ++i) {
            if (typeParams[i] != param->path[0])
                continue;
            const TypeInfo* candidate = actual;
            for (std::uint32_t rank = 0; rank < param->arrayRank; ++rank) {
                if (!candidate || candidate->kind != TypeKind::Array)
                    return false;
                candidate = candidate->element;
            }
            if (!candidate)
                return false;
            if (bindings[i] && bindings[i] != candidate) {
                // Two arguments disagree. Keep the one everything converts to.
                if (types_.implicitlyConverts(candidate, bindings[i]))
                    return true;
                if (!types_.implicitlyConverts(bindings[i], candidate))
                    return false;
            }
            bindings[i] = candidate;
            return true;
        }
    }
    // `List<T>` against `List<int>`, and `Func<T, U>` against `Func<int, bool>`.
    if (!param->typeArgs.empty() && param->arrayRank == 0) {
        if (actual->typeArgs.size() != param->typeArgs.size())
            return false;
        bool ok = true;
        for (std::size_t i = 0; i < param->typeArgs.size(); ++i)
            ok = unify(param->typeArgs[i], actual->typeArgs[i], typeParams, bindings) && ok;
        return ok;
    }
    if (param->arrayRank > 0 && actual->kind == TypeKind::Array) {
        TypeRef inner = *param;
        inner.arrayRank = param->arrayRank - 1;
        return unify(&inner, actual->element, typeParams, bindings);
    }
    return true;  // nothing to infer from this parameter
}

bool Checker::inferTypeArguments(const MethodDecl* templateDecl,
                                 const std::vector<Argument>& args,
                                 std::vector<const TypeInfo*>& out)
{
    out.assign(templateDecl->typeParams.size(), nullptr);
    for (std::size_t i = 0; i < args.size() && i < templateDecl->params.size(); ++i) {
        const Expr* value = args[i].value;
        if (!value || !value->type)
            continue;
        unify(templateDecl->params[i].typeRef, value->type, templateDecl->typeParams, out);
    }
    for (const TypeInfo* bound : out) {
        if (!bound)
            return false;
    }
    return true;
}

MethodInfo* Checker::instantiateMethod(MethodDecl* templateDecl,
                                       const std::vector<const TypeInfo*>& args,
                                       std::uint32_t line, std::uint32_t column, bool reportErrors)
{
    const CoreTypes& core = types_.core();
    for (const TypeInfo* arg : args) {
        if (!arg || arg == core.errorType)
            return nullptr;
    }
    TypeDecl* owner = templateDecl->owner;
    if (!owner || !owner->info)
        return nullptr;

    const String key = String(owner->info->fullName) + u"." +
                       instantiatedName(templateDecl->name, args);
    const auto cached = methodInstantiations_.find(key);
    if (cached != methodInstantiations_.end())
        return cached->second;

    if (instantiationDepth_ > 24) {
        if (reportErrors)
            error(line, column, 307,
                  "the generic method '" + toUtf8(templateDecl->name) +
                      "' is nested too deeply; the instantiation does not terminate");
        return nullptr;
    }
    checkConstraints(templateDecl->constraints, templateDecl->typeParams, args, line, column,
                     reportErrors);

    MethodDecl* decl = cloneMethodDecl(arena_, templateDecl, owner);
    decl->name = instantiatedName(templateDecl->name, args);
    decl->typeParams.clear();
    decl->typeArgs = args;

    typeParamScopes_.emplace_back();
    for (std::size_t i = 0; i < templateDecl->typeParams.size() && i < args.size(); ++i)
        typeParamScopes_.back()[templateDecl->typeParams[i]] = args[i];
    ++instantiationDepth_;

    declareMethod(owner, decl);
    MethodInfo* info = decl->info;
    methodInstantiations_[key] = info;

    // The body is checked with the same bindings in place, right away: the
    // call site that asked is in the middle of being checked, so deferring
    // would mean restoring this scope later for no gain.
    if (info) {
        owner->methods.push_back(decl);
        checkMethodBody(owner, decl);
    }

    --instantiationDepth_;
    typeParamScopes_.pop_back();
    return info;
}

// ------------------------------------------------------------- scopes ---

void Checker::pushScope()
{
    fn_->scopes.emplace_back();
}

void Checker::popScope()
{
    fn_->scopes.pop_back();
}

Checker::Local* Checker::findLocal(StringView name, Function** owner)
{
    for (Function* fn = fn_; fn; fn = fn->parent) {
        for (auto scope = fn->scopes.rbegin(); scope != fn->scopes.rend(); ++scope) {
            const auto it = scope->find(String(name));
            if (it != scope->end()) {
                if (owner)
                    *owner = fn;
                return &it->second;
            }
        }
        // Only a lambda sees the enclosing method's locals.
        if (!fn->lambda)
            break;
    }
    return nullptr;
}

std::uint32_t Checker::addLocal(Function& fn, const TypeInfo* type)
{
    const std::uint32_t slot = std::uint32_t(fn.localTypes.size());
    fn.localTypes.push_back(type);
    fn.boxed.push_back(0);
    const std::uint32_t base =
        (fn.method->isStatic() ? 0u : 1u) + std::uint32_t(fn.method->params.size());
    fn.method->numLocals = std::uint32_t(fn.localTypes.size()) - base;
    return slot;
}

std::uint32_t Checker::addTemporary(const TypeInfo* type)
{
    return addLocal(*fn_, type);
}

std::uint32_t Checker::declareLocal(StringView name, const TypeInfo* type, std::uint32_t line,
                                    bool isConst)
{
    if (fn_->scopes.back().count(String(name)))
        error(line, 1, 128, "a local named '" + toUtf8(name) + "' is already defined");
    Local local;
    local.slot = addTemporary(type);
    local.type = type;
    local.isConst = isConst;
    fn_->scopes.back()[String(name)] = local;
    return local.slot;
}

std::uint32_t Checker::capture(Function* from, Function* owner, std::uint32_t slot)
{
    if (from == owner)
        return slot;

    // Thread the box out through every lambda between the two, so a variable
    // captured two levels down is still one indexed load at the bottom.
    const std::uint32_t outer = capture(from->parent, owner, slot);
    for (std::size_t i = 0; i < from->captures.size(); ++i) {
        if (from->captures[i] == outer)
            return from->method->captureSlots[i];
    }
    const std::uint32_t local = addLocal(*from, from->parent->localTypes[outer]);
    from->boxed[local] = 1;
    from->captures.push_back(outer);
    from->method->captureSlots.push_back(local);
    from->lambda->capturedSlots.push_back(outer);
    return local;
}

// ---------------------------------------------------------- statements ---

void Checker::checkBodies(CompilationUnit* unit)
{
    for (TypeDecl* decl : unit->types) {
        if (!decl->info || (!decl->typeParams.empty() && decl->typeArgs.empty()))
            continue;
        // Checking a body can append methods to this type -- a local function
        // or a lambda is hoisted onto it -- so index rather than iterate.
        for (std::size_t i = 0; i < decl->methods.size(); ++i) {
            MethodDecl* method = decl->methods[i];
            if (!method->info || !method->typeParams.empty())
                continue;
            checkMethodBody(decl, method);
        }
    }
    // Checking a body can instantiate a generic type, whose own bodies then
    // need checking, which can instantiate more. Drain rather than iterate.
    while (!pendingInstantiations_.empty()) {
        TypeDecl* decl = pendingInstantiations_.back();
        pendingInstantiations_.pop_back();
        typeParamScopes_.emplace_back();
        for (std::size_t i = 0; i < decl->typeParams.size() && i < decl->typeArgs.size(); ++i)
            typeParamScopes_.back()[decl->typeParams[i]] = decl->typeArgs[i];
        // `methods` grows while this runs, when a body instantiates a generic
        // method of the same type, so index rather than iterate.
        for (std::size_t i = 0; i < decl->methods.size(); ++i) {
            MethodDecl* method = decl->methods[i];
            if (method->info && method->typeParams.empty())
                checkMethodBody(decl, method);
        }
        typeParamScopes_.pop_back();
    }
}

void Checker::checkMethodBody(TypeDecl* decl, MethodDecl* method)
{
    MethodInfo* info = method->info;
    if (info->isAbstract() || (!method->body && !method->expressionBody &&
                               info->accessorSlot == MethodInfo::kNoSlot &&
                               method->name != ascii(".cctor")))
        return;

    Function function;
    function.method = info;
    function.decl = method;
    function.owner = decl->info;
    function.isStatic = info->isStatic();
    Function* saved = fn_;
    fn_ = &function;

    if (!function.isStatic)
        function.localTypes.push_back(decl->info);
    for (const ParamInfo& param : info->params)
        function.localTypes.push_back(param.type);
    function.boxed.assign(function.localTypes.size(), 0);

    pushScope();
    std::uint32_t slot = function.isStatic ? 0u : 1u;
    for (const ParamInfo& param : info->params) {
        Local local;
        local.slot = slot++;
        local.type = param.type;
        // A `ref` or `out` parameter is a box the caller owns; reading it is
        // the same indirection a captured variable goes through.
        if (param.mode == ParamMode::Ref || param.mode == ParamMode::Out)
            function.boxed[local.slot] = 1;
        function.scopes.back()[param.name] = local;
    }

    // The initializers a constructor will run are part of its body as far as
    // the checker is concerned: they see the same type and the same `this`.
    if (method->isConstructor && !decl->instanceInitializersChecked) {
        decl->instanceInitializersChecked = true;
        for (FieldDecl& field : decl->fields) {
            if (field.init && !(field.modifiers & (ModStatic | ModConst)))
                field.init = coerce(field.init, resolve(field.typeRef), "assignment");
        }
        for (PropertyDecl& prop : decl->props) {
            if (prop.init && !(prop.modifiers & ModStatic))
                prop.init = coerce(prop.init, resolve(prop.typeRef), "assignment");
        }
    }
    if (method->name == ascii(".cctor") && !decl->staticInitializersChecked) {
        decl->staticInitializersChecked = true;
        for (FieldDecl& field : decl->fields) {
            if (field.init && (field.modifiers & ModStatic) && !(field.modifiers & ModConst))
                field.init = coerce(field.init, resolve(field.typeRef), "assignment");
        }
        for (PropertyDecl& prop : decl->props) {
            if (prop.init && (prop.modifiers & ModStatic))
                prop.init = coerce(prop.init, resolve(prop.typeRef), "assignment");
        }
    }

    if (method->expressionBody) {
        Expr* value = method->expressionBody;
        if (info->returnType->kind == TypeKind::Void) {
            checkExpr(value);
        } else {
            value = coerce(value, info->returnType, "return");
        }
        method->expressionBody = value;
    } else if (method->body) {
        checkStatement(method->body);
    }

    // Constructor chaining is checked in the constructor's own scope.
    if (method->isConstructor && (method->chainsToBase || method->chainsToThis)) {
        const TypeInfo* target = method->chainsToThis ? decl->info : decl->info->base;
        std::vector<MethodInfo*> ctors;
        if (target)
            target->collectMethods(ascii(".ctor"), ctors);
        MethodInfo* chosen = pickOverload(ctors, method->chainArgs, ascii(".ctor"), method->line,
                                          1, true);
        method->chainCtor = chosen;
    }

    popScope();
    info->body.localTypes = function.localTypes;
    info->body.localBoxed = function.boxed;
    fn_ = saved;
}

void Checker::checkBlock(BlockStmt* block)
{
    pushScope();
    localFunctionScopes_.emplace_back();
    // Local functions become static methods of the enclosing type, declared
    // before any statement runs so one can be called above the line that
    // declares it -- and so two can call each other.
    for (Stmt* statement : block->statements) {
        if (statement->kind != StmtKind::LocalFunction)
            continue;
        MethodDecl* method = static_cast<LocalFunctionStmt*>(statement)->method;
        if (!method || method->info)
            continue;
        TypeDecl* owner = fn_->decl ? fn_->decl->owner : nullptr;
        if (!owner || !owner->info) {
            error(statement->line, 1, 8112, "a local function needs an enclosing type");
            continue;
        }
        method->owner = owner;
        const String written = method->name;
        method->name = written + ascii("<local>") + fromUtf8(std::to_string(lambdaCounter_++));
        localFunctionScopes_.back()[written] = method;
        if (!method->typeParams.empty()) {
            genericMethods_[owner->info].push_back(method);
            continue;
        }
        declareMethod(owner, method);
        owner->methods.push_back(method);
    }

    // `using var r = ...;` disposes at the end of this block, so everything
    // after it becomes the body of the using.
    for (std::size_t i = 0; i < block->statements.size(); ++i) {
        Stmt* statement = block->statements[i];
        if (statement->kind != StmtKind::Using)
            continue;
        auto* node = static_cast<UsingStmt*>(statement);
        if (node->body || !node->declaration)
            continue;
        BlockStmt* rest = arena_.make<BlockStmt>();
        rest->line = statement->line;
        rest->statements.assign(block->statements.begin() + std::ptrdiff_t(i) + 1,
                                block->statements.end());
        block->statements.resize(i + 1);
        node->body = rest;
        break;
    }

    for (Stmt* statement : block->statements)
        checkStatement(statement);
    localFunctionScopes_.pop_back();
    popScope();
}

void Checker::checkStatement(Stmt* stmt)
{
    checkNativeStack();
    if (!stmt)
        return;
    const CoreTypes& core = types_.core();

    switch (stmt->kind) {
        case StmtKind::Block:
            checkBlock(static_cast<BlockStmt*>(stmt));
            return;
        case StmtKind::Empty:
            return;
        case StmtKind::Expression: {
            auto* node = static_cast<ExprStmt*>(stmt);
            checkExpr(node->expr);
            return;
        }
        case StmtKind::LocalDeclaration: {
            auto* node = static_cast<LocalDeclStmt*>(stmt);
            if (node->deconstruction) {
                checkDeconstruction(node->deconstruction, node->deconstructionValue, stmt->line);
                return;
            }
            const bool inferred = node->typeRef && node->typeRef->isVar;
            const TypeInfo* declared = inferred ? nullptr : resolve(node->typeRef);
            for (VarDeclarator& var : node->vars) {
                const TypeInfo* type = declared;
                if (inferred) {
                    if (!var.init) {
                        error(var.line, 1, 818, "an implicitly typed variable must be initialized");
                        type = core.errorType;
                    } else {
                        type = checkExpr(var.init);
                        if (type == core.nullType) {
                            error(var.line, 1, 815, "cannot assign null to an implicitly typed "
                                                    "variable");
                            type = core.objectType;
                        }
                    }
                } else if (var.init) {
                    var.init = coerce(var.init, type, "assignment");
                }
                var.type = type;
                var.slot = declareLocal(var.name, type, var.line, node->isConst);
                if (node->isConst) {
                    if (!var.init || !var.init->isConstant) {
                        error(var.line, 1, 133,
                              "the expression assigned to '" + toUtf8(var.name) +
                                  "' must be constant");
                    } else if (Local* local = findLocal(var.name)) {
                        local->constant = var.init->constant;
                    }
                }
            }
            return;
        }
        case StmtKind::If: {
            auto* node = static_cast<IfStmt*>(stmt);
            node->condition = coerce(node->condition, core.boolType, "if");
            checkStatement(node->thenBranch);
            checkStatement(node->elseBranch);
            return;
        }
        case StmtKind::While: {
            auto* node = static_cast<WhileStmt*>(stmt);
            node->condition = coerce(node->condition, core.boolType, "while");
            ++fn_->loopDepth;
            checkStatement(node->body);
            --fn_->loopDepth;
            return;
        }
        case StmtKind::DoWhile: {
            auto* node = static_cast<DoWhileStmt*>(stmt);
            ++fn_->loopDepth;
            checkStatement(node->body);
            --fn_->loopDepth;
            node->condition = coerce(node->condition, core.boolType, "do");
            return;
        }
        case StmtKind::For: {
            auto* node = static_cast<ForStmt*>(stmt);
            pushScope();
            for (Stmt* init : node->init)
                checkStatement(init);
            if (node->condition)
                node->condition = coerce(node->condition, core.boolType, "for");
            for (Expr*& update : node->update)
                checkExpr(update);
            ++fn_->loopDepth;
            checkStatement(node->body);
            --fn_->loopDepth;
            popScope();
            return;
        }
        case StmtKind::ForEach: {
            auto* node = static_cast<ForEachStmt*>(stmt);
            const TypeInfo* sequence = checkExpr(node->sequence);
            const TypeInfo* element = core.errorType;
            // A dictionary is walked through a list of its entries.
            const TypeInfo* walked = sequence;
            if (sequence->nativeKind == NativeKind::Dictionary) {
                std::vector<MethodInfo*> entries;
                sequence->collectMethods(ascii("$Entries"), entries);
                if (!entries.empty()) {
                    node->getEnumerator = entries[0];
                    walked = entries[0]->returnType;
                }
            }
            if (sequence->kind == TypeKind::Array) {
                node->over = ForEachStmt::Over::Array;
                element = sequence->element;
            } else if (sequence->kind == TypeKind::String) {
                node->over = ForEachStmt::Over::String;
                element = core.charType;
                // A string is walked through its Length and indexer, like a List.
                if (const PropertyInfo* length = sequence->findProperty(ascii("Length")))
                    node->countGetter = length->getter;
                std::vector<MethodInfo*> chars;
                sequence->collectMethods(ascii("get_Chars"), chars);
                node->itemGetter = chars.empty() ? nullptr : chars[0];
            } else if (walked->nativeKind == NativeKind::List) {
                node->over = walked == sequence ? ForEachStmt::Over::List : ForEachStmt::Over::Dictionary;
                element = walked->typeArgs.empty() ? core.objectType : walked->typeArgs[0];
                std::vector<MethodInfo*> getters;
                walked->collectMethods(ascii("get_Item"), getters);
                node->itemGetter = getters.empty() ? nullptr : getters[0];
                std::vector<MethodInfo*> counts;
                walked->collectMethods(ascii("get_Count"), counts);
                node->countGetter = counts.empty() ? nullptr : counts[0];
            } else if (sequence->kind != TypeKind::Error) {
                error(stmt->line, 1, 1579,
                      "foreach cannot operate on '" + describe(sequence) +
                          "'; it is not an array, a string, a List<T> or a Dictionary<K,V>");
            }

            pushScope();
            const TypeInfo* declared =
                node->typeRef && node->typeRef->isVar ? element : resolve(node->typeRef);
            if (declared != element && !types_.implicitlyConverts(element, declared) &&
                element->kind != TypeKind::Error)
                error(stmt->line, 1, 30,
                      "cannot convert '" + describe(element) + "' to '" + describe(declared) + "'");
            node->sequenceSlot = addTemporary(walked);
            node->cursorSlot = addTemporary(core.intType);
            node->slot = declareLocal(node->name, declared, stmt->line);
            ++fn_->loopDepth;
            checkStatement(node->body);
            --fn_->loopDepth;
            popScope();
            return;
        }
        case StmtKind::Switch: {
            auto* node = static_cast<SwitchStmt*>(stmt);
            const TypeInfo* subject = checkExpr(node->subject);
            for (const SwitchSection& section : node->sections)
                node->usesPatterns = node->usesPatterns || !section.patterns.empty();

            if (!node->usesPatterns && !isIntegral(subject->kind) &&
                subject->kind != TypeKind::String && subject->kind != TypeKind::Enum &&
                subject->kind != TypeKind::Bool && subject->kind != TypeKind::Error)
                error(stmt->line, 1, 151,
                      "a switch expression must be an integer, char, string, bool or enum");
            if (node->usesPatterns)
                node->subjectSlot = addTemporary(subject);

            ++fn_->loopDepth;  // `break` is legal in a switch
            for (SwitchSection& section : node->sections) {
                pushScope();
                for (Expr*& label : section.labels) {
                    label = coerce(label, subject, "case");
                    if (!label->isConstant)
                        errorAt(label, 150, "a case label must be a constant");
                }
                // Every pattern of one section reaches the same statements,
                // so their tests are joined with `or`.
                for (SwitchCaseLabel& label : section.patterns) {
                    Expr* read = makeLocalRead(node->subjectSlot, subject, node->subject);
                    Expr* test = lowerPattern(label.pattern, read, subject);
                    if (label.guard) {
                        label.guard = coerce(label.guard, types_.core().boolType, "a when clause");
                        test = makeAnd(test, label.guard);
                    }
                    if (!section.test) {
                        section.test = test;
                    } else {
                        BinaryExpr* either = arena_.make<BinaryExpr>();
                        either->line = stmt->line;
                        either->op = BinaryOp::LogicalOr;
                        either->left = section.test;
                        either->right = test;
                        either->type = types_.core().boolType;
                        either->operandType = types_.core().boolType;
                        section.test = either;
                    }
                }
                for (Stmt* inner : section.statements)
                    checkStatement(inner);
                popScope();
            }
            --fn_->loopDepth;
            return;
        }
        case StmtKind::Break:
        case StmtKind::Continue:
            if (fn_->loopDepth == 0)
                error(stmt->line, 1, 139, "no enclosing loop out of which to break or continue");
            return;
        case StmtKind::Return: {
            auto* node = static_cast<ReturnStmt*>(stmt);
            const TypeInfo* expected = fn_->method->returnType;
            if (!node->value) {
                if (expected->kind != TypeKind::Void)
                    error(stmt->line, 1, 126,
                          "a return of type '" + describe(expected) + "' is required");
                return;
            }
            if (expected->kind == TypeKind::Void) {
                error(stmt->line, 1, 127, "a void method cannot return a value");
                checkExpr(node->value);
                return;
            }
            node->value = coerce(node->value, expected, "return");
            return;
        }
        case StmtKind::Throw: {
            auto* node = static_cast<ThrowStmt*>(stmt);
            if (!node->value)
                return;
            const TypeInfo* thrown = checkExpr(node->value);
            if (thrown->kind != TypeKind::Error && core.exceptionType &&
                !thrown->derivesFrom(core.exceptionType))
                errorAt(node->value, 155, "the type thrown must derive from System.Exception");
            return;
        }
        case StmtKind::Try: {
            auto* node = static_cast<TryStmt*>(stmt);
            mergeFilteredCatches(node);
            checkStatement(node->body);
            for (CatchClause& clause : node->catches) {
                clause.type = clause.typeRef ? resolve(clause.typeRef) : core.exceptionType;
                if (clause.type && core.exceptionType &&
                    !clause.type->derivesFrom(core.exceptionType) &&
                    clause.type->kind != TypeKind::Error)
                    error(clause.line, 1, 155,
                          "the type caught must derive from System.Exception");
                pushScope();
                clause.slot = clause.name.empty()
                                  ? addTemporary(clause.type)
                                  : declareLocal(clause.name, clause.type, clause.line);
                checkStatement(clause.body);
                popScope();
            }
            checkStatement(node->finallyBlock);
            return;
        }
        case StmtKind::Using:
            pushScope();
            checkUsing(static_cast<UsingStmt*>(stmt));
            popScope();
            return;
        case StmtKind::LocalFunction: {
            auto* node = static_cast<LocalFunctionStmt*>(stmt);
            // Declared eagerly when the block was entered, so it can be
            // called before the line that declares it.
            if (node->method && node->method->info && !node->method->checked) {
                node->method->checked = true;
                checkMethodBody(node->method->owner, node->method);
            }
            return;
        }
        case StmtKind::Yield:
            error(stmt->line, 1, 1621,
                  "yield is only allowed in a method that returns IEnumerable<T>");
            return;
    }
}

// --------------------------------------------------------- conversions ---

Expr* Checker::convert(Expr* expr, const TypeInfo* target)
{
    if (!expr || !target || expr->type == target)
        return expr;
    const ConvKindResult conv = types_.classify(expr->type, target);
    if (conv.kind == ConvKind::Identity) {
        expr->type = target;
        return expr;
    }
    ConvertExpr* node = arena_.make<ConvertExpr>();
    node->line = expr->line;
    node->column = expr->column;
    node->operand = expr;
    node->conv = conv.kind;
    node->type = target;
    // A conversion of a constant is still a constant.
    if (expr->isConstant && isNumeric(target->kind)) {
        node->isConstant = true;
        node->constant = foldConversion(expr->constant, target);
    }
    return node;
}

Expr* Checker::coerce(Expr* expr, const TypeInfo* target, const char* context)
{
    if (!expr)
        return expr;
    const CoreTypes& core = types_.core();

    // A uint or ulong joined to a string must keep its type: `convert` would retype the expression as a string in place,
    // and the emitter could then no longer tell that its bits are to be printed as unsigned.
    if (target == core.stringType && expr->type && std::string(context) == "concatenation" &&
        (expr->type->kind == TypeKind::UInt || expr->type->kind == TypeKind::ULong)) {
        ConvertExpr* node = arena_.make<ConvertExpr>();
        node->line = expr->line;
        node->column = expr->column;
        node->operand = expr;
        node->conv = ConvKind::ToStringCall;
        node->type = target;
        return node;
    }
    // A lambda and a method group only get a type from the slot they go into.
    if (expr->kind == ExprKind::Lambda && target && target->kind == TypeKind::Delegate) {
        checkLambda(static_cast<LambdaExpr*>(expr), target);
        return expr;
    }
    // So do `[...]`, `new(...)` and `throw`, each of which is whatever the
    // context asked for.
    if (expr->kind == ExprKind::Collection && !expr->type) {
        expr->type = checkCollection(static_cast<CollectionExpr*>(expr), target);
        return expr;
    }
    if (expr->kind == ExprKind::ObjectCreation && !expr->type &&
        !static_cast<ObjectCreationExpr*>(expr)->typeRef) {
        auto* node = static_cast<ObjectCreationExpr*>(expr);
        if (!target || target->kind == TypeKind::Error) {
            errorAt(expr, 8754, "there is no target type for this 'new'");
            return expr;
        }
        node->type = checkObjectCreationOf(node, target);
        return expr;
    }
    if (expr->kind == ExprKind::Throw) {
        auto* node = static_cast<ThrowExpr*>(expr);
        checkThrowValue(node->value, expr);
        node->type = target;
        return expr;
    }
    const TypeInfo* from = checkExpr(expr);

    if (target && target->kind == TypeKind::Delegate &&
        (expr->kind == ExprKind::Name || expr->kind == ExprKind::MemberAccess)) {
        std::vector<MethodInfo*>* group = nullptr;
        if (expr->kind == ExprKind::Name)
            group = &static_cast<NameExpr*>(expr)->group;
        else
            group = &static_cast<MemberAccessExpr*>(expr)->group;
        if (!group->empty()) {
            MethodInfo* match = matchDelegate(*group, target);
            if (!match) {
                errorAt(expr, 123, "no overload matches the delegate '" + describe(target) + "'");
                return expr;
            }
            ConvertExpr* node = arena_.make<ConvertExpr>();
            node->line = expr->line;
            node->column = expr->column;
            node->operand = expr;
            node->conv = ConvKind::MethodGroup;
            node->type = target;
            *group = {match};
            return node;
        }
    }

    if (!from || from->kind == TypeKind::Error || !target || target->kind == TypeKind::Error)
        return expr;

    if (from == target)
        return expr;
    const ConvKindResult conv = types_.classify(from, target);
    if (!conv.exists || conv.requiresCast) {
        // The implicit constant conversion: an `int` constant that fits its
        // target needs no cast, which is what makes `byte b = 1;` legal.
        if (expr->isConstant && from->kind == TypeKind::Int && isIntegral(target->kind) &&
            constantFits(expr->constant, target)) {
            return convert(expr, target);
        }
        if (target == core.stringType && std::string(context) == "concatenation")
            return convert(expr, target);
        errorAt(expr, conv.exists ? 266 : 29,
                "cannot implicitly convert '" + describe(from) + "' to '" + describe(target) +
                    "'" + (conv.exists ? "; an explicit conversion exists" : ""));
        return expr;
    }
    return convert(expr, target);
}

Value Checker::foldConversion(const Value& value, const TypeInfo* target)
{
    switch (target->kind) {
        case TypeKind::Bool: return Value(value.toBool());
        case TypeKind::Char: return Value(char16_t(value.toInt()));
        case TypeKind::SByte: return Value(std::int32_t(std::int8_t(value.toInt())));
        case TypeKind::Byte: return Value(std::int32_t(std::uint8_t(value.toInt())));
        case TypeKind::Short: return Value(std::int32_t(std::int16_t(value.toInt())));
        case TypeKind::UShort: return Value(std::int32_t(std::uint16_t(value.toInt())));
        case TypeKind::Int:
        case TypeKind::UInt:
        case TypeKind::Enum: return Value(std::int32_t(value.toLong()));
        case TypeKind::Long:
        case TypeKind::ULong: return Value(value.toLong());
        case TypeKind::Float: return Value(float(value.toDouble()));
        case TypeKind::Double: return Value(value.toDouble());
        default: return value;
    }
}

bool Checker::constantFits(const Value& value, const TypeInfo* target)
{
    if (!value.isNumber())
        return false;
    const std::int64_t n = value.toLong();
    if (value.isFloat() || value.isDouble())
        return false;
    switch (target->kind) {
        case TypeKind::SByte: return n >= -128 && n <= 127;
        case TypeKind::Byte: return n >= 0 && n <= 255;
        case TypeKind::Short: return n >= -32768 && n <= 32767;
        case TypeKind::UShort:
        case TypeKind::Char: return n >= 0 && n <= 65535;
        case TypeKind::UInt: return n >= 0 && n <= 0xffffffffll;
        case TypeKind::ULong: return n >= 0;
        case TypeKind::Int: return n >= -2147483648ll && n <= 2147483647ll;
        case TypeKind::Long: return true;
        default: return false;
    }
}

MethodInfo* Checker::matchDelegate(const std::vector<MethodInfo*>& group,
                                   const TypeInfo* delegateType)
{
    const MethodInfo* wanted = delegateType->invoke;
    if (!wanted)
        return nullptr;
    for (MethodInfo* candidate : group) {
        if (candidate->params.size() != wanted->params.size())
            continue;
        if (!types_.implicitlyConverts(candidate->returnType, wanted->returnType) &&
            candidate->returnType != wanted->returnType)
            continue;
        bool fits = true;
        for (std::size_t i = 0; i < wanted->params.size(); ++i)
            fits = fits && wanted->params[i].type == candidate->params[i].type;
        if (fits)
            return candidate;
    }
    return nullptr;
}

// --------------------------------------------------- overload resolution ---

bool Checker::bindArguments(MethodInfo* method, std::vector<Argument>& args, bool apply)
{
    const std::size_t count = method->params.size();
    const bool hasParams = count > 0 && method->params.back().mode == ParamMode::Params;

    std::vector<Expr*> positional(count, nullptr);
    std::vector<Argument*> sources(count, nullptr);
    std::size_t next = 0;
    std::vector<Argument> extras;

    for (Argument& arg : args) {
        if (!arg.name.empty()) {
            bool matched = false;
            for (std::size_t i = 0; i < count; ++i) {
                if (method->params[i].name != arg.name)
                    continue;
                if (positional[i])
                    return false;
                positional[i] = arg.value;
                sources[i] = &arg;
                matched = true;
                break;
            }
            if (!matched)
                return false;
            continue;
        }
        if (next < count && !(hasParams && next == count - 1)) {
            positional[next] = arg.value;
            sources[next] = &arg;
            ++next;
            continue;
        }
        if (!hasParams)
            return false;
        extras.push_back(arg);
    }

    // The trailing `params` parameter takes whatever is left, unless a single
    // argument of the array type was passed straight through.
    Expr* packed = nullptr;
    if (hasParams) {
        const TypeInfo* arrayType = method->params.back().type;
        if (extras.size() == 1 && !positional[count - 1]) {
            const TypeInfo* single = checkExpr(extras[0].value);
            if (types_.implicitlyConverts(single, arrayType)) {
                positional[count - 1] = extras[0].value;
                extras.clear();
            }
        }
        if (!positional[count - 1]) {
            ArrayCreationExpr* array = arena_.make<ArrayCreationExpr>();
            array->line = args.empty() ? 0 : args.front().value->line;
            array->hasInitializer = true;
            array->type = arrayType;
            for (Argument& extra : extras)
                array->elements.push_back(extra.value);
            packed = array;
            positional[count - 1] = array;
        }
    }

    for (std::size_t i = 0; i < count; ++i) {
        const ParamInfo& param = method->params[i];
        // `out var x` has no argument expression yet: it declares one.
        if (!positional[i] && sources[i] && !sources[i]->outName.empty()) {
            if (param.mode != ParamMode::Out)
                return false;
            if (sources[i]->outTypeRef && !sources[i]->outTypeRef->isVar &&
                resolve(sources[i]->outTypeRef, false) != param.type)
                return false;
            if (!apply)
                continue;
            const std::uint32_t slot =
                declareLocal(sources[i]->outName, param.type, sources[i]->outLine);
            fn_->boxed[slot] = 1;
            NameExpr* read = arena_.make<NameExpr>();
            read->line = sources[i]->outLine;
            read->symbol = SymbolKind::Local;
            read->slot = slot;
            read->type = param.type;
            positional[i] = read;
            sources[i]->value = read;
            continue;
        }
        if (!positional[i]) {
            if (!param.hasDefault)
                return false;
            if (apply) {
                LiteralExpr* literal = arena_.make<LiteralExpr>();
                literal->type = param.type;
                literal->isConstant = true;
                literal->constant = param.defaultValue;
                literal->literal = LiteralKind::Int;
                positional[i] = literal;
            }
            continue;
        }
        if (packed == positional[i]) {
            if (apply) {
                const TypeInfo* element = param.type->element;
                for (Expr*& element_expr : static_cast<ArrayCreationExpr*>(packed)->elements)
                    element_expr = coerce(element_expr, element, "argument");
            }
            continue;
        }

        Expr* value = positional[i];
        if (value && value->kind == ExprKind::Discard) {
            if (param.mode != ParamMode::Out)
                return false;
            if (!apply)
                continue;
            const std::uint32_t slot = addTemporary(param.type);
            fn_->boxed[slot] = 1;
            positional[i] = makeLocalRead(slot, param.type, value);
            sources[i]->value = positional[i];
            continue;
        }
        if (value->kind == ExprKind::Lambda) {
            // A lambda fits any delegate parameter with the right arity; the
            // body is only checked once the overload is settled.
            if (param.type->kind != TypeKind::Delegate || !param.type->invoke ||
                param.type->invoke->params.size() !=
                    static_cast<LambdaExpr*>(value)->params.size())
                return false;
            if (apply)
                positional[i] = coerce(value, param.type, "argument");
            continue;
        }
        const TypeInfo* actual = checkExpr(positional[i]);
        value = positional[i];
        if (param.mode == ParamMode::Ref || param.mode == ParamMode::Out) {
            if (sources[i] && sources[i]->mode == ParamMode::Value)
                return false;
            if (actual != param.type)
                return false;
            if (apply) {
                if (!assignable(value)) {
                    errorAt(value, 1510, "a ref or out argument must be an assignable variable");
                } else if (checkNotReadOnly(value), value->kind == ExprKind::Name) {
                    // Taking its address means the variable has to live in a
                    // box, the same way a captured one does.
                    auto* name = static_cast<NameExpr*>(value);
                    if (name->symbol == SymbolKind::Local ||
                        name->symbol == SymbolKind::Parameter)
                        fn_->boxed[name->slot] = 1;
                    else
                        errorAt(value, 1510, "only a local or a parameter can be passed by "
                                             "reference");
                }
            }
            continue;
        }
        if (!apply) {
            if (!types_.implicitlyConverts(actual, param.type) &&
                !(value->isConstant && constantFits(value->constant, param.type)) &&
                actual->kind != TypeKind::Error)
                return false;
        } else {
            positional[i] = coerce(value, param.type, "argument");
        }
    }

    if (apply) {
        std::vector<Argument> rebuilt;
        for (std::size_t i = 0; i < count; ++i) {
            Argument arg;
            arg.value = positional[i];
            arg.mode = method->params[i].mode;
            rebuilt.push_back(std::move(arg));
        }
        args = std::move(rebuilt);
    }
    return true;
}

MethodInfo* Checker::pickOverload(const std::vector<MethodInfo*>& candidates,
                                  std::vector<Argument>& args, const String& name,
                                  std::uint32_t line, std::uint32_t column, bool report)
{
    std::vector<MethodInfo*> viable;
    for (MethodInfo* candidate : candidates) {
        if (bindArguments(candidate, args, false))
            viable.push_back(candidate);
    }
    if (viable.empty()) {
        if (report) {
            if (candidates.empty()) {
                error(line, column, 117, "no method named '" + toUtf8(name) + "' was found");
            } else {
                // With one candidate there is a specific reason, and saying
                // which argument did not fit is far more useful than the count.
                const std::size_t before = diagnostics_.size();
                if (candidates.size() == 1)
                    bindArguments(candidates.front(), args, true);
                if (diagnostics_.size() == before)
                    error(line, column, 1501,
                          "no overload of '" + toUtf8(name) + "' takes these " +
                              std::to_string(args.size()) + " argument(s)");
            }
        }
        return nullptr;
    }
    if (viable.size() > 1) {
        // Prefer the one that needs no conversions at all, then the one with
        // no `params` expansion -- which is what the language's better-ness
        // rules amount to for the subset the engine accepts.
        std::stable_sort(viable.begin(), viable.end(), [&](MethodInfo* a, MethodInfo* b) {
            const int scoreA = overloadScore(a, args);
            const int scoreB = overloadScore(b, args);
            return scoreA > scoreB;
        });
    }
    MethodInfo* chosen = viable.front();
    bindArguments(chosen, args, true);
    return chosen;
}

int Checker::overloadScore(MethodInfo* method, const std::vector<Argument>& args)
{
    int score = 0;
    if (!method->params.empty() && method->params.back().mode == ParamMode::Params)
        score -= 4;
    if (method->params.size() != args.size())
        score -= 2;
    for (std::size_t i = 0; i < args.size() && i < method->params.size(); ++i) {
        const TypeInfo* actual = args[i].value ? args[i].value->type : nullptr;
        if (!actual)
            continue;
        if (actual == method->params[i].type)
            score += 4;
        else if (method->params[i].type->kind == TypeKind::Object)
            score += 0;  // boxing into object is the last resort
        else
            score += 2;
    }
    return score;
}

// ---------------------------------------------------------- expressions ---

const TypeInfo* Checker::checkExpr(Expr*& expr)
{
    checkNativeStack();
    const CoreTypes& core = types_.core();
    if (!expr)
        return core.errorType;
    if (expr->type)
        return expr->type;

    const TypeInfo* result = core.errorType;
    switch (expr->kind) {
        case ExprKind::Literal: result = checkLiteral(static_cast<LiteralExpr*>(expr)); break;
        case ExprKind::Interpolated:
            result = checkInterpolated(static_cast<InterpolatedExpr*>(expr));
            break;
        case ExprKind::Name: result = checkName(static_cast<NameExpr*>(expr)); break;
        case ExprKind::MemberAccess:
            result = checkMember(static_cast<MemberAccessExpr*>(expr));
            break;
        case ExprKind::Invocation:
            result = checkInvocation(static_cast<InvocationExpr*>(expr));
            break;
        case ExprKind::ElementAccess:
            result = checkElementAccess(static_cast<ElementAccessExpr*>(expr));
            break;
        case ExprKind::ObjectCreation:
            result = checkObjectCreation(static_cast<ObjectCreationExpr*>(expr));
            break;
        case ExprKind::ArrayCreation:
            result = checkArrayCreation(static_cast<ArrayCreationExpr*>(expr));
            break;
        case ExprKind::Unary: result = checkUnary(static_cast<UnaryExpr*>(expr)); break;
        case ExprKind::Binary: result = checkBinary(static_cast<BinaryExpr*>(expr)); break;
        case ExprKind::Assignment:
            result = checkAssignment(static_cast<AssignmentExpr*>(expr));
            break;
        case ExprKind::Conditional:
            result = checkConditional(static_cast<ConditionalExpr*>(expr));
            break;
        case ExprKind::Cast: result = checkCast(static_cast<CastExpr*>(expr)); break;
        case ExprKind::TypeTest: result = checkTypeTest(static_cast<TypeTestExpr*>(expr)); break;
        case ExprKind::Lambda:
            errorAt(expr, 8917, "the delegate type of this lambda cannot be inferred here");
            break;
        case ExprKind::This:
            if (fn_->isStatic) {
                errorAt(expr, 26, "'this' is not available in a static method");
            } else {
                result = fn_->owner;
                if (fn_->lambda)
                    fn_->capturesThis = true;
            }
            break;
        case ExprKind::Base:
            result = fn_->owner && fn_->owner->base ? fn_->owner->base : core.errorType;
            break;
        case ExprKind::Default: {
            auto* node = static_cast<DefaultExpr*>(expr);
            result = node->typeRef ? resolve(node->typeRef) : core.objectType;
            expr->isConstant = isValueKind(result->kind);
            expr->constant = defaultValue(result);
            break;
        }
        case ExprKind::TypeOf:
            errorAt(expr, 8078, "typeof is not supported; there is no reflection at run time");
            break;
        case ExprKind::NameOf: {
            auto* node = static_cast<NameOfExpr*>(expr);
            result = core.stringType;
            expr->isConstant = true;
            expr->constant = rt_.str(node->name);
            break;
        }
        case ExprKind::Convert: result = expr->type; break;
        case ExprKind::Switch:
            result = checkSwitchExpr(static_cast<SwitchExpr*>(expr));
            break;
        case ExprKind::Throw: {
            auto* node = static_cast<ThrowExpr*>(expr);
            checkThrowValue(node->value, expr);
            // A throw expression has whatever type the context wants; the
            // context is what coerces it.
            result = core.nullType;
            break;
        }
        case ExprKind::Discard:
            errorAt(expr, 8183, "'_' cannot be used as a value here");
            break;
        case ExprKind::Tuple:
            result = checkTuple(static_cast<TupleExpr*>(expr));
            break;
        case ExprKind::Sequence: {
            auto* node = static_cast<SequenceExpr*>(expr);
            for (Expr*& before : node->before)
                checkExpr(before);
            result = checkExpr(node->value);
            break;
        }
        case ExprKind::Range:
        case ExprKind::Index:
            errorAt(expr, 8790, "a range or index is only allowed inside []");
            break;
        case ExprKind::Collection:
            errorAt(expr, 9176, "there is no target type for this collection expression");
            break;
        case ExprKind::With:
            result = checkWith(static_cast<WithExpr*>(expr));
            break;
    }

    expr->type = result ? result : core.errorType;
    return expr->type;
}

const TypeInfo* Checker::checkLiteral(LiteralExpr* expr)
{
    const CoreTypes& core = types_.core();
    expr->isConstant = true;
    switch (expr->literal) {
        case LiteralKind::Null: expr->isConstant = false; return core.nullType;
        case LiteralKind::Bool:
            expr->constant = Value(expr->intValue != 0);
            return core.boolType;
        case LiteralKind::Char:
            expr->constant = Value(char16_t(expr->intValue));
            return core.charType;
        case LiteralKind::Int:
            expr->constant = Value(std::int32_t(expr->intValue));
            return core.intType;
        case LiteralKind::UInt:
            expr->constant = Value(std::int32_t(expr->intValue));
            return core.uintType;
        case LiteralKind::Long: expr->constant = Value(expr->intValue); return core.longType;
        case LiteralKind::ULong: expr->constant = Value(expr->intValue); return core.ulongType;
        case LiteralKind::Float:
            expr->constant = Value(float(expr->realValue));
            return core.floatType;
        case LiteralKind::Double:
            expr->constant = Value(expr->realValue);
            return core.doubleType;
        case LiteralKind::String:
            expr->constant = rt_.str(expr->text);
            return core.stringType;
    }
    return core.errorType;
}

const TypeInfo* Checker::checkInterpolated(InterpolatedExpr* expr)
{
    for (Expr*& hole : expr->holes)
        checkExpr(hole);
    return types_.core().stringType;
}

const TypeInfo* Checker::checkName(NameExpr* expr)
{
    const CoreTypes& core = types_.core();

    Function* owner = nullptr;
    if (Local* local = findLocal(expr->name, &owner)) {
        expr->symbol = SymbolKind::Local;
        expr->slot = owner == fn_ ? local->slot : capture(fn_, owner, local->slot);
        if (owner != fn_)
            owner->boxed[local->slot] = 1;
        if (local->isConst && !local->constant.isNull()) {
            expr->isConstant = true;
            expr->constant = local->constant;
        }
        return local->type;
    }

    // A local function of this body, or of one that encloses it.
    for (auto scope = localFunctionScopes_.rbegin(); scope != localFunctionScopes_.rend();
         ++scope) {
        const auto it = scope->find(expr->name);
        if (it == scope->end())
            continue;
        expr->localFunction = it->second;
        expr->symbol = SymbolKind::Method;
        if (it->second->info)
            expr->group.push_back(it->second->info);
        else
            expr->genericGroup.push_back(it->second);
        return core.errorType;
    }

    if (fn_->owner) {
        if (const FieldInfo* field = fn_->owner->findField(expr->name)) {
            expr->field = field;
            expr->slot = field->slot;
            expr->symbol = field->isStatic ? SymbolKind::StaticField : SymbolKind::Field;
            if (field->isConst) {
                expr->isConstant = true;
                expr->constant = field->constValue;
            }
            if (!field->isStatic && fn_->isStatic)
                errorAt(expr, 120, "an object reference is required for '" +
                                       toUtf8(field->name) + "'");
            if (!field->isStatic && fn_->lambda)
                fn_->capturesThis = true;
            return field->type;
        }
        if (const PropertyInfo* prop = fn_->owner->findProperty(expr->name)) {
            expr->prop = prop;
            expr->symbol = prop->isStatic ? SymbolKind::StaticProperty : SymbolKind::Property;
            if (!prop->isStatic && fn_->lambda)
                fn_->capturesThis = true;
            return prop->type;
        }
        std::vector<MethodInfo*> group;
        fn_->owner->collectMethods(expr->name, group);
        collectGenericMethods(fn_->owner, expr->name, expr->genericGroup);
        if (!group.empty() || !expr->genericGroup.empty()) {
            expr->group = group;
            expr->symbol = SymbolKind::Method;
            return core.errorType;  // only an invocation or a delegate gives it a type
        }
    }

    if (const TypeInfo* type = lookupType(expr->name)) {
        expr->symbol = SymbolKind::Type;
        expr->namedType = type;
        return core.errorType;
    }

    errorAt(expr, 103, "the name '" + toUtf8(expr->name) + "' does not exist in the current "
                                                           "context");
    return core.errorType;
}

namespace {

/// `a.b.c` as one dotted name, or empty if the expression is not a plain path.
String dottedPath(const Expr* expr)
{
    checkNativeStack();
    if (expr->kind == ExprKind::Name)
        return static_cast<const NameExpr*>(expr)->name;
    if (expr->kind != ExprKind::MemberAccess)
        return String();
    const auto* member = static_cast<const MemberAccessExpr*>(expr);
    if (!member->target || member->nullConditional)
        return String();
    const String prefix = dottedPath(member->target);
    if (prefix.empty())
        return String();
    return prefix + u"." + member->name;
}

}  // namespace

const TypeInfo* Checker::checkMember(MemberAccessExpr* expr)
{
    const CoreTypes& core = types_.core();

    // `Console.WriteLine`: the left side may name a type rather than a value.
    const String path = dottedPath(expr->target);
    if (!path.empty()) {
        const String head = path.substr(0, path.find(u'.'));
        const bool shadowed = findLocal(head) || (fn_->owner && fn_->owner->findField(head)) ||
                              (fn_->owner && fn_->owner->findProperty(head));
        if (!shadowed) {
            if (const TypeInfo* type = lookupType(path)) {
                expr->staticTarget = type;
                expr->target = nullptr;
            }
        }
    }

    const TypeInfo* target = expr->staticTarget;
    if (!target) {
        target = checkExpr(expr->target);
        if (target->kind == TypeKind::Error)
            return core.errorType;
        if (expr->target->kind == ExprKind::Name &&
            static_cast<NameExpr*>(expr->target)->symbol == SymbolKind::Type) {
            expr->staticTarget = static_cast<NameExpr*>(expr->target)->namedType;
            expr->target = nullptr;
            target = expr->staticTarget;
        }
    }
    const bool isStaticAccess = expr->target == nullptr;

    if (!isStaticAccess && target->kind == TypeKind::Array && expr->name == u"Length") {
        expr->arrayLength = true;
        expr->symbol = SymbolKind::Property;
        return core.intType;
    }

    if (const FieldInfo* field = target->findField(expr->name)) {
        if (field->isStatic != isStaticAccess && !field->isConst) {
            errorAt(expr, field->isStatic ? 176 : 120,
                    "'" + toUtf8(field->name) + "' cannot be accessed this way");
        }
        expr->field = field;
        expr->symbol = field->isStatic ? SymbolKind::StaticField : SymbolKind::Field;
        if (field->isConst) {
            expr->isConstant = true;
            expr->constant = field->constValue;
        }
        return field->type;
    }
    if (const PropertyInfo* prop = target->findProperty(expr->name)) {
        expr->prop = prop;
        expr->symbol = prop->isStatic ? SymbolKind::StaticProperty : SymbolKind::Property;
        return prop->type;
    }

    std::vector<MethodInfo*> group;
    target->collectMethods(expr->name, group);
    collectGenericMethods(target, expr->name, expr->genericGroup);
    if (!group.empty() || !expr->genericGroup.empty()) {
        expr->group = group;
        expr->symbol = SymbolKind::Method;
        return core.errorType;
    }

    errorAt(expr, 117,
            "'" + describe(target) + "' does not contain a definition for '" +
                toUtf8(expr->name) + "'");
    return core.errorType;
}

void Checker::collectGenericMethods(const TypeInfo* type, StringView name,
                                    std::vector<MethodDecl*>& out) const
{
    for (const TypeInfo* t = type; t; t = t->base) {
        const auto it = genericMethods_.find(t);
        if (it == genericMethods_.end())
            continue;
        for (MethodDecl* method : it->second) {
            if (method->name == name)
                out.push_back(method);
        }
    }
}

MethodInfo* Checker::resolveGenericCall(const std::vector<MethodDecl*>& candidates,
                                        const std::vector<TypeRef*>& explicitArgs,
                                        std::vector<Argument>& args, const String& name,
                                        std::uint32_t line, std::uint32_t column)
{
    // The arguments have to be typed before anything can be inferred from
    // them. A lambda has no type until its target is known, so it sits this
    // round out and gets its type from the parameter once T is bound.
    for (Argument& arg : args) {
        if (arg.value && arg.value->kind != ExprKind::Lambda)
            checkExpr(arg.value);
    }

    std::vector<MethodInfo*> instantiated;
    for (MethodDecl* candidate : candidates) {
        if (candidate->params.size() != args.size()) {
            const bool variadic = !candidate->params.empty() &&
                                  candidate->params.back().mode == ParamMode::Params;
            if (!variadic && args.size() > candidate->params.size())
                continue;
        }
        std::vector<const TypeInfo*> bound;
        if (!explicitArgs.empty()) {
            if (explicitArgs.size() != candidate->typeParams.size())
                continue;
            for (TypeRef* ref : explicitArgs)
                bound.push_back(resolve(ref));
        } else if (!inferTypeArguments(candidate, args, bound)) {
            continue;
        }
        if (MethodInfo* info = instantiateMethod(candidate, bound, line, column, false))
            instantiated.push_back(info);
    }

    if (instantiated.empty()) {
        error(line, column, 411,
              "the type arguments for '" + toUtf8(name) +
                  "' cannot be inferred from the arguments; specify them explicitly");
        return nullptr;
    }
    return pickOverload(instantiated, args, name, line, column, true);
}

const TypeInfo* Checker::checkInvocation(InvocationExpr* expr)
{
    const CoreTypes& core = types_.core();
    std::vector<MethodInfo*>* group = nullptr;
    std::vector<MethodDecl*>* genericGroup = nullptr;
    const std::vector<TypeRef*>* explicitTypeArgs = nullptr;
    String name;
    bool staticCall = false;

    if (expr->callee->kind == ExprKind::Name) {
        auto* callee = static_cast<NameExpr*>(expr->callee);
        checkExpr(expr->callee);
        name = callee->name;
        explicitTypeArgs = &callee->typeArgs;
        if (callee->symbol == SymbolKind::Method) {
            group = &callee->group;
            genericGroup = &callee->genericGroup;
            staticCall = fn_->isStatic;
            const bool instanceTarget =
                (!group->empty() && !group->front()->isStatic()) ||
                (group->empty() && !genericGroup->empty() &&
                 !(genericGroup->front()->modifiers & ModStatic));
            if (instanceTarget) {
                ThisExpr* self = arena_.make<ThisExpr>();
                self->line = expr->line;
                self->column = expr->column;
                self->type = fn_->owner;
                expr->receiver = self;
                if (fn_->lambda)
                    fn_->capturesThis = true;
            }
        } else if (callee->type && callee->type->kind == TypeKind::Delegate) {
            expr->delegateCall = true;
        }
    } else if (expr->callee->kind == ExprKind::MemberAccess) {
        auto* callee = static_cast<MemberAccessExpr*>(expr->callee);
        const TypeInfo* calleeType = checkExpr(expr->callee);
        name = callee->name;
        explicitTypeArgs = &callee->typeArgs;
        expr->nullConditional = callee->nullConditional;
        if (callee->symbol == SymbolKind::Method) {
            group = &callee->group;
            genericGroup = &callee->genericGroup;
            staticCall = callee->target == nullptr;
            expr->receiver = callee->target;
        } else if (calleeType && calleeType->kind == TypeKind::Delegate) {
            expr->delegateCall = true;
        }
    } else {
        const TypeInfo* calleeType = checkExpr(expr->callee);
        if (calleeType->kind == TypeKind::Delegate)
            expr->delegateCall = true;
    }

    if (expr->delegateCall) {
        const TypeInfo* delegateType = expr->callee->type;
        const MethodInfo* invoke = delegateType ? delegateType->invoke : nullptr;
        if (!invoke) {
            errorAt(expr, 149, "the delegate has no Invoke signature");
            return core.errorType;
        }
        if (expr->args.size() != invoke->params.size()) {
            errorAt(expr, 1593,
                    "the delegate takes " + std::to_string(invoke->params.size()) +
                        " argument(s)");
            return core.errorType;
        }
        for (std::size_t i = 0; i < expr->args.size(); ++i)
            expr->args[i].value = coerce(expr->args[i].value, invoke->params[i].type, "argument");
        return invoke->returnType;
    }

    if (!group) {
        errorAt(expr, 149, "this expression is not a method or a delegate");
        for (Argument& arg : expr->args)
            checkExpr(arg.value);
        return core.errorType;
    }

    MethodInfo* method = nullptr;
    const bool wantsGeneric =
        genericGroup && !genericGroup->empty() &&
        (group->empty() || (explicitTypeArgs && !explicitTypeArgs->empty()) ||
         !pickOverload(*group, expr->args, name, expr->line, expr->column, false));
    if (wantsGeneric) {
        static const std::vector<TypeRef*> kNone;
        method = resolveGenericCall(*genericGroup, explicitTypeArgs ? *explicitTypeArgs : kNone,
                                    expr->args, name, expr->line, expr->column);
    } else {
        method = pickOverload(*group, expr->args, name, expr->line, expr->column, true);
    }
    if (!method)
        return core.errorType;

    if (method->isStatic() != staticCall && !method->isStatic() && staticCall) {
        errorAt(expr, 120,
                "an object reference is required for '" + method->qualifiedName() + "'");
    }
    expr->method = method;
    if (!method->isStatic()) {
        if (!expr->receiver) {
            ThisExpr* self = arena_.make<ThisExpr>();
            self->line = expr->line;
            self->column = expr->column;
            self->type = fn_->owner;
            expr->receiver = self;
        }
        const bool viaBase = expr->receiver->kind == ExprKind::Base;
        expr->interfaceCall = method->owner && method->owner->kind == TypeKind::Interface;
        expr->virtualCall = !viaBase && !expr->interfaceCall && method->isVirtual();
    }
    return method->returnType;
}

const TypeInfo* Checker::checkElementAccess(ElementAccessExpr* expr)
{
    const CoreTypes& core = types_.core();
    const TypeInfo* target = checkExpr(expr->target);
    if (target->kind == TypeKind::Error)
        return core.errorType;

    // `a[^n]` and `a[i..j]` are rewritten into arithmetic over the length,
    // which is all they are; there is no Index or Range object at run time.
    if (expr->index && expr->index->kind == ExprKind::Index) {
        auto* fromEnd = static_cast<IndexExpr*>(expr->index);
        Expr* offset = coerce(fromEnd->operand, core.intType, "index");
        Expr* length = lengthOf(expr->target, target, expr);
        if (!length)
            return core.errorType;
        BinaryExpr* position = arena_.make<BinaryExpr>();
        position->line = expr->line;
        position->column = expr->column;
        position->op = BinaryOp::Subtract;
        position->left = length;
        position->right = offset;
        position->type = core.intType;
        position->operandType = core.intType;
        expr->index = position;
        expr->fromEnd = true;
    } else if (expr->index && expr->index->kind == ExprKind::Range) {
        return checkSlice(expr, target);
    }

    if (target->kind == TypeKind::Array) {
        expr->index = coerce(expr->index, core.intType, "index");
        return target->element;
    }
    if (target->kind == TypeKind::String) {
        expr->index = coerce(expr->index, core.intType, "index");
        std::vector<MethodInfo*> getters;
        target->collectMethods(ascii("get_Chars"), getters);
        expr->getter = getters.empty() ? nullptr : getters[0];
        return core.charType;
    }
    if (target->nativeKind == NativeKind::List || target->nativeKind == NativeKind::Dictionary) {
        std::vector<MethodInfo*> getters;
        std::vector<MethodInfo*> setters;
        target->collectMethods(ascii("get_Item"), getters);
        target->collectMethods(ascii("set_Item"), setters);
        expr->getter = getters.empty() ? nullptr : getters[0];
        expr->setter = setters.empty() ? nullptr : setters[0];
        if (!expr->getter)
            return core.errorType;
        expr->index = coerce(expr->index, expr->getter->params[0].type, "index");
        return expr->getter->returnType;
    }

    errorAt(expr, 21, "'" + describe(target) + "' cannot be indexed");
    return core.errorType;
}

Expr* Checker::lengthOf(Expr* target, const TypeInfo* type, const Expr* at)
{
    const CoreTypes& core = types_.core();
    if (type->kind == TypeKind::Array) {
        MemberAccessExpr* node = arena_.make<MemberAccessExpr>();
        node->line = at->line;
        node->column = at->column;
        node->target = target;
        node->name = ascii("Length");
        node->arrayLength = true;
        node->type = core.intType;
        node->symbol = SymbolKind::Field;
        return node;
    }
    if (type->kind == TypeKind::String) {
        std::vector<MethodInfo*> getters;
        type->collectMethods(ascii("get_Length"), getters);
        if (getters.empty())
            return nullptr;
        InvocationExpr* call = arena_.make<InvocationExpr>();
        call->line = at->line;
        call->column = at->column;
        call->method = getters.front();
        call->receiver = target;
        call->callee = target;
        call->type = core.intType;
        return call;
    }
    std::vector<MethodInfo*> getters;
    type->collectMethods(ascii("get_Count"), getters);
    if (getters.empty())
        return nullptr;
    InvocationExpr* call = arena_.make<InvocationExpr>();
    call->line = at->line;
    call->column = at->column;
    call->method = getters.front();
    call->receiver = target;
    call->callee = target;
    call->type = core.intType;
    return call;
}

const TypeInfo* Checker::checkSlice(ElementAccessExpr* expr, const TypeInfo* target)
{
    const CoreTypes& core = types_.core();
    auto* range = static_cast<RangeExpr*>(expr->index);

    // The subject is read several times, so it goes through a temporary.
    const std::uint32_t slot = addTemporary(target);
    AssignmentExpr* store = arena_.make<AssignmentExpr>();
    store->line = expr->line;
    store->column = expr->column;
    store->target = makeLocalRead(slot, target, expr);
    store->value = expr->target;
    store->type = target;

    const auto position = [&](Expr* bound, bool isEnd) -> Expr* {
        if (!bound)
            return isEnd ? lengthOf(makeLocalRead(slot, target, expr), target, expr)
                         : makeInt(0, expr);
        if (bound->kind == ExprKind::Index) {
            Expr* offset = coerce(static_cast<IndexExpr*>(bound)->operand, core.intType, "index");
            BinaryExpr* node = arena_.make<BinaryExpr>();
            node->line = expr->line;
            node->column = expr->column;
            node->op = BinaryOp::Subtract;
            node->left = lengthOf(makeLocalRead(slot, target, expr), target, expr);
            node->right = offset;
            node->type = core.intType;
            node->operandType = core.intType;
            return node;
        }
        return coerce(bound, core.intType, "index");
    };

    Expr* start = position(range->start, false);
    Expr* end = position(range->end, true);
    if (!start || !end) {
        errorAt(expr, 21, "'" + describe(target) + "' cannot be sliced");
        return core.errorType;
    }

    // A slice is Substring for a string and an array copy otherwise, both of
    // which the library already provides.
    if (target->kind == TypeKind::String) {
        std::vector<MethodInfo*> candidates;
        target->collectMethods(ascii("Substring"), candidates);
        MethodInfo* chosen = nullptr;
        for (MethodInfo* candidate : candidates) {
            if (candidate->params.size() == 2)
                chosen = candidate;
        }
        if (!chosen) {
            errorAt(expr, 21, "string slicing needs Substring(int, int)");
            return core.errorType;
        }
        BinaryExpr* count = arena_.make<BinaryExpr>();
        count->line = expr->line;
        count->op = BinaryOp::Subtract;
        count->left = end;
        count->right = start;
        count->type = core.intType;
        count->operandType = core.intType;

        InvocationExpr* call = arena_.make<InvocationExpr>();
        call->line = expr->line;
        call->column = expr->column;
        call->method = chosen;
        call->receiver = makeLocalRead(slot, target, expr);
        call->callee = call->receiver;
        call->type = core.stringType;
        Argument a;
        a.value = start;
        Argument b;
        b.value = count;
        call->args.push_back(std::move(a));
        call->args.push_back(std::move(b));

        SequenceExpr* seq = arena_.make<SequenceExpr>();
        seq->line = expr->line;
        seq->column = expr->column;
        seq->before.push_back(store);
        seq->value = call;
        seq->type = core.stringType;
        expr->slice = seq;
        expr->isSlice = true;
        return core.stringType;
    }

    if (target->kind != TypeKind::Array) {
        errorAt(expr, 21, "only an array or a string can be sliced");
        return core.errorType;
    }
    const TypeInfo* arrayType = lookupType(ascii("System.Array"));
    std::vector<MethodInfo*> candidates;
    if (arrayType)
        arrayType->collectMethods(ascii("Slice"), candidates);
    if (candidates.empty()) {
        errorAt(expr, 21, "array slicing needs System.Array.Slice");
        return core.errorType;
    }
    InvocationExpr* call = arena_.make<InvocationExpr>();
    call->line = expr->line;
    call->column = expr->column;
    call->method = candidates.front();
    call->callee = nullptr;
    call->type = target;
    Argument source;
    source.value = makeLocalRead(slot, target, expr);
    Argument from;
    from.value = start;
    Argument to;
    to.value = end;
    call->args.push_back(std::move(source));
    call->args.push_back(std::move(from));
    call->args.push_back(std::move(to));

    SequenceExpr* seq = arena_.make<SequenceExpr>();
    seq->line = expr->line;
    seq->column = expr->column;
    seq->before.push_back(store);
    seq->value = call;
    seq->type = target;
    expr->slice = seq;
    expr->isSlice = true;
    return target;
}

const TypeInfo* Checker::checkCollection(CollectionExpr* expr, const TypeInfo* target)
{
    const CoreTypes& core = types_.core();
    if (!target || target->kind == TypeKind::Error) {
        errorAt(expr, 9176, "there is no target type for this collection expression");
        return core.errorType;
    }

    // `[a, b]` builds whatever it is assigned to: an array directly, or a
    // collection through its constructor and Add.
    if (target->kind == TypeKind::Array) {
        ArrayCreationExpr* array = arena_.make<ArrayCreationExpr>();
        array->line = expr->line;
        array->column = expr->column;
        array->hasInitializer = true;
        array->type = target;
        for (CollectionElement& element : expr->elements) {
            if (element.spread) {
                errorAt(element.value, 9176,
                        "a spread element needs a collection target, not an array");
                continue;
            }
            array->elements.push_back(coerce(element.value, target->element, "element"));
        }
        expr->lowered = array;
        return target;
    }

    std::vector<MethodInfo*> ctors;
    target->collectMethods(ascii(".ctor"), ctors);
    for (MethodInfo* candidate : ctors) {
        if (candidate->params.empty())
            expr->ctor = candidate;
    }
    std::vector<MethodInfo*> adders;
    target->collectMethods(ascii("Add"), adders);
    for (MethodInfo* candidate : adders) {
        if (candidate->params.size() == 1)
            expr->adder = candidate;
    }
    if (!expr->ctor || !expr->adder) {
        errorAt(expr, 9176,
                "'" + describe(target) +
                    "' cannot be built from a collection expression; it needs a parameterless "
                    "constructor and Add");
        return core.errorType;
    }

    const TypeInfo* element = expr->adder->params[0].type;
    const std::uint32_t slot = addTemporary(target);
    ObjectCreationExpr* create = arena_.make<ObjectCreationExpr>();
    create->line = expr->line;
    create->column = expr->column;
    create->ctor = expr->ctor;
    create->type = target;

    AssignmentExpr* store = arena_.make<AssignmentExpr>();
    store->line = expr->line;
    store->column = expr->column;
    store->target = makeLocalRead(slot, target, expr);
    store->value = create;
    store->type = target;

    SequenceExpr* seq = arena_.make<SequenceExpr>();
    seq->line = expr->line;
    seq->column = expr->column;
    seq->before.push_back(store);

    for (CollectionElement& element_expr : expr->elements) {
        if (element_expr.spread) {
            // `..other` copies each of its elements in.
            const TypeInfo* sourceType = checkExpr(element_expr.value);
            ForEachStmt* loop = nullptr;
            (void)loop;
            if (!sourceType || sourceType->kind == TypeKind::Error)
                continue;
            errorAt(element_expr.value, 9176,
                    "a spread element is not supported yet; add the items in a loop");
            continue;
        }
        InvocationExpr* add = arena_.make<InvocationExpr>();
        add->line = element_expr.value->line;
        add->column = element_expr.value->column;
        add->method = expr->adder;
        add->receiver = makeLocalRead(slot, target, expr);
        add->callee = add->receiver;
        add->type = core.voidType;
        Argument arg;
        arg.value = coerce(element_expr.value, element, "element");
        add->args.push_back(std::move(arg));
        seq->before.push_back(add);
    }
    seq->value = makeLocalRead(slot, target, expr);
    seq->type = target;
    expr->lowered = seq;
    return target;
}

const TypeInfo* Checker::checkObjectCreation(ObjectCreationExpr* expr)
{
    if (!expr->typeRef) {
        errorAt(expr, 8754, "there is no target type for this 'new'");
        return types_.core().errorType;
    }
    return checkObjectCreationOf(expr, resolve(expr->typeRef));
}

const TypeInfo* Checker::checkObjectCreationOf(ObjectCreationExpr* expr, const TypeInfo* type)
{
    const CoreTypes& core = types_.core();
    if (!type || type->kind == TypeKind::Error)
        return core.errorType;
    if (type->isAbstract)
        errorAt(expr, 144, "cannot create an instance of the abstract type '" + describe(type) +
                               "'");

    std::vector<MethodInfo*> ctors;
    type->collectMethods(ascii(".ctor"), ctors);
    if (!ctors.empty()) {
        expr->ctor = pickOverload(ctors, expr->args, type->name, expr->line, expr->column, true);
    } else if (!expr->args.empty()) {
        errorAt(expr, 1729, "'" + describe(type) + "' does not have a constructor taking these "
                            "arguments");
    }

    for (InitializerEntry& entry : expr->initializers) {
        if (!entry.name.empty()) {
            if (const FieldInfo* field = type->findField(entry.name)) {
                entry.field = field;
                entry.value = coerce(entry.value, field->type, "assignment");
                continue;
            }
            if (const PropertyInfo* prop = type->findProperty(entry.name)) {
                entry.prop = prop;
                entry.value = coerce(entry.value, prop->type, "assignment");
                continue;
            }
            errorAt(entry.value, 117,
                    "'" + describe(type) + "' has no member '" + toUtf8(entry.name) + "'");
            continue;
        }
        // A dictionary initializer entry: `{ key, value }` or `[key] = value`.
        if (type->nativeKind == NativeKind::Dictionary && type->typeArgs.size() == 2) {
            entry.key = coerce(entry.key, type->typeArgs[0], "key");
            entry.value = coerce(entry.value, type->typeArgs[1], "value");
        } else {
            checkExpr(entry.key);
            checkExpr(entry.value);
            errorAt(entry.value, 1922, "a collection initializer needs a Dictionary<K,V>");
        }
    }
    if (!expr->elements.empty()) {
        const TypeInfo* element =
            type->nativeKind == NativeKind::List && !type->typeArgs.empty() ? type->typeArgs[0]
                                                                           : nullptr;
        if (!element) {
            errorAt(expr, 1922, "a collection initializer needs a List<T>");
        } else {
            for (Expr*& item : expr->elements)
                item = coerce(item, element, "element");
        }
    }
    return type;
}

const TypeInfo* Checker::checkArrayCreation(ArrayCreationExpr* expr)
{
    const CoreTypes& core = types_.core();
    const TypeInfo* element = nullptr;
    if (expr->type) {
        element = expr->type->element;
    } else if (expr->elementTypeRef) {
        element = resolve(expr->elementTypeRef);
    } else {
        // `new[] { ... }`: the one element type every other element converts to.
        for (Expr* item : expr->elements)
            checkExpr(item);
        for (const Expr* candidate : expr->elements) {
            const TypeInfo* type = candidate->type;
            if (!type || type->kind == TypeKind::Null)
                continue;
            bool fits = true;
            for (const Expr* item : expr->elements)
                fits = fits && (item->type == type || types_.implicitlyConverts(item->type, type));
            if (fits) {
                element = type;
                break;
            }
        }
        if (!element) {
            errorAt(expr, 826, "no best type found for the implicitly typed array");
            return core.errorType;
        }
    }
    if (!element)
        element = core.objectType;
    if (expr->size)
        expr->size = coerce(expr->size, core.intType, "array length");
    for (Expr*& item : expr->elements)
        item = coerce(item, element, "element");
    return types_.arrayOf(element);
}

const TypeInfo* Checker::checkUnary(UnaryExpr* expr)
{
    const CoreTypes& core = types_.core();
    const TypeInfo* operand = checkExpr(expr->operand);
    if (operand->kind == TypeKind::Error)
        return core.errorType;

    switch (expr->op) {
        case UnaryOp::Not:
            expr->operand = coerce(expr->operand, core.boolType, "operator !");
            if (expr->operand->isConstant) {
                expr->isConstant = true;
                expr->constant = Value(!expr->operand->constant.toBool());
            }
            return core.boolType;
        case UnaryOp::Complement: {
            if (!isIntegral(operand->kind) && operand->kind != TypeKind::Enum) {
                errorAt(expr, 23, "operator '~' cannot be applied to '" + describe(operand) + "'");
                return core.errorType;
            }
            const TypeInfo* promoted = types_.promote(operand, core.intType);
            expr->operand = convert(expr->operand, promoted);
            return promoted;
        }
        case UnaryOp::Plus:
        case UnaryOp::Minus: {
            if (!isNumeric(operand->kind)) {
                errorAt(expr, 23, std::string("operator '") + operatorText(expr->op) +
                                      "' cannot be applied to '" + describe(operand) + "'");
                return core.errorType;
            }
            const TypeInfo* promoted = types_.promote(operand, core.intType);
            expr->operand = convert(expr->operand, promoted);
            if (expr->operand->isConstant && expr->op == UnaryOp::Minus) {
                expr->isConstant = true;
                const Value& v = expr->operand->constant;
                switch (promoted->kind) {
                    case TypeKind::Double: expr->constant = Value(-v.toDouble()); break;
                    case TypeKind::Float: expr->constant = Value(float(-v.toDouble())); break;
                    case TypeKind::Long:
                    case TypeKind::ULong: expr->constant = Value(-v.toLong()); break;
                    default: expr->constant = Value(-v.toInt()); break;
                }
            } else if (expr->operand->isConstant) {
                expr->isConstant = true;
                expr->constant = expr->operand->constant;
            }
            return promoted;
        }
        case UnaryOp::PreIncrement:
        case UnaryOp::PreDecrement:
        case UnaryOp::PostIncrement:
        case UnaryOp::PostDecrement:
            if (!isNumeric(operand->kind)) {
                errorAt(expr, 23, "the operand must be numeric");
                return core.errorType;
            }
            if (!assignable(expr->operand))
                errorAt(expr, 1059, "the operand must be a variable, a field or a property");
            else
                checkNotReadOnly(expr->operand);
            return operand;
    }
    return core.errorType;
}

/// A shift is not a binary numeric promotion: the result is the left
/// operand's own type, widened to at least int.
const TypeInfo* Checker::shiftResult(const TypeInfo* left) const
{
    const CoreTypes& core = types_.core();
    if (!left)
        return core.intType;
    switch (left->kind) {
        case TypeKind::UInt: return core.uintType;
        case TypeKind::Long: return core.longType;
        case TypeKind::ULong: return core.ulongType;
        default: return core.intType;
    }
}

bool Checker::foldBinary(BinaryExpr* expr, const TypeInfo* operandType)
{
    if (!expr->left->isConstant || !expr->right->isConstant || !operandType)
        return false;
    const Value& a = expr->left->constant;
    const Value& b = expr->right->constant;

    if (operandType->kind == TypeKind::Double || operandType->kind == TypeKind::Float) {
        const double x = a.toDouble();
        const double y = b.toDouble();
        double result = 0;
        switch (expr->op) {
            case BinaryOp::Add: result = x + y; break;
            case BinaryOp::Subtract: result = x - y; break;
            case BinaryOp::Multiply: result = x * y; break;
            case BinaryOp::Divide:
                if (y == 0)
                    return false;
                result = x / y;
                break;
            default: return false;
        }
        expr->isConstant = true;
        expr->constant = operandType->kind == TypeKind::Float ? Value(float(result))
                                                              : Value(result);
        return true;
    }
    if (!isIntegral(operandType->kind))
        return false;

    const std::int64_t x = a.toLong();
    const std::int64_t y = b.toLong();
    std::int64_t result = 0;
    switch (expr->op) {
        case BinaryOp::Add: result = x + y; break;
        case BinaryOp::Subtract: result = x - y; break;
        case BinaryOp::Multiply: result = x * y; break;
        case BinaryOp::Divide:
            if (y == 0)
                return false;
            result = x / y;
            break;
        case BinaryOp::Modulo:
            if (y == 0)
                return false;
            result = x % y;
            break;
        case BinaryOp::And: result = x & y; break;
        case BinaryOp::Or: result = x | y; break;
        case BinaryOp::Xor: result = x ^ y; break;
        case BinaryOp::ShiftLeft: result = x << (y & 63); break;
        case BinaryOp::ShiftRight: result = x >> (y & 63); break;
        default: return false;
    }
    expr->isConstant = true;
    expr->constant = operandType->kind == TypeKind::Long || operandType->kind == TypeKind::ULong
                         ? Value(result)
                         : Value(std::int32_t(result));
    return true;
}

const TypeInfo* Checker::checkBinary(BinaryExpr* expr)
{
    const CoreTypes& core = types_.core();

    if (expr->op == BinaryOp::LogicalAnd || expr->op == BinaryOp::LogicalOr) {
        expr->left = coerce(expr->left, core.boolType, "operator");
        expr->right = coerce(expr->right, core.boolType, "operator");
        expr->operandType = core.boolType;
        return core.boolType;
    }
    if (expr->op == BinaryOp::Coalesce) {
        const TypeInfo* left = checkExpr(expr->left);
        const TypeInfo* right = checkExpr(expr->right);
        // `a?.Length` has type `int` but evaluates to null when `a` is, which
        // is the one place a value type may sit to the left of `??`.
        const bool conditional =
            (expr->left->kind == ExprKind::MemberAccess &&
             static_cast<MemberAccessExpr*>(expr->left)->nullConditional) ||
            (expr->left->kind == ExprKind::Invocation &&
             static_cast<InvocationExpr*>(expr->left)->nullConditional) ||
            (expr->left->kind == ExprKind::ElementAccess &&
             static_cast<ElementAccessExpr*>(expr->left)->nullConditional);
        if (left->isValueType() && left->kind != TypeKind::Error && !conditional)
            errorAt(expr, 19, "the left operand of '?\?' must be a reference type");
        expr->right = coerce(expr->right, left, "operator ??");
        (void)right;
        expr->operandType = left;
        return left;
    }

    const TypeInfo* left = checkExpr(expr->left);
    const TypeInfo* right = checkExpr(expr->right);
    if (left->kind == TypeKind::Error || right->kind == TypeKind::Error)
        return core.errorType;

    switch (expr->op) {
        case BinaryOp::Add:
            if (isStringKind(left) || isStringKind(right)) {
                expr->left = coerce(expr->left, core.stringType, "concatenation");
                expr->right = coerce(expr->right, core.stringType, "concatenation");
                expr->operandType = core.stringType;
                return core.stringType;
            }
            break;
        case BinaryOp::Equal:
        case BinaryOp::NotEqual: {
            if (isStringKind(left) && isStringKind(right)) {
                expr->operandType = core.stringType;
                return core.boolType;
            }
            if (left->kind == TypeKind::Bool && right->kind == TypeKind::Bool) {
                expr->operandType = core.boolType;
                return core.boolType;
            }
            const bool leftRef = isReferenceKind(left->kind);
            const bool rightRef = isReferenceKind(right->kind);
            if (leftRef || rightRef) {
                if (left->kind == TypeKind::Null || right->kind == TypeKind::Null) {
                    expr->operandType = core.nullType;
                    return core.boolType;
                }
                if (!leftRef || !rightRef) {
                    // One side is a value type: box it and compare as objects.
                    expr->left = coerce(expr->left, core.objectType, "operator");
                    expr->right = coerce(expr->right, core.objectType, "operator");
                    expr->operandType = core.objectType;
                    return core.boolType;
                }
                expr->operandType = core.objectType;
                return core.boolType;
            }
            break;
        }
        default: break;
    }

    const TypeInfo* promoted = types_.promote(left, right);
    if (!promoted) {
        errorAt(expr, 19, std::string("operator '") + operatorText(expr->op) +
                              "' cannot be applied to '" + describe(left) + "' and '" +
                              describe(right) + "'");
        return core.errorType;
    }

    if (expr->op == BinaryOp::ShiftLeft || expr->op == BinaryOp::ShiftRight) {
        // A shift is not a binary numeric promotion: the result is the left
        // operand's own type, widened to at least int.
        const TypeInfo* shifted = shiftResult(left);
        expr->left = convert(expr->left, shifted);
        expr->right = coerce(expr->right, core.intType, "shift count");
        expr->operandType = shifted;
        foldBinary(expr, shifted);
        return shifted;
    }

    expr->left = convert(expr->left, promoted);
    expr->right = convert(expr->right, promoted);
    expr->operandType = promoted;

    switch (expr->op) {
        case BinaryOp::Less:
        case BinaryOp::LessEqual:
        case BinaryOp::Greater:
        case BinaryOp::GreaterEqual:
        case BinaryOp::Equal:
        case BinaryOp::NotEqual: return core.boolType;
        default: break;
    }
    if ((expr->op == BinaryOp::And || expr->op == BinaryOp::Or || expr->op == BinaryOp::Xor) &&
        !isIntegral(promoted->kind)) {
        errorAt(expr, 19, "the operands must be integral");
        return core.errorType;
    }
    foldBinary(expr, promoted);
    return promoted;
}

/// A readonly field takes a value in its declaration or in a constructor of its
/// own type, through `this`, and nowhere else. A field of a struct held in a
/// readonly field is part of that field's value.
void Checker::checkNotReadOnly(const Expr* target)
{
    while (target) {
        const FieldInfo* field = nullptr;
        const Expr* receiver = nullptr;
        if (target->kind == ExprKind::Name) {
            field = static_cast<const NameExpr*>(target)->field;
        } else if (target->kind == ExprKind::MemberAccess) {
            const auto* member = static_cast<const MemberAccessExpr*>(target);
            field = member->field;
            receiver = member->target;
        }
        if (!field)
            return;
        if (field->isReadOnly) {
            const MethodInfo* method = fn_ ? fn_->method : nullptr;
            const bool inInitializer =
                method && !fn_->lambda && fn_->owner == field->owner &&
                (method->isConstructor() || method->name == ascii(".cctor")) &&
                method->isStatic() == field->isStatic;
            const bool throughThis = field->isStatic || !receiver || receiver->kind == ExprKind::This;
            if (!inInitializer || !throughThis) {
                errorAt(target, field->isStatic ? 198 : 191,
                        "the readonly field '" + toUtf8(field->name) +
                            "' can only be assigned in a constructor of its own type");
                return;
            }
        }
        if (!field->owner || !field->owner->isValueType())
            return;
        target = receiver;
    }
}

bool Checker::assignable(Expr* expr) const
{
    if (!expr)
        return false;
    switch (expr->kind) {
        case ExprKind::Name: {
            const auto* name = static_cast<const NameExpr*>(expr);
            if (name->symbol == SymbolKind::Property || name->symbol == SymbolKind::StaticProperty)
                return name->prop && name->prop->setter;
            return name->symbol == SymbolKind::Local || name->symbol == SymbolKind::Field ||
                   name->symbol == SymbolKind::StaticField;
        }
        case ExprKind::MemberAccess: {
            const auto* member = static_cast<const MemberAccessExpr*>(expr);
            if (member->arrayLength)
                return false;
            if (member->symbol == SymbolKind::Property ||
                member->symbol == SymbolKind::StaticProperty)
                return member->prop && member->prop->setter;
            return member->symbol == SymbolKind::Field ||
                   member->symbol == SymbolKind::StaticField;
        }
        case ExprKind::ElementAccess: {
            const auto* element = static_cast<const ElementAccessExpr*>(expr);
            // A string is indexable but not assignable through the indexer.
            return !element->getter || element->setter;
        }
        default: return false;
    }
}

const TypeInfo* Checker::checkAssignment(AssignmentExpr* expr)
{
    const CoreTypes& core = types_.core();
    const TypeInfo* target = checkExpr(expr->target);
    if (target->kind == TypeKind::Error) {
        checkExpr(expr->value);
        return core.errorType;
    }
    if (!assignable(expr->target)) {
        const bool readOnlyProperty =
            (expr->target->kind == ExprKind::MemberAccess &&
             static_cast<MemberAccessExpr*>(expr->target)->prop) ||
            (expr->target->kind == ExprKind::Name &&
             static_cast<NameExpr*>(expr->target)->prop);
        errorAt(expr, readOnlyProperty ? 200 : 131,
                readOnlyProperty
                    ? "the property has no setter"
                    : "the left-hand side of an assignment must be a variable, a field or a "
                      "property");
        checkExpr(expr->value);
        return target;
    }
    if (expr->target->kind == ExprKind::Name) {
        const auto* name = static_cast<const NameExpr*>(expr->target);
        if (name->field && name->field->isConst)
            errorAt(expr, 131, "a const field cannot be assigned to");
        else if (name->symbol == SymbolKind::Local && name->isConstant)
            errorAt(expr, 131, "a const local cannot be assigned to");
    }

    checkNotReadOnly(expr->target);

    // An init-only property closes as soon as the object is built, so it can
    // only be assigned from inside its own type.
    const PropertyInfo* assigned = nullptr;
    if (expr->target->kind == ExprKind::MemberAccess)
        assigned = static_cast<MemberAccessExpr*>(expr->target)->prop;
    else if (expr->target->kind == ExprKind::Name)
        assigned = static_cast<NameExpr*>(expr->target)->prop;
    if (assigned && assigned->isInitOnly && fn_->owner != assigned->owner)
        errorAt(expr, 8852,
                "'" + toUtf8(assigned->name) +
                    "' is init-only; it can only be set in an object initializer");

    if (!expr->compound) {
        expr->value = coerce(expr->value, target, "assignment");
        return target;
    }
    if (expr->op == BinaryOp::Coalesce) {
        expr->value = coerce(expr->value, target, "assignment");
        expr->operandType = target;
        return target;
    }

    const TypeInfo* value = checkExpr(expr->value);
    if (isStringKind(target) && expr->op == BinaryOp::Add) {
        expr->value = coerce(expr->value, core.stringType, "concatenation");
        expr->operandType = core.stringType;
        return target;
    }
    const TypeInfo* promoted = types_.promote(target, value);
    if (!promoted) {
        errorAt(expr, 19, std::string("operator '") + operatorText(expr->op) +
                              "=' cannot be applied to '" + describe(target) + "' and '" +
                              describe(value) + "'");
        return core.errorType;
    }
    if (expr->op == BinaryOp::ShiftLeft || expr->op == BinaryOp::ShiftRight) {
        expr->value = coerce(expr->value, core.intType, "shift count");
        expr->operandType = shiftResult(target);
        return target;
    }
    expr->value = convert(expr->value, promoted);
    expr->operandType = promoted;
    return target;
}

const TypeInfo* Checker::checkConditional(ConditionalExpr* expr)
{
    const CoreTypes& core = types_.core();
    expr->condition = coerce(expr->condition, core.boolType, "condition");
    const TypeInfo* whenTrue = checkExpr(expr->whenTrue);
    const TypeInfo* whenFalse = checkExpr(expr->whenFalse);

    if (whenTrue == whenFalse)
        return whenTrue;
    if (whenTrue->kind == TypeKind::Null) {
        expr->whenTrue = convert(expr->whenTrue, whenFalse);
        return whenFalse;
    }
    if (whenFalse->kind == TypeKind::Null) {
        expr->whenFalse = convert(expr->whenFalse, whenTrue);
        return whenTrue;
    }
    if (types_.implicitlyConverts(whenFalse, whenTrue)) {
        expr->whenFalse = convert(expr->whenFalse, whenTrue);
        return whenTrue;
    }
    if (types_.implicitlyConverts(whenTrue, whenFalse)) {
        expr->whenTrue = convert(expr->whenTrue, whenFalse);
        return whenFalse;
    }
    if (const TypeInfo* promoted = types_.promote(whenTrue, whenFalse)) {
        expr->whenTrue = convert(expr->whenTrue, promoted);
        expr->whenFalse = convert(expr->whenFalse, promoted);
        return promoted;
    }
    errorAt(expr, 173,
            "there is no common type between '" + describe(whenTrue) + "' and '" +
                describe(whenFalse) + "'");
    return core.errorType;
}

const TypeInfo* Checker::checkCast(CastExpr* expr)
{
    const CoreTypes& core = types_.core();
    const TypeInfo* target = resolve(expr->typeRef);
    const TypeInfo* from = checkExpr(expr->operand);
    if (from->kind == TypeKind::Error || target->kind == TypeKind::Error)
        return target;

    const ConvKindResult conv = types_.classify(from, target);
    if (!conv.exists) {
        errorAt(expr, 30,
                "cannot convert '" + describe(from) + "' to '" + describe(target) + "'");
        return target;
    }
    expr->operand = convert(expr->operand, target);
    if (expr->operand->isConstant) {
        expr->isConstant = true;
        expr->constant = expr->operand->constant;
    }
    (void)core;
    return target;
}

// ------------------------------------------------------------- patterns ---

bool Checker::resolvesToType(TypeRef* ref) const
{
    if (!ref)
        return false;
    if (ref->predefined != Tok::Eof || ref->isTuple || !ref->typeArgs.empty())
        return true;
    if (ref->path.size() == 1 && lookupTypeParam(ref->path[0]))
        return true;
    String name;
    for (std::size_t i = 0; i < ref->path.size(); ++i) {
        if (i)
            name.push_back(u'.');
        name += ref->path[i];
    }
    return lookupType(name) != nullptr;
}

Expr* Checker::typeRefAsExpr(const TypeRef* ref)
{
    Expr* result = nullptr;
    for (const String& part : ref->path) {
        if (!result) {
            NameExpr* node = arena_.make<NameExpr>();
            node->line = ref->line;
            node->column = ref->column;
            node->name = part;
            result = node;
        } else {
            MemberAccessExpr* node = arena_.make<MemberAccessExpr>();
            node->line = ref->line;
            node->column = ref->column;
            node->target = result;
            node->name = part;
            result = node;
        }
    }
    return result;
}

Expr* Checker::makeBool(bool value, const Expr* at)
{
    LiteralExpr* node = arena_.make<LiteralExpr>();
    node->line = at->line;
    node->column = at->column;
    node->literal = LiteralKind::Bool;
    node->intValue = value ? 1 : 0;
    node->type = types_.core().boolType;
    node->isConstant = true;
    node->constant = Value(value);
    return node;
}

Expr* Checker::makeLocalRead(std::uint32_t slot, const TypeInfo* type, const Expr* at)
{
    NameExpr* node = arena_.make<NameExpr>();
    node->line = at->line;
    node->column = at->column;
    node->symbol = SymbolKind::Local;
    node->slot = slot;
    node->type = type;
    return node;
}

Expr* Checker::makeAnd(Expr* left, Expr* right)
{
    if (!left)
        return right;
    if (!right)
        return left;
    if (left->isConstant && left->constant.toBool())
        return right;
    if (right->isConstant && right->constant.toBool())
        return left;
    BinaryExpr* node = arena_.make<BinaryExpr>();
    node->line = left->line;
    node->column = left->column;
    node->op = BinaryOp::LogicalAnd;
    node->left = left;
    node->right = right;
    node->type = types_.core().boolType;
    node->operandType = types_.core().boolType;
    return node;
}

Expr* Checker::lowerPattern(Pattern* pattern, Expr* subject, const TypeInfo* subjectType)
{
    const CoreTypes& core = types_.core();
    if (!pattern)
        return makeBool(true, subject);

    switch (pattern->kind) {
        case PatternKind::Discard:
            return makeBool(true, subject);

        case PatternKind::Var: {
            // Always matches; the point is the binding.
            const std::uint32_t slot =
                declareLocal(pattern->bindingName, subjectType, pattern->line);
            pattern->bindingSlot = slot;
            pattern->boundType = subjectType;
            pattern->hasSlot = true;
            AssignmentExpr* assign = arena_.make<AssignmentExpr>();
            assign->line = pattern->line;
            assign->column = pattern->column;
            assign->target = makeLocalRead(slot, subjectType, subject);
            assign->value = subject;
            assign->type = subjectType;
            SequenceExpr* seq = arena_.make<SequenceExpr>();
            seq->line = pattern->line;
            seq->column = pattern->column;
            seq->before.push_back(assign);
            seq->value = makeBool(true, subject);
            seq->type = core.boolType;
            return seq;
        }

        case PatternKind::Constant: {
            Expr* literal = pattern->value;
            checkExpr(literal);
            BinaryExpr* test = arena_.make<BinaryExpr>();
            test->line = pattern->line;
            test->column = pattern->column;
            test->op = BinaryOp::Equal;
            test->left = subject;
            test->right = literal;
            Expr* node = test;
            checkExpr(node);
            return node;
        }

        case PatternKind::Relational: {
            Expr* operand = pattern->value;
            checkExpr(operand);
            BinaryExpr* test = arena_.make<BinaryExpr>();
            test->line = pattern->line;
            test->column = pattern->column;
            test->op = pattern->op;
            test->left = subject;
            test->right = operand;
            Expr* node = test;
            checkExpr(node);
            return node;
        }

        case PatternKind::Not: {
            Expr* inner = lowerPattern(pattern->children[0], subject, subjectType);
            UnaryExpr* node = arena_.make<UnaryExpr>();
            node->line = pattern->line;
            node->column = pattern->column;
            node->op = UnaryOp::Not;
            node->operand = inner;
            node->type = core.boolType;
            return node;
        }

        case PatternKind::And: {
            // `o is int n and > 100`: the right operand tests what the left
            // narrowed to, not the original subject.
            Expr* left = lowerPattern(pattern->children[0], subject, subjectType);
            Pattern* first = pattern->children[0];
            Expr* narrowedSubject = subject;
            const TypeInfo* narrowedType = subjectType;
            if (first->boundType && first->hasSlot) {
                narrowedType = first->boundType;
                narrowedSubject = makeLocalRead(first->bindingSlot, narrowedType, subject);
            }
            Expr* right = lowerPattern(pattern->children[1], narrowedSubject, narrowedType);
            return makeAnd(left, right);
        }

        case PatternKind::Or: {
            Expr* left = lowerPattern(pattern->children[0], subject, subjectType);
            Expr* right = lowerPattern(pattern->children[1], subject, subjectType);
            BinaryExpr* node = arena_.make<BinaryExpr>();
            node->line = pattern->line;
            node->column = pattern->column;
            node->op = BinaryOp::LogicalOr;
            node->left = left;
            node->right = right;
            node->type = core.boolType;
            node->operandType = core.boolType;
            return node;
        }

        case PatternKind::Declaration:
        case PatternKind::Property:
        case PatternKind::Positional:
        case PatternKind::List: {
            // `case Red:` parses as a type pattern because the parser cannot
            // tell a type from an enum member. If the name is not a type, it
            // was a constant all along.
            if (pattern->kind == PatternKind::Declaration && pattern->typeRef &&
                pattern->bindingName.empty() && pattern->properties.empty() &&
                pattern->children.empty() && !resolvesToType(pattern->typeRef)) {
                pattern->kind = PatternKind::Constant;
                pattern->value = typeRefAsExpr(pattern->typeRef);
                return lowerPattern(pattern, subject, subjectType);
            }

            Expr* test = nullptr;
            const TypeInfo* narrowed = subjectType;
            Expr* value = subject;

            // A type was named: test it, and from here on work through a
            // local of that type so the members are reachable.
            if (pattern->typeRef) {
                narrowed = resolve(pattern->typeRef);
                pattern->testType = narrowed;
                if (narrowed != subjectType && narrowed->kind != TypeKind::Error) {
                    TypeTestExpr* isTest = arena_.make<TypeTestExpr>();
                    isTest->line = pattern->line;
                    isTest->column = pattern->column;
                    isTest->operand = subject;
                    isTest->typeRef = pattern->typeRef;
                    isTest->type = core.boolType;
                    isTest->operand->type = subjectType;
                    test = isTest;
                }
            }

            // Everything below reads the subject more than once, so it goes
            // through a temporary. A narrowing test also needs one, so that
            // an `and` to the right sees the narrowed type.
            const bool needsTemp = !pattern->properties.empty() ||
                                   !pattern->children.empty() ||
                                   !pattern->bindingName.empty() || pattern->typeRef != nullptr;
            std::uint32_t slot = 0;
            if (needsTemp) {
                if (!pattern->bindingName.empty()) {
                    slot = declareLocal(pattern->bindingName, narrowed, pattern->line);
                } else {
                    slot = addTemporary(narrowed);
                }
                pattern->bindingSlot = slot;
                pattern->boundType = narrowed;
                pattern->hasSlot = true;
                Expr* source = subject;
                if (narrowed != subjectType && narrowed->kind != TypeKind::Error)
                    source = convert(source, narrowed);
                AssignmentExpr* assign = arena_.make<AssignmentExpr>();
                assign->line = pattern->line;
                assign->column = pattern->column;
                assign->target = makeLocalRead(slot, narrowed, subject);
                assign->value = source;
                assign->type = narrowed;
                SequenceExpr* seq = arena_.make<SequenceExpr>();
                seq->line = pattern->line;
                seq->column = pattern->column;
                seq->before.push_back(assign);
                seq->value = makeBool(true, subject);
                seq->type = core.boolType;
                test = makeAnd(test, seq);
                value = makeLocalRead(slot, narrowed, subject);
            }

            if (pattern->kind == PatternKind::List)
                test = makeAnd(test, lowerListPattern(pattern, slot, narrowed, subject));
            else if (pattern->kind == PatternKind::Positional)
                test = makeAnd(test, lowerPositionalPattern(pattern, slot, narrowed, subject));

            for (PropertySubpattern& sub : pattern->properties) {
                Expr* member = makeMemberRead(slot, narrowed, sub.name, subject);
                if (!member)
                    continue;
                test = makeAnd(test, lowerPattern(sub.pattern, member, member->type));
            }
            (void)value;
            return test ? test : makeBool(true, subject);
        }

        case PatternKind::Slice:
            return makeBool(true, subject);
    }
    return makeBool(true, subject);
}

Expr* Checker::makeMemberRead(std::uint32_t slot, const TypeInfo* type, const String& name,
                              const Expr* at)
{
    MemberAccessExpr* node = arena_.make<MemberAccessExpr>();
    node->line = at->line;
    node->column = at->column;
    node->target = makeLocalRead(slot, type, at);
    node->name = name;
    Expr* expr = node;
    const TypeInfo* result = checkExpr(expr);
    if (!result || result->kind == TypeKind::Error)
        return nullptr;
    return expr;
}

Expr* Checker::lowerPositionalPattern(Pattern* pattern, std::uint32_t slot, const TypeInfo* type,
                                      const Expr* at)
{
    // A tuple matches by element; anything else needs a Deconstruct method.
    Expr* test = nullptr;
    if (type && type->nativeKind == NativeKind::Tuple) {
        if (type->fields.size() != pattern->children.size()) {
            error(pattern->line, pattern->column, 8502,
                  "the tuple has " + std::to_string(type->fields.size()) + " element(s)");
            return makeBool(false, at);
        }
        for (std::size_t i = 0; i < pattern->children.size(); ++i) {
            Expr* member = makeMemberRead(slot, type, type->fields[i].name, at);
            if (!member)
                continue;
            test = makeAnd(test, lowerPattern(pattern->children[i], member, member->type));
        }
        return test ? test : makeBool(true, at);
    }

    std::vector<MethodInfo*> candidates;
    if (type)
        type->collectMethods(ascii("Deconstruct"), candidates);
    MethodInfo* chosen = nullptr;
    for (MethodInfo* candidate : candidates) {
        if (candidate->params.size() == pattern->children.size())
            chosen = candidate;
    }
    if (!chosen) {
        error(pattern->line, pattern->column, 1061,
              "'" + describe(type) + "' has no Deconstruct with " +
                  std::to_string(pattern->children.size()) + " parameter(s)");
        return makeBool(false, at);
    }
    pattern->deconstructor = chosen;

    // Each `out` parameter lands in a temporary the subpatterns then read.
    InvocationExpr* call = arena_.make<InvocationExpr>();
    call->line = pattern->line;
    call->column = pattern->column;
    call->method = chosen;
    call->receiver = makeLocalRead(slot, type, at);
    call->type = types_.core().voidType;
    call->callee = call->receiver;
    std::vector<std::uint32_t> slots;
    for (std::size_t i = 0; i < chosen->params.size(); ++i) {
        const std::uint32_t out = addTemporary(chosen->params[i].type);
        fn_->boxed[out] = 1;
        slots.push_back(out);
        Argument arg;
        arg.mode = ParamMode::Out;
        arg.value = makeLocalRead(out, chosen->params[i].type, at);
        call->args.push_back(std::move(arg));
    }
    SequenceExpr* seq = arena_.make<SequenceExpr>();
    seq->line = pattern->line;
    seq->column = pattern->column;
    seq->before.push_back(call);
    seq->value = makeBool(true, at);
    seq->type = types_.core().boolType;
    test = makeAnd(test, seq);

    for (std::size_t i = 0; i < pattern->children.size(); ++i) {
        Expr* piece = makeLocalRead(slots[i], chosen->params[i].type, at);
        test = makeAnd(test, lowerPattern(pattern->children[i], piece, chosen->params[i].type));
    }
    return test ? test : makeBool(true, at);
}

Expr* Checker::lowerListPattern(Pattern* pattern, std::uint32_t slot, const TypeInfo* type,
                                const Expr* at)
{
    const CoreTypes& core = types_.core();
    const TypeInfo* element = nullptr;
    if (type && type->kind == TypeKind::Array)
        element = type->element;
    else if (type && type->nativeKind == NativeKind::List)
        element = type->typeArgs.empty() ? core.objectType : type->typeArgs[0];
    if (!element) {
        error(pattern->line, pattern->column, 8985,
              "a list pattern needs an array or a List<T>, not '" + describe(type) + "'");
        return makeBool(false, at);
    }

    std::size_t before = 0;
    std::size_t after = 0;
    bool sawSlice = false;
    Pattern* slice = nullptr;
    for (Pattern* child : pattern->children) {
        if (child->kind == PatternKind::Slice) {
            if (sawSlice) {
                error(child->line, child->column, 8980,
                      "a list pattern may have only one slice");
                return makeBool(false, at);
            }
            sawSlice = true;
            slice = child;
        } else if (sawSlice) {
            ++after;
        } else {
            ++before;
        }
    }

    Expr* length = makeLengthRead(slot, type, at);
    if (!length)
        return makeBool(false, at);
    BinaryExpr* lengthTest = arena_.make<BinaryExpr>();
    lengthTest->line = pattern->line;
    lengthTest->column = pattern->column;
    lengthTest->op = sawSlice ? BinaryOp::GreaterEqual : BinaryOp::Equal;
    lengthTest->left = length;
    lengthTest->right = makeInt(std::int32_t(before + after), at);
    lengthTest->type = core.boolType;
    lengthTest->operandType = core.intType;
    Expr* test = lengthTest;

    // Elements before the slice count from the start; elements after it count
    // back from the length, which is the only thing a slice changes.
    bool past = false;
    std::size_t fromStart = 0;
    std::size_t fromEnd = 0;
    for (Pattern* child : pattern->children) {
        if (child->kind == PatternKind::Slice) {
            past = true;
            continue;
        }
        Expr* position = nullptr;
        if (!past) {
            position = makeInt(std::int32_t(fromStart++), at);
        } else {
            BinaryExpr* back = arena_.make<BinaryExpr>();
            back->line = child->line;
            back->column = child->column;
            back->op = BinaryOp::Subtract;
            back->left = makeLengthRead(slot, type, at);
            back->right = makeInt(std::int32_t(after - fromEnd), at);
            back->type = core.intType;
            back->operandType = core.intType;
            position = back;
            ++fromEnd;
        }
        Expr* item = makeElementRead(slot, type, position, at);
        if (item)
            test = makeAnd(test, lowerPattern(child, item, element));
    }
    (void)slice;
    return test;
}

Expr* Checker::makeInt(std::int32_t value, const Expr* at)
{
    LiteralExpr* node = arena_.make<LiteralExpr>();
    node->line = at->line;
    node->column = at->column;
    node->literal = LiteralKind::Int;
    node->intValue = value;
    node->type = types_.core().intType;
    node->isConstant = true;
    node->constant = Value(value);
    return node;
}

Expr* Checker::makeLengthRead(std::uint32_t slot, const TypeInfo* type, const Expr* at)
{
    if (type && type->kind == TypeKind::Array) {
        MemberAccessExpr* node = arena_.make<MemberAccessExpr>();
        node->line = at->line;
        node->column = at->column;
        node->target = makeLocalRead(slot, type, at);
        node->name = ascii("Length");
        Expr* expr = node;
        checkExpr(expr);
        return expr;
    }
    return makeMemberRead(slot, type, ascii("Count"), at);
}

Expr* Checker::makeElementRead(std::uint32_t slot, const TypeInfo* type, Expr* index,
                               const Expr* at)
{
    ElementAccessExpr* node = arena_.make<ElementAccessExpr>();
    node->line = at->line;
    node->column = at->column;
    node->target = makeLocalRead(slot, type, at);
    node->index = index;
    Expr* expr = node;
    const TypeInfo* result = checkExpr(expr);
    if (!result || result->kind == TypeKind::Error)
        return nullptr;
    return expr;
}

const TypeInfo* Checker::checkTuple(TupleExpr* expr)
{
    std::vector<const TypeInfo*> elements;
    std::vector<String> names;
    for (TupleElement& element : expr->elements) {
        const TypeInfo* type = checkExpr(element.value);
        if (type == types_.core().nullType)
            type = types_.core().objectType;
        elements.push_back(type);
        names.push_back(element.name);
    }
    if (elements.size() < 2) {
        errorAt(expr, 8124, "a tuple needs at least two elements");
        return types_.core().errorType;
    }
    const TypeInfo* type = types_.tupleOf(elements, names);
    // Construction is an ordinary call to the tuple's constructor.
    std::vector<MethodInfo*> ctors;
    type->collectMethods(ascii(".ctor"), ctors);
    expr->ctor = ctors.empty() ? nullptr : ctors.front();
    return type;
}

const TypeInfo* Checker::checkWith(WithExpr* expr)
{
    const TypeInfo* type = checkExpr(expr->target);
    if (!type || type->kind == TypeKind::Error)
        return types_.core().errorType;
    std::vector<MethodInfo*> clones;
    type->collectMethods(ascii(kRecordClone), clones);
    if (clones.empty()) {
        errorAt(expr, 8858, "'" + describe(type) + "' is not a record; 'with' needs one");
        return types_.core().errorType;
    }
    expr->cloneMethod = clones.front();
    for (InitializerEntry& entry : expr->initializers) {
        const PropertyInfo* prop = type->findProperty(entry.name);
        const FieldInfo* field = prop ? nullptr : type->findField(entry.name);
        if (!prop && !field) {
            errorAt(expr, 117,
                    "'" + describe(type) + "' has no member '" + toUtf8(entry.name) + "'");
            continue;
        }
        entry.prop = prop;
        entry.field = field;
        entry.value = coerce(entry.value, prop ? prop->type : field->type, "assignment");
        if (prop) {
            std::vector<MethodInfo*> setters;
            type->collectMethods(u"set_" + entry.name, setters);
            entry.setter = setters.empty() ? nullptr : setters.front();
        }
    }
    return type;
}

void Checker::checkThrowValue(Expr*& value, const Expr* at)
{
    const TypeInfo* type = checkExpr(value);
    const TypeInfo* exception = lookupType(ascii("System.Exception"));
    if (exception && type && type->kind != TypeKind::Error &&
        !types_.implicitlyConverts(type, exception))
        errorAt(at, 155, "what is thrown must derive from Exception");
}

void Checker::checkDeconstruction(Deconstruction* node, Expr*& value, std::uint32_t line)
{
    const CoreTypes& core = types_.core();
    const TypeInfo* source = checkExpr(value);
    if (!source || source->kind == TypeKind::Error)
        return;

    // A tuple is taken apart by field; anything else needs a Deconstruct.
    std::vector<const TypeInfo*> pieces;
    if (source->nativeKind == NativeKind::Tuple) {
        for (const TypeInfo* arg : source->typeArgs)
            pieces.push_back(arg);
    } else {
        std::vector<MethodInfo*> candidates;
        source->collectMethods(ascii("Deconstruct"), candidates);
        for (MethodInfo* candidate : candidates) {
            if (candidate->params.size() == node->targets.size())
                node->deconstructor = candidate;
        }
        if (!node->deconstructor) {
            error(line, 1, 1061,
                  "'" + describe(source) + "' has no Deconstruct with " +
                      std::to_string(node->targets.size()) + " parameter(s)");
            return;
        }
        for (const ParamInfo& param : node->deconstructor->params)
            pieces.push_back(param.type);
    }
    if (pieces.size() != node->targets.size()) {
        error(line, 1, 8132,
              "cannot take " + std::to_string(node->targets.size()) + " values out of '" +
                  describe(source) + "'");
        return;
    }

    node->sourceSlot = addTemporary(source);
    node->sourceType = source;
    for (std::size_t i = 0; i < node->targets.size(); ++i) {
        DeconstructTarget& target = node->targets[i];
        target.type = pieces[i];
        if (target.nested) {
            // The piece becomes the source of the inner deconstruction.
            Expr* piece = makeElementOf(node, i, line);
            checkDeconstruction(target.nested, piece, line);
            target.nestedValue = piece;
            continue;
        }
        if (target.discard) {
            target.slot = addTemporary(pieces[i]);
            continue;
        }
        if (target.typeRef && !target.typeRef->isVar) {
            const TypeInfo* written = resolve(target.typeRef);
            if (written != pieces[i] && !types_.implicitlyConverts(pieces[i], written))
                error(target.line, 1, 29,
                      "cannot convert '" + describe(pieces[i]) + "' to '" + describe(written) +
                          "'");
            target.type = written;
        }
        target.slot = declareLocal(target.name, target.type, target.line);
    }
    if (node->deconstructor) {
        for (DeconstructTarget& target : node->targets)
            fn_->boxed[target.slot] = 1;

        // Built as an ordinary call with `out` arguments, so the emitter's
        // call path handles the boxes and the void result.
        NameExpr* site = arena_.make<NameExpr>();
        site->line = line;
        InvocationExpr* call = arena_.make<InvocationExpr>();
        call->line = line;
        call->method = node->deconstructor;
        call->receiver = makeLocalRead(node->sourceSlot, source, site);
        call->callee = call->receiver;
        call->type = core.voidType;
        call->virtualCall = node->deconstructor->isVirtual();
        for (std::size_t i = 0; i < node->targets.size(); ++i) {
            Argument arg;
            arg.mode = ParamMode::Out;
            arg.value = makeLocalRead(node->targets[i].slot, node->targets[i].type, site);
            call->args.push_back(std::move(arg));
        }
        node->call = call;
    }
}

Expr* Checker::makeElementOf(Deconstruction* node, std::size_t index, std::uint32_t line)
{
    NameExpr* at = arena_.make<NameExpr>();
    at->line = line;
    if (node->sourceType && node->sourceType->nativeKind == NativeKind::Tuple)
        return makeMemberRead(node->sourceSlot, node->sourceType,
                              node->sourceType->fields[index].name, at);
    return makeLocalRead(node->targets[index].slot, node->targets[index].type, at);
}

void Checker::checkUsing(UsingStmt* stmt)
{
    const CoreTypes& core = types_.core();
    const TypeInfo* disposable = lookupType(ascii("System.IDisposable"));

    // Whatever shape it was written in, it becomes one slot holding the
    // resource and a try/finally that disposes it.
    const TypeInfo* type = nullptr;
    std::vector<Stmt*> before;
    if (stmt->declaration) {
        auto* decl = static_cast<LocalDeclStmt*>(stmt->declaration);
        checkStatement(decl);
        if (decl->vars.size() != 1) {
            error(stmt->line, 1, 8175, "a using declaration declares one variable");
            return;
        }
        stmt->resourceSlot = decl->vars[0].slot;
        type = decl->vars[0].type;
        before.push_back(decl);
    } else if (stmt->resource) {
        type = checkExpr(stmt->resource);
        stmt->resourceSlot = addTemporary(type);
        ExprStmt* store = arena_.make<ExprStmt>();
        store->line = stmt->line;
        AssignmentExpr* assign = arena_.make<AssignmentExpr>();
        assign->line = stmt->line;
        assign->target = makeLocalRead(stmt->resourceSlot, type, stmt->resource);
        assign->value = stmt->resource;
        assign->type = type;
        store->expr = assign;
        before.push_back(store);
    }

    if (type && type->kind != TypeKind::Error && disposable &&
        !types_.implicitlyConverts(type, disposable))
        error(stmt->line, 1, 1674,
              "'" + describe(type) + "' is not IDisposable, so it cannot be used with 'using'");

    std::vector<MethodInfo*> disposers;
    if (type)
        type->collectMethods(ascii("Dispose"), disposers);
    for (MethodInfo* candidate : disposers) {
        if (candidate->params.empty())
            stmt->dispose = candidate;
    }

    if (stmt->body)
        checkStatement(stmt->body);

    if (!stmt->dispose) {
        // Nothing to call; the body still runs.
        BlockStmt* block = arena_.make<BlockStmt>();
        block->line = stmt->line;
        block->statements = before;
        if (stmt->body)
            block->statements.push_back(stmt->body);
        stmt->lowered = block;
        return;
    }

    // finally { if (r != null) r.Dispose(); }
    NameExpr* site = arena_.make<NameExpr>();
    site->line = stmt->line;
    InvocationExpr* call = arena_.make<InvocationExpr>();
    call->line = stmt->line;
    call->method = stmt->dispose;
    call->receiver = makeLocalRead(stmt->resourceSlot, type, site);
    call->callee = call->receiver;
    call->type = core.voidType;
    call->virtualCall = stmt->dispose->isVirtual();
    call->interfaceCall = stmt->dispose->owner &&
                          stmt->dispose->owner->kind == TypeKind::Interface;
    ExprStmt* callStmt = arena_.make<ExprStmt>();
    callStmt->line = stmt->line;
    callStmt->expr = call;

    Stmt* cleanup = callStmt;
    if (!type->isValueType()) {
        BinaryExpr* notNull = arena_.make<BinaryExpr>();
        notNull->line = stmt->line;
        notNull->op = BinaryOp::NotEqual;
        notNull->left = makeLocalRead(stmt->resourceSlot, type, call->receiver);
        LiteralExpr* null = arena_.make<LiteralExpr>();
        null->line = stmt->line;
        null->literal = LiteralKind::Null;
        null->type = core.nullType;
        notNull->right = null;
        notNull->type = core.boolType;
        notNull->operandType = type;
        IfStmt* guard = arena_.make<IfStmt>();
        guard->line = stmt->line;
        guard->condition = notNull;
        guard->thenBranch = callStmt;
        cleanup = guard;
    }

    BlockStmt* finallyBlock = arena_.make<BlockStmt>();
    finallyBlock->line = stmt->line;
    finallyBlock->statements.push_back(cleanup);

    TryStmt* guarded = arena_.make<TryStmt>();
    guarded->line = stmt->line;
    guarded->body = stmt->body ? stmt->body : arena_.make<BlockStmt>();
    guarded->finallyBlock = finallyBlock;

    BlockStmt* block = arena_.make<BlockStmt>();
    block->line = stmt->line;
    block->statements = before;
    block->statements.push_back(guarded);
    stmt->lowered = block;
}

void Checker::mergeFilteredCatches(TryStmt* stmt)
{
    bool anyFilter = false;
    for (const CatchClause& clause : stmt->catches)
        anyFilter = anyFilter || clause.filter != nullptr;
    if (!anyFilter)
        return;

    // Which handler runs is decided while the exception is being matched, so
    // a filter cannot be a test inside its own handler -- failing it has to
    // fall through to the next clause of the same try, not out of it. The
    // clauses therefore become one handler holding the chain they describe.
    const std::uint32_t line = stmt->line;
    const String caught = ascii("<exception>") + fromUtf8(std::to_string(lambdaCounter_++));

    Stmt* chain = arena_.make<ThrowStmt>();  // nothing matched: it keeps going
    chain->line = line;

    for (std::size_t i = stmt->catches.size(); i-- > 0;) {
        CatchClause& clause = stmt->catches[i];
        BlockStmt* body = arena_.make<BlockStmt>();
        body->line = clause.line;

        Expr* test = nullptr;
        // Each clause binds its own name, and two clauses may use the same
        // one. The test binds a private name and the body aliases it, so the
        // name the program wrote is scoped to the body that wrote it.
        const String bound =
            clause.name.empty()
                ? String()
                : clause.name + ascii("<caught>") + fromUtf8(std::to_string(lambdaCounter_++));
        if (clause.typeRef) {
            TypeTestExpr* isTest = arena_.make<TypeTestExpr>();
            isTest->line = clause.line;
            NameExpr* subject = arena_.make<NameExpr>();
            subject->line = clause.line;
            subject->name = caught;
            isTest->operand = subject;
            isTest->typeRef = clause.typeRef;
            isTest->bindingName = bound;
            test = isTest;
        }
        if (!clause.name.empty()) {
            LocalDeclStmt* alias = arena_.make<LocalDeclStmt>();
            alias->line = clause.line;
            TypeRef* inferred = arena_.make<TypeRef>();
            inferred->line = clause.line;
            inferred->isVar = true;
            alias->typeRef = inferred;
            VarDeclarator var;
            var.name = clause.name;
            var.line = clause.line;
            NameExpr* source = arena_.make<NameExpr>();
            source->line = clause.line;
            source->name = clause.typeRef ? bound : caught;
            var.init = source;
            alias->vars.push_back(std::move(var));
            body->statements.push_back(alias);
        }
        if (clause.filter) {
            // Built untyped on purpose: the pieces have not been checked yet,
            // and `checkExpr` skips a node that already has a type.
            if (!test) {
                test = clause.filter;
            } else {
                BinaryExpr* both = arena_.make<BinaryExpr>();
                both->line = clause.line;
                both->op = BinaryOp::LogicalAnd;
                both->left = test;
                both->right = clause.filter;
                test = both;
            }
        }

        body->statements.push_back(clause.body);
        if (!test) {
            chain = body;
            continue;
        }
        IfStmt* branch = arena_.make<IfStmt>();
        branch->line = clause.line;
        branch->condition = test;
        branch->thenBranch = body;
        branch->elseBranch = chain;
        chain = branch;
    }

    CatchClause merged;
    merged.line = line;
    merged.name = caught;
    merged.body = chain;
    stmt->catches.clear();
    stmt->catches.push_back(std::move(merged));
}

const TypeInfo* Checker::checkSwitchExpr(SwitchExpr* expr)
{
    const CoreTypes& core = types_.core();
    const TypeInfo* subjectType = checkExpr(expr->subject);
    if (expr->arms.empty()) {
        errorAt(expr, 8506, "a switch expression needs at least one arm");
        return core.errorType;
    }

    // The subject is evaluated once, into a temporary every arm then reads.
    const std::uint32_t slot = addTemporary(subjectType);
    expr->subjectSlot = slot;

    // Each arm becomes one level of a conditional chain, built from the last
    // arm backwards so the tail is in hand when the head is made.
    Expr* result = nullptr;
    const TypeInfo* resultType = nullptr;
    std::vector<Expr*> values;
    std::vector<Expr*> tests;
    for (SwitchArm& arm : expr->arms) {
        pushScope();
        Expr* subject = makeLocalRead(slot, subjectType, expr);
        Expr* test = lowerPattern(arm.pattern, subject, subjectType);
        if (arm.guard) {
            Expr* guard = coerce(arm.guard, core.boolType, "a when clause");
            arm.guard = guard;
            test = makeAnd(test, guard);
        }
        checkExpr(arm.value);
        popScope();
        tests.push_back(test);
        values.push_back(arm.value);
        const TypeInfo* armType = arm.value->type;
        if (!armType || armType->kind == TypeKind::Error)
            continue;
        if (!resultType) {
            resultType = armType;
        } else if (resultType != armType) {
            if (types_.implicitlyConverts(armType, resultType)) {
                // keep resultType
            } else if (types_.implicitlyConverts(resultType, armType)) {
                resultType = armType;
            } else {
                errorAt(arm.value, 8506,
                        "no common type between '" + describe(resultType) + "' and '" +
                            describe(armType) + "'");
            }
        }
    }
    if (!resultType)
        resultType = core.errorType;

    // Nothing matched is an exception, the way C# says.
    Expr* tail = makeMatchFailure(expr, resultType);
    for (std::size_t i = values.size(); i-- > 0;) {
        values[i] = coerce(values[i], resultType, "a switch arm");
        // A final arm that always matches makes the failure unreachable.
        if (tests[i]->isConstant && tests[i]->constant.toBool()) {
            tail = values[i];
            continue;
        }
        ConditionalExpr* node = arena_.make<ConditionalExpr>();
        node->line = expr->arms[i].line;
        node->column = expr->arms[i].column;
        node->condition = tests[i];
        node->whenTrue = values[i];
        node->whenFalse = tail;
        node->type = resultType;
        tail = node;
    }

    AssignmentExpr* store = arena_.make<AssignmentExpr>();
    store->line = expr->line;
    store->column = expr->column;
    store->target = makeLocalRead(slot, subjectType, expr);
    store->value = expr->subject;
    store->type = subjectType;

    SequenceExpr* seq = arena_.make<SequenceExpr>();
    seq->line = expr->line;
    seq->column = expr->column;
    seq->before.push_back(store);
    seq->value = tail;
    seq->type = resultType;
    result = seq;
    expr->lowered = result;
    return resultType;
}

Expr* Checker::makeMatchFailure(const Expr* at, const TypeInfo* type)
{
    const TypeInfo* failure = lookupType(ascii("System.InvalidOperationException"));
    if (!failure)
        return makeDefault(type, at);
    ObjectCreationExpr* create = arena_.make<ObjectCreationExpr>();
    create->line = at->line;
    create->column = at->column;
    create->type = failure;
    std::vector<MethodInfo*> ctors;
    failure->collectMethods(ascii(".ctor"), ctors);
    for (MethodInfo* candidate : ctors) {
        if (candidate->params.size() == 1 &&
            candidate->params[0].type == types_.core().stringType)
            create->ctor = candidate;
    }
    if (!create->ctor) {
        for (MethodInfo* candidate : ctors) {
            if (candidate->params.empty())
                create->ctor = candidate;
        }
    } else {
        LiteralExpr* message = arena_.make<LiteralExpr>();
        message->line = at->line;
        message->column = at->column;
        message->literal = LiteralKind::String;
        message->text = ascii("no switch arm matched the value");
        message->type = types_.core().stringType;
        message->isConstant = true;
        message->constant = rt_.makeString(message->text);
        Argument arg;
        arg.value = message;
        create->args.push_back(std::move(arg));
    }
    if (!create->ctor)
        return makeDefault(type, at);

    ThrowExpr* node = arena_.make<ThrowExpr>();
    node->line = at->line;
    node->column = at->column;
    node->value = create;
    node->type = type;
    return node;
}

Expr* Checker::makeDefault(const TypeInfo* type, const Expr* at)
{
    DefaultExpr* node = arena_.make<DefaultExpr>();
    node->line = at->line;
    node->column = at->column;
    node->type = type;
    return node;
}

const TypeInfo* Checker::checkTypeTest(TypeTestExpr* expr)
{
    const CoreTypes& core = types_.core();
    const TypeInfo* operand = checkExpr(expr->operand);

    // Anything richer than a bare type test is lowered here, and the result
    // replaces this node from the caller's point of view.
    if (expr->pattern) {
        expr->loweredPattern = lowerPattern(expr->pattern, expr->operand, operand);
        return core.boolType;
    }

    const TypeInfo* target = resolve(expr->typeRef);
    if (operand->isValueType() && operand->kind != TypeKind::Error)
        errorAt(expr, 8121, "the left operand must be a reference type");

    if (expr->isAs) {
        if (target->isValueType())
            errorAt(expr, 77, "'as' requires a reference type");
        return target;
    }
    if (!expr->bindingName.empty())
        expr->bindingSlot = declareLocal(expr->bindingName, target, expr->line);
    return core.boolType;
}

const TypeInfo* Checker::checkLambda(LambdaExpr* expr, const TypeInfo* expected)
{
    const MethodInfo* invoke = expected ? expected->invoke : nullptr;
    if (!invoke) {
        errorAt(expr, 8917, "a lambda needs a delegate type");
        return types_.core().errorType;
    }
    if (invoke->params.size() != expr->params.size()) {
        errorAt(expr, 1593,
                "the delegate '" + describe(expected) + "' takes " +
                    std::to_string(invoke->params.size()) + " parameter(s)");
        return expected;
    }

    TypeInfo* owner = const_cast<TypeInfo*>(fn_->owner);
    const String name = ascii("<lambda>") + fromUtf8(std::to_string(lambdaCounter_++));
    const std::uint16_t flags =
        std::uint16_t(MethodLambda) | (fn_->isStatic ? std::uint16_t(MethodStatic) : 0);
    MethodInfo* method = newMethod(owner, name, invoke->returnType, flags);
    for (std::size_t i = 0; i < expr->params.size(); ++i) {
        ParamInfo param;
        param.name = expr->params[i].name;
        param.type = invoke->params[i].type;
        expr->params[i].type = param.type;
        method->params.push_back(std::move(param));
    }
    expr->method = method;

    Function function;
    function.method = method;
    function.parent = fn_;
    function.lambda = expr;
    function.owner = fn_->owner;
    function.isStatic = fn_->isStatic;
    if (!function.isStatic)
        function.localTypes.push_back(fn_->owner);
    for (const ParamInfo& param : method->params)
        function.localTypes.push_back(param.type);
    function.boxed.assign(function.localTypes.size(), 0);

    Function* saved = fn_;
    fn_ = &function;
    pushScope();
    std::uint32_t slot = function.isStatic ? 0u : 1u;
    for (const ParamInfo& param : method->params) {
        Local local;
        local.slot = slot++;
        local.type = param.type;
        function.scopes.back()[param.name] = local;
    }
    if (expr->bodyExpr) {
        if (invoke->returnType->kind == TypeKind::Void)
            checkExpr(expr->bodyExpr);
        else
            expr->bodyExpr = coerce(expr->bodyExpr, invoke->returnType, "return");
    } else {
        checkStatement(expr->bodyBlock);
    }
    popScope();
    method->body.localTypes = function.localTypes;
    method->body.localBoxed = function.boxed;
    expr->capturesThis = function.capturesThis;
    fn_ = saved;

    // Capturing `this` makes the lambda an instance method of the same type,
    // so the delegate carries the receiver rather than a box.
    if (!expr->capturesThis && !function.isStatic && function.captures.empty())
        expr->capturesThis = true;  // the frame still has a `this` slot to fill
    return expected;
}

}  // namespace typed
}  // namespace internal
}  // namespace script

