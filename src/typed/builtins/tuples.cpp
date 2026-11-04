// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Tuples: `(int, string)` and the values that inhabit them.
//
// A tuple is a constructed type like `List<T>` is -- one `TypeInfo` per shape,
// made on demand, with `Item1`..`ItemN` as real fields so that member access,
// pattern matching and deconstruction all go through the paths that already
// exist. The runtime representation is an ordinary instance cell; nothing here
// needs its own object kind.
#include "script/internal/typed/builtins.hpp"

namespace script {
namespace internal {
namespace typed {

void instantiateTuple(TypeTable& table, TypeInfo* type)
{
    const CoreTypes& core = table.core();
    const TypeInfo* tupleType = type;
    const std::uint32_t arity = std::uint32_t(type->typeArgs.size());

    auto newTuple = [tupleType, arity](CallInfo& call) {
        Runtime& rt = runtimeOf(call);
        Value instance = newInstanceOf(rt, tupleType);
        auto* cell = static_cast<CSInstance*>(detail::Access::ptr(instance));
        for (std::uint32_t i = 0; i < arity; ++i)
            cell->slots()[i] = call.arg(i);
        return instance;
    };

    std::vector<ParamInfo> params;
    for (std::uint32_t i = 0; i < arity; ++i)
        params.push_back(table.param(toUtf8(type->fields[i].name).c_str(), type->typeArgs[i]));
    table.addMethod(type, ".ctor", core.voidType, std::move(params), MethodConstructor, newTuple);

    // `(1, "a").ToString()` is `(1, a)`, which is what C# prints.
    table.addMethod(type, "ToString", core.stringType, {}, MethodVirtual | MethodOverride,
                    [arity](CallInfo& call) {
                        Runtime& rt = runtimeOf(call);
                        auto* cell =
                            static_cast<CSInstance*>(detail::Access::ptr(call.thisValue()));
                        String out;
                        out.push_back(u'(');
                        for (std::uint32_t i = 0; i < arity; ++i) {
                            if (i) {
                                out.push_back(u',');
                                out.push_back(u' ');
                            }
                            out += toStringValue(rt, cell->slots()[i]);
                        }
                        out.push_back(u')');
                        return rt.str(out);
                    });

    table.addMethod(type, "Equals", core.boolType, {table.param("other", core.objectType)},
                    MethodVirtual | MethodOverride, [tupleType, arity](CallInfo& call) {
                        const Value& other = call.arg(0);
                        auto* mine =
                            static_cast<CSInstance*>(detail::Access::ptr(call.thisValue()));
                        auto* theirs = static_cast<CSInstance*>(detail::Access::ptr(other));
                        if (!theirs || theirs->type != tupleType)
                            return Value(false);
                        for (std::uint32_t i = 0; i < arity; ++i) {
                            if (!valueEquals(mine->slots()[i], theirs->slots()[i]))
                                return Value(false);
                        }
                        return Value(true);
                    });

    table.addMethod(type, "GetHashCode", core.intType, {}, MethodVirtual | MethodOverride,
                    [arity](CallInfo& call) {
                        auto* cell =
                            static_cast<CSInstance*>(detail::Access::ptr(call.thisValue()));
                        std::size_t hash = 17;
                        for (std::uint32_t i = 0; i < arity; ++i)
                            hash = hash * 31 + hashValue(cell->slots()[i]);
                        return Value(std::int32_t(hash));
                    });

    // `var (a, b) = pair;` goes through the same Deconstruct any other type
    // would, so the checker needs no special case for tuples.
    std::vector<ParamInfo> outs;
    for (std::uint32_t i = 0; i < arity; ++i) {
        ParamInfo out = table.param(toUtf8(type->fields[i].name).c_str(), type->typeArgs[i]);
        out.mode = ParamMode::Out;
        outs.push_back(std::move(out));
    }
    table.addMethod(type, "Deconstruct", core.voidType, std::move(outs), 0,
                    [arity](CallInfo& call) {
                        auto* cell =
                            static_cast<CSInstance*>(detail::Access::ptr(call.thisValue()));
                        for (std::uint32_t i = 0; i < arity; ++i)
                            storeRef(call.arg(i), cell->slots()[i]);
                        return Value();
                    });
}

}  // namespace typed
}  // namespace internal
}  // namespace script
