// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/typed/module.hpp"

#include <cstring>
#include <unordered_map>

#include "script/internal/typed/runtime.hpp"
#include "script/internal/typed/object.hpp"
#include "script/internal/typed/types.hpp"

namespace script {
namespace internal {
namespace typed {

namespace {

// ---------------------------------------------------------------- writer ---

class Writer
{
public:
    void u8(std::uint8_t v) { bytes.push_back(v); }
    void u32(std::uint32_t v)
    {
        bytes.push_back(std::uint8_t(v));
        bytes.push_back(std::uint8_t(v >> 8));
        bytes.push_back(std::uint8_t(v >> 16));
        bytes.push_back(std::uint8_t(v >> 24));
    }
    void u64(std::uint64_t v)
    {
        u32(std::uint32_t(v));
        u32(std::uint32_t(v >> 32));
    }
    void raw(const void* data, std::size_t size)
    {
        const auto* p = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), p, p + size);
    }

    /// Strings are pooled, so a name shared by a hundred methods is stored
    /// once and every use of it is four bytes.
    std::uint32_t intern(StringView text)
    {
        const std::string utf8 = toUtf8(text);
        const auto it = pool.find(utf8);
        if (it != pool.end())
            return it->second;
        const std::uint32_t index = std::uint32_t(strings.size());
        strings.push_back(utf8);
        pool.emplace(utf8, index);
        return index;
    }
    std::uint32_t intern(const std::string& utf8) { return intern(fromUtf8(utf8)); }

    std::vector<std::uint8_t> bytes;
    std::vector<std::string> strings;
    std::unordered_map<std::string, std::uint32_t> pool;
};

class Reader
{
public:
    Reader(const std::uint8_t* data, std::size_t size) : p_(data), end_(data + size) {}

    std::uint8_t u8()
    {
        check(1);
        return *p_++;
    }
    std::uint32_t u32()
    {
        check(4);
        const std::uint32_t v = std::uint32_t(p_[0]) | (std::uint32_t(p_[1]) << 8) |
                                (std::uint32_t(p_[2]) << 16) | (std::uint32_t(p_[3]) << 24);
        p_ += 4;
        return v;
    }
    std::uint64_t u64()
    {
        const std::uint64_t low = u32();
        return low | (std::uint64_t(u32()) << 32);
    }
    void raw(void* out, std::size_t size)
    {
        if (size == 0)
            return;
        check(size);
        std::memcpy(out, p_, size);
        p_ += size;
    }
    std::size_t remaining() const noexcept { return std::size_t(end_ - p_); }

private:
    void check(std::size_t size) const
    {
        if (std::size_t(end_ - p_) < size)
            throw Error(ErrorCode::BadImage, "the assembly image is truncated");
    }
    const std::uint8_t* p_;
    const std::uint8_t* end_;
};

/// How a reference is encoded. Anything this assembly defines is an index into
/// its own table; anything else is a name, resolved when the image is loaded.
enum RefKind : std::uint8_t
{
    RefNull = 0,
    RefLocal = 1,
    RefExternal = 2,
};

class ImageWriter
{
public:
    ImageWriter(const AssemblyImpl& assembly, Writer& writer) : w_(writer)
    {
        for (std::size_t i = 0; i < assembly.types.size(); ++i)
            typeIndex_[assembly.types[i].get()] = std::uint32_t(i);
        for (std::size_t i = 0; i < assembly.methodTable.size(); ++i)
            methodIndex_[assembly.methodTable[i].get()] = std::uint32_t(i);
    }

    void typeRef(const TypeInfo* type)
    {
        if (!type) {
            w_.u8(RefNull);
            return;
        }
        const auto local = typeIndex_.find(type);
        if (local != typeIndex_.end()) {
            w_.u8(RefLocal);
            w_.u32(local->second);
            return;
        }
        w_.u8(RefExternal);
        w_.u32(w_.intern(type->fullName));
    }

