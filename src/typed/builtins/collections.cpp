// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// List<T> and Dictionary<K,V>.
//
// Both are constructed types: the table makes one `TypeInfo` per element type
// and calls back in here to fill in its members with the element type
// substituted, so the checker sees a `List<int>` whose `Add` really does take
// an `int`. The implementations behind them are shared -- one `CSList`, one
// `CSDict` -- because the runtime representation does not depend on T.
#include "script/internal/typed/builtins.hpp"

namespace script {
namespace internal {
namespace typed {

void installCollections(TypeTable& table)
{
    // The constructed types are made on demand; nothing to install up front
    // beyond making sure `object`'s list exists for the common untyped case.
    (void)table;
}

void instantiateList(TypeTable& table, TypeInfo* type)
{
    const CoreTypes& core = table.core();
    const TypeInfo* element = type->typeArgs.empty() ? core.objectType : type->typeArgs[0];
    const TypeInfo* intType = core.intType;
    const TypeInfo* boolType = core.boolType;
    const TypeInfo* voidType = core.voidType;
    const TypeInfo* listType = type;

    auto newList = [listType](CallInfo& call) {
        Runtime& rt = runtimeOf(call);
        auto* cell = rt.newCell<CSList>();
        cell->type = listType;
        return detail::Access::adopt(Type::Object, cell);
    };
    table.addMethod(type, ".ctor", voidType, {}, MethodConstructor, newList);
    table.addMethod(type, ".ctor", voidType, {table.param("capacity", intType)},
                    MethodConstructor, [newList](CallInfo& call) {
                        Value result = newList(call);
                        const std::int32_t capacity = call.arg(0).toInt();
                        if (capacity > 0)
                            asList(result)->items.reserve(std::size_t(capacity));
                        return result;
                    });

    table.addMethod(type, "Add", voidType, {table.param("item", element)}, 0,
                    [](CallInfo& call) {
                        if (auto* list = asList(call.thisValue()))
                            list->add(call.arg(0));
                        return Value();
                    });
    table.addMethod(type, "Insert", voidType,
                    {table.param("index", intType), table.param("item", element)}, 0,
                    [](CallInfo& call) {
                        auto* list = asList(call.thisValue());
                        const std::int32_t index = call.arg(0).toInt();
                        if (!list || index < 0 || std::size_t(index) > list->items.size())
                            fail(call, ErrorCode::ArgumentOutOfRange, "index is out of range");
                        list->items.insert(list->items.begin() + index, call.arg(1));
                        return Value();
                    });
    table.addMethod(type, "RemoveAt", voidType, {table.param("index", intType)}, 0,
                    [](CallInfo& call) {
                        auto* list = asList(call.thisValue());
                        const std::int32_t index = call.arg(0).toInt();
                        if (!list || index < 0 || std::size_t(index) >= list->items.size())
                            fail(call, ErrorCode::ArgumentOutOfRange, "index is out of range");
                        list->items.erase(list->items.begin() + index);
                        return Value();
                    });
    table.addMethod(type, "Remove", boolType, {table.param("item", element)}, 0,
                    [](CallInfo& call) {
                        auto* list = asList(call.thisValue());
                        if (!list)
                            return Value(false);
                        for (std::size_t i = 0; i < list->items.size(); ++i) {
                            if (!valueEquals(list->items[i], call.arg(0)))
                                continue;
                            list->items.erase(list->items.begin() + std::ptrdiff_t(i));
                            return Value(true);
                        }
                        return Value(false);
                    });
    table.addMethod(type, "IndexOf", intType, {table.param("item", element)}, 0,
                    [](CallInfo& call) {
                        auto* list = asList(call.thisValue());
                        for (std::size_t i = 0; list && i < list->items.size(); ++i) {
                            if (valueEquals(list->items[i], call.arg(0)))
                                return Value(std::int32_t(i));
                        }
                        return Value(-1);
                    });
    table.addMethod(type, "Contains", boolType, {table.param("item", element)}, 0,
                    [](CallInfo& call) {
                        auto* list = asList(call.thisValue());
                        for (std::size_t i = 0; list && i < list->items.size(); ++i) {
                            if (valueEquals(list->items[i], call.arg(0)))
                                return Value(true);
                        }
                        return Value(false);
                    });
    table.addMethod(type, "Clear", voidType, {}, 0, [](CallInfo& call) {
        if (auto* list = asList(call.thisValue()))
            list->items.clear();
        return Value();
    });
    table.addMethod(type, "ToArray", table.arrayOf(element), {}, 0,
                    [element](CallInfo& call) {
                        Runtime& rt = runtimeOf(call);
                        auto* list = asList(call.thisValue());
                        const std::int32_t count =
                            std::int32_t(list ? list->items.size() : std::size_t(0));
                        Value result = newArrayOf(rt, element, count);
                        auto* array = asArray(result);
                        for (std::int32_t i = 0; i < count; ++i)
                            array->set(std::uint32_t(i), list->items[std::size_t(i)]);
                        return result;
                    });
    table.addMethod(type, "get_Item", element, {table.param("index", intType)}, 0,
                    [](CallInfo& call) {
                        auto* list = asList(call.thisValue());
                        const std::int32_t index = call.arg(0).toInt();
                        if (!list || index < 0 || std::size_t(index) >= list->items.size())
                            fail(call, ErrorCode::ArgumentOutOfRange,
                                 "index was out of range; it must be non-negative and less than "
                                 "the size of the collection");
                        return list->items[std::size_t(index)];
                    });
    table.addMethod(type, "set_Item", voidType,
                    {table.param("index", intType), table.param("value", element)}, 0,
                    [](CallInfo& call) {
                        auto* list = asList(call.thisValue());
                        const std::int32_t index = call.arg(0).toInt();
                        if (!list || index < 0 || std::size_t(index) >= list->items.size())
                            fail(call, ErrorCode::ArgumentOutOfRange, "index was out of range");
                        list->items[std::size_t(index)] = call.arg(1);
                        return Value();
                    });

    PropertyInfo count;
    count.name = ascii("Count");
    count.type = intType;
    count.owner = type;
    count.access = Access::Public;
    count.getter = table.addMethod(type, "get_Count", intType, {}, MethodAccessor,
                                   [](CallInfo& call) {
                                       auto* list = asList(call.thisValue());
                                       return Value(std::int32_t(list ? list->items.size() : 0));
                                   });
    type->props.push_back(count);

    PropertyInfo item;
    item.name = ascii("Item");
    item.type = element;
    item.owner = type;
    item.access = Access::Public;
    type->props.push_back(item);
}

void instantiateDictionary(TypeTable& table, TypeInfo* type)
{
    const CoreTypes& core = table.core();
    const TypeInfo* keyType = type->typeArgs.size() > 0 ? type->typeArgs[0] : core.objectType;
    const TypeInfo* valueType = type->typeArgs.size() > 1 ? type->typeArgs[1] : core.objectType;
    const TypeInfo* intType = core.intType;
    const TypeInfo* boolType = core.boolType;
    const TypeInfo* voidType = core.voidType;
    const TypeInfo* dictType = type;

    table.addMethod(type, ".ctor", voidType, {}, MethodConstructor, [dictType](CallInfo& call) {
        Runtime& rt = runtimeOf(call);
        auto* cell = rt.newCell<CSDict>();
        cell->type = dictType;
        return detail::Access::adopt(Type::Object, cell);
    });

    table.addMethod(type, "Add", voidType,
                    {table.param("key", keyType), table.param("value", valueType)}, 0,
                    [](CallInfo& call) {
                        auto* dict = asDict(call.thisValue());
                        if (!dict)
                            return Value();
                        if (dict->find(call.arg(0)))
                            fail(call, ErrorCode::ArgumentException,
                                 "an item with the same key has already been added");
                        dict->put(call.arg(0), call.arg(1));
                        return Value();
                    });
    table.addMethod(type, "ContainsKey", boolType, {table.param("key", keyType)}, 0,
                    [](CallInfo& call) {
                        auto* dict = asDict(call.thisValue());
                        return Value(dict && dict->find(call.arg(0)) != nullptr);
                    });
    table.addMethod(type, "Remove", boolType, {table.param("key", keyType)}, 0,
                    [](CallInfo& call) {
                        auto* dict = asDict(call.thisValue());
                        return Value(dict && dict->erase(call.arg(0)));
                    });
    table.addMethod(type, "Clear", voidType, {}, 0, [](CallInfo& call) {
        if (auto* dict = asDict(call.thisValue())) {
            dict->entries.clear();
            dict->index.clear();
            dict->liveCount = 0;
        }
        return Value();
    });
    table.addMethod(type, "TryGetValue", boolType,
                    {table.param("key", keyType),
                     table.param("value", valueType, ParamMode::Out)},
                    0, [valueType](CallInfo& call) {
                        auto* dict = asDict(call.thisValue());
                        CSDict::Entry* entry = dict ? dict->find(call.arg(0)) : nullptr;
                        storeRef(call.arg(1), entry ? entry->value : defaultValue(valueType));
                        return Value(entry != nullptr);
                    });
    table.addMethod(type, "get_Item", valueType, {table.param("key", keyType)}, 0,
                    [](CallInfo& call) {
                        auto* dict = asDict(call.thisValue());
                        CSDict::Entry* entry = dict ? dict->find(call.arg(0)) : nullptr;
                        if (!entry)
                            fail(call, ErrorCode::KeyNotFound,
                                 "the given key was not present in the dictionary");
                        return entry->value;
                    });
    table.addMethod(type, "set_Item", voidType,
                    {table.param("key", keyType), table.param("value", valueType)}, 0,
                    [](CallInfo& call) {
                        if (auto* dict = asDict(call.thisValue()))
                            dict->put(call.arg(0), call.arg(1));
                        return Value();
                    });

    // `Keys` and `Values` hand back a List<T>, which is what `foreach` walks.
    const TypeInfo* keyList = table.listOf(keyType);
    const TypeInfo* valueList = table.listOf(valueType);
    table.addProperty(type, "Keys", keyList,
                      [keyList](CallInfo& call) {
                          Runtime& rt = runtimeOf(call);
                          auto* dict = asDict(call.thisValue());
                          auto* result = rt.newCell<CSList>();
                          Value handle = detail::Access::adopt(Type::Object, result);
                          result->type = keyList;
                          for (const CSDict::Entry& entry : dict->entries) {
                              if (entry.live)
                                  result->add(entry.key);
                          }
                          return handle;
                      },
                      nullptr);
    table.addProperty(type, "Values", valueList,
                      [valueList](CallInfo& call) {
                          Runtime& rt = runtimeOf(call);
                          auto* dict = asDict(call.thisValue());
                          auto* result = rt.newCell<CSList>();
                          Value handle = detail::Access::adopt(Type::Object, result);
                          result->type = valueList;
                          for (const CSDict::Entry& entry : dict->entries) {
                              if (entry.live)
                                  result->add(entry.value);
                          }
                          return handle;
                      },
                      nullptr);

    PropertyInfo count;
    count.name = ascii("Count");
    count.type = intType;
    count.owner = type;
    count.access = Access::Public;
    count.getter = table.addMethod(type, "get_Count", intType, {}, MethodAccessor,
                                   [](CallInfo& call) {
                                       auto* dict = asDict(call.thisValue());
                                       return Value(std::int32_t(dict ? dict->liveCount : 0));
                                   });
    type->props.push_back(count);

    PropertyInfo item;
    item.name = ascii("Item");
    item.type = valueType;
    item.owner = type;
    item.access = Access::Public;
    type->props.push_back(item);
}

}  // namespace typed
}  // namespace internal
}  // namespace script
