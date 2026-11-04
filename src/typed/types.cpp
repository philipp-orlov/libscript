// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/typed/types.hpp"

#include <algorithm>

#include "script/internal/typed/runtime.hpp"
#include "script/internal/typed/object.hpp"

namespace script {
namespace internal {
namespace typed {

// --------------------------------------------------------------- kinds ---

const char* kindName(TypeKind k) noexcept
{
    switch (k) {
        case TypeKind::Void: return "void";
        case TypeKind::Bool: return "bool";
        case TypeKind::SByte: return "sbyte";
        case TypeKind::Byte: return "byte";
        case TypeKind::Short: return "short";
        case TypeKind::UShort: return "ushort";
        case TypeKind::Int: return "int";
        case TypeKind::UInt: return "uint";
        case TypeKind::Long: return "long";
        case TypeKind::ULong: return "ulong";
        case TypeKind::Char: return "char";
        case TypeKind::Float: return "float";
        case TypeKind::Double: return "double";
        case TypeKind::Enum: return "enum";
        case TypeKind::Struct: return "struct";
        case TypeKind::String: return "string";
        case TypeKind::Object: return "object";
        case TypeKind::Class: return "class";
        case TypeKind::Interface: return "interface";
        case TypeKind::Delegate: return "delegate";
        case TypeKind::Array: return "array";
        case TypeKind::Null: return "null";
        case TypeKind::Error: return "?";
    }
    return "?";
}

Type runtimeTag(TypeKind k) noexcept
{
    switch (k) {
        case TypeKind::Bool: return Type::Bool;
        case TypeKind::Char: return Type::Char;
        case TypeKind::SByte:
        case TypeKind::Byte:
        case TypeKind::Short:
        case TypeKind::UShort:
        case TypeKind::Int:
        case TypeKind::UInt:
        case TypeKind::Enum: return Type::Int;
        case TypeKind::Long:
        case TypeKind::ULong: return Type::Long;
        case TypeKind::Float: return Type::Float;
        case TypeKind::Double: return Type::Double;
        case TypeKind::String: return Type::String;
        case TypeKind::Array: return Type::Array;
        case TypeKind::Delegate: return Type::Delegate;
        default: return Type::Object;
    }
}

// ------------------------------------------------------------ metadata ---

std::string MethodInfo::qualifiedName() const
{
    std::string out;
    if (owner)
        out = toUtf8(owner->fullName) + ".";
    return out + toUtf8(name);
}

bool TypeInfo::derivesFrom(const TypeInfo* other) const noexcept
{
    for (const TypeInfo* t = this; t; t = t->base) {
        if (t == other)
            return true;
    }
    return false;
}

bool TypeInfo::implements(const TypeInfo* iface) const noexcept
{
    for (const TypeInfo* t = this; t; t = t->base) {
        for (const TypeInfo* i : t->interfaces) {
            if (i == iface || (i && i->implements(iface)))
                return true;
        }
    }
    return false;
}

const FieldInfo* TypeInfo::findField(StringView fieldName) const noexcept
{
    for (const TypeInfo* t = this; t; t = t->base) {
        for (const FieldInfo& f : t->fields) {
            if (f.name == fieldName)
                return &f;
        }
        for (const FieldInfo& f : t->statics) {
            if (f.name == fieldName)
                return &f;
        }
    }
    return nullptr;
}

const PropertyInfo* TypeInfo::findProperty(StringView propName) const noexcept
{
    for (const TypeInfo* t = this; t; t = t->base) {
        for (const PropertyInfo& p : t->props) {
            if (p.name == propName)
                return &p;
        }
        for (const TypeInfo* i : t->interfaces) {
            if (const PropertyInfo* p = i ? i->findProperty(propName) : nullptr)
                return p;
        }
    }
    return nullptr;
}

void TypeInfo::collectMethods(StringView methodName, std::vector<MethodInfo*>& out) const
{
    for (const TypeInfo* t = this; t; t = t->base) {
        for (MethodInfo* m : t->methods) {
            if (m->name != methodName)
                continue;
            // A method the derived type already contributed hides the base's.
            const bool hidden = std::any_of(out.begin(), out.end(), [m](const MethodInfo* seen) {
                if (seen->params.size() != m->params.size())
                    return false;
                for (std::size_t i = 0; i < m->params.size(); ++i) {
                    if (seen->params[i].type != m->params[i].type)
                        return false;
                }
                return true;
            });
            if (!hidden)
                out.push_back(m);
        }
        for (const TypeInfo* i : t->interfaces) {
            if (i)
                i->collectMethods(methodName, out);
        }
    }
}

// ------------------------------------------------------------ assembly ---

AssemblyImpl::~AssemblyImpl()
{
    // The context's type table points at types this assembly owns; unloading
    // has to take them back out of it.
    if (rt && rt->types) {
        for (auto& type : types)
            rt->types->forget(type.get());
    }
    release();
}

void AssemblyImpl::release() noexcept
{
    strings.clear();
    statics.clear();
    for (auto& type : types) {
        for (FieldInfo& f : type->statics)
            f.constValue = Value();
    }
    for (auto& method : methodTable) {
        for (ParamInfo& p : method->params)
            p.defaultValue = Value();
        method->native = nullptr;
    }
}

TypeInfo* AssemblyImpl::findType(StringView fullTypeName) const noexcept
{
    const auto it = typeIndex.find(String(fullTypeName));
    return it == typeIndex.end() ? nullptr : it->second;
}

MethodInfo* AssemblyImpl::findMethod(StringView qualified) const noexcept
{
    // `Type..ctor` is how a constructor is spelled, so the split cannot just
    // be at the last dot.
    StringView typeName;
    StringView methodName;
    const StringView ctorSuffix = u"..ctor";
    if (qualified.size() > ctorSuffix.size() &&
        qualified.compare(qualified.size() - ctorSuffix.size(), ctorSuffix.size(), ctorSuffix) ==
            0) {
        typeName = qualified.substr(0, qualified.size() - ctorSuffix.size());
        methodName = u".ctor";
    } else {
        const std::size_t dot = qualified.rfind(u'.');
        if (dot == StringView::npos)
            return nullptr;
        typeName = qualified.substr(0, dot);
        methodName = qualified.substr(dot + 1);
    }

    TypeInfo* type = findType(typeName);
    if (!type)
        return nullptr;
    for (MethodInfo* m : type->methods) {
        if (m->name == methodName)
            return m;
    }
    return nullptr;
}

std::vector<std::string> AssemblyImpl::methods() const
{
    std::vector<std::string> names;
    for (const auto& method : methodTable)
        names.push_back(method->qualifiedName());
    return names;
}

std::string AssemblyImpl::entryPoint() const
{
    return entry ? entry->qualifiedName() : std::string();
}

std::size_t AssemblyImpl::imageSize() const noexcept
{
    std::size_t bytes = 0;
    for (const auto& method : methodTable)
        bytes += method->body.byteSize() + sizeof(MethodInfo);
    for (const auto& type : types)
        bytes += sizeof(TypeInfo) + type->fields.size() * sizeof(FieldInfo) +
                 type->vtable.size() * sizeof(void*);
    return bytes;
}

// ----------------------------------------------------------- type table ---

TypeTable::TypeTable(Runtime& runtime) : rt_(&runtime)
{
    installPrimitives();
    installCore(*this);
    installString(*this);
    installMath(*this);
    installCollections(*this);
}

TypeTable::~TypeTable() = default;

TypeInfo* TypeTable::create(TypeKind kind, const char* fullName)
{
    auto owned = std::make_unique<TypeInfo>();
    TypeInfo* type = owned.get();
    type->kind = kind;
    type->fullName = ascii(fullName);
    const std::size_t dot = type->fullName.rfind(u'.');
    type->name = dot == String::npos ? type->fullName : type->fullName.substr(dot + 1);
    owned_.push_back(std::move(owned));
    index_[type->fullName] = type;
    return type;
}

void TypeTable::adopt(TypeInfo* type)
{
    index_[type->fullName] = type;
}

void TypeTable::forget(TypeInfo* type) noexcept
{
    const auto it = index_.find(type->fullName);
    if (it != index_.end() && it->second == type)
        index_.erase(it);
}

ParamInfo TypeTable::param(const char* name, const TypeInfo* type, ParamMode mode) const
{
    ParamInfo p;
    p.name = ascii(name);
    p.type = type;
    p.mode = mode;
    return p;
}

MethodInfo* TypeTable::addMethod(TypeInfo* owner, const char* name, const TypeInfo* returnType,
                                 std::vector<ParamInfo> params, std::uint16_t flags,
                                 NativeFunction fn)
{
    auto owned = std::make_unique<MethodInfo>();
    MethodInfo* method = owned.get();
    method->name = ascii(name);
    method->owner = owner;
    method->returnType = returnType;
    method->params = std::move(params);
    method->flags = flags | (fn ? MethodNative : 0);
    method->access = Access::Public;
    method->native = std::move(fn);
    methods_.push_back(std::move(owned));
    owner->methods.push_back(method);
    return method;
}

PropertyInfo* TypeTable::addProperty(TypeInfo* owner, const char* name, const TypeInfo* type,
                                     NativeFunction getter, NativeFunction setter, bool isStatic)
{
    PropertyInfo prop;
    prop.name = ascii(name);
    prop.type = type;
    prop.owner = owner;
    prop.access = Access::Public;
    prop.isStatic = isStatic;
    const std::uint16_t flags =
        std::uint16_t(MethodAccessor) | (isStatic ? std::uint16_t(MethodStatic) : 0);
    if (getter)
        prop.getter = addMethod(owner, (std::string("get_") + name).c_str(), type, {}, flags,
                                std::move(getter));
    if (setter)
        prop.setter = addMethod(owner, (std::string("set_") + name).c_str(), core_.voidType,
                                {param("value", type)}, flags, std::move(setter));
    owner->props.push_back(std::move(prop));
    return &owner->props.back();
}

void TypeTable::installPrimitives()
{
    struct Spec
    {
        TypeKind kind;
        const char* fullName;
        const TypeInfo** slot;
    };
    const Spec specs[] = {
        {TypeKind::Void, "System.Void", &core_.voidType},
        {TypeKind::Bool, "System.Boolean", &core_.boolType},
        {TypeKind::SByte, "System.SByte", &core_.sbyteType},
        {TypeKind::Byte, "System.Byte", &core_.byteType},
        {TypeKind::Short, "System.Int16", &core_.shortType},
        {TypeKind::UShort, "System.UInt16", &core_.ushortType},
        {TypeKind::Int, "System.Int32", &core_.intType},
        {TypeKind::UInt, "System.UInt32", &core_.uintType},
        {TypeKind::Long, "System.Int64", &core_.longType},
        {TypeKind::ULong, "System.UInt64", &core_.ulongType},
        {TypeKind::Char, "System.Char", &core_.charType},
        {TypeKind::Float, "System.Single", &core_.floatType},
        {TypeKind::Double, "System.Double", &core_.doubleType},
        {TypeKind::Object, "System.Object", &core_.objectType},
        {TypeKind::String, "System.String", &core_.stringType},
        {TypeKind::Null, "<null>", &core_.nullType},
        {TypeKind::Error, "<error>", &core_.errorType},
    };
    for (const Spec& spec : specs) {
        TypeInfo* type = create(spec.kind, spec.fullName);
        *spec.slot = type;
        byKind_[std::size_t(spec.kind)] = type;
    }
    // Every value type is sealed and derives from object.
    for (const auto& type : owned_) {
        if (type->kind != TypeKind::Object && type->kind != TypeKind::Null &&
            type->kind != TypeKind::Error)
            type->base = core_.objectType;
        type->isSealed = type->kind != TypeKind::Object;
    }

    // The keyword spelling is a name a program can use on its own -- `int.
    // Parse`, `string.Join` -- so the table answers to both.
    const std::pair<const char*, const TypeInfo*> aliases[] = {
        {"void", core_.voidType},     {"bool", core_.boolType},
        {"sbyte", core_.sbyteType},   {"byte", core_.byteType},
        {"short", core_.shortType},   {"ushort", core_.ushortType},
        {"int", core_.intType},       {"uint", core_.uintType},
        {"long", core_.longType},     {"ulong", core_.ulongType},
        {"char", core_.charType},     {"float", core_.floatType},
        {"double", core_.doubleType}, {"string", core_.stringType},
        {"object", core_.objectType},
    };
    for (const auto& alias : aliases)
        index_[ascii(alias.first)] = const_cast<TypeInfo*>(alias.second);
}

const TypeInfo* TypeTable::forKind(TypeKind kind) const noexcept
{
    return byKind_[std::size_t(kind)];
}

const TypeInfo* TypeTable::find(StringView fullName) const noexcept
{
    const auto it = index_.find(String(fullName));
    return it == index_.end() ? nullptr : it->second;
}

const TypeInfo* TypeTable::arrayOf(const TypeInfo* element)
{
    if (!element)
        element = core_.objectType;
    const auto it = arrays_.find(element);
    if (it != arrays_.end())
        return it->second;

    auto owned = std::make_unique<TypeInfo>();
    TypeInfo* type = owned.get();
    type->kind = TypeKind::Array;
    type->element = element;
    type->base = core_.objectType;
    type->isSealed = true;
    type->fullName = element->fullName + ascii("[]");
    type->name = element->name + ascii("[]");
    arrays_[element] = type;
    index_[type->fullName] = type;
    owned_.push_back(std::move(owned));
    return type;
}

namespace {

String constructedName(const char* base, const std::vector<const TypeInfo*>& args)
{
    String name = ascii(base);
    name.push_back(u'<');
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i)
            name.push_back(u',');
        name += args[i]->fullName;
    }
    name.push_back(u'>');
    return name;
}

}  // namespace

const TypeInfo* TypeTable::tupleOf(const std::vector<const TypeInfo*>& elements,
                                  const std::vector<String>& names)
{
    const String name = constructedName("System.ValueTuple", elements);
    const auto it = constructed_.find(name);
    if (it != constructed_.end()) {
        // `(int, string)` and `(int x, string y)` are the same type; the
        // names are aliases onto its slots, added the first time they appear.
        TypeInfo* existing = const_cast<TypeInfo*>(it->second);
        for (std::size_t i = 0; i < names.size() && i < elements.size(); ++i) {
            if (names[i].empty() || existing->findField(names[i]))
                continue;
            FieldInfo alias;
            alias.name = names[i];
            alias.type = elements[i];
            alias.owner = existing;
            alias.slot = std::uint32_t(i);
            alias.access = Access::Public;
            existing->fields.push_back(std::move(alias));
        }
        return existing;
    }

    auto owned = std::make_unique<TypeInfo>();
    TypeInfo* type = owned.get();
    type->kind = TypeKind::Class;
    type->nativeKind = NativeKind::Tuple;
    type->base = core_.objectType;
    type->isSealed = true;
    type->typeArgs = elements;
    type->fullName = name;
    type->name = ascii("ValueTuple");
    type->instanceSlots = std::uint32_t(elements.size());
    for (std::size_t i = 0; i < elements.size(); ++i) {
        FieldInfo field;
        field.name = ascii(("Item" + std::to_string(i + 1)).c_str());
        field.type = elements[i];
        field.owner = type;
        field.slot = std::uint32_t(i);
        field.access = Access::Public;
        type->fields.push_back(std::move(field));
    }
    // `(x: 1, y: 2)` reaches the same slot under either name.
    for (std::size_t i = 0; i < names.size() && i < elements.size(); ++i) {
        if (names[i].empty())
            continue;
        FieldInfo alias = type->fields[i];
        alias.name = names[i];
        type->fields.push_back(std::move(alias));
    }
    constructed_[name] = type;
    index_[name] = type;
    owned_.push_back(std::move(owned));
    instantiateTuple(*this, type);
    return type;
}

const TypeInfo* TypeTable::enumerableOf(const TypeInfo* element)
{
    if (!element)
        element = core_.objectType;
    const String name = constructedName("System.Collections.Generic.IEnumerable", {element});
    const auto it = constructed_.find(name);
    if (it != constructed_.end())
        return it->second;

    auto owned = std::make_unique<TypeInfo>();
    TypeInfo* type = owned.get();
    type->kind = TypeKind::Interface;
    type->base = nullptr;
    type->typeArgs = {element};
    type->element = element;
    type->fullName = name;
    type->name = ascii("IEnumerable");
    // Every closed `IEnumerable<T>` is also the open one, so a sequence can be
    // handed to anything that only cares that it is iterable.
    if (core_.enumerableType)
        type->interfaces.push_back(core_.enumerableType);
    constructed_[name] = type;
    index_[name] = type;
    owned_.push_back(std::move(owned));
    return type;
}

const TypeInfo* TypeTable::listOf(const TypeInfo* element)
{
    if (!element)
        element = core_.objectType;
    const String name = constructedName("System.Collections.Generic.List", {element});
    const auto it = constructed_.find(name);
    if (it != constructed_.end())
        return it->second;

    auto owned = std::make_unique<TypeInfo>();
    TypeInfo* type = owned.get();
    type->kind = TypeKind::Class;
    type->nativeKind = NativeKind::List;
    type->base = core_.objectType;
    type->isSealed = true;
    type->typeArgs = {element};
    type->element = element;
    type->fullName = name;
    type->name = ascii("List");
    if (core_.enumerableType)
        type->interfaces.push_back(core_.enumerableType);
    constructed_[name] = type;
    index_[name] = type;
    owned_.push_back(std::move(owned));
    type->interfaces.push_back(enumerableOf(element));
    instantiateList(*this, type);
    return type;
}

const TypeInfo* TypeTable::dictionaryOf(const TypeInfo* key, const TypeInfo* value)
{
    if (!key)
        key = core_.objectType;
    if (!value)
        value = core_.objectType;
    const String name = constructedName("System.Collections.Generic.Dictionary", {key, value});
    const auto it = constructed_.find(name);
    if (it != constructed_.end())
        return it->second;

    auto owned = std::make_unique<TypeInfo>();
    TypeInfo* type = owned.get();
    type->kind = TypeKind::Class;
    type->nativeKind = NativeKind::Dictionary;
    type->base = core_.objectType;
    type->isSealed = true;
    type->typeArgs = {key, value};
    type->fullName = name;
    type->name = ascii("Dictionary");
    constructed_[name] = type;
    index_[name] = type;
    owned_.push_back(std::move(owned));
    instantiateDictionary(*this, type);
    return type;
}

const TypeInfo* TypeTable::keyValuePairOf(const TypeInfo* key, const TypeInfo* value)
{
    if (!key)
        key = core_.objectType;
    if (!value)
        value = core_.objectType;
    const String name = constructedName("System.Collections.Generic.KeyValuePair", {key, value});
    const auto it = constructed_.find(name);
    if (it != constructed_.end())
        return it->second;

    auto owned = std::make_unique<TypeInfo>();
    TypeInfo* type = owned.get();
    type->kind = TypeKind::Class;
    type->nativeKind = NativeKind::Tuple;
    type->base = core_.objectType;
    type->isSealed = true;
    type->typeArgs = {key, value};
    type->fullName = name;
    type->name = ascii("KeyValuePair");
    type->instanceSlots = 2;
    const char* const names[] = {"Key", "Value"};
    for (std::uint32_t i = 0; i < 2; ++i) {
        FieldInfo field;
        field.name = ascii(names[i]);
        field.type = type->typeArgs[i];
        field.owner = type;
        field.slot = i;
        field.access = Access::Public;
        field.isReadOnly = true;
        type->fields.push_back(std::move(field));
    }
    constructed_[name] = type;
    index_[name] = type;
    owned_.push_back(std::move(owned));
    instantiateTuple(*this, type);
    return type;
}

const TypeInfo* TypeTable::funcOf(const std::vector<const TypeInfo*>& params,
                                  const TypeInfo* result)
{
    std::vector<const TypeInfo*> args = params;
    args.push_back(result);
    const String name = constructedName("System.Func", args);
    const auto it = constructed_.find(name);
    if (it != constructed_.end())
        return it->second;

    auto owned = std::make_unique<TypeInfo>();
    TypeInfo* type = owned.get();
    type->kind = TypeKind::Delegate;
    type->base = core_.objectType;
    type->isSealed = true;
    type->typeArgs = args;
    type->fullName = name;
    type->name = ascii("Func");
    constructed_[name] = type;
    index_[name] = type;
    owned_.push_back(std::move(owned));

    std::vector<ParamInfo> invokeParams;
    for (std::size_t i = 0; i < params.size(); ++i)
        invokeParams.push_back(param(("arg" + std::to_string(i)).c_str(), params[i]));
    type->invoke = addMethod(type, "Invoke", result, std::move(invokeParams), 0, nullptr);
    return type;
}

const TypeInfo* TypeTable::actionOf(const std::vector<const TypeInfo*>& params)
{
    const String name = constructedName("System.Action", params);
    const auto it = constructed_.find(name);
    if (it != constructed_.end())
        return it->second;

    auto owned = std::make_unique<TypeInfo>();
    TypeInfo* type = owned.get();
    type->kind = TypeKind::Delegate;
    type->base = core_.objectType;
    type->isSealed = true;
    type->typeArgs = params;
    type->fullName = name;
    type->name = ascii("Action");
    constructed_[name] = type;
    index_[name] = type;
    owned_.push_back(std::move(owned));

    std::vector<ParamInfo> invokeParams;
    for (std::size_t i = 0; i < params.size(); ++i)
        invokeParams.push_back(param(("arg" + std::to_string(i)).c_str(), params[i]));
    type->invoke = addMethod(type, "Invoke", core_.voidType, std::move(invokeParams), 0, nullptr);
    return type;
}

// --------------------------------------------------------- conversions ---

namespace {

/// The implicit numeric conversions, exactly as the specification lists them.
bool implicitNumeric(TypeKind from, TypeKind to) noexcept
{
    if (from == to)
        return true;
    switch (from) {
        case TypeKind::SByte:
            return to == TypeKind::Short || to == TypeKind::Int || to == TypeKind::Long ||
                   to == TypeKind::Float || to == TypeKind::Double;
        case TypeKind::Byte:
            return to == TypeKind::Short || to == TypeKind::UShort || to == TypeKind::Int ||
                   to == TypeKind::UInt || to == TypeKind::Long || to == TypeKind::ULong ||
                   to == TypeKind::Float || to == TypeKind::Double;
        case TypeKind::Short:
            return to == TypeKind::Int || to == TypeKind::Long || to == TypeKind::Float ||
                   to == TypeKind::Double;
        case TypeKind::UShort:
            return to == TypeKind::Int || to == TypeKind::UInt || to == TypeKind::Long ||
                   to == TypeKind::ULong || to == TypeKind::Float || to == TypeKind::Double;
        case TypeKind::Int:
            return to == TypeKind::Long || to == TypeKind::Float || to == TypeKind::Double;
        case TypeKind::UInt:
            return to == TypeKind::Long || to == TypeKind::ULong || to == TypeKind::Float ||
                   to == TypeKind::Double;
        case TypeKind::Long:
        case TypeKind::ULong:
            return to == TypeKind::Float || to == TypeKind::Double;
        case TypeKind::Char:
            return to == TypeKind::UShort || to == TypeKind::Int || to == TypeKind::UInt ||
                   to == TypeKind::Long || to == TypeKind::ULong || to == TypeKind::Float ||
                   to == TypeKind::Double;
        case TypeKind::Float: return to == TypeKind::Double;
        default: return false;
    }
}

bool sameSignature(const MethodInfo* a, const MethodInfo* b) noexcept
{
    if (!a || !b || a->returnType != b->returnType || a->params.size() != b->params.size())
        return false;
    for (std::size_t i = 0; i < a->params.size(); ++i) {
        if (a->params[i].type != b->params[i].type)
            return false;
    }
    return true;
}

}  // namespace

ConvKindResult TypeTable::classify(const TypeInfo* from, const TypeInfo* to) const noexcept
{
    ConvKindResult result;
    if (!from || !to)
        return result;
    if (from == to || from->kind == TypeKind::Error || to->kind == TypeKind::Error) {
        result.exists = true;
        return result;
    }

    // null literal
    if (from->kind == TypeKind::Null) {
        result.exists = isReferenceKind(to->kind);
        result.kind = ConvKind::NullLiteral;
        return result;
    }

    // numeric
    if (isNumeric(from->kind) && isNumeric(to->kind)) {
        const TypeKind f = from->kind == TypeKind::Enum ? TypeKind::Int : from->kind;
        const TypeKind t = to->kind == TypeKind::Enum ? TypeKind::Int : to->kind;
        result.exists = true;
        result.kind = from->kind == TypeKind::Enum
                          ? ConvKind::EnumToInt
                          : (to->kind == TypeKind::Enum ? ConvKind::IntToEnum : ConvKind::Numeric);
        // Between an enum and its underlying type a cast is always required.
        result.requiresCast = from->kind == TypeKind::Enum || to->kind == TypeKind::Enum ||
                              !implicitNumeric(f, t);
        return result;
    }

    // boxing into object
    if (to->kind == TypeKind::Object) {
        result.exists = true;
        result.kind = from->isValueType() ? ConvKind::Boxing : ConvKind::Reference;
        return result;
    }
    // unboxing out of object
    if (from->kind == TypeKind::Object) {
        result.exists = true;
        result.requiresCast = true;
        result.kind = to->isValueType() ? ConvKind::Unboxing : ConvKind::Reference;
        return result;
    }

    // reference conversions
    if (isReferenceKind(from->kind) && isReferenceKind(to->kind)) {
        if (to->kind == TypeKind::Interface &&
            (from->implements(to) || from->kind == TypeKind::Interface)) {
            result.exists = true;
            result.kind = ConvKind::Reference;
            result.requiresCast = !from->implements(to);
            return result;
        }
        if (from->derivesFrom(to)) {
            result.exists = true;
            result.kind = ConvKind::Reference;
            return result;
        }
        if (to->derivesFrom(from) || from->kind == TypeKind::Interface) {
            result.exists = true;
            result.requiresCast = true;
            result.kind = ConvKind::Reference;
            return result;
        }
        if (from->kind == TypeKind::Array && to->kind == TypeKind::Array &&
            from->element == to->element) {
            result.exists = true;
            result.kind = ConvKind::Reference;
            return result;
        }
        if (from->kind == TypeKind::Delegate && to->kind == TypeKind::Delegate &&
            sameSignature(from->invoke, to->invoke)) {
            result.exists = true;
            result.kind = ConvKind::Reference;
            return result;
        }
    }
    return result;
}

bool TypeTable::implicitlyConverts(const TypeInfo* from, const TypeInfo* to) const noexcept
{
    const ConvKindResult result = classify(from, to);
    return result.exists && !result.requiresCast;
}

bool TypeTable::explicitlyConverts(const TypeInfo* from, const TypeInfo* to) const noexcept
{
    return classify(from, to).exists;
}

const TypeInfo* TypeTable::promote(const TypeInfo* a, const TypeInfo* b) const noexcept
{
    if (!a || !b)
        return nullptr;
    if (!isNumeric(a->kind) || !isNumeric(b->kind))
        return nullptr;
    const TypeKind x = a->kind == TypeKind::Enum ? TypeKind::Int : a->kind;
    const TypeKind y = b->kind == TypeKind::Enum ? TypeKind::Int : b->kind;
    if (x == TypeKind::Double || y == TypeKind::Double)
        return core_.doubleType;
    if (x == TypeKind::Float || y == TypeKind::Float)
        return core_.floatType;
    if (x == TypeKind::ULong || y == TypeKind::ULong)
        return core_.ulongType;
    if (x == TypeKind::Long || y == TypeKind::Long)
        return core_.longType;
    if (x == TypeKind::UInt || y == TypeKind::UInt) {
        // `uint` with a signed operand widens to `long`, which is the only way
        // both halves of the range survive.
        const bool signedOther = x == TypeKind::SByte || x == TypeKind::Short ||
                                 x == TypeKind::Int || y == TypeKind::SByte ||
                                 y == TypeKind::Short || y == TypeKind::Int;
        return signedOther ? core_.longType : core_.uintType;
    }
    return core_.intType;
}

std::string TypeTable::describe(const TypeInfo* type) const
{
    if (!type)
        return "?";
    if (type->kind == TypeKind::Array)
        return describe(type->element) + "[]";
    switch (type->kind) {
        case TypeKind::Void:
        case TypeKind::Bool:
        case TypeKind::SByte:
        case TypeKind::Byte:
        case TypeKind::Short:
        case TypeKind::UShort:
        case TypeKind::Int:
        case TypeKind::UInt:
        case TypeKind::Long:
        case TypeKind::ULong:
        case TypeKind::Char:
        case TypeKind::Float:
        case TypeKind::Double:
        case TypeKind::String:
        case TypeKind::Object: return kindName(type->kind);
        case TypeKind::Null: return "<null>";
        default: break;
    }
    if (!type->typeArgs.empty()) {
        std::string out = toUtf8(type->name) + "<";
        for (std::size_t i = 0; i < type->typeArgs.size(); ++i) {
            if (i)
                out += ", ";
            out += describe(type->typeArgs[i]);
        }
        return out + ">";
    }
    return toUtf8(type->fullName);
}

const TypeInfo* TypeTable::exceptionFor(ErrorCode code) const noexcept
{
    const auto it = exceptions_.find(int(code));
    if (it != exceptions_.end())
        return it->second;
    return core_.exceptionType;
}

ErrorCode TypeTable::errorCodeFor(const TypeInfo* type) const noexcept
{
    for (const TypeInfo* t = type; t; t = t->base) {
        for (const auto& entry : exceptions_) {
            if (entry.second == t)
                return ErrorCode(entry.first);
        }
    }
    return ErrorCode::Exception;
}

}  // namespace typed
}  // namespace internal
}  // namespace script