    void methodRef(const MethodInfo* method)
    {
        if (!method) {
            w_.u8(RefNull);
            return;
        }
        const auto local = methodIndex_.find(method);
        if (local != methodIndex_.end()) {
            w_.u8(RefLocal);
            w_.u32(local->second);
            return;
        }
        // An external method is found again by owner, name and signature --
        // which is exactly what overload resolution already settled.
        w_.u8(RefExternal);
        typeRef(method->owner);
        w_.u32(w_.intern(method->name));
        w_.u32(std::uint32_t(method->params.size()));
        for (const ParamInfo& param : method->params)
            typeRef(param.type);
    }

    void value(const Value& v)
    {
        w_.u8(std::uint8_t(v.type()));
        switch (v.type()) {
            case Type::Bool: w_.u8(detail::Access::boolean(v) ? 1 : 0); break;
            case Type::Char:
            case Type::Int: w_.u32(std::uint32_t(detail::Access::i4(v))); break;
            case Type::Long: w_.u64(std::uint64_t(detail::Access::i8(v))); break;
            case Type::Float: {
                std::uint32_t bits = 0;
                const float f = detail::Access::r4(v);
                std::memcpy(&bits, &f, sizeof bits);
                w_.u32(bits);
                break;
            }
            case Type::Double: {
                std::uint64_t bits = 0;
                const double d = detail::Access::r8(v);
                std::memcpy(&bits, &d, sizeof bits);
                w_.u64(bits);
                break;
            }
            case Type::String: {
                auto* text = static_cast<StringCell*>(detail::Access::ptr(v));
                w_.u32(w_.intern(text ? text->text() : StringView()));
                break;
            }
            default: break;  // null, and live references, do not serialize
        }
    }

