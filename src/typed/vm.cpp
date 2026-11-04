// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/typed/vm.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "script/internal/typed/bytecode.hpp"
#include "script/internal/typed/jit.hpp"
#include "script/internal/typed/numconv.hpp"
#include "script/internal/stack_guard.hpp"
#include "script/internal/typed/types.hpp"

namespace script {
namespace internal {
namespace typed {

// --------------------------------------------------------- instructions ---

const char* opName(Op op) noexcept
{
    switch (op) {
#define CS_OP(name)  \
    case Op::name: return #name
        CS_OP(Nop); CS_OP(LdNull); CS_OP(LdTrue); CS_OP(LdFalse); CS_OP(LdcI4);
        CS_OP(LdcI8); CS_OP(LdcR4); CS_OP(LdcR8); CS_OP(LdStr); CS_OP(Pop);
        CS_OP(Dup); CS_OP(Dup2); CS_OP(Swap); CS_OP(LdLoc); CS_OP(StLoc);
        CS_OP(NewBox); CS_OP(LdBoxed); CS_OP(StBoxed); CS_OP(LdBoxRef);
        CS_OP(LdInd); CS_OP(StInd); CS_OP(LdFld); CS_OP(StFld); CS_OP(LdSFld);
        CS_OP(StSFld); CS_OP(NewArr); CS_OP(LdElem); CS_OP(StElem); CS_OP(LdLen);
        CS_OP(NewObj); CS_OP(InitObj); CS_OP(CloneVal); CS_OP(Box); CS_OP(Unbox);
        CS_OP(CastClass); CS_OP(IsInst); CS_OP(AsInst); CS_OP(Call);
        CS_OP(CallVirt); CS_OP(CallIface); CS_OP(NewDlg); CS_OP(CallDlg);
        CS_OP(Ret); CS_OP(RetVoid); CS_OP(Br); CS_OP(BrTrue); CS_OP(BrFalse);
        CS_OP(Leave); CS_OP(EndFinally); CS_OP(Throw); CS_OP(Rethrow);
        CS_OP(ConvI1); CS_OP(ConvI2); CS_OP(ConvI4); CS_OP(ConvI8); CS_OP(ConvU1);
        CS_OP(ConvU2); CS_OP(ConvU4); CS_OP(ConvU8); CS_OP(ConvR4); CS_OP(ConvR8);
        CS_OP(ConvR8Un); CS_OP(ConvChar); CS_OP(ConvBool);
        CS_OP(AddI4); CS_OP(SubI4); CS_OP(MulI4); CS_OP(DivI4); CS_OP(DivU4);
        CS_OP(RemI4); CS_OP(RemU4); CS_OP(NegI4); CS_OP(AddI8); CS_OP(SubI8);
        CS_OP(MulI8); CS_OP(DivI8); CS_OP(DivU8); CS_OP(RemI8); CS_OP(RemU8);
        CS_OP(NegI8); CS_OP(AddR4); CS_OP(SubR4); CS_OP(MulR4); CS_OP(DivR4);
        CS_OP(RemR4); CS_OP(NegR4); CS_OP(AddR8); CS_OP(SubR8); CS_OP(MulR8);
        CS_OP(DivR8); CS_OP(RemR8); CS_OP(NegR8); CS_OP(AddI4Ovf); CS_OP(SubI4Ovf);
        CS_OP(MulI4Ovf); CS_OP(AddI8Ovf); CS_OP(SubI8Ovf); CS_OP(MulI8Ovf);
        CS_OP(AndI4); CS_OP(OrI4); CS_OP(XorI4); CS_OP(NotI4); CS_OP(ShlI4);
        CS_OP(ShrI4); CS_OP(ShrU4); CS_OP(AndI8); CS_OP(OrI8); CS_OP(XorI8);
        CS_OP(NotI8); CS_OP(ShlI8); CS_OP(ShrI8); CS_OP(ShrU8);
        CS_OP(CeqI4); CS_OP(CneI4); CS_OP(CltI4); CS_OP(CltU4); CS_OP(CleI4);
        CS_OP(CleU4); CS_OP(CgtI4); CS_OP(CgtU4); CS_OP(CgeI4); CS_OP(CgeU4);
        CS_OP(CeqI8); CS_OP(CneI8); CS_OP(CltI8); CS_OP(CltU8); CS_OP(CleI8);
        CS_OP(CleU8); CS_OP(CgtI8); CS_OP(CgtU8); CS_OP(CgeI8); CS_OP(CgeU8);
        CS_OP(CeqR8); CS_OP(CneR8); CS_OP(CltR8); CS_OP(CleR8); CS_OP(CgtR8);
        CS_OP(CgeR8); CS_OP(CeqRef); CS_OP(CneRef); CS_OP(CeqStr); CS_OP(CneStr);
        CS_OP(LogicalNot); CS_OP(IsNull); CS_OP(Concat); CS_OP(ToStr); CS_OP(ToStrU);
#undef CS_OP
        case Op::OpCount: break;
    }
    return "?";
}

int opArity(Op op) noexcept
{
    switch (op) {
        case Op::LdcI8:
        case Op::LdcR8:
        case Op::Call:
        case Op::CallVirt:
        case Op::CallIface:
        case Op::LdSFld:
        case Op::StSFld:
        case Op::NewObj: return 2;
        case Op::NewDlg: return 3;
        case Op::LdcI4:
        case Op::LdcR4:
        case Op::LdStr:
        case Op::LdLoc:
        case Op::StLoc:
        case Op::NewBox:
        case Op::LdBoxed:
        case Op::StBoxed:
        case Op::LdBoxRef:
        case Op::LdFld:
        case Op::StFld:
        case Op::NewArr:
        case Op::InitObj:
        case Op::Box:
        case Op::Unbox:
        case Op::CastClass:
        case Op::IsInst:
        case Op::AsInst:
        case Op::CallDlg:
        case Op::Br:
        case Op::BrTrue:
        case Op::BrFalse:
        case Op::Leave:
        case Op::Concat: return 1;
        default: return 0;
    }
}

/// Exact for every fixed-arity instruction. The variable ones -- calls,
/// `Concat`, `NewDlg` -- report their fixed part and the emitter adjusts,
/// which is also why the code generators refuse them.
int opStackDelta(Op op) noexcept
{
    switch (op) {
        case Op::LdNull:
        case Op::LdTrue:
        case Op::LdFalse:
        case Op::LdcI4:
        case Op::LdcI8:
        case Op::LdcR4:
        case Op::LdcR8:
        case Op::LdStr:
        case Op::LdLoc:
        case Op::LdBoxed:
        case Op::LdBoxRef:
        case Op::LdSFld:
        case Op::Dup:
        case Op::InitObj: return 1;
        case Op::Dup2: return 2;
        case Op::Pop:
        case Op::StLoc:
        case Op::StBoxed:
        case Op::StSFld:
        case Op::BrTrue:
        case Op::BrFalse:
        case Op::Throw:
        case Op::Ret:
        case Op::StInd:
        case Op::LdElem: return -1;
        case Op::StFld:
        case Op::StElem: return -2;

        // Binary operators consume two and produce one.
        case Op::AddI4:
        case Op::SubI4:
        case Op::MulI4:
        case Op::DivI4:
        case Op::DivU4:
        case Op::RemI4:
        case Op::RemU4:
        case Op::AddI8:
        case Op::SubI8:
        case Op::MulI8:
        case Op::DivI8:
        case Op::DivU8:
        case Op::RemI8:
        case Op::RemU8:
        case Op::AddR4:
        case Op::SubR4:
        case Op::MulR4:
        case Op::DivR4:
        case Op::RemR4:
        case Op::AddR8:
        case Op::SubR8:
        case Op::MulR8:
        case Op::DivR8:
        case Op::RemR8:
        case Op::AddI4Ovf:
        case Op::SubI4Ovf:
        case Op::MulI4Ovf:
        case Op::AddI8Ovf:
        case Op::SubI8Ovf:
        case Op::MulI8Ovf:
        case Op::AndI4:
        case Op::OrI4:
        case Op::XorI4:
        case Op::ShlI4:
        case Op::ShrI4:
        case Op::ShrU4:
        case Op::AndI8:
        case Op::OrI8:
        case Op::XorI8:
        case Op::ShlI8:
        case Op::ShrI8:
        case Op::ShrU8:
        case Op::CeqI4:
        case Op::CneI4:
        case Op::CltI4:
        case Op::CltU4:
        case Op::CleI4:
        case Op::CleU4:
        case Op::CgtI4:
        case Op::CgtU4:
        case Op::CgeI4:
        case Op::CgeU4:
        case Op::CeqI8:
        case Op::CneI8:
        case Op::CltI8:
        case Op::CltU8:
        case Op::CleI8:
        case Op::CleU8:
        case Op::CgtI8:
        case Op::CgtU8:
        case Op::CgeI8:
        case Op::CgeU8:
        case Op::CeqR8:
        case Op::CneR8:
        case Op::CltR8:
        case Op::CleR8:
        case Op::CgtR8:
        case Op::CgeR8:
        case Op::CeqRef:
        case Op::CneRef:
        case Op::CeqStr:
        case Op::CneStr: return -1;

        default: return 0;
    }
}

std::string disassemble(const MethodInfo& method)
{
    std::string out = method.qualifiedName() + ":\n";
    const std::vector<std::uint8_t>& code = method.body.code;
    std::size_t pc = 0;
    char buf[32];
    while (pc < code.size()) {
        std::snprintf(buf, sizeof buf, "%6zu  ", pc);
        out += buf;
        const Op op = Op(code[pc]);
        out += opName(op);
        const int arity = opArity(op);
        for (int i = 0; i < arity; ++i) {
            std::snprintf(buf, sizeof buf, " %u", readU32(&code[pc + 1 + std::size_t(i) * 4]));
            out += buf;
        }
        out += "\n";
        pc += 1 + std::size_t(arity) * 4;
    }
    return out;
}

// --------------------------------------------------------------- frames ---

void Frame::begin(const MethodInfo* target)
{
    method = target;
    pc = 0;
    line = 0;
    unwinding = false;
    exception = Value();
    stack.clear();
    pending.clear();
    const std::uint32_t size = target->frameSize();
    slots.assign(size, Value());
    for (std::uint32_t i = 0; i < size && i < target->body.localTypes.size(); ++i)
        slots[i] = defaultValue(target->body.localTypes[i]);
    if (stack.capacity() < target->body.maxStack)
        stack.reserve(target->body.maxStack);
}

void Frame::end() noexcept
{
    // Keep the buffers, drop everything they referenced.
    for (Value& slot : slots)
        slot = Value();
    stack.clear();
    pending.clear();
    exception = Value();
    method = nullptr;
}

Frame& Runtime::acquireFrame(std::size_t depth)
{
    while (framePool.size() <= depth)
        framePool.push_back(std::make_unique<Frame>());
    return *framePool[depth];
}

std::string Runtime::captureStack() const
{
    std::string out;
    for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
        const Frame* frame = *it;
        if (!frame || !frame->method)
            continue;
        out += "   at " + frame->method->qualifiedName();
        if (frame->line) {
            const std::string& file =
                frame->method->assembly && frame->method->assembly->sourceName
                    ? *frame->method->assembly->sourceName
                    : std::string("<assembly>");
            out += " (" + file + ":" + std::to_string(frame->line) + ")";
        }
        out += "\n";
    }
    return out;
}

// --------------------------------------------------------- error raising ---

Value makeExceptionValue(Runtime& rt, ErrorCode code, const std::string& message)
{
    return rt.makeException(code, fromUtf8(message));
}

void throwValue(Runtime& rt, const Value& exception)
{
    std::string message;
    ErrorCode code = ErrorCode::Exception;
    const TypeInfo* type = typeOf(rt, exception);
    if (type) {
        code = rt.types->errorCodeFor(type);
        if (const FieldInfo* field = type->findField(ascii("_message"))) {
            auto* instance = static_cast<CSInstance*>(detail::Access::ptr(exception));
            if (instance && field->slot < instance->count)
                message = instance->at(field->slot).toString();
        }
    }
    throw Exception(code, message.empty() ? std::string("Exception of type '") +
                                                (type ? toUtf8(type->fullName) : "?") +
                                                "' was thrown."
                                          : message,
                    exception, rt.captureStack());
}

void throwError(Runtime& rt, ErrorCode code, const std::string& message)
{
    // The code is kept as raised rather than re-derived from the exception
    // object, because several of them -- `Interrupted` above all -- have no
    // exception class of their own.
    throw Exception(code, message, rt.makeException(code, fromUtf8(message)), rt.captureStack());
}

void rethrowAsScript(Runtime& rt)
{
    try {
        throw;
    } catch (const Exception&) {
        throw;
    } catch (const Error& e) {
        throwError(rt, e.code(), e.what());
    } catch (const std::exception& e) {
        throwError(rt, ErrorCode::Exception, e.what());
    } catch (...) {
        throwError(rt, ErrorCode::Exception, "a native call failed");
    }
}

// ------------------------------------------------------- abstract ops ---

Value defaultValue(const TypeInfo* type) noexcept
{
    if (!type)
        return Value();
    switch (type->kind) {
        case TypeKind::Bool: return Value(false);
        case TypeKind::Char: return Value(char16_t(0));
        case TypeKind::SByte:
        case TypeKind::Byte:
        case TypeKind::Short:
        case TypeKind::UShort:
        case TypeKind::Int:
        case TypeKind::UInt:
        case TypeKind::Enum: return Value(std::int32_t(0));
        case TypeKind::Long:
        case TypeKind::ULong: return Value(std::int64_t(0));
        case TypeKind::Float: return Value(0.0f);
        case TypeKind::Double: return Value(0.0);
        default: return Value();
    }
}

const TypeInfo* typeOf(Runtime& rt, const Value& v) noexcept
{
    const CoreTypes& core = rt.types->core();
    switch (v.type()) {
        case Type::Null: return nullptr;
        case Type::Bool: return core.boolType;
        case Type::Char: return core.charType;
        case Type::Int: return core.intType;
        case Type::Long: return core.longType;
        case Type::Float: return core.floatType;
        case Type::Double: return core.doubleType;
        case Type::String: return core.stringType;
        default: break;
    }
    GCObject* cell = detail::Access::ptr(v);
    if (cell && cell->kind == CellKind::Array) {
        return nullptr;  // arrays compare by element type; see isInstanceOf
    }
    if (cell && cell->kind == CellKind::Native) {
        // A wrapper made before this engine existed learns its class now.
        auto* native = static_cast<NativeCell*>(cell);
        if (!native->typedType) {
            rt.syncHostClasses();
            const auto it = rt.nativeClasses.find(native->slot.typeKey);
            if (it != rt.nativeClasses.end())
                native->typedType = it->second;
        }
        return static_cast<const TypeInfo*>(native->typedType);
    }
    return typeOfCell(cell);
}

bool isInstanceOf(Runtime& rt, const Value& v, const TypeInfo* type) noexcept
{
    if (v.isNull() || !type)
        return false;
    if (type->kind == TypeKind::Object)
        return true;
    if (v.isArray()) {
        auto* array = static_cast<CSArray*>(detail::Access::ptr(v));
        return type->kind == TypeKind::Array && array && array->elemType == type->element;
    }
    const TypeInfo* actual = typeOf(rt, v);
    if (!actual)
        return false;
    if (type->kind == TypeKind::Interface)
        return actual->implements(type);
    return actual->derivesFrom(type);
}

Value castTo(Runtime& rt, const Value& v, const TypeInfo* type)
{
    if (v.isNull() || isInstanceOf(rt, v, type))
        return v;
    throwError(rt, ErrorCode::InvalidCast,
               "unable to cast an object of type '" + rt.types->describe(typeOf(rt, v)) +
                   "' to '" + rt.types->describe(type) + "'");
}

Value cloneValue(Runtime& rt, const Value& v)
{
    if (!v.isObject())
        return v;
    GCObject* cell = detail::Access::ptr(v);
    if (!cell || cell->kind != CellKind::Instance)
        return v;
    auto* source = static_cast<CSInstance*>(cell);
    CSInstance* copy = rt.newInstance(source->type);
    for (std::uint32_t i = 0; i < source->count && i < copy->count; ++i)
        copy->at(i) = source->at(i);
    return detail::Access::adopt(Type::Object, copy);
}

String toStringValue(Runtime& rt, const Value& v)
{
    switch (v.type()) {
        case Type::Null: return String();
        case Type::Bool: return ascii(detail::Access::boolean(v) ? "True" : "False");
        case Type::Char: return String(1, Char(detail::Access::i4(v)));
        case Type::Int: return longToString(detail::Access::i4(v));
        case Type::Long: return longToString(detail::Access::i8(v));
        case Type::Float: return floatToString(detail::Access::r4(v));
        case Type::Double: return doubleToString(detail::Access::r8(v));
        case Type::String: {
            auto* text = static_cast<StringCell*>(detail::Access::ptr(v));
            return text ? String(text->text()) : String();
        }
        default: break;
    }

    // A user `ToString` override wins; otherwise the type name, like .NET.
    const TypeInfo* type = typeOf(rt, v);
    if (v.isArray())
        return ascii("System.Array");
    if (type) {
        std::vector<MethodInfo*> methods;
        type->collectMethods(ascii("ToString"), methods);
        for (MethodInfo* method : methods) {
            if (!method->params.empty() || method->isStatic())
                continue;
            if (method->owner && method->owner->kind == TypeKind::Object)
                break;
            Value self = v;
            Value result = invoke(rt, resolveVirtual(rt, v, method), &self, 1);
            return result.isNull() ? String() : toStringValue(rt, result);
        }
        return type->fullName;
    }
    return ascii("System.Object");
}

Value concatStrings(Runtime& rt, const Value* parts, std::size_t count)
{
    const auto textOf = [](const Value& v) {
        auto* cell = static_cast<StringCell*>(detail::Access::ptr(v));
        return cell ? cell->text() : StringView();
    };
    if (count == 2 && parts[0].isString() && parts[1].isString())
        return rt.ctx.concat(textOf(parts[0]), textOf(parts[1]));
    String out;
    std::size_t total = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (parts[i].isString()) {
            auto* text = static_cast<StringCell*>(detail::Access::ptr(parts[i]));
            total += text ? text->text().size() : 0;
        } else {
            total += 8;
        }
    }
    out.reserve(total);
    for (std::size_t i = 0; i < count; ++i)
        out += toStringValue(rt, parts[i]);
    return rt.str(out);
}

bool objectEquals(const Value& a, const Value& b) noexcept
{
    return valueEquals(a, b);
}

// ------------------------------------------------------------- objects ---

Value newInstanceOf(Runtime& rt, const TypeInfo* type)
{
    return detail::Access::adopt(Type::Object, rt.newInstance(type));
}

Value newArrayOf(Runtime& rt, const TypeInfo* elementType, std::int32_t length)
{
    if (length < 0)
        throwError(rt, ErrorCode::ArgumentOutOfRange, "the array length cannot be negative");
    return detail::Access::adopt(Type::Array,
                                 rt.newArray(elementType, std::uint32_t(length)));
}

Value getElement(Runtime& rt, const Value& array, std::int32_t index)
{
    auto* cell = static_cast<CSArray*>(detail::Access::ptr(array));
    if (!cell || !array.isArray())
        throwError(rt, ErrorCode::NullReference, "the array reference is null");
    if (index < 0 || std::uint32_t(index) >= cell->length)
        throwError(rt, ErrorCode::IndexOutOfRange,
                   "index " + std::to_string(index) + " is outside the bounds of the array");
    return cell->get(std::uint32_t(index));
}

void setElement(Runtime& rt, const Value& array, std::int32_t index, const Value& v)
{
    auto* cell = static_cast<CSArray*>(detail::Access::ptr(array));
    if (!cell || !array.isArray())
        throwError(rt, ErrorCode::NullReference, "the array reference is null");
    if (index < 0 || std::uint32_t(index) >= cell->length)
        throwError(rt, ErrorCode::IndexOutOfRange,
                   "index " + std::to_string(index) + " is outside the bounds of the array");
    cell->set(std::uint32_t(index), v);
}

// --------------------------------------------------------------- calls ---

const MethodInfo* resolveVirtual(Runtime& rt, const Value& receiver,
                                 const MethodInfo* declared)
{
    if (!declared || declared->vtableSlot < 0)
        return declared;
    const TypeInfo* type = typeOf(rt, receiver);
    if (!type || std::size_t(declared->vtableSlot) >= type->vtable.size())
        return declared;
    return type->vtable[std::size_t(declared->vtableSlot)];
}

const MethodInfo* resolveInterface(Runtime& rt, const Value& receiver,
                                   const MethodInfo* declared)
{
    const TypeInfo* type = typeOf(rt, receiver);
    if (!type || !declared)
        return declared;

    // (concrete type, interface method) never changes, so one lookup per pair
    // is enough for the life of the context.
    const std::uint64_t key = (std::uint64_t(reinterpret_cast<std::uintptr_t>(type)) >> 3) ^
                              (std::uint64_t(reinterpret_cast<std::uintptr_t>(declared)) << 17);
    const auto cached = rt.ifaceCache.find(key);
    if (cached != rt.ifaceCache.end())
        return cached->second;

    MethodInfo* found = nullptr;
    for (const TypeInfo* t = type; t && !found; t = t->base) {
        for (MethodInfo* candidate : t->methods) {
            if (candidate->name != declared->name ||
                candidate->params.size() != declared->params.size())
                continue;
            bool same = true;
            for (std::size_t i = 0; i < candidate->params.size(); ++i)
                same = same && candidate->params[i].type == declared->params[i].type;
            if (same) {
                found = candidate;
                break;
            }
        }
    }
    if (!found)
        return declared;
    rt.ifaceCache.emplace(key, found);
    return found;
}

namespace {

Value runFrame(Runtime& rt, Frame& frame);

Value invokeNative(Runtime& rt, const MethodInfo* method, Value* args, std::uint32_t argc)
{
    const bool instance = !method->isStatic();
    Value self = instance && argc > 0 ? args[0] : Value();
    ContextImpl::ArgBuffer buffer(rt.ctx);
    std::vector<Value>& rest = buffer.args;
    const std::uint32_t first = instance ? 1u : 0u;
    for (std::uint32_t i = first; i < argc; ++i)
        rest.push_back(args[i]);

    CallInfo info = detail::Access::makeCallInfo(rt.ctx.owner, &self, &rest, false);
    ContextImpl::ActiveEngine active(rt.ctx, EngineKind::Typed);
    try {
        return method->native(info);
    } catch (const Exception&) {
        throw;
    } catch (...) {
        rethrowAsScript(rt);
    }
}

}  // namespace

Value invoke(Runtime& rt, const MethodInfo* method, Value* args, std::uint32_t argc)
{
    if (!method)
        throwError(rt, ErrorCode::NullReference, "the method reference is null");
    if (method->isAbstract())
        throwError(rt, ErrorCode::InvalidOperation,
                   "'" + method->qualifiedName() + "' is abstract");
    if (method->isNative()) {
        if (!method->native)
            throwError(rt, ErrorCode::NotSupported,
                       "'" + method->qualifiedName() + "' has no implementation");
        return invokeNative(rt, method, args, argc);
    }
    if (rt.ctx.callDepth >= rt.ctx.options.stackLimit || nativeStackLow())
        throwError(rt, ErrorCode::StackOverflow, "the call stack went too deep");

    // Already compiled? Then the frame is all the native code needs.
    MethodInfo& mutableMethod = const_cast<MethodInfo&>(*method);
    if (NativeCode* code = mutableMethod.code.load(std::memory_order_acquire)) {
        Frame& native = rt.acquireFrame(rt.ctx.callDepth);
        native.begin(method);
        for (std::uint32_t i = 0; i < argc && i < native.slots.size(); ++i)
            native.slots[i] = args[i];
        ++rt.ctx.callDepth;
        rt.frames.push_back(&native);

        Value result;
        rt.nativeError = 0;
        code->entry(&rt, method, native.slots.data(), argc, &result);

        rt.frames.pop_back();
        --rt.ctx.callDepth;
        native.end();
        // Generated code cannot throw; it leaves word here instead.
        if (rt.nativeError != 0) {
            const ErrorCode raised = ErrorCode(rt.nativeError);
            rt.nativeError = 0;
            throwError(rt, raised,
                       raised == ErrorCode::DivideByZero ? "attempted to divide by zero"
                                                         : "execution was interrupted");
        }
        return result;
    }

    Frame& frame = rt.acquireFrame(rt.ctx.callDepth);
    frame.begin(method);
    for (std::uint32_t i = 0; i < argc && i < frame.slots.size(); ++i)
        frame.slots[i] = args[i];

    ++rt.ctx.callDepth;
    rt.frames.push_back(&frame);
    Value result;
    try {
        result = runFrame(rt, frame);
    } catch (...) {
        rt.frames.pop_back();
        --rt.ctx.callDepth;
        frame.end();
        throw;
    }
    rt.frames.pop_back();
    --rt.ctx.callDepth;
    frame.end();

    // Tiering: the counter is bumped here rather than in the loop so that the
    // decision costs one compare on a path that already returned.
    if (rt.jit && rt.jit->enabled() &&
        mutableMethod.hotness.load(std::memory_order_relaxed) != MethodInfo::kNeverCompile) {
        const std::uint32_t hot =
            mutableMethod.hotness.fetch_add(1, std::memory_order_relaxed) + 1;
        if (hot == rt.jit->threshold())
            rt.jit->onHot(rt, mutableMethod);
    }
    return result;
}

Value execute(Runtime& rt, const MethodInfo& method, Value* frameSlots, std::uint32_t argc)
{
    Frame& frame = rt.acquireFrame(rt.ctx.callDepth);
    frame.begin(&method);
    for (std::uint32_t i = 0; i < argc && i < frame.slots.size(); ++i)
        frame.slots[i] = frameSlots[i];
    ++rt.ctx.callDepth;
    rt.frames.push_back(&frame);
    Value result;
    try {
        result = runFrame(rt, frame);
    } catch (...) {
        rt.frames.pop_back();
        --rt.ctx.callDepth;
        frame.end();
        throw;
    }
    rt.frames.pop_back();
    --rt.ctx.callDepth;
    frame.end();
    return result;
}

Value construct(Runtime& rt, const MethodInfo* ctor, Value* args, std::uint32_t argc)
{
    const TypeInfo* type = ctor ? ctor->owner : nullptr;

    // A type the engine implements -- `List<T>`, `StringBuilder`, a host class
    // -- allocates its own cell, so its constructor returns the instance
    // instead of being handed one.
    if (type && type->nativeKind != NativeKind::None) {
        ContextImpl::ArgBuffer buffer(rt.ctx);
        std::vector<Value>& call = buffer.args;
        call.emplace_back();
        for (std::uint32_t i = 0; i < argc; ++i)
            call.push_back(args[i]);
        return invoke(rt, ctor, call.data(), std::uint32_t(call.size()));
    }

    Value instance = newInstanceOf(rt, type);
    ContextImpl::ArgBuffer buffer(rt.ctx);
    std::vector<Value>& call = buffer.args;
    call.push_back(instance);
    for (std::uint32_t i = 0; i < argc; ++i)
        call.push_back(args[i]);
    invoke(rt, ctor, call.data(), std::uint32_t(call.size()));
    return instance;
}

Value invokeDelegate(Runtime& rt, const Value& delegateValue, Value* args,
                     std::uint32_t argc)
{
    auto* target = static_cast<CSDelegate*>(detail::Access::ptr(delegateValue));
    if (!target || delegateValue.type() != Type::Delegate)
        throwError(rt, ErrorCode::NullReference, "the delegate is null");

    Value result;
    for (CSDelegate* link = target; link; link = link->next.get()) {
        const MethodInfo* method = link->method;
        if (!method)
            continue;
        ContextImpl::ArgBuffer buffer(rt.ctx);
        std::vector<Value>& call = buffer.args;
        if (!method->isStatic())
            call.push_back(link->target);
        for (std::uint32_t i = 0; i < argc; ++i)
            call.push_back(args[i]);
        // The captured boxes go into the slots the compiler set aside.
        const std::uint32_t base = std::uint32_t(call.size());
        if (!method->captureSlots.empty()) {
            call.resize(method->frameSize());
            for (std::size_t i = 0;
                 i < method->captureSlots.size() && i < link->captured.size(); ++i) {
                const std::uint32_t slot = method->captureSlots[i];
                if (slot < call.size())
                    call[slot] = detail::Access::make(Type::Object, link->captured[i].get());
            }
        }
        result = invoke(rt, method, call.data(),
                        method->captureSlots.empty() ? base
                                                     : std::uint32_t(call.size()));
    }
    return result;
}

// ------------------------------------------------------ the interpreter ---

namespace {

bool covers(const Handler& handler, std::uint32_t pc) noexcept
{
    return pc >= handler.start && pc < handler.end;
}

/// Find the handler for an exception raised at `frame.pc`. Returns false when
/// this method has none and the exception has to leave it.
bool dispatch(Runtime& rt, Frame& frame, const Value& exception)
{
    const std::uint32_t pc = frame.opPc;
    for (const Handler& handler : frame.method->body.handlers) {
        if (!covers(handler, pc))
            continue;
        frame.stack.resize(handler.stackDepth);
        if (handler.kind == HandlerKind::Catch) {
            if (handler.catchType && !isInstanceOf(rt, exception, handler.catchType))
                continue;
            if (handler.exceptionSlot < frame.slots.size())
                frame.slots[handler.exceptionSlot] = exception;
            frame.exception = exception;
            frame.unwinding = false;
            frame.pc = handler.target;
            return true;
        }
        frame.exception = exception;
        frame.unwinding = true;
        frame.pc = handler.target;
        return true;
    }
    return false;
}

/// `Leave`: run the finally blocks between here and there, then jump.
void leave(Frame& frame, std::uint32_t target)
{
    std::vector<std::uint32_t> chain;
    for (const Handler& handler : frame.method->body.handlers) {
        if (handler.kind != HandlerKind::Finally)
            continue;
        if (covers(handler, frame.pc) && !covers(handler, target))
            chain.push_back(handler.target);
    }
    if (chain.empty()) {
        frame.pc = target;
        return;
    }
    frame.pending.push_back(target);
    for (std::size_t i = chain.size(); i-- > 1;)
        frame.pending.push_back(chain[i]);
    frame.pc = chain.front();
}

Value runFrame(Runtime& rt, Frame& frame)
{
    const MethodInfo& method = *frame.method;
    const std::uint8_t* const code = method.body.code.data();
    const std::size_t codeSize = method.body.code.size();
    Vec<Value>& stack = frame.stack;
    Vec<Value>& slots = frame.slots;
    AssemblyImpl* assembly = method.assembly;

    std::size_t lineIndex = 0;
    std::uint32_t ticks = 0;

    for (;;) {
        try {
            while (frame.pc < codeSize) {
                const std::uint32_t pc = frame.pc;
                const Op op = Op(code[pc]);
                const std::uint8_t* const operands = code + pc + 1;
                frame.opPc = pc;
                frame.pc = pc + 1 + std::uint32_t(opArity(op)) * 4;

                // Line tracking is for stack traces; it costs one compare.
                while (lineIndex < method.body.lines.size() &&
                       method.body.lines[lineIndex].offset <= pc)
                    frame.line = method.body.lines[lineIndex++].line;

                switch (op) {
                    case Op::Nop: break;

                    // ---- constants ----
                    case Op::LdNull: stack.emplace_back(); break;
                    case Op::LdTrue: stack.emplace_back(true); break;
                    case Op::LdFalse: stack.emplace_back(false); break;
                    case Op::LdcI4:
                        stack.emplace_back(std::int32_t(readU32(operands)));
                        break;
                    case Op::LdcI8: {
                        const std::uint64_t bits =
                            std::uint64_t(readU32(operands)) |
                            (std::uint64_t(readU32(operands + 4)) << 32);
                        stack.emplace_back(std::int64_t(bits));
                        break;
                    }
                    case Op::LdcR4: {
                        const std::uint32_t bits = readU32(operands);
                        float value = 0;
                        std::memcpy(&value, &bits, sizeof value);
                        stack.emplace_back(value);
                        break;
                    }
                    case Op::LdcR8: {
                        const std::uint64_t bits =
                            std::uint64_t(readU32(operands)) |
                            (std::uint64_t(readU32(operands + 4)) << 32);
                        double value = 0;
                        std::memcpy(&value, &bits, sizeof value);
                        stack.emplace_back(value);
                        break;
                    }
                    case Op::LdStr: {
                        const std::uint32_t index = readU32(operands);
                        stack.push_back(index < assembly->strings.size()
                                            ? assembly->strings[index]
                                            : Value());
                        break;
                    }

                    // ---- stack ----
                    case Op::Pop: stack.pop_back(); break;
                    case Op::Dup: stack.push_back(stack.back()); break;
                    case Op::Dup2: {
                        const std::size_t n = stack.size();
                        stack.push_back(stack[n - 2]);
                        stack.push_back(stack[n - 1]);
                        break;
                    }
                    case Op::Swap: std::swap(stack[stack.size() - 1], stack[stack.size() - 2]);
                        break;

                    // ---- locals ----
                    case Op::LdLoc: stack.push_back(slots[readU32(operands)]); break;
                    case Op::StLoc:
                        slots[readU32(operands)] = std::move(stack.back());
                        stack.pop_back();
                        break;
                    case Op::NewBox: {
                        const std::uint32_t slot = readU32(operands);
                        Box* box = rt.newBox(slots[slot]);
                        slots[slot] = detail::Access::adopt(Type::Object, box);
                        break;
                    }
                    case Op::LdBoxed: {
                        auto* box = static_cast<Box*>(
                            detail::Access::ptr(slots[readU32(operands)]));
                        stack.push_back(box ? box->value : Value());
                        break;
                    }
                    case Op::StBoxed: {
                        auto* box = static_cast<Box*>(
                            detail::Access::ptr(slots[readU32(operands)]));
                        if (box)
                            box->value = std::move(stack.back());
                        stack.pop_back();
                        break;
                    }
                    case Op::LdBoxRef: stack.push_back(slots[readU32(operands)]); break;
                    case Op::LdInd: {
                        auto* box = static_cast<Box*>(detail::Access::ptr(stack.back()));
                        stack.back() = box ? box->value : Value();
                        break;
                    }
                    case Op::StInd: {
                        Value value = std::move(stack.back());
                        stack.pop_back();
                        auto* box = static_cast<Box*>(detail::Access::ptr(stack.back()));
                        stack.pop_back();
                        if (box)
                            box->value = std::move(value);
                        break;
                    }

                    // ---- fields ----
                    case Op::LdFld: {
                        auto* instance =
                            static_cast<CSInstance*>(detail::Access::ptr(stack.back()));
                        if (!instance)
                            throwError(rt, ErrorCode::NullReference,
                                       "object reference not set to an instance of an object");
                        stack.back() = instance->at(readU32(operands));
                        break;
                    }
                    case Op::StFld: {
                        Value value = std::move(stack.back());
                        stack.pop_back();
                        auto* instance =
                            static_cast<CSInstance*>(detail::Access::ptr(stack.back()));
                        stack.pop_back();
                        if (!instance)
                            throwError(rt, ErrorCode::NullReference,
                                       "object reference not set to an instance of an object");
                        instance->at(readU32(operands)) = std::move(value);
                        break;
                    }
                    case Op::LdSFld: {
                        const TypeInfo* owner = assembly->typeRefs[readU32(operands)];
                        AssemblyImpl* home = owner && owner->assembly ? owner->assembly : assembly;
                        stack.push_back(home->statics[readU32(operands + 4)]);
                        break;
                    }
                    case Op::StSFld: {
                        const TypeInfo* owner = assembly->typeRefs[readU32(operands)];
                        AssemblyImpl* home = owner && owner->assembly ? owner->assembly : assembly;
                        home->statics[readU32(operands + 4)] = std::move(stack.back());
                        stack.pop_back();
                        break;
                    }

                    // ---- arrays ----
                    case Op::NewArr: {
                        const TypeInfo* element = assembly->typeRefs[readU32(operands)];
                        const std::int32_t length = stack.back().toInt();
                        stack.back() = newArrayOf(rt, element, length);
                        break;
                    }
                    case Op::LdElem: {
                        const std::int32_t index = stack.back().toInt();
                        stack.pop_back();
                        stack.back() = getElement(rt, stack.back(), index);
                        break;
                    }
                    case Op::StElem: {
                        Value value = std::move(stack.back());
                        stack.pop_back();
                        const std::int32_t index = stack.back().toInt();
                        stack.pop_back();
                        setElement(rt, stack.back(), index, value);
                        stack.pop_back();
                        break;
                    }
                    case Op::LdLen: {
                        auto* array = static_cast<CSArray*>(detail::Access::ptr(stack.back()));
                        if (!array)
                            throwError(rt, ErrorCode::NullReference,
                                       "object reference not set to an instance of an object");
                        stack.back() = Value(std::int32_t(array->length));
                        break;
                    }

                    // ---- objects ----
                    case Op::InitObj: {
                        const TypeInfo* type = assembly->typeRefs[readU32(operands)];
                        stack.push_back(type && type->kind == TypeKind::Struct
                                            ? newInstanceOf(rt, type)
                                            : defaultValue(type));
                        break;
                    }
                    case Op::CloneVal: stack.back() = cloneValue(rt, stack.back()); break;
                    case Op::Box: break;
                    case Op::Unbox: {
                        const TypeInfo* type = assembly->typeRefs[readU32(operands)];
                        if (type && isValueKind(type->kind)) {
                            if (!stack.back().isNumber() && !stack.back().isBool() &&
                                !stack.back().isChar())
                                throwError(rt, ErrorCode::InvalidCast,
                                           "the boxed value is not a '" +
                                               rt.types->describe(type) + "'");
                        } else {
                            stack.back() = castTo(rt, stack.back(), type);
                        }
                        break;
                    }
                    case Op::CastClass:
                        stack.back() =
                            castTo(rt, stack.back(), assembly->typeRefs[readU32(operands)]);
                        break;
                    case Op::IsInst:
                        stack.back() = Value(
                            isInstanceOf(rt, stack.back(), assembly->typeRefs[readU32(operands)]));
                        break;
                    case Op::AsInst:
                        if (!isInstanceOf(rt, stack.back(),
                                          assembly->typeRefs[readU32(operands)]))
                            stack.back() = Value();
                        break;

                    // ---- calls ----
                    case Op::Call:
                    case Op::CallVirt:
                    case Op::CallIface: {
                        const MethodInfo* target = assembly->methodRefs[readU32(operands)];
                        const std::uint32_t argc = readU32(operands + 4);
                        Value* args = stack.data() + stack.size() - argc;
                        if (op == Op::CallVirt)
                            target = resolveVirtual(rt, args[0], target);
                        else if (op == Op::CallIface)
                            target = resolveInterface(rt, args[0], target);
                        if (!target->isStatic() && args[0].isNull())
                            throwError(rt, ErrorCode::NullReference,
                                       "object reference not set to an instance of an object");
                        Value result = invoke(rt, target, args, argc);
                        stack.resize(stack.size() - argc);
                        if (target->returnType && target->returnType->kind != TypeKind::Void)
                            stack.push_back(std::move(result));
                        break;
                    }
                    case Op::NewObj: {
                        const MethodInfo* ctor = assembly->methodRefs[readU32(operands)];
                        const std::uint32_t argc = readU32(operands + 4);
                        Value* args = stack.data() + stack.size() - argc;
                        Value result = construct(rt, ctor, args, argc);
                        stack.resize(stack.size() - argc);
                        stack.push_back(std::move(result));
                        break;
                    }
                    case Op::NewDlg: {
                        const TypeInfo* type = assembly->typeRefs[readU32(operands)];
                        const MethodInfo* target = assembly->methodRefs[readU32(operands + 4)];
                        const std::uint32_t captures = readU32(operands + 8);
                        CSDelegate* delegateCell = rt.newCell<CSDelegate>();
                        Value result = detail::Access::adopt(Type::Delegate, delegateCell);
                        delegateCell->type = type;
                        delegateCell->method = target;
                        for (std::uint32_t i = 0; i < captures; ++i) {
                            Value& box = stack[stack.size() - captures + i];
                            delegateCell->captured.emplace_back(
                                static_cast<Box*>(detail::Access::ptr(box)));
                        }
                        stack.resize(stack.size() - captures);
                        delegateCell->target = std::move(stack.back());
                        stack.back() = std::move(result);
                        break;
                    }
                    case Op::CallDlg: {
                        const std::uint32_t argc = readU32(operands);
                        Value* args = stack.data() + stack.size() - argc;
                        Value target = stack[stack.size() - argc - 1];
                        if (target.isNull())
                            throwError(rt, ErrorCode::NullReference, "the delegate is null");
                        Value result = invokeDelegate(rt, target, args, argc);
                        stack.resize(stack.size() - argc - 1);
                        const MethodInfo* invokeInfo =
                            target.type() == Type::Delegate
                                ? static_cast<CSDelegate*>(detail::Access::ptr(target))->type
                                      ? static_cast<CSDelegate*>(detail::Access::ptr(target))
                                            ->type->invoke
                                      : nullptr
                                : nullptr;
                        if (!invokeInfo || !invokeInfo->returnType ||
                            invokeInfo->returnType->kind != TypeKind::Void)
                            stack.push_back(std::move(result));
                        break;
                    }
                    case Op::Ret: {
                        Value result = std::move(stack.back());
                        stack.pop_back();
                        return result;
                    }
                    case Op::RetVoid: return Value();

                    // ---- control ----
                    case Op::Br: frame.pc = readU32(operands); break;
                    case Op::BrTrue: {
                        const bool taken = stack.back().toBool();
                        stack.pop_back();
                        if (taken)
                            frame.pc = readU32(operands);
                        break;
                    }
                    case Op::BrFalse: {
                        const bool taken = stack.back().toBool();
                        stack.pop_back();
                        if (!taken)
                            frame.pc = readU32(operands);
                        break;
                    }
                    case Op::Leave:
                        frame.pc = pc;  // `leave` looks at where it came from
                        leave(frame, readU32(operands));
                        break;
                    case Op::EndFinally:
                        if (frame.unwinding) {
                            frame.unwinding = false;
                            Value pending = frame.exception;
                            frame.exception = Value();
                            throwValue(rt, pending);
                        }
                        if (!frame.pending.empty()) {
                            frame.pc = frame.pending.back();
                            frame.pending.pop_back();
                        }
                        break;
                    case Op::Throw: {
                        Value thrown = std::move(stack.back());
                        stack.pop_back();
                        frame.pc = pc;
                        if (thrown.isNull())
                            throwError(rt, ErrorCode::NullReference, "the thrown value is null");
                        throwValue(rt, thrown);
                    }
                    case Op::Rethrow:
                        frame.pc = pc;
                        throwValue(rt, frame.exception);

                    // ---- conversions ----
                    case Op::ConvI1:
                        stack.back() = Value(std::int32_t(std::int8_t(stack.back().toInt())));
                        break;
                    case Op::ConvU1:
                        stack.back() = Value(std::int32_t(std::uint8_t(stack.back().toInt())));
                        break;
                    case Op::ConvI2:
                        stack.back() = Value(std::int32_t(std::int16_t(stack.back().toInt())));
                        break;
                    case Op::ConvU2:
                        stack.back() = Value(std::int32_t(std::uint16_t(stack.back().toInt())));
                        break;
                    case Op::ConvChar:
                        stack.back() = Value(char16_t(stack.back().toInt()));
                        break;
                    case Op::ConvI4:
                    case Op::ConvU4:
                        stack.back() = Value(std::int32_t(stack.back().toLong()));
                        break;
                    case Op::ConvI8:
                        stack.back() = Value(stack.back().toLong());
                        break;
                    case Op::ConvU8: {
                        const Value& v = stack.back();
                        std::int64_t widened = v.toLong();
                        if (v.isInt())
                            widened = std::int64_t(std::uint32_t(detail::Access::i4(v)));
                        stack.back() = Value(widened);
                        break;
                    }
                    case Op::ConvR4:
                        stack.back() = Value(float(stack.back().toDouble()));
                        break;
                    case Op::ConvR8: stack.back() = Value(stack.back().toDouble()); break;
                    case Op::ConvR8Un: {
                        // A uint is held as its 32 bits in an int slot. Widening it through toLong() sign-extends
                        // it first, so (double)(uint)-1 came out as 1.8e19 instead of 4294967295.
                        const Value& v = stack.back();
                        const double widened = v.isInt() ? double(std::uint32_t(detail::Access::i4(v)))
                                                         : double(std::uint64_t(v.toLong()));
                        stack.back() = Value(widened);
                        break;
                    }
                    case Op::ConvBool: stack.back() = Value(stack.back().toBool()); break;

                    // ---- arithmetic ----
#define CS_BINARY_I4(name, expr)                                        \
    case Op::name: {                                                    \
        const std::int32_t b = stack.back().toInt();                    \
        stack.pop_back();                                               \
        const std::int32_t a = stack.back().toInt();                    \
        (void)a;                                                        \
        stack.back() = Value(expr);                                     \
        break;                                                          \
    }
#define CS_BINARY_I8(name, expr)                                        \
    case Op::name: {                                                    \
        const std::int64_t b = stack.back().toLong();                   \
        stack.pop_back();                                               \
        const std::int64_t a = stack.back().toLong();                   \
        (void)a;                                                        \
        stack.back() = Value(expr);                                     \
        break;                                                          \
    }
#define CS_BINARY_R8(name, expr)                                        \
    case Op::name: {                                                    \
        const double b = stack.back().toDouble();                       \
        stack.pop_back();                                               \
        const double a = stack.back().toDouble();                       \
        (void)a;                                                        \
        stack.back() = Value(expr);                                     \
        break;                                                          \
    }
#define CS_BINARY_R4(name, expr)                                        \
    case Op::name: {                                                    \
        const float b = float(stack.back().toDouble());                 \
        stack.pop_back();                                               \
        const float a = float(stack.back().toDouble());                 \
        (void)a;                                                        \
        stack.back() = Value(expr);                                     \
        break;                                                          \
    }

                    CS_BINARY_I4(AddI4, std::int32_t(std::uint32_t(a) + std::uint32_t(b)))
                    CS_BINARY_I4(SubI4, std::int32_t(std::uint32_t(a) - std::uint32_t(b)))
                    CS_BINARY_I4(MulI4, std::int32_t(std::uint32_t(a) * std::uint32_t(b)))
                    CS_BINARY_I4(AndI4, a & b)
                    CS_BINARY_I4(OrI4, a | b)
                    CS_BINARY_I4(XorI4, a ^ b)
                    CS_BINARY_I4(ShlI4, std::int32_t(std::uint32_t(a) << (b & 31)))
                    CS_BINARY_I4(ShrI4, std::int32_t(a >> (b & 31)))
                    CS_BINARY_I4(ShrU4, std::int32_t(std::uint32_t(a) >> (b & 31)))
                    CS_BINARY_I4(CeqI4, a == b)
                    CS_BINARY_I4(CneI4, a != b)
                    CS_BINARY_I4(CltI4, a < b)
                    CS_BINARY_I4(CleI4, a <= b)
                    CS_BINARY_I4(CgtI4, a > b)
                    CS_BINARY_I4(CgeI4, a >= b)
                    CS_BINARY_I4(CltU4, std::uint32_t(a) < std::uint32_t(b))
                    CS_BINARY_I4(CleU4, std::uint32_t(a) <= std::uint32_t(b))
                    CS_BINARY_I4(CgtU4, std::uint32_t(a) > std::uint32_t(b))
                    CS_BINARY_I4(CgeU4, std::uint32_t(a) >= std::uint32_t(b))

                    CS_BINARY_I8(AddI8, std::int64_t(std::uint64_t(a) + std::uint64_t(b)))
                    CS_BINARY_I8(SubI8, std::int64_t(std::uint64_t(a) - std::uint64_t(b)))
                    CS_BINARY_I8(MulI8, std::int64_t(std::uint64_t(a) * std::uint64_t(b)))
                    CS_BINARY_I8(AndI8, a & b)
                    CS_BINARY_I8(OrI8, a | b)
                    CS_BINARY_I8(XorI8, a ^ b)
                    CS_BINARY_I8(ShlI8, std::int64_t(std::uint64_t(a) << (b & 63)))
                    CS_BINARY_I8(ShrI8, a >> (b & 63))
                    CS_BINARY_I8(ShrU8, std::int64_t(std::uint64_t(a) >> (b & 63)))
                    CS_BINARY_I8(CeqI8, a == b)
                    CS_BINARY_I8(CneI8, a != b)
                    CS_BINARY_I8(CltI8, a < b)
                    CS_BINARY_I8(CleI8, a <= b)
                    CS_BINARY_I8(CgtI8, a > b)
                    CS_BINARY_I8(CgeI8, a >= b)
                    CS_BINARY_I8(CltU8, std::uint64_t(a) < std::uint64_t(b))
                    CS_BINARY_I8(CleU8, std::uint64_t(a) <= std::uint64_t(b))
                    CS_BINARY_I8(CgtU8, std::uint64_t(a) > std::uint64_t(b))
                    CS_BINARY_I8(CgeU8, std::uint64_t(a) >= std::uint64_t(b))

                    CS_BINARY_R8(AddR8, a + b)
                    CS_BINARY_R8(SubR8, a - b)
                    CS_BINARY_R8(MulR8, a * b)
                    CS_BINARY_R8(DivR8, a / b)
                    CS_BINARY_R8(RemR8, std::fmod(a, b))
                    CS_BINARY_R8(CeqR8, a == b)
                    CS_BINARY_R8(CneR8, a != b)
                    CS_BINARY_R8(CltR8, a < b)
                    CS_BINARY_R8(CleR8, a <= b)
                    CS_BINARY_R8(CgtR8, a > b)
                    CS_BINARY_R8(CgeR8, a >= b)

                    CS_BINARY_R4(AddR4, a + b)
                    CS_BINARY_R4(SubR4, a - b)
                    CS_BINARY_R4(MulR4, a * b)
                    CS_BINARY_R4(DivR4, a / b)
                    CS_BINARY_R4(RemR4, std::fmod(a, b))
#undef CS_BINARY_I4
#undef CS_BINARY_I8
#undef CS_BINARY_R8
#undef CS_BINARY_R4

                    case Op::DivI4:
                    case Op::RemI4:
                    case Op::DivU4:
                    case Op::RemU4: {
                        const std::int32_t b = stack.back().toInt();
                        stack.pop_back();
                        const std::int32_t a = stack.back().toInt();
                        if (b == 0)
                            throwError(rt, ErrorCode::DivideByZero, "attempted to divide by zero");
                        if (op == Op::DivI4)
                            stack.back() = Value(b == -1 ? std::int32_t(-std::uint32_t(a))
                                                         : std::int32_t(a / b));
                        else if (op == Op::RemI4)
                            stack.back() = Value(b == -1 ? 0 : std::int32_t(a % b));
                        else if (op == Op::DivU4)
                            stack.back() =
                                Value(std::int32_t(std::uint32_t(a) / std::uint32_t(b)));
                        else
                            stack.back() =
                                Value(std::int32_t(std::uint32_t(a) % std::uint32_t(b)));
                        break;
                    }
                    case Op::DivI8:
                    case Op::RemI8:
                    case Op::DivU8:
                    case Op::RemU8: {
                        const std::int64_t b = stack.back().toLong();
                        stack.pop_back();
                        const std::int64_t a = stack.back().toLong();
                        if (b == 0)
                            throwError(rt, ErrorCode::DivideByZero, "attempted to divide by zero");
                        if (op == Op::DivI8)
                            stack.back() = Value(b == -1 ? std::int64_t(-std::uint64_t(a))
                                                         : std::int64_t(a / b));
                        else if (op == Op::RemI8)
                            stack.back() = Value(b == -1 ? std::int64_t(0) : std::int64_t(a % b));
                        else if (op == Op::DivU8)
                            stack.back() =
                                Value(std::int64_t(std::uint64_t(a) / std::uint64_t(b)));
                        else
                            stack.back() =
                                Value(std::int64_t(std::uint64_t(a) % std::uint64_t(b)));
                        break;
                    }

                    case Op::AddI4Ovf:
                    case Op::SubI4Ovf:
                    case Op::MulI4Ovf: {
                        const std::int64_t b = stack.back().toInt();
                        stack.pop_back();
                        const std::int64_t a = stack.back().toInt();
                        const std::int64_t result = op == Op::AddI4Ovf   ? a + b
                                                    : op == Op::SubI4Ovf ? a - b
                                                                         : a * b;
                        if (result > 2147483647ll || result < -2147483648ll)
                            throwError(rt, ErrorCode::Overflow,
                                       "the arithmetic operation overflowed");
                        stack.back() = Value(std::int32_t(result));
                        break;
                    }
                    case Op::AddI8Ovf:
                    case Op::SubI8Ovf:
                    case Op::MulI8Ovf: {
                        const std::int64_t b = stack.back().toLong();
                        stack.pop_back();
                        const std::int64_t a = stack.back().toLong();
                        std::int64_t result = 0;
                        const bool overflow = op == Op::AddI8Ovf
                                                  ? __builtin_add_overflow(a, b, &result)
                                                  : (op == Op::SubI8Ovf
                                                         ? __builtin_sub_overflow(a, b, &result)
                                                         : __builtin_mul_overflow(a, b, &result));
                        if (overflow)
                            throwError(rt, ErrorCode::Overflow,
                                       "the arithmetic operation overflowed");
                        stack.back() = Value(result);
                        break;
                    }

                    case Op::NegI4:
                        stack.back() = Value(std::int32_t(-std::uint32_t(stack.back().toInt())));
                        break;
                    case Op::NegI8:
                        stack.back() = Value(std::int64_t(-std::uint64_t(stack.back().toLong())));
                        break;
                    case Op::NegR4: stack.back() = Value(float(-stack.back().toDouble())); break;
                    case Op::NegR8: stack.back() = Value(-stack.back().toDouble()); break;
                    case Op::NotI4: stack.back() = Value(~stack.back().toInt()); break;
                    case Op::NotI8: stack.back() = Value(~stack.back().toLong()); break;
                    case Op::LogicalNot: stack.back() = Value(!stack.back().toBool()); break;
                    case Op::IsNull: stack.back() = Value(stack.back().isNull()); break;

                    case Op::CeqRef:
                    case Op::CneRef: {
                        Value b = std::move(stack.back());
                        stack.pop_back();
                        const bool same = valueEquals(stack.back(), b);
                        stack.back() = Value(op == Op::CeqRef ? same : !same);
                        break;
                    }
                    case Op::CeqStr:
                    case Op::CneStr: {
                        Value b = std::move(stack.back());
                        stack.pop_back();
                        const bool same = valueEquals(stack.back(), b);
                        stack.back() = Value(op == Op::CeqStr ? same : !same);
                        break;
                    }

                    // ---- strings ----
                    case Op::Concat: {
                        const std::uint32_t count = readU32(operands);
                        Value result =
                            concatStrings(rt, stack.data() + stack.size() - count, count);
                        stack.resize(stack.size() - count);
                        stack.push_back(std::move(result));
                        break;
                    }
                    case Op::ToStr:
                        stack.back() = rt.str(toStringValue(rt, stack.back()));
                        break;
                    case Op::ToStrU: {
                        const Value& v = stack.back();
                        const std::string text = v.type() == Type::Long
                                                     ? std::to_string(std::uint64_t(v.toLong()))
                                                     : std::to_string(std::uint32_t(v.toInt()));
                        stack.back() = rt.str(text.c_str());
                        break;
                    }

                    case Op::OpCount: break;
                }

                // An interrupt handler gets a chance every so often; the
                // counter is a single decrement on the hot path.
                if (++ticks >= 4096) {
                    ticks = 0;
                    if (rt.ctx.interrupt && rt.ctx.interrupt())
                        throwError(rt, ErrorCode::Interrupted, "execution was interrupted");
                    rt.maybeCollect();
                }
            }
            return Value();
        } catch (Exception& raised) {
            // The engine's own limits are not the program's to catch: a
            // `catch (Exception)` around an infinite loop would otherwise
            // swallow the interrupt that was trying to stop it.
            if (raised.code() == ErrorCode::Interrupted ||
                raised.code() == ErrorCode::StackOverflow ||
                raised.code() == ErrorCode::OutOfMemory ||
                !dispatch(rt, frame, raised.value()))
                throw;
        } catch (Error& raised) {
            // An engine-level failure the program can still catch, e.g. a
            // memory limit; give it the same treatment.
            Value exception = rt.makeException(raised.code(), fromUtf8(raised.what()));
            if (!dispatch(rt, frame, exception))
                throw;
        }
    }
}

}  // namespace
}  // namespace typed
}  // namespace internal
}  // namespace script
