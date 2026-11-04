// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// System.String and System.Text.StringBuilder.
#include <algorithm>

#include "script/internal/typed/builtins.hpp"
#include "script/internal/typed/numconv.hpp"

namespace script {
namespace internal {
namespace typed {

namespace {

String upper(StringView s)
{
    String out(s);
    for (Char& c : out) {
        if (c >= u'a' && c <= u'z')
            c = Char(c - 32);
    }
    return out;
}

String lower(StringView s)
{
    String out(s);
    for (Char& c : out) {
        if (c >= u'A' && c <= u'Z')
            c = Char(c + 32);
    }
    return out;
}

bool isSpace(Char c) noexcept
{
    return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r' || c == u'\f' || c == u'\v';
}

}  // namespace

void installString(TypeTable& table)
{
    const CoreTypes& core = table.core();
    TypeInfo* stringType = const_cast<TypeInfo*>(core.stringType);
    const TypeInfo* intType = core.intType;
    const TypeInfo* boolType = core.boolType;
    const TypeInfo* charType = core.charType;
    const TypeInfo* objectType = core.objectType;
    const TypeInfo* voidType = core.voidType;
    const TypeInfo* self = core.stringType;
    const TypeInfo* charArray = table.arrayOf(charType);
    const TypeInfo* stringArray = table.arrayOf(stringType);

    table.addProperty(stringType, "Length", intType,
                      [](CallInfo& call) {
                          return Value(std::int32_t(textOf(call.thisValue()).size()));
                      },
                      nullptr);
    table.addMethod(stringType, "get_Chars", charType, {table.param("index", intType)}, 0,
                    [](CallInfo& call) {
                        const StringView text = textOf(call.thisValue());
                        const std::int32_t index = call.arg(0).toInt();
                        if (index < 0 || std::size_t(index) >= text.size())
                            fail(call, ErrorCode::IndexOutOfRange,
                                 "index was outside the bounds of the string");
                        return Value(char16_t(text[std::size_t(index)]));
                    });
    table.addMethod(stringType, "Substring", self, {table.param("startIndex", intType)}, 0,
                    [](CallInfo& call) {
                        const StringView text = textOf(call.thisValue());
                        const std::int32_t start = call.arg(0).toInt();
                        if (start < 0 || std::size_t(start) > text.size())
                            fail(call, ErrorCode::ArgumentOutOfRange, "startIndex is out of range");
                        return runtimeOf(call).str(text.substr(std::size_t(start)));
                    });
    table.addMethod(stringType, "Substring", self,
                    {table.param("startIndex", intType), table.param("length", intType)}, 0,
                    [](CallInfo& call) {
                        const StringView text = textOf(call.thisValue());
                        const std::int32_t start = call.arg(0).toInt();
                        const std::int32_t length = call.arg(1).toInt();
                        if (start < 0 || length < 0 ||
                            std::size_t(start) + std::size_t(length) > text.size())
                            fail(call, ErrorCode::ArgumentOutOfRange,
                                 "the substring is outside the string");
                        return runtimeOf(call).str(
                            text.substr(std::size_t(start), std::size_t(length)));
                    });
    table.addMethod(stringType, "IndexOf", intType, {table.param("value", self)}, 0,
                    [](CallInfo& call) {
                        const StringView text = textOf(call.thisValue());
                        const std::size_t at = text.find(textOf(call.arg(0)));
                        return Value(at == String::npos ? -1 : std::int32_t(at));
                    });
    table.addMethod(stringType, "IndexOf", intType, {table.param("value", charType)}, 0,
                    [](CallInfo& call) {
                        const StringView text = textOf(call.thisValue());
                        const std::size_t at = text.find(Char(call.arg(0).toInt()));
                        return Value(at == String::npos ? -1 : std::int32_t(at));
                    });
    table.addMethod(stringType, "LastIndexOf", intType, {table.param("value", self)}, 0,
                    [](CallInfo& call) {
                        const StringView text = textOf(call.thisValue());
                        const std::size_t at = text.rfind(textOf(call.arg(0)));
                        return Value(at == String::npos ? -1 : std::int32_t(at));
                    });
    table.addMethod(stringType, "Contains", boolType, {table.param("value", self)}, 0,
                    [](CallInfo& call) {
                        return Value(textOf(call.thisValue()).find(textOf(call.arg(0))) !=
                                     String::npos);
                    });
    table.addMethod(stringType, "StartsWith", boolType, {table.param("value", self)}, 0,
                    [](CallInfo& call) {
                        const StringView text = textOf(call.thisValue());
                        const StringView prefix = textOf(call.arg(0));
                        return Value(text.size() >= prefix.size() &&
                                     text.compare(0, prefix.size(), prefix) == 0);
                    });
    table.addMethod(stringType, "EndsWith", boolType, {table.param("value", self)}, 0,
                    [](CallInfo& call) {
                        const StringView text = textOf(call.thisValue());
                        const StringView suffix = textOf(call.arg(0));
                        return Value(text.size() >= suffix.size() &&
                                     text.compare(text.size() - suffix.size(), suffix.size(),
                                                  suffix) == 0);
                    });
    table.addMethod(stringType, "ToUpper", self, {}, 0, [](CallInfo& call) {
        return runtimeOf(call).str(upper(textOf(call.thisValue())));
    });
    table.addMethod(stringType, "ToLower", self, {}, 0, [](CallInfo& call) {
        return runtimeOf(call).str(lower(textOf(call.thisValue())));
    });
    table.addMethod(stringType, "Trim", self, {}, 0, [](CallInfo& call) {
        StringView text = textOf(call.thisValue());
        while (!text.empty() && isSpace(text.front()))
            text.remove_prefix(1);
        while (!text.empty() && isSpace(text.back()))
            text.remove_suffix(1);
        return runtimeOf(call).str(text);
    });
    table.addMethod(stringType, "Replace", self,
                    {table.param("oldValue", self), table.param("newValue", self)}, 0,
                    [](CallInfo& call) {
                        const StringView text = textOf(call.thisValue());
                        const StringView from = textOf(call.arg(0));
                        const StringView to = textOf(call.arg(1));
                        if (from.empty())
                            return runtimeOf(call).str(text);
                        String out;
                        std::size_t at = 0;
                        for (;;) {
                            const std::size_t found = text.find(from, at);
                            if (found == String::npos) {
                                out += text.substr(at);
                                break;
                            }
                            out += text.substr(at, found - at);
                            out += to;
                            at = found + from.size();
                        }
                        return runtimeOf(call).str(out);
                    });
    table.addMethod(stringType, "Split", stringArray, {table.param("separator", charType)}, 0,
                    [](CallInfo& call) {
                        Runtime& rt = runtimeOf(call);
                        const StringView text = textOf(call.thisValue());
                        const Char separator = Char(call.arg(0).toInt());
                        std::vector<String> parts;
                        String current;
                        for (Char c : text) {
                            if (c == separator) {
                                parts.push_back(current);
                                current.clear();
                            } else {
                                current.push_back(c);
                            }
                        }
                        parts.push_back(current);
                        Value result = newArrayOf(rt, rt.types->core().stringType,
                                                  std::int32_t(parts.size()));
                        auto* array = asArray(result);
                        for (std::size_t i = 0; i < parts.size(); ++i)
                            array->set(std::uint32_t(i), rt.str(parts[i]));
                        return result;
                    });
    table.addMethod(stringType, "ToCharArray", charArray, {}, 0, [](CallInfo& call) {
        Runtime& rt = runtimeOf(call);
        const StringView text = textOf(call.thisValue());
        Value result = newArrayOf(rt, rt.types->core().charType, std::int32_t(text.size()));
        auto* array = asArray(result);
        for (std::size_t i = 0; i < text.size(); ++i)
            array->set(std::uint32_t(i), Value(char16_t(text[i])));
        return result;
    });
    table.addMethod(stringType, "PadLeft", self, {table.param("totalWidth", intType)}, 0,
                    [](CallInfo& call) {
                        const StringView text = textOf(call.thisValue());
                        const std::int32_t width = call.arg(0).toInt();
                        String out;
                        for (std::size_t i = text.size(); i < std::size_t(std::max(width, 0)); ++i)
                            out.push_back(u' ');
                        return runtimeOf(call).str(out + String(text));
                    });
    table.addMethod(stringType, "PadRight", self, {table.param("totalWidth", intType)}, 0,
                    [](CallInfo& call) {
                        const StringView text = textOf(call.thisValue());
                        const std::int32_t width = call.arg(0).toInt();
                        String out(text);
                        while (out.size() < std::size_t(std::max(width, 0)))
                            out.push_back(u' ');
                        return runtimeOf(call).str(out);
                    });
    table.addMethod(stringType, "CompareTo", intType, {table.param("other", self)}, 0,
                    [](CallInfo& call) {
                        const int result = textOf(call.thisValue()).compare(textOf(call.arg(0)));
                        return Value(result < 0 ? -1 : (result > 0 ? 1 : 0));
                    });
    table.addMethod(stringType, "Equals", boolType, {table.param("other", self)}, 0,
                    [](CallInfo& call) {
                        return Value(textOf(call.thisValue()) == textOf(call.arg(0)));
                    });
    table.addMethod(stringType, "ToString", self, {}, 0,
                    [](CallInfo& call) { return call.thisValue(); });

    // ---- statics ----
    table.addMethod(stringType, "IsNullOrEmpty", boolType, {table.param("value", self)},
                    MethodStatic, [](CallInfo& call) {
                        return Value(call.arg(0).isNull() || textOf(call.arg(0)).empty());
                    });
    table.addMethod(stringType, "IsNullOrWhiteSpace", boolType, {table.param("value", self)},
                    MethodStatic, [](CallInfo& call) {
                        const StringView text = textOf(call.arg(0));
                        return Value(call.arg(0).isNull() ||
                                     std::all_of(text.begin(), text.end(), isSpace));
                    });
    table.addMethod(stringType, "Concat", self,
                    {table.param("a", self), table.param("b", self)}, MethodStatic,
                    [](CallInfo& call) {
                        return runtimeOf(call).str(String(textOf(call.arg(0))) +
                                                   String(textOf(call.arg(1))));
                    });
    table.addMethod(stringType, "Join", self,
                    {table.param("separator", self), table.param("values", stringArray)},
                    MethodStatic, [](CallInfo& call) {
                        Runtime& rt = runtimeOf(call);
                        const StringView separator = textOf(call.arg(0));
                        auto* array = asArray(call.arg(1));
                        String out;
                        for (std::uint32_t i = 0; array && i < array->length; ++i) {
                            if (i)
                                out += separator;
                            out += toStringValue(rt, array->get(i));
                        }
                        return rt.str(out);
                    });
    table.addMethod(stringType, "Format", self,
                    {table.param("format", self),
                     table.param("args", table.arrayOf(objectType), ParamMode::Params)},
                    MethodStatic, [](CallInfo& call) {
                        // `{0}` and `{1}` only; the rest of the composite
                        // format syntax is what interpolation is for.
                        Runtime& rt = runtimeOf(call);
                        const StringView format = textOf(call.arg(0));
                        auto* args = asArray(call.arg(1));
                        String out;
                        for (std::size_t i = 0; i < format.size(); ++i) {
                            if (format[i] != u'{' || i + 1 >= format.size()) {
                                out.push_back(format[i]);
                                continue;
                            }
                            if (format[i + 1] == u'{') {
                                out.push_back(u'{');
                                ++i;
                                continue;
                            }
                            std::size_t j = i + 1;
                            std::uint32_t index = 0;
                            while (j < format.size() && format[j] >= u'0' && format[j] <= u'9')
                                index = index * 10 + std::uint32_t(format[j++] - u'0');
                            if (j >= format.size() || format[j] != u'}') {
                                out.push_back(format[i]);
                                continue;
                            }
                            if (args && index < args->length)
                                out += toStringValue(rt, args->get(index));
                            i = j;
                        }
                        return rt.str(out);
                    });
    /// The engine's own helper: what an interpolation hole with a `:format`
    /// specifier compiles to.
    table.addMethod(stringType, "FormatValue", self,
                    {table.param("value", objectType), table.param("format", self)},
                    MethodStatic, [](CallInfo& call) {
                        Runtime& rt = runtimeOf(call);
                        const Value& value = call.arg(0);
                        String out;
                        if (value.isNumber() &&
                            formatNumber(value.toDouble(), !value.isDouble() && !value.isFloat(),
                                         value.toLong(), textOf(call.arg(1)), out))
                            return rt.str(out);
                        return rt.str(toStringValue(rt, value));
                    });
    FieldInfo empty;
    empty.name = ascii("Empty");
    empty.type = self;
    empty.owner = stringType;
    empty.access = Access::Public;
    empty.isStatic = true;
    empty.isConst = true;
    empty.constValue = table.runtime().str(String());
    stringType->statics.push_back(std::move(empty));

    // ---- System.Text.StringBuilder ----
    TypeInfo* builder = table.create(TypeKind::Class, "System.Text.StringBuilder");
    builder->base = core.objectType;
    builder->nativeKind = NativeKind::StringBuilder;
    builder->isSealed = true;
    table.addMethod(builder, ".ctor", voidType, {}, MethodConstructor, [](CallInfo& call) {
        Runtime& rt = runtimeOf(call);
        auto* cell = rt.newCell<CSBuilder>();
        cell->type = rt.types->find("System.Text.StringBuilder");
        return detail::Access::adopt(Type::Object, cell);
    });
    const TypeInfo* builderRef = builder;
    for (const TypeInfo* argType : {objectType, self, intType, core.doubleType, charType}) {
        table.addMethod(builder, "Append", builderRef, {table.param("value", argType)}, 0,
                        [](CallInfo& call) {
                            Runtime& rt = runtimeOf(call);
                            if (auto* cell = asBuilder(call.thisValue())) {
                                const std::size_t before = cell->buf.capacity();
                                cell->buf += toStringValue(rt, call.arg(0));
                                cell->chargeIfGrown(cell->buf, before);
                            }
                            return call.thisValue();
                        });
    }
    table.addMethod(builder, "AppendLine", builderRef, {table.param("value", self)}, 0,
                    [](CallInfo& call) {
                        Runtime& rt = runtimeOf(call);
                        if (auto* cell = asBuilder(call.thisValue())) {
                            cell->buf += toStringValue(rt, call.arg(0));
                            cell->buf.push_back(u'\n');
                        }
                        return call.thisValue();
                    });
    table.addMethod(builder, "AppendLine", builderRef, {}, 0, [](CallInfo& call) {
        if (auto* cell = asBuilder(call.thisValue()))
            cell->buf.push_back(u'\n');
        return call.thisValue();
    });
    table.addMethod(builder, "Clear", builderRef, {}, 0, [](CallInfo& call) {
        if (auto* cell = asBuilder(call.thisValue()))
            cell->buf.clear();
        return call.thisValue();
    });
    table.addMethod(builder, "ToString", self, {}, 0, [](CallInfo& call) {
        auto* cell = asBuilder(call.thisValue());
        return runtimeOf(call).str(cell ? StringView(cell->buf) : StringView());
    });
    table.addProperty(builder, "Length", intType,
                      [](CallInfo& call) {
                          auto* cell = asBuilder(call.thisValue());
                          return Value(std::int32_t(cell ? cell->buf.size() : 0));
                      },
                      nullptr);
}

}  // namespace typed
}  // namespace internal
}  // namespace script