    void name(StringView text) { w_.u32(w_.intern(text)); }
    void name(const std::string& text) { w_.u32(w_.intern(text)); }

private:
    Writer& w_;
    std::unordered_map<const TypeInfo*, std::uint32_t> typeIndex_;
    std::unordered_map<const MethodInfo*, std::uint32_t> methodIndex_;
};

}  // namespace

// ----------------------------------------------------------------- write ---

std::vector<std::uint8_t> writeImage(const AssemblyImpl& assembly)
{
    Writer body;
    ImageWriter w(assembly, body);

    // Both counts come first so a loader can create every type and method
    // object before it reads anything that points at one.
    body.u32(std::uint32_t(assembly.types.size()));
    body.u32(std::uint32_t(assembly.methodTable.size()));

    for (const auto& type : assembly.types) {
        w.name(type->fullName);
        body.u8(std::uint8_t(type->kind));
        body.u8(std::uint8_t(type->nativeKind));
        body.u8(std::uint8_t(type->access));
        body.u8(std::uint8_t((type->isSealed ? 1 : 0) | (type->isAbstract ? 2 : 0) |
                             (type->isStaticClass ? 4 : 0)));
        w.typeRef(type->base);
        w.typeRef(type->element);
        w.typeRef(type->underlying);
        body.u32(type->instanceSlots);
        body.u32(std::uint32_t(type->interfaces.size()));
        for (const TypeInfo* iface : type->interfaces)
            w.typeRef(iface);

        auto writeFields = [&](const std::vector<FieldInfo>& fields) {
            body.u32(std::uint32_t(fields.size()));
            for (const FieldInfo& field : fields) {
                w.name(field.name);
                w.typeRef(field.type);
                body.u32(field.slot);
                body.u8(std::uint8_t(field.access));
                body.u8(std::uint8_t((field.isStatic ? 1 : 0) | (field.isReadOnly ? 2 : 0) |
                                     (field.isConst ? 4 : 0)));
                w.value(field.constValue);
            }
        };
        writeFields(type->fields);
        writeFields(type->statics);

        body.u32(std::uint32_t(type->props.size()));
        for (const PropertyInfo& prop : type->props) {
            w.name(prop.name);
            w.typeRef(prop.type);
            w.methodRef(prop.getter);
            w.methodRef(prop.setter);
            body.u8(prop.isStatic ? 1 : 0);
            body.u8(std::uint8_t(prop.access));
        }

        body.u32(std::uint32_t(type->methods.size()));
        for (const MethodInfo* method : type->methods)
            w.methodRef(method);
        body.u32(std::uint32_t(type->vtable.size()));
        for (const MethodInfo* method : type->vtable)
            w.methodRef(method);
        w.methodRef(type->invoke);
    }

    for (const auto& method : assembly.methodTable) {
        w.name(method->name);
        w.typeRef(method->owner);
        w.typeRef(method->returnType);
        body.u32(method->flags);
        body.u8(std::uint8_t(method->access));
        body.u32(std::uint32_t(method->vtableSlot));
        body.u32(method->numLocals);
        body.u32(method->accessorSlot);

        body.u32(std::uint32_t(method->params.size()));
        for (const ParamInfo& param : method->params) {
            w.name(param.name);
            w.typeRef(param.type);
            body.u8(std::uint8_t(param.mode));
            body.u8(param.hasDefault ? 1 : 0);
            w.value(param.defaultValue);
        }
        body.u32(std::uint32_t(method->captureSlots.size()));
        for (std::uint32_t slot : method->captureSlots)
            body.u32(slot);

        const MethodBody& code = method->body;
        body.u32(std::uint32_t(code.code.size()));
        body.raw(code.code.data(), code.code.size());
        body.u32(code.maxStack);
        body.u32(std::uint32_t(code.handlers.size()));
        for (const Handler& handler : code.handlers) {
            body.u32(handler.start);
            body.u32(handler.end);
            body.u32(handler.target);
            body.u32(handler.stackDepth);
            body.u32(handler.exceptionSlot);
            body.u8(std::uint8_t(handler.kind));
            w.typeRef(handler.catchType);
        }
        body.u32(std::uint32_t(code.lines.size()));
        for (const LineEntry& entry : code.lines) {
            body.u32(entry.offset);
            body.u32(entry.line);
        }
        body.u32(std::uint32_t(code.localTypes.size()));
        for (const TypeInfo* localType : code.localTypes)
            w.typeRef(localType);
        body.u32(std::uint32_t(code.localBoxed.size()));
        for (std::uint8_t boxed : code.localBoxed)
            body.u8(boxed);
    }

    body.u32(std::uint32_t(assembly.typeRefs.size()));
    for (const TypeInfo* type : assembly.typeRefs)
        w.typeRef(type);
    body.u32(std::uint32_t(assembly.methodRefs.size()));
    for (const MethodInfo* method : assembly.methodRefs)
        w.methodRef(method);

    body.u32(std::uint32_t(assembly.strings.size()));
    for (const Value& literal : assembly.strings)
        w.value(literal);
    body.u32(std::uint32_t(assembly.statics.size()));
    for (const Value& value : assembly.statics)
        w.value(value);

    w.methodRef(assembly.entry);
    body.u32(std::uint32_t(assembly.initializers.size()));
    for (const MethodInfo* initializer : assembly.initializers)
        w.methodRef(initializer);
    w.name(assembly.name);
    w.name(assembly.sourceName ? *assembly.sourceName : std::string());

    // The string pool goes first but is built last: writing the body is what
    // discovers which strings the image needs.
    Writer image;
    image.u32(kImageMagic);
    image.u32(kImageVersion);
    image.u32(std::uint32_t(body.strings.size()));
    for (const std::string& text : body.strings) {
        image.u32(std::uint32_t(text.size()));
        image.raw(text.data(), text.size());
    }
    image.u32(std::uint32_t(body.bytes.size()));
    image.raw(body.bytes.data(), body.bytes.size());
    return std::move(image.bytes);
}

// ------------------------------------------------------------------ read ---

const TypeInfo* resolveTypeName(Runtime& rt, StringView fullName)
{
    if (fullName.empty())
        return nullptr;
    if (const TypeInfo* found = rt.types->find(fullName))
        return found;

    if (fullName.size() > 2 && fullName.compare(fullName.size() - 2, 2, u"[]") == 0) {
        const TypeInfo* element = resolveTypeName(rt, fullName.substr(0, fullName.size() - 2));
        return element ? rt.types->arrayOf(element) : nullptr;
    }

    const std::size_t open = fullName.find(u'<');
    if (open == StringView::npos || fullName.back() != u'>') {
        const auto host = rt.hostTypes.find(String(fullName));
        return host == rt.hostTypes.end() ? nullptr : host->second;
    }

    const StringView head = fullName.substr(0, open);
    const StringView argText = fullName.substr(open + 1, fullName.size() - open - 2);
    std::vector<const TypeInfo*> args;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= argText.size(); ++i) {
        const bool last = i == argText.size();
        const Char c = last ? u',' : argText[i];
        if (c == u'<')
            ++depth;
        else if (c == u'>')
            --depth;
        if (last || (c == u',' && depth == 0)) {
            args.push_back(resolveTypeName(rt, argText.substr(start, i - start)));
            start = i + 1;
        }
    }
    for (const TypeInfo* arg : args) {
        if (!arg)
            return nullptr;
    }

    if (head == u"System.Collections.Generic.List" && args.size() == 1)
        return rt.types->listOf(args[0]);
    if (head == u"System.Collections.Generic.Dictionary" && args.size() == 2)
        return rt.types->dictionaryOf(args[0], args[1]);
    if (head == u"System.Func" && !args.empty()) {
        const TypeInfo* result = args.back();
        args.pop_back();
        return rt.types->funcOf(args, result);
    }
    if (head == u"System.Action")
        return rt.types->actionOf(args);
    return nullptr;
}

namespace {

class ImageReader
{
public:
    ImageReader(Runtime& rt, Reader& reader, const std::vector<String>& strings,
                AssemblyImpl& assembly)
        : rt_(rt), r_(reader), strings_(strings), assembly_(assembly)
    {}

    StringView name()
    {
        const std::uint32_t index = r_.u32();
        if (index >= strings_.size())
            throw Error(ErrorCode::BadImage, "a string index is out of range");
        return strings_[index];
    }

    const TypeInfo* typeRef()
    {
        const std::uint8_t kind = r_.u8();
        if (kind == RefNull)
            return nullptr;
        if (kind == RefLocal) {
            const std::uint32_t index = r_.u32();
            if (index >= assembly_.types.size())
                throw Error(ErrorCode::BadImage, "a type index is out of range");
            return assembly_.types[index].get();
        }
        const String text = String(name());
        const TypeInfo* resolved = resolveTypeName(rt_, text);
        if (!resolved)
            throw Error(ErrorCode::BadImage,
                        "the image needs a type this context does not have: " + toUtf8(text));
        return resolved;
    }

    MethodInfo* methodRef()
    {
        const std::uint8_t kind = r_.u8();
        if (kind == RefNull)
            return nullptr;
        if (kind == RefLocal) {
            const std::uint32_t index = r_.u32();
            if (index >= assembly_.methodTable.size())
                throw Error(ErrorCode::BadImage, "a method index is out of range");
            return assembly_.methodTable[index].get();
        }
        const TypeInfo* owner = typeRef();
        const String methodName = String(name());
        const std::uint32_t count = r_.u32();
        std::vector<const TypeInfo*> params;
        params.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
            params.push_back(typeRef());

        std::vector<MethodInfo*> candidates;
        if (owner)
            owner->collectMethods(methodName, candidates);
        for (MethodInfo* candidate : candidates) {
            if (candidate->params.size() != params.size())
                continue;
            bool same = true;
            for (std::size_t i = 0; i < params.size(); ++i)
                same = same && candidate->params[i].type == params[i];
            if (same)
                return candidate;
        }
        throw Error(ErrorCode::BadImage,
                    "the image needs a method this context does not have: " +
                        (owner ? toUtf8(owner->fullName) + "." : std::string()) +
                        toUtf8(methodName));
    }

    Value value()
    {
        const Type tag = Type(r_.u8());
        switch (tag) {
            case Type::Bool: return Value(r_.u8() != 0);
            case Type::Char: return Value(char16_t(r_.u32()));
            case Type::Int: return Value(std::int32_t(r_.u32()));
            case Type::Long: return Value(std::int64_t(r_.u64()));
            case Type::Float: {
                const std::uint32_t bits = r_.u32();
                float f = 0;
                std::memcpy(&f, &bits, sizeof f);
                return Value(f);
            }
            case Type::Double: {
                const std::uint64_t bits = r_.u64();
                double d = 0;
                std::memcpy(&d, &bits, sizeof d);
                return Value(d);
            }
            case Type::String: return rt_.str(name());
            default: return Value();
        }
    }

private:
    Runtime& rt_;
    Reader& r_;
    const std::vector<String>& strings_;
    AssemblyImpl& assembly_;
};

}  // namespace

std::shared_ptr<AssemblyImpl> readImage(Runtime& rt, const void* data, std::size_t bytes)
{
    Reader header(static_cast<const std::uint8_t*>(data), bytes);
    if (header.u32() != kImageMagic)
        throw Error(ErrorCode::BadImage, "not a libscript assembly image");
    if (header.u32() != kImageVersion)
        throw Error(ErrorCode::BadImage, "the image was written by another version of libscript");

    const std::uint32_t stringCount = header.u32();
    std::vector<String> strings;
    strings.reserve(stringCount);
    for (std::uint32_t i = 0; i < stringCount; ++i) {
        const std::uint32_t length = header.u32();
        std::string utf8(length, '\0');
        header.raw(&utf8[0], length);
        strings.push_back(fromUtf8(utf8));
    }
    const std::uint32_t bodySize = header.u32();
    std::vector<std::uint8_t> bodyBytes(bodySize);
    header.raw(bodyBytes.data(), bodySize);

    auto assembly = std::make_shared<AssemblyImpl>();
    assembly->rt = &rt;
    Reader body(bodyBytes.data(), bodyBytes.size());
    ImageReader r(rt, body, strings, *assembly);

    const std::uint32_t typeCount = body.u32();
    const std::uint32_t methodCount = body.u32();
    for (std::uint32_t i = 0; i < typeCount; ++i)
        assembly->types.push_back(std::make_unique<TypeInfo>());
    for (std::uint32_t i = 0; i < methodCount; ++i) {
        auto method = std::make_unique<MethodInfo>();
        method->index = i;
        method->assembly = assembly.get();
        assembly->methodTable.push_back(std::move(method));
    }

    for (std::uint32_t i = 0; i < typeCount; ++i) {
        TypeInfo* type = assembly->types[i].get();
        type->assembly = assembly.get();
        type->fullName = String(r.name());
        const std::size_t dot = type->fullName.rfind(u'.');
        type->name = dot == String::npos ? type->fullName : type->fullName.substr(dot + 1);
        type->kind = TypeKind(body.u8());
        type->nativeKind = NativeKind(body.u8());
        type->access = Access(body.u8());
        const std::uint8_t flags = body.u8();
        type->isSealed = (flags & 1) != 0;
        type->isAbstract = (flags & 2) != 0;
        type->isStaticClass = (flags & 4) != 0;
        type->base = r.typeRef();
        type->element = r.typeRef();
        type->underlying = r.typeRef();
        type->instanceSlots = body.u32();
        const std::uint32_t interfaceCount = body.u32();
        for (std::uint32_t k = 0; k < interfaceCount; ++k)
            type->interfaces.push_back(r.typeRef());

        auto readFields = [&](std::vector<FieldInfo>& fields) {
            const std::uint32_t count = body.u32();
            for (std::uint32_t k = 0; k < count; ++k) {
                FieldInfo field;
                field.name = String(r.name());
                field.type = r.typeRef();
                field.owner = type;
                field.slot = body.u32();
                field.access = Access(body.u8());
                const std::uint8_t fieldFlags = body.u8();
                field.isStatic = (fieldFlags & 1) != 0;
                field.isReadOnly = (fieldFlags & 2) != 0;
                field.isConst = (fieldFlags & 4) != 0;
                field.constValue = r.value();
                fields.push_back(std::move(field));
            }
        };
        readFields(type->fields);
        readFields(type->statics);

        const std::uint32_t propCount = body.u32();
        for (std::uint32_t k = 0; k < propCount; ++k) {
            PropertyInfo prop;
            prop.name = String(r.name());
            prop.type = r.typeRef();
            prop.owner = type;
            prop.getter = r.methodRef();
            prop.setter = r.methodRef();
            prop.isStatic = body.u8() != 0;
            prop.access = Access(body.u8());
            type->props.push_back(std::move(prop));
        }

        const std::uint32_t typeMethodCount = body.u32();
        for (std::uint32_t k = 0; k < typeMethodCount; ++k)
            type->methods.push_back(r.methodRef());
        const std::uint32_t vtableCount = body.u32();
        for (std::uint32_t k = 0; k < vtableCount; ++k)
            type->vtable.push_back(r.methodRef());
        type->invoke = r.methodRef();

        rt.types->adopt(type);
        assembly->typeIndex[type->fullName] = type;
    }

    for (std::uint32_t i = 0; i < methodCount; ++i) {
        MethodInfo* method = assembly->methodTable[i].get();
        method->name = String(r.name());
        method->owner = r.typeRef();
        method->returnType = r.typeRef();
        method->flags = std::uint16_t(body.u32());
        method->access = Access(body.u8());
        method->vtableSlot = std::int32_t(body.u32());
        method->numLocals = body.u32();
        method->accessorSlot = body.u32();

        const std::uint32_t paramCount = body.u32();
        for (std::uint32_t k = 0; k < paramCount; ++k) {
            ParamInfo param;
            param.name = String(r.name());
            param.type = r.typeRef();
            param.mode = ParamMode(body.u8());
            param.hasDefault = body.u8() != 0;
            param.defaultValue = r.value();
            method->params.push_back(std::move(param));
        }
        const std::uint32_t captureCount = body.u32();
        for (std::uint32_t k = 0; k < captureCount; ++k)
            method->captureSlots.push_back(body.u32());

        MethodBody& code = method->body;
        const std::uint32_t codeSize = body.u32();
        code.code.resize(codeSize);
        body.raw(code.code.data(), codeSize);
        code.maxStack = body.u32();
        const std::uint32_t handlerCount = body.u32();
        for (std::uint32_t k = 0; k < handlerCount; ++k) {
            Handler handler;
            handler.start = body.u32();
            handler.end = body.u32();
            handler.target = body.u32();
            handler.stackDepth = body.u32();
            handler.exceptionSlot = body.u32();
            handler.kind = HandlerKind(body.u8());
            handler.catchType = r.typeRef();
            code.handlers.push_back(handler);
        }
        const std::uint32_t lineCount = body.u32();
        for (std::uint32_t k = 0; k < lineCount; ++k) {
            LineEntry entry;
            entry.offset = body.u32();
            entry.line = body.u32();
            code.lines.push_back(entry);
        }
        const std::uint32_t localCount = body.u32();
        for (std::uint32_t k = 0; k < localCount; ++k)
            code.localTypes.push_back(r.typeRef());
        const std::uint32_t boxedCount = body.u32();
        for (std::uint32_t k = 0; k < boxedCount; ++k)
            code.localBoxed.push_back(body.u8());
    }

    const std::uint32_t typeRefCount = body.u32();
    for (std::uint32_t i = 0; i < typeRefCount; ++i)
        assembly->typeRefs.push_back(r.typeRef());
    const std::uint32_t methodRefCount = body.u32();
    for (std::uint32_t i = 0; i < methodRefCount; ++i)
        assembly->methodRefs.push_back(r.methodRef());

    const std::uint32_t literalCount = body.u32();
    for (std::uint32_t i = 0; i < literalCount; ++i)
        assembly->strings.push_back(r.value());
    const std::uint32_t staticCount = body.u32();
    for (std::uint32_t i = 0; i < staticCount; ++i)
        assembly->statics.push_back(r.value());

    assembly->entry = r.methodRef();
    const std::uint32_t initializerCount = body.u32();
    for (std::uint32_t i = 0; i < initializerCount; ++i)
        assembly->initializers.push_back(r.methodRef());
    assembly->name = toUtf8(r.name());
    assembly->sourceName = std::make_shared<const std::string>(toUtf8(r.name()));
    return assembly;
}

}  // namespace typed
}  // namespace internal
}  // namespace script
