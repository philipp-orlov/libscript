// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// RegExp, and the String methods that hand over to it. The engine itself is regexp.cpp.

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/regexp.hpp"
#include "script/internal/dyn/vm.hpp"

namespace script {
namespace internal {
namespace dyn {

/// What a runtime keeps for its regular expressions.
struct RegExpState
{
    regex::Matcher matcher;
    /// Programs by flags and source: a literal in a loop or `new RegExp(s)` of the same `s` compiles once.
    std::unordered_map<std::u16string, std::shared_ptr<const regex::Program>> programs;
    Ref<DynFunction> exec;
    Ref<DynFunction> ctor;
    Ref<StringCell> lastIndex, index, input, groups, indices, flags, source, exec_;
    /// The prototype's own methods and getters that the short ways below stand in for, as they were installed.
    static constexpr int kWatched = 10;
    Value watched[kWatched];
    Ref<StringCell> watchedKeys[kWatched];
    /// Where those are in its table, while `protoEpoch` stays this.
    std::int32_t watchedSlots[kWatched] = {};
    std::uint64_t watchedAt = ~std::uint64_t(0);
    // What the last match was, for `RegExp.$1` and its kin.
    Value speciesGetter = Value::undefined();
    Value lastInput = Value::undefined();
    std::vector<std::int32_t> lastCaptures;
};

namespace {

constexpr double kMaxLength = 9007199254740991.0;
constexpr std::size_t kMaxCachedPrograms = 1024;

RegExpState& stateOf(Runtime& rt) { return *rt.regexp; }

Value objectValue(DynObject* o) { return detail::Access::make(Type::Object, o); }
Value symbolValue(SymbolCell* s) { return detail::Access::make(Type::Symbol, s); }

double toLength(Runtime& rt, const Value& v)
{
    const double d = toNumber(rt, v);
    if (std::isnan(d) || d <= 0)
        return 0;
    return std::min(std::trunc(d), kMaxLength);
}

/// Set(o, key, v, true): a property that will not take the value is an error here, whatever the mode of the caller.
void setOrThrow(Runtime& rt, const Value& target, GCObject* key, const Value& v)
{
    if (DynObject* o = asObject(target)) {
        if (const auto own = o->findOwn(key)) {
            if (!o->accessorOf(*own) && !(o->attrsOf(*own) & PropWritable))
                throwError(rt, ErrorCode::TypeError, "cannot assign to a read-only property");
        }
    }
    setProperty(rt, target, key, v);
}

/// GetMethod: undefined and null are no method, anything else that cannot be called is an error.
Value methodOf(Runtime& rt, const Value& base, GCObject* key)
{
    Value f = getProperty(rt, base, key);
    if (f.isNullish())
        return Value::undefined();
    if (!asFunction(f))
        throwError(rt, ErrorCode::TypeError, "a well-known method is not a function");
    return f;
}

bool isRegExpLike(Runtime& rt, const Value& v)
{
    if (!asObject(v))
        return false;
    const Value matcher = getProperty(rt, v, rt.sym.match.get());
    if (!matcher.isUndefined())
        return toBoolean(matcher);
    return asObject(v)->cls == ObjClass::RegExp;
}

std::size_t advanceIndex(StringView s, double index, bool pairs)
{
    if (index >= kMaxLength)
        return std::size_t(kMaxLength);
    const std::size_t i = std::size_t(index);
    if (pairs && i + 1 < s.size() && s[i] >= 0xd800 && s[i] <= 0xdbff && s[i + 1] >= 0xdc00 && s[i + 1] <= 0xdfff)
        return i + 2;
    return i + 1;
}

std::shared_ptr<const regex::Program> programFor(Runtime& rt, StringView source, unsigned flags, std::string& error)
{
    RegExpState& state = stateOf(rt);
    std::u16string key;
    key.reserve(source.size() + 1);
    key.push_back(char16_t(flags));
    key.append(source);
    auto hit = state.programs.find(key);
    if (hit != state.programs.end())
        return hit->second;
    std::shared_ptr<const regex::Program> program = regex::compile(source, flags, error);
    if (!program)
        return nullptr;
    if (state.programs.size() >= kMaxCachedPrograms)
        state.programs.clear();
    state.programs.emplace(std::move(key), program);
    return program;
}

DynRegExp* allocate(Runtime& rt, DynObject* proto)
{
    auto* re = rt.newCell<DynRegExp>();
    re->cls = ObjClass::RegExp;
    re->setProto(proto ? proto : rt.regExpProto.get());
    re->putOwn(stateOf(rt).lastIndex.get(), Value(0.0), PropWritable);
    return re;
}

void initialize(Runtime& rt, DynRegExp* re, const Value& pattern, const Value& flags)
{
    const String source = pattern.isUndefined() ? String() : toStringValue(rt, pattern);
    const String flagText = flags.isUndefined() ? String() : toStringValue(rt, flags);
    unsigned bits = 0;
    if (!regex::parseFlags(StringView(flagText), bits))
        throwError(rt, ErrorCode::SyntaxError, "invalid regular expression flags");
    std::string error;
    std::shared_ptr<const regex::Program> program = programFor(rt, source, bits, error);
    if (!program)
        throwError(rt, ErrorCode::SyntaxError, "invalid regular expression: " + error);
    re->source = rt.str(source);
    re->flagText = rt.str(flagText);
    re->flags = bits;
    re->program = std::move(program);
    setOrThrow(rt, objectValue(re), stateOf(rt).lastIndex.get(), Value(0.0));
}

/// Nothing a program could have changed stands between this object and the engine: its own table holds only `lastIndex`,
/// and the prototype's methods are the ones it started with.
bool isPristine(Runtime& rt, DynObject* o)
{
    RegExpState& state = stateOf(rt);
    DynObject* proto = rt.regExpProto.get();
    if (!o || o->cls != ObjClass::RegExp || o->proto.get() != proto || o->props.size() != 1)
        return false;
    if (state.watchedAt != rt.ctx.protoEpoch) {
        for (int i = 0; i < RegExpState::kWatched; ++i) {
            const Prop* own = proto->findOwn(state.watchedKeys[i].get());
            state.watchedSlots[i] = own ? std::int32_t(own - &proto->props[0]) : -1;
        }
        state.watchedAt = rt.ctx.protoEpoch;
    }
    for (int i = 0; i < RegExpState::kWatched; ++i) {
        const std::int32_t slot = state.watchedSlots[i];
        if (slot < 0 || detail::Access::ptr(proto->props[std::size_t(slot)].value) != detail::Access::ptr(state.watched[i]))
            return false;
    }
    return true;
}

/// `lastIndex` is always the first property of a regular expression: it was put there first and cannot be deleted.
double readLastIndex(Runtime& rt, DynRegExp* re)
{
    const Value& v = re->props[0].value;
    if (v.isDouble()) {
        const double d = detail::Access::r8(v);
        if (d >= 0 && d <= kMaxLength && d == std::floor(d))
            return d;
    }
    return toLength(rt, v);
}

void writeLastIndex(Runtime& rt, DynRegExp* re, double value)
{
    if (re->frozen || !(re->attrsAt(0) & PropWritable))
        throwError(rt, ErrorCode::TypeError, "cannot assign to a read-only lastIndex");
    re->props[0].value = Value(value);
}

/// ToString of an argument, kept as the value it is when it is one already.
Value stringArgument(Runtime& rt, const Value& v)
{
    return v.isString() ? v : rt.str(toStringValue(rt, v));
}

void pollMatch(void* context, std::size_t bytes) { checkLongLoopSlow(*static_cast<Runtime*>(context), bytes); }

bool runMatch(Runtime& rt, DynRegExp* re, StringView s, std::size_t start)
{
    RegExpState& state = stateOf(rt);
    state.matcher.poll = pollMatch;
    state.matcher.pollContext = &rt;
    return regex::exec(*re->program, s, start, state.matcher);
}

void rememberMatch(RegExpState& state, DynRegExp* re, const Value& input)
{
    const std::int32_t* regs = state.matcher.registers.data();
    state.lastInput = input;
    state.lastCaptures.assign(regs, regs + std::min<std::uint32_t>(regex::groupCount(*re->program), 10) * 2);
}

/// The source as a literal would have to write it.
String escapedSource(StringView source)
{
    if (source.empty())
        return ascii("(?:)");
    String out;
    out.reserve(source.size());
    bool inClass = false;
    const auto lineEnd = [&](Char c) {
        switch (c) {
            case 10: out += ascii("n"); return true;
            case 13: out += ascii("r"); return true;
            case 0x2028: out += ascii("u2028"); return true;
            case 0x2029: out += ascii("u2029"); return true;
            default: return false;
        }
    };
    for (std::size_t i = 0; i < source.size(); ++i) {
        const Char c = source[i];
        if (c == '\\' && i + 1 < source.size()) {
            out.push_back(c);
            ++i;
            if (!lineEnd(source[i]))
                out.push_back(source[i]);
            continue;
        }
        if (c == '[')
            inClass = true;
        else if (c == ']')
            inClass = false;
        if (c == '/' && !inClass) {
            out += ascii("\\/");
            continue;
        }
        if (c == 10 || c == 13 || c == 0x2028 || c == 0x2029) {
            out.push_back('\\');
            lineEnd(c);
            continue;
        }
        out.push_back(c);
    }
    return out;
}

/// RegExpBuiltinExec: null, or the array a match makes.
Value builtinExec(Runtime& rt, DynRegExp* re, const Value& input)
{
    RegExpState& state = stateOf(rt);
    const Value self = objectValue(re);
    const StringView s = asString(input)->text();
    double lastIndex = readLastIndex(rt, re);
    const bool tracks = re->flags & (regex::kGlobal | regex::kSticky);
    if (!tracks)
        lastIndex = 0;
    if (lastIndex > double(s.size())) {
        writeLastIndex(rt, re, 0);
        return Value::null();
    }
    if (!runMatch(rt, re, s, std::size_t(lastIndex))) {
        if (tracks)
            writeLastIndex(rt, re, 0);
        return Value::null();
    }
    const std::int32_t* regs = state.matcher.registers.data();
    const std::uint32_t groups = regex::groupCount(*re->program);
    if (tracks)
        writeLastIndex(rt, re, double(regs[1]));
    rememberMatch(state, re, input);

    DynArray* result = rt.newArray(groups);
    Value handle = detail::Access::adopt(Type::Object, result);
    const auto& names = regex::namedGroups(*re->program);
    for (std::uint32_t i = 0; i < groups; ++i) {
        if (regs[i * 2] < 0 || regs[i * 2 + 1] < 0)
            result->append(Value::undefined());
        else
            result->append(rt.str(s.substr(std::size_t(regs[i * 2]), std::size_t(regs[i * 2 + 1] - regs[i * 2]))));
    }
    result->putOwn(state.index.get(), Value(double(regs[0])));
    result->putOwn(state.input.get(), input);
    Value groupsObject = Value::undefined();
    if (!names.empty()) {
        DynObject* g = rt.newObject();
        g->setProto(nullptr);
        groupsObject = objectValue(g);
        for (const regex::NamedGroup& named : names) {
            Value found = Value::undefined();
            for (std::uint32_t number : named.groups) {
                if (regs[number * 2] >= 0)
                    found = getIndex(rt, handle, number);
            }
            g->putOwn(rt.intern(StringView(named.name)), found);
        }
    }
    result->putOwn(state.groups.get(), groupsObject);
    if (re->flags & regex::kHasIndices) {
        DynArray* indices = rt.newArray(groups);
        Value indicesHandle = detail::Access::adopt(Type::Object, indices);
        // Reading `regs` again below is safe: nothing here matches.
        const auto pairOf = [&](std::uint32_t i) {
            if (regs[i * 2] < 0 || regs[i * 2 + 1] < 0)
                return Value::undefined();
            return rt.makeArray({Value(double(regs[i * 2])), Value(double(regs[i * 2 + 1]))});
        };
        for (std::uint32_t i = 0; i < groups; ++i)
            indices->append(pairOf(i));
        Value indexGroups = Value::undefined();
        if (!names.empty()) {
            DynObject* g = rt.newObject();
            g->setProto(nullptr);
            indexGroups = objectValue(g);
            for (const regex::NamedGroup& named : names) {
                Value found = Value::undefined();
                for (std::uint32_t number : named.groups) {
                    if (regs[number * 2] >= 0)
                        found = pairOf(number);
                }
                g->putOwn(rt.intern(StringView(named.name)), found);
            }
        }
        indices->putOwn(state.groups.get(), indexGroups);
        result->putOwn(state.indices.get(), indicesHandle);
    }
    return handle;
}

/// RegExpExec: whatever `exec` the object has now.
Value regExpExec(Runtime& rt, const Value& rx, const Value& input)
{
    RegExpState& state = stateOf(rt);
    const Value exec = getProperty(rt, rx, state.exec_.get());
    DynFunction* fn = asFunction(exec);
    DynObject* object = asObject(rx);
    if (fn && !(fn == state.exec.get() && object && object->cls == ObjClass::RegExp)) {
        Value result = callValue(rt, exec, rx, {input});
        if (!result.isNull() && !asObject(result))
            throwError(rt, ErrorCode::TypeError, "exec must return an object or null");
        return result;
    }
    if (!object || object->cls != ObjClass::RegExp)
        throwError(rt, ErrorCode::TypeError, "not a RegExp");
    return builtinExec(rt, static_cast<DynRegExp*>(object), input);
}

DynObject* requireObject(CallInfo& ci, const char* what)
{
    DynObject* o = asObject(ci.thisValue());
    if (!o)
        ci.fail(ErrorCode::TypeError, std::string(what) + " called on something not an object");
    return o;
}

/// SpeciesConstructor.
Value speciesOf(Runtime& rt, const Value& object, const Value& fallback)
{
    const Value ctor = getProperty(rt, object, rt.atom.constructor.get());
    if (ctor.isUndefined())
        return fallback;
    if (!asObject(ctor))
        throwError(rt, ErrorCode::TypeError, "constructor is not an object");
    const Value species = getProperty(rt, ctor, rt.sym.species.get());
    if (species.isNullish())
        return fallback;
    DynFunction* fn = asFunction(species);
    if (!fn || !isConstructor(rt, fn))
        throwError(rt, ErrorCode::TypeError, "Symbol.species is not a constructor");
    return species;
}

/// GetSubstitution.
void substitute(Runtime& rt, String& out, StringView matched, StringView s, std::size_t position, const std::vector<Value>& captures, const Value& named, StringView tpl)
{
    const std::size_t m = captures.size();
    for (std::size_t i = 0; i < tpl.size(); ++i) {
        const Char c = tpl[i];
        if (c != '$' || i + 1 >= tpl.size()) {
            out.push_back(c);
            continue;
        }
        const Char d = tpl[i + 1];
        if (d == '$') {
            out.push_back('$');
            ++i;
        } else if (d == '&') {
            out += matched;
            ++i;
        } else if (d == '`') {
            out += s.substr(0, position);
            ++i;
        } else if (d == '\'') {
            const std::size_t tail = std::min(position + matched.size(), s.size());
            out += s.substr(tail);
            ++i;
        } else if (d >= '0' && d <= '9') {
            std::size_t digits = 1;
            std::size_t number = std::size_t(d - '0');
            if (i + 2 < tpl.size() && tpl[i + 2] >= '0' && tpl[i + 2] <= '9') {
                const std::size_t two = number * 10 + std::size_t(tpl[i + 2] - '0');
                if (two >= 1 && two <= m) {
                    number = two;
                    digits = 2;
                }
            }
            if (number >= 1 && number <= m) {
                const Value& capture = captures[number - 1];
                if (!capture.isUndefined())
                    out += toStringValue(rt, capture);
                i += digits;
            } else {
                out.push_back(c);
            }
        } else if (d == '<' && !named.isUndefined()) {
            const std::size_t close = tpl.find(u'>', i + 2);
            if (close == StringView::npos) {
                out.push_back(c);
            } else {
                const Value capture = getProperty(rt, named, rt.intern(tpl.substr(i + 2, close - i - 2)));
                if (!capture.isUndefined())
                    out += toStringValue(rt, capture);
                i = close;
            }
        } else {
            out.push_back(c);
        }
    }
}

/// `replace` of an object nothing has interfered with, by a string: the same steps, with no arrays made to carry each
/// match from the engine to here.
Value replaceDirectly(Runtime& rt, DynRegExp* re, const Value& input, const String& replacement)
{
    RegExpState& state = stateOf(rt);
    const StringView s = asString(input)->text();
    const bool global = re->flags & regex::kGlobal;
    const bool sticky = re->flags & regex::kSticky;
    const bool pairs = re->flags & (regex::kUnicode | regex::kUnicodeSets);
    const bool plain = replacement.find(u'$') == String::npos;
    const std::uint32_t groups = regex::groupCount(*re->program);
    const auto& names = regex::namedGroups(*re->program);
    double at = 0;
    if (!global) {
        at = sticky ? readLastIndex(rt, re) : 0;
        if (at > double(s.size())) {
            writeLastIndex(rt, re, 0);
            return input;
        }
    }
    String out;
    std::size_t copied = 0;
    bool any = false;
    std::size_t matches = 0;
    for (;;) {
        if (at > double(s.size()) || !runMatch(rt, re, s, std::size_t(at)))
            break;
        checkLongLoop(rt, ++matches, out.size() * sizeof(Char));
        any = true;
        const std::int32_t* regs = state.matcher.registers.data();
        const std::size_t begin = std::size_t(regs[0]);
        const std::size_t end = std::size_t(regs[1]);
        out += s.substr(copied, begin - copied);
        if (plain) {
            out += replacement;
        } else {
            std::vector<Value> captures;
            captures.reserve(groups - 1);
            for (std::uint32_t i = 1; i < groups; ++i) {
                if (regs[i * 2] < 0 || regs[i * 2 + 1] < 0)
                    captures.push_back(Value::undefined());
                else
                    captures.push_back(rt.str(s.substr(std::size_t(regs[i * 2]), std::size_t(regs[i * 2 + 1] - regs[i * 2]))));
            }
            Value named = Value::undefined();
            if (!names.empty()) {
                DynObject* g = rt.newObject();
                g->setProto(nullptr);
                named = objectValue(g);
                for (const regex::NamedGroup& group : names) {
                    Value found = Value::undefined();
                    for (std::uint32_t number : group.groups) {
                        if (regs[number * 2] >= 0)
                            found = captures[number - 1];
                    }
                    g->putOwn(rt.intern(StringView(group.name)), found);
                }
            }
            substitute(rt, out, s.substr(begin, end - begin), s, begin, captures, named, replacement);
            regs = state.matcher.registers.data();
        }
        copied = end;
        rememberMatch(state, re, input);
        if (!global) {
            if (sticky)
                writeLastIndex(rt, re, double(end));
            break;
        }
        at = end == begin ? double(advanceIndex(s, double(end), pairs)) : double(end);
        if (sticky && end == begin)
            at = double(advanceIndex(s, double(end), pairs));
    }
    if (global || (sticky && !any))
        writeLastIndex(rt, re, 0);
    if (!any)
        return input;
    out += s.substr(copied);
    return rt.str(out);
}

Value symbolReplace(CallInfo& ci)
{
    Runtime& rt = rtOf(ci);
    RegExpState& state = stateOf(rt);
    requireObject(ci, "RegExp.prototype[Symbol.replace]");
    const Value rx = ci.thisValue();
    const Value input = stringArgument(rt, ci.arg(0));
    const StringView s = asString(input)->text();
    Value replaceValue = ci.arg(1);
    const bool functional = asFunction(replaceValue) != nullptr;
    String replaceText;
    if (!functional)
        replaceText = toStringValue(rt, replaceValue);
    // Converting the replacement can have run anything: this is asked only now.
    if (DynObject* object = asObject(rx); !functional && isPristine(rt, object)) {
        auto* re = static_cast<DynRegExp*>(object);
        return replaceDirectly(rt, re, input, replaceText);
    }
    const String flags = toStringValue(rt, getProperty(rt, rx, state.flags.get()));
    const bool global = flags.find(u'g') != String::npos;
    const bool pairs = flags.find(u'u') != String::npos || flags.find(u'v') != String::npos;
    if (global)
        setOrThrow(rt, rx, state.lastIndex.get(), Value(0.0));
    std::vector<Value> results;
    for (;;) {
        Value result = regExpExec(rt, rx, input);
        if (result.isNull())
            break;
        results.push_back(result);
        if (!global)
            break;
        const String text = toStringValue(rt, getIndex(rt, result, 0));
        if (text.empty()) {
            const double at = toLength(rt, getProperty(rt, rx, state.lastIndex.get()));
            setOrThrow(rt, rx, state.lastIndex.get(), Value(double(advanceIndex(s, at, pairs))));
        }
    }
    String out;
    std::size_t nextPosition = 0;
    for (const Value& result : results) {
        const double groups = toLength(rt, getProperty(rt, result, rt.atom.length.get()));
        const std::size_t captureCount = groups > 1 ? std::size_t(groups) - 1 : 0;
        const String matched = toStringValue(rt, getIndex(rt, result, 0));
        const double rawPosition = toNumber(rt, getProperty(rt, result, state.index.get()));
        const double integer = std::isnan(rawPosition) ? 0 : std::trunc(rawPosition);
        const std::size_t position = std::size_t(std::max(std::min(integer, double(s.size())), 0.0));
        std::vector<Value> captures;
        for (std::size_t n = 1; n <= captureCount; ++n) {
            Value capture = getIndex(rt, result, std::uint32_t(n));
            if (!capture.isUndefined())
                capture = rt.str(toStringValue(rt, capture));
            captures.push_back(capture);
        }
        Value named = getProperty(rt, result, state.groups.get());
        String replacement;
        if (functional) {
            std::vector<Value> args;
            args.push_back(rt.str(matched));
            args.insert(args.end(), captures.begin(), captures.end());
            args.push_back(Value(double(position)));
            args.push_back(input);
            if (!named.isUndefined())
                args.push_back(named);
            replacement = toStringValue(rt, callValue(rt, replaceValue, Value::undefined(), args));
        } else {
            if (!named.isUndefined())
                named = toObject(rt, named);
            substitute(rt, replacement, matched, s, position, captures, named, replaceText);
        }
        if (position >= nextPosition) {
            out += s.substr(nextPosition, position - nextPosition);
            out += replacement;
            nextPosition = position + matched.size();
        }
    }
    if (nextPosition < s.size())
        out += s.substr(nextPosition);
    return rt.str(out);
}

Value symbolMatch(CallInfo& ci)
{
    Runtime& rt = rtOf(ci);
    RegExpState& state = stateOf(rt);
    requireObject(ci, "RegExp.prototype[Symbol.match]");
    const Value rx = ci.thisValue();
    const Value input = stringArgument(rt, ci.arg(0));
    if (DynObject* object = asObject(rx); isPristine(rt, object) && (object->attrsAt(0) & PropWritable) && !object->frozen) {
        auto* re = static_cast<DynRegExp*>(object);
        if (!(re->flags & regex::kGlobal))
            return builtinExec(rt, re, input);
        const StringView s = asString(input)->text();
        const bool pairs = re->flags & (regex::kUnicode | regex::kUnicodeSets);
        Vec<Value> found;
        double at = 0;
        std::size_t count = 0;
        while (at <= double(s.size()) && runMatch(rt, re, s, std::size_t(at))) {
            checkLongLoop(rt, ++count, count * sizeof(Value));
            const std::int32_t* regs = state.matcher.registers.data();
            const std::size_t begin = std::size_t(regs[0]);
            const std::size_t end = std::size_t(regs[1]);
            rememberMatch(state, re, input);
            found.push_back(rt.str(s.substr(begin, end - begin)));
            at = end == begin ? double(advanceIndex(s, double(end), pairs)) : double(end);
        }
        writeLastIndex(rt, re, 0);
        return found.empty() ? Value::null() : rt.makeArray(std::move(found));
    }
    const String flags = toStringValue(rt, getProperty(rt, rx, state.flags.get()));
    if (flags.find(u'g') == String::npos)
        return regExpExec(rt, rx, input);
    const bool pairs = flags.find(u'u') != String::npos || flags.find(u'v') != String::npos;
    setOrThrow(rt, rx, state.lastIndex.get(), Value(0.0));
    Vec<Value> found;
    for (;;) {
        Value result = regExpExec(rt, rx, input);
        if (result.isNull())
            break;
        const String text = toStringValue(rt, getIndex(rt, result, 0));
        found.push_back(rt.str(text));
        if (text.empty()) {
            const double at = toLength(rt, getProperty(rt, rx, state.lastIndex.get()));
            setOrThrow(rt, rx, state.lastIndex.get(), Value(double(advanceIndex(asString(input)->text(), at, pairs))));
        }
    }
    return found.empty() ? Value::null() : rt.makeArray(std::move(found));
}

Value symbolSearch(CallInfo& ci)
{
    Runtime& rt = rtOf(ci);
    RegExpState& state = stateOf(rt);
    requireObject(ci, "RegExp.prototype[Symbol.search]");
    const Value rx = ci.thisValue();
    const Value input = rt.str(toStringValue(rt, ci.arg(0)));
    const Value previous = getProperty(rt, rx, state.lastIndex.get());
    if (!sameValue(previous, Value(0.0)))
        setOrThrow(rt, rx, state.lastIndex.get(), Value(0.0));
    Value result = regExpExec(rt, rx, input);
    const Value current = getProperty(rt, rx, state.lastIndex.get());
    if (!sameValue(previous, current))
        setOrThrow(rt, rx, state.lastIndex.get(), previous);
    if (result.isNull())
        return Value(-1.0);
    return getProperty(rt, result, state.index.get());
}

/// `split` would make its copy with %RegExp% itself.
bool splitDefaults(Runtime& rt)
{
    RegExpState& state = stateOf(rt);
    const Value ctor = getProperty(rt, objectValue(rt.regExpProto.get()), rt.atom.constructor.get());
    if (asObject(ctor) != state.ctor.get())
        return false;
    const Prop* species = state.ctor->findOwn(rt.sym.species.get());
    return species && detail::Access::ptr(species->value) == detail::Access::ptr(state.speciesGetter);
}

Value symbolSplit(CallInfo& ci)
{
    Runtime& rt = rtOf(ci);
    RegExpState& state = stateOf(rt);
    requireObject(ci, "RegExp.prototype[Symbol.split]");
    const Value rx = ci.thisValue();
    const Value input = stringArgument(rt, ci.arg(0));
    const StringView s = asString(input)->text();
    // A copy with `y`, tried at each place in turn, finds what the object itself finds searching ahead, and that is quicker.
    if (DynObject* object = asObject(rx); isPristine(rt, object) && !(static_cast<DynRegExp*>(object)->flags & regex::kSticky) && !s.empty() && splitDefaults(rt)) {
        auto* re = static_cast<DynRegExp*>(object);
        const bool pairs = re->flags & (regex::kUnicode | regex::kUnicodeSets);
        const std::uint32_t limit = ci.arg(1).isUndefined() ? 0xffffffffu : toUint32(toNumber(rt, ci.arg(1)));
        Vec<Value> parts;
        if (limit == 0)
            return rt.makeArray(std::move(parts));
        const std::uint32_t groups = regex::groupCount(*re->program);
        std::size_t p = 0;
        std::size_t q = 0;
        std::size_t count = 0;
        while (q < s.size() && runMatch(rt, re, s, q)) {
            checkLongLoop(rt, ++count, parts.size() * sizeof(Value));
            const std::int32_t* regs = state.matcher.registers.data();
            const std::size_t begin = std::size_t(regs[0]);
            const std::size_t end = std::min<std::size_t>(std::size_t(regs[1]), s.size());
            if (begin >= s.size())
                break;
            if (end == p || (end == begin && begin == p)) {
                q = advanceIndex(s, double(std::max(q, begin)), pairs);
                continue;
            }
            rememberMatch(state, re, input);
            parts.push_back(rt.str(s.substr(p, begin - p)));
            if (parts.size() == limit)
                return rt.makeArray(std::move(parts));
            p = end;
            for (std::uint32_t i = 1; i < groups; ++i) {
                regs = state.matcher.registers.data();
                if (regs[i * 2] < 0 || regs[i * 2 + 1] < 0)
                    parts.push_back(Value::undefined());
                else
                    parts.push_back(rt.str(s.substr(std::size_t(regs[i * 2]), std::size_t(regs[i * 2 + 1] - regs[i * 2]))));
                if (parts.size() == limit)
                    return rt.makeArray(std::move(parts));
            }
            q = p;
        }
        parts.push_back(rt.str(s.substr(p)));
        return rt.makeArray(std::move(parts));
    }
    const Value ctor = speciesOf(rt, rx, objectValue(state.ctor.get()));
    const String flags = toStringValue(rt, getProperty(rt, rx, state.flags.get()));
    const bool pairs = flags.find(u'u') != String::npos || flags.find(u'v') != String::npos;
    String newFlags = flags;
    if (flags.find(u'y') == String::npos)
        newFlags.push_back(u'y');
    const Value splitter = constructValue(rt, ctor, {rx, rt.str(newFlags)});
    Vec<Value> parts;
    const std::uint32_t limit = ci.arg(1).isUndefined() ? 0xffffffffu : toUint32(toNumber(rt, ci.arg(1)));
    if (limit == 0)
        return rt.makeArray(std::move(parts));
    if (s.empty()) {
        if (regExpExec(rt, splitter, input).isNull())
            parts.push_back(input);
        return rt.makeArray(std::move(parts));
    }
    std::size_t p = 0;
    std::size_t q = 0;
    while (q < s.size()) {
        setOrThrow(rt, splitter, state.lastIndex.get(), Value(double(q)));
        const Value z = regExpExec(rt, splitter, input);
        if (z.isNull()) {
            q = advanceIndex(s, double(q), pairs);
            continue;
        }
        const double e = std::min(toLength(rt, getProperty(rt, splitter, state.lastIndex.get())), double(s.size()));
        if (std::size_t(e) == p) {
            q = advanceIndex(s, double(q), pairs);
            continue;
        }
        parts.push_back(rt.str(s.substr(p, q - p)));
        if (parts.size() == limit)
            return rt.makeArray(std::move(parts));
        p = std::size_t(e);
        const double groups = toLength(rt, getProperty(rt, z, rt.atom.length.get()));
        const std::size_t captureCount = groups > 1 ? std::size_t(groups) - 1 : 0;
        for (std::size_t i = 1; i <= captureCount; ++i) {
            parts.push_back(getIndex(rt, z, std::uint32_t(i)));
            if (parts.size() == limit)
                return rt.makeArray(std::move(parts));
        }
        q = p;
    }
    parts.push_back(rt.str(s.substr(p)));
    return rt.makeArray(std::move(parts));
}

DynRegExpIterator* thisStringIterator(CallInfo& ci)
{
    DynObject* o = asObject(ci.thisValue());
    if (!o || o->cls != ObjClass::RegExpStringIterator)
        ci.fail(ErrorCode::TypeError, "not a RegExp String Iterator");
    return static_cast<DynRegExpIterator*>(o);
}

Value symbolMatchAll(CallInfo& ci)
{
    Runtime& rt = rtOf(ci);
    RegExpState& state = stateOf(rt);
    requireObject(ci, "RegExp.prototype[Symbol.matchAll]");
    const Value rx = ci.thisValue();
    const Value input = rt.str(toStringValue(rt, ci.arg(0)));
    const Value ctor = speciesOf(rt, rx, objectValue(state.ctor.get()));
    const String flags = toStringValue(rt, getProperty(rt, rx, state.flags.get()));
    const Value matcher = constructValue(rt, ctor, {rx, rt.str(flags)});
    const double last = toLength(rt, getProperty(rt, rx, state.lastIndex.get()));
    setOrThrow(rt, matcher, state.lastIndex.get(), Value(last));
    auto* it = rt.newCell<DynRegExpIterator>();
    it->cls = ObjClass::RegExpStringIterator;
    it->setProto(rt.regExpStringIteratorProto.get());
    it->regexp = matcher;
    it->input = input;
    it->global = flags.find(u'g') != String::npos;
    it->pairs = flags.find(u'u') != String::npos || flags.find(u'v') != String::npos;
    return detail::Access::adopt(Type::Object, it);
}

Value flagGetter(CallInfo& ci, unsigned bit, const char* name)
{
    Runtime& rt = rtOf(ci);
    DynObject* o = asObject(ci.thisValue());
    if (!o)
        ci.fail(ErrorCode::TypeError, std::string("RegExp.prototype.") + name + " called on something not an object");
    if (o->cls == ObjClass::RegExp)
        return Value((static_cast<DynRegExp*>(o)->flags & bit) != 0);
    if (o == rt.regExpProto.get())
        return Value::undefined();
    ci.fail(ErrorCode::TypeError, std::string("RegExp.prototype.") + name + " called on something not a RegExp");
}

void installStringMethods(Runtime& rt)
{
    DynObject* proto = rt.stringProto.get();
    const auto coercible = [](CallInfo& ci) {
        if (ci.thisValue().isNullish())
            ci.fail(ErrorCode::TypeError, "String.prototype method called on null or undefined");
    };
    /// `this[symbol]` of the argument, called if there is one.
    // A string is asked nothing, so no argument list is made for one.
    const auto delegated = [](Runtime& rt2, const Value& target, GCObject* key, const Value& first, const Value& second, bool two, Value& result) {
        if (!asObject(target))
            return false;
        const Value method = methodOf(rt2, target, key);
        if (method.isUndefined())
            return false;
        result = two ? callValue(rt2, method, target, {first, second}) : callValue(rt2, method, target, {first});
        return true;
    };

    rt.method(proto, "match", [coercible, delegated](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        coercible(ci);
        Value result;
        if (delegated(rt2, ci.arg(0), rt2.sym.match.get(), ci.thisValue(), Value::undefined(), false, result))
            return result;
        const Value text = rt2.str(toStringValue(rt2, ci.thisValue()));
        const Value rx = constructValue(rt2, objectValue(stateOf(rt2).ctor.get()), {ci.arg(0)});
        return invokeMethod(rt2, rx, rt2.sym.match.get(), {text});
    }, 1);

    rt.method(proto, "matchAll", [coercible, delegated](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        coercible(ci);
        const Value pattern = ci.arg(0);
        if (!pattern.isNullish()) {
            if (isRegExpLike(rt2, pattern)) {
                const Value flags = getProperty(rt2, pattern, stateOf(rt2).flags.get());
                if (flags.isNullish())
                    ci.fail(ErrorCode::TypeError, "String.prototype.matchAll: flags are null or undefined");
                if (toStringValue(rt2, flags).find(u'g') == String::npos)
                    ci.fail(ErrorCode::TypeError, "String.prototype.matchAll needs a global regular expression");
            }
            Value result;
            if (delegated(rt2, pattern, rt2.sym.matchAll.get(), ci.thisValue(), Value::undefined(), false, result))
                return result;
        }
        const Value text = rt2.str(toStringValue(rt2, ci.thisValue()));
        const Value rx = constructValue(rt2, objectValue(stateOf(rt2).ctor.get()), {pattern, rt2.str("g")});
        return invokeMethod(rt2, rx, rt2.sym.matchAll.get(), {text});
    }, 1);

    rt.method(proto, "search", [coercible, delegated](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        coercible(ci);
        Value result;
        if (delegated(rt2, ci.arg(0), rt2.sym.search.get(), ci.thisValue(), Value::undefined(), false, result))
            return result;
        const Value text = rt2.str(toStringValue(rt2, ci.thisValue()));
        const Value rx = constructValue(rt2, objectValue(stateOf(rt2).ctor.get()), {ci.arg(0)});
        return invokeMethod(rt2, rx, rt2.sym.search.get(), {text});
    }, 1);

    rt.method(proto, "split", [coercible, delegated](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        coercible(ci);
        Value result;
        if (delegated(rt2, ci.arg(0), rt2.sym.split.get(), ci.thisValue(), ci.arg(1), true, result))
            return result;
        const Value text = stringArgument(rt2, ci.thisValue());
        const StringView s = asString(text)->text();
        const std::uint32_t limit = ci.arg(1).isUndefined() ? 0xffffffffu : toUint32(toNumber(rt2, ci.arg(1)));
        const String sep = toStringValue(rt2, ci.arg(0));
        Vec<Value> parts;
        if (limit == 0)
            return rt2.makeArray(std::move(parts));
        if (ci.arg(0).isUndefined()) {
            parts.push_back(text);
            return rt2.makeArray(std::move(parts));
        }
        if (sep.empty()) {
            const std::size_t count = std::min<std::size_t>(s.size(), limit);
            for (std::size_t i = 0; i < count; ++i) {
                checkLongLoop(rt2, i, i * sizeof(Value));
                parts.push_back(rt2.str(s.substr(i, 1)));
            }
            return rt2.makeArray(std::move(parts));
        }
        std::size_t at = 0;
        for (;;) {
            const std::size_t hit = s.find(StringView(sep), at);
            if (hit == StringView::npos) {
                parts.push_back(rt2.str(s.substr(at)));
                break;
            }
            parts.push_back(rt2.str(s.substr(at, hit - at)));
            if (parts.size() >= limit)
                break;
            at = hit + sep.size();
        }
        return rt2.makeArray(std::move(parts));
    }, 2);

    const auto replacePlain = [](CallInfo& ci, bool all) -> Value {
        Runtime& rt2 = rtOf(ci);
        const Value text = rt2.str(toStringValue(rt2, ci.thisValue()));
        const StringView s = asString(text)->text();
        const String needle = toStringValue(rt2, ci.arg(0));
        const Value replaceValue = ci.arg(1);
        const bool functional = asFunction(replaceValue) != nullptr;
        String replaceText;
        if (!functional)
            replaceText = toStringValue(rt2, replaceValue);
        const std::size_t step = std::max<std::size_t>(needle.size(), 1);
        std::vector<std::size_t> hits;
        std::size_t hit = s.find(StringView(needle), 0);
        while (hit != StringView::npos) {
            hits.push_back(hit);
            if (!all || hit + step > s.size())
                break;
            hit = s.find(StringView(needle), hit + step);
        }
        if (hits.empty())
            return text;
        String out;
        std::size_t end = 0;
        for (std::size_t at : hits) {
            out += s.substr(end, at - end);
            if (functional) {
                out += toStringValue(rt2, callValue(rt2, replaceValue, Value::undefined(), {rt2.str(needle), Value(double(at)), text}));
            } else {
                substitute(rt2, out, needle, s, at, {}, Value::undefined(), replaceText);
            }
            end = at + needle.size();
        }
        if (end < s.size())
            out += s.substr(end);
        return rt2.str(out);
    };

    rt.method(proto, "replace", [coercible, delegated, replacePlain](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        coercible(ci);
        Value result;
        if (delegated(rt2, ci.arg(0), rt2.sym.replace.get(), ci.thisValue(), ci.arg(1), true, result))
            return result;
        return replacePlain(ci, false);
    }, 2);

    rt.method(proto, "replaceAll", [coercible, delegated, replacePlain](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        coercible(ci);
        const Value pattern = ci.arg(0);
        if (!pattern.isNullish()) {
            if (isRegExpLike(rt2, pattern)) {
                const Value flags = getProperty(rt2, pattern, stateOf(rt2).flags.get());
                if (flags.isNullish())
                    ci.fail(ErrorCode::TypeError, "String.prototype.replaceAll: flags are null or undefined");
                if (toStringValue(rt2, flags).find(u'g') == String::npos)
                    ci.fail(ErrorCode::TypeError, "String.prototype.replaceAll needs a global regular expression");
            }
            Value result;
            if (delegated(rt2, pattern, rt2.sym.replace.get(), ci.thisValue(), ci.arg(1), true, result))
                return result;
        }
        return replacePlain(ci, true);
    }, 2);
}

/// The old statics: `RegExp.$1`, `RegExp.lastMatch` and so on, of the last match anywhere.
void installLegacyStatics(Runtime& rt, DynFunction* ctor)
{
    const auto statics = [&](const char* name, const char* shortName, std::function<Value(Runtime&, StringView, const std::vector<std::int32_t>&)> read) {
        NativeFunction getter = [read](CallInfo& ci) -> Value {
            Runtime& rt2 = rtOf(ci);
            RegExpState& state = stateOf(rt2);
            if (asObject(ci.thisValue()) != state.ctor.get())
                ci.fail(ErrorCode::TypeError, "a legacy RegExp static read from something other than RegExp");
            if (state.lastInput.isUndefined())
                return rt2.str("");
            return read(rt2, asString(state.lastInput)->text(), state.lastCaptures);
        };
        const auto define = [&](const char* key) {
            const Value get = rt.makeFunction(ascii("get ") + fromUtf8(key), getter, 0);
            ctor->putOwn(rt.intern(key), get, PropConfigurable | PropAccessor);
        };
        define(name);
        if (shortName)
            define(shortName);
    };
    for (int i = 1; i <= 9; ++i) {
        const std::string name = "$" + std::to_string(i);
        statics(name.c_str(), nullptr, [i](Runtime& rt2, StringView s, const std::vector<std::int32_t>& c) {
            const std::size_t n = std::size_t(i) * 2;
            if (n + 1 >= c.size() || c[n] < 0 || c[n + 1] < 0)
                return rt2.str("");
            return rt2.str(s.substr(std::size_t(c[n]), std::size_t(c[n + 1] - c[n])));
        });
    }
    statics("lastMatch", "$&", [](Runtime& rt2, StringView s, const std::vector<std::int32_t>& c) { return rt2.str(s.substr(std::size_t(c[0]), std::size_t(c[1] - c[0]))); });
    statics("lastParen", "$+", [](Runtime& rt2, StringView s, const std::vector<std::int32_t>& c) {
        const std::size_t n = c.size() - 2;
        if (n == 0 || c[n] < 0 || c[n + 1] < 0)
            return rt2.str("");
        return rt2.str(s.substr(std::size_t(c[n]), std::size_t(c[n + 1] - c[n])));
    });
    statics("leftContext", "$`", [](Runtime& rt2, StringView s, const std::vector<std::int32_t>& c) { return rt2.str(s.substr(0, std::size_t(c[0]))); });
    statics("rightContext", "$'", [](Runtime& rt2, StringView s, const std::vector<std::int32_t>& c) { return rt2.str(s.substr(std::size_t(c[1]))); });
    statics("input", "$_", [](Runtime& rt2, StringView s, const std::vector<std::int32_t>&) { return rt2.str(s); });
}

}  // namespace

void releaseRegExpState(Runtime& rt) noexcept { rt.regexp.reset(); }

Value makeRegExpTemplate(Runtime& rt, StringView pattern, StringView flags)
{
    DynRegExp* re = allocate(rt, nullptr);
    Value handle = detail::Access::adopt(Type::Object, re);
    initialize(rt, re, rt.str(pattern), rt.str(flags));
    return handle;
}

DynObject* newRegExpFromTemplate(Runtime& rt, DynObject* templateObject)
{
    const auto* from = static_cast<DynRegExp*>(templateObject);
    DynRegExp* re = allocate(rt, nullptr);
    re->source = from->source;
    re->flagText = from->flagText;
    re->flags = from->flags;
    re->program = from->program;
    return re;
}

void installRegExp(Runtime& rt)
{
    rt.regexp = std::make_shared<RegExpState>();
    RegExpState& state = *rt.regexp;
    state.lastIndex.reset(rt.intern("lastIndex"));
    state.index.reset(rt.intern("index"));
    state.input.reset(rt.intern("input"));
    state.groups.reset(rt.intern("groups"));
    state.indices.reset(rt.intern("indices"));
    state.flags.reset(rt.intern("flags"));
    state.source.reset(rt.intern("source"));
    state.exec_.reset(rt.intern("exec"));

    DynObject* proto = rt.newObject(rt.objectProto.get());
    rt.regExpProto.reset(proto, true);

    DynFunction* ctor = rt.makeConstructor("RegExp", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        RegExpState& state2 = stateOf(rt2);
        const Value pattern = ci.arg(0);
        const Value flags = ci.arg(1);
        const bool like = isRegExpLike(rt2, pattern);
        if (!ci.isConstructorCall() && like && flags.isUndefined()) {
            const Value patternCtor = getProperty(rt2, pattern, rt2.atom.constructor.get());
            if (asObject(patternCtor) == state2.ctor.get())
                return pattern;
        }
        Value source, flagValue;
        DynObject* object = asObject(pattern);
        if (object && object->cls == ObjClass::RegExp) {
            const auto* from = static_cast<DynRegExp*>(object);
            source = from->source;
            flagValue = flags.isUndefined() ? from->flagText : flags;
        } else if (like) {
            source = getProperty(rt2, pattern, state2.source.get());
            flagValue = flags.isUndefined() ? getProperty(rt2, pattern, state2.flags.get()) : flags;
        } else {
            source = pattern;
            flagValue = flags;
        }
        DynRegExp* re = allocate(rt2, nullptr);
        Value handle = detail::Access::adopt(Type::Object, re);
        initialize(rt2, re, source, flagValue);
        return handle;
    }, 2, proto);
    state.ctor.reset(ctor);
    state.speciesGetter = rt.makeFunction(ascii("get [Symbol.species]"), [](CallInfo& ci) { return ci.thisValue(); }, 0);
    ctor->putOwn(rt.sym.species.get(), state.speciesGetter, PropConfigurable | PropAccessor);

    rt.method(ctor, "escape", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        StringCell* text = asString(ci.arg(0));
        if (!text)
            ci.fail(ErrorCode::TypeError, "RegExp.escape takes a string");
        const StringView s = text->text();
        String out;
        const auto hex = [&](char32_t c, int digits, const char* prefix) {
            static const char kHex[] = "0123456789abcdef";
            out += ascii(prefix);
            for (int i = digits - 1; i >= 0; --i)
                out.push_back(Char(kHex[(c >> (i * 4)) & 15]));
        };
        for (std::size_t i = 0; i < s.size(); ++i) {
            const Char c = s[i];
            const bool asciiWord = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
            if (i == 0 && asciiWord) {
                hex(c, 2, "\\x");
            } else if (std::u16string_view(u"^$\\.*+?()[]{}|/").find(c) != std::u16string_view::npos) {
                out.push_back('\\');
                out.push_back(c);
            } else if (c == 9) {
                out += ascii("\\t");
            } else if (c == 10) {
                out += ascii("\\n");
            } else if (c == 11) {
                out += ascii("\\v");
            } else if (c == 12) {
                out += ascii("\\f");
            } else if (c == 13) {
                out += ascii("\\r");
            } else if (std::u16string_view(u",-=<>#&!%:;@~'`\"").find(c) != std::u16string_view::npos || c == ' ') {
                hex(c, 2, "\\x");
            } else if (c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000 || c == 0xfeff) {
                if (c <= 0xff)
                    hex(c, 2, "\\x");
                else
                    hex(c, 4, "\\u");
            } else if (c >= 0xd800 && c <= 0xdfff) {
                const bool pair = c <= 0xdbff && i + 1 < s.size() && s[i + 1] >= 0xdc00 && s[i + 1] <= 0xdfff;
                if (pair) {
                    out.push_back(c);
                    out.push_back(s[++i]);
                } else {
                    hex(c, 4, "\\u");
                }
            } else {
                out.push_back(c);
            }
        }
        return rt2.str(out);
    }, 1);

    Value exec = rt.makeFunction(ascii("exec"), [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynObject* o = asObject(ci.thisValue());
        if (!o || o->cls != ObjClass::RegExp)
            ci.fail(ErrorCode::TypeError, "RegExp.prototype.exec called on something not a RegExp");
        const Value input = stringArgument(rt2, ci.arg(0));
        return builtinExec(rt2, static_cast<DynRegExp*>(o), input);
    }, 1);
    state.exec.reset(asFunction(exec));
    proto->putOwn(rt.intern("exec"), exec, PropWritable | PropConfigurable);

    rt.method(proto, "test", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        requireObject(ci, "RegExp.prototype.test");
        const Value input = stringArgument(rt2, ci.arg(0));
        if (DynObject* object = asObject(ci.thisValue()); isPristine(rt2, object)) {
            auto* re = static_cast<DynRegExp*>(object);
            const bool tracks = re->flags & (regex::kGlobal | regex::kSticky);
            const StringView s = asString(input)->text();
            const double at = tracks ? readLastIndex(rt2, re) : 0;
            if (at > double(s.size()) || !runMatch(rt2, re, s, std::size_t(at))) {
                if (tracks)
                    writeLastIndex(rt2, re, 0);
                return Value(false);
            }
            if (tracks)
                writeLastIndex(rt2, re, double(stateOf(rt2).matcher.registers[1]));
            rememberMatch(stateOf(rt2), re, input);
            return Value(true);
        }
        return Value(!regExpExec(rt2, ci.thisValue(), input).isNull());
    }, 1);

    rt.method(proto, "toString", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        requireObject(ci, "RegExp.prototype.toString");
        String out = ascii("/");
        out += toStringValue(rt2, getProperty(rt2, ci.thisValue(), stateOf(rt2).source.get()));
        out += ascii("/");
        out += toStringValue(rt2, getProperty(rt2, ci.thisValue(), stateOf(rt2).flags.get()));
        return rt2.str(out);
    }, 0);

    rt.method(proto, "compile", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynObject* o = asObject(ci.thisValue());
        if (!o || o->cls != ObjClass::RegExp)
            ci.fail(ErrorCode::TypeError, "RegExp.prototype.compile called on something not a RegExp");
        auto* re = static_cast<DynRegExp*>(o);
        Value source = ci.arg(0);
        Value flags = ci.arg(1);
        DynObject* other = asObject(source);
        if (other && other->cls == ObjClass::RegExp) {
            if (!flags.isUndefined())
                ci.fail(ErrorCode::TypeError, "RegExp.prototype.compile: flags given with a RegExp");
            flags = static_cast<DynRegExp*>(other)->flagText;
            source = static_cast<DynRegExp*>(other)->source;
        }
        initialize(rt2, re, source, flags);
        return ci.thisValue();
    }, 2);

    rt.getter(proto, "flags", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        requireObject(ci, "RegExp.prototype.flags");
        const Value self = ci.thisValue();
        String out;
        static const struct { const char* name; char letter; } kFlags[] = {
            {"hasIndices", 'd'}, {"global", 'g'}, {"ignoreCase", 'i'}, {"multiline", 'm'}, {"dotAll", 's'}, {"unicode", 'u'}, {"unicodeSets", 'v'}, {"sticky", 'y'},
        };
        for (const auto& flag : kFlags) {
            if (toBoolean(getProperty(rt2, self, rt2.intern(flag.name))))
                out.push_back(Char(flag.letter));
        }
        return rt2.str(out);
    });
    rt.getter(proto, "source", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynObject* o = asObject(ci.thisValue());
        if (o && o->cls == ObjClass::RegExp)
            return rt2.str(escapedSource(asString(static_cast<DynRegExp*>(o)->source)->text()));
        if (o == rt2.regExpProto.get())
            return rt2.str("(?:)");
        ci.fail(ErrorCode::TypeError, "RegExp.prototype.source called on something not a RegExp");
    });
    static const struct { const char* name; unsigned bit; } kFlagGetters[] = {
        {"hasIndices", regex::kHasIndices}, {"global", regex::kGlobal}, {"ignoreCase", regex::kIgnoreCase}, {"multiline", regex::kMultiline},
        {"dotAll", regex::kDotAll}, {"unicode", regex::kUnicode}, {"unicodeSets", regex::kUnicodeSets}, {"sticky", regex::kSticky},
    };
    for (const auto& getter : kFlagGetters) {
        const char* name = getter.name;
        const unsigned bit = getter.bit;
        rt.getter(proto, name, [name, bit](CallInfo& ci) { return flagGetter(ci, bit, name); });
    }

    const auto symbolMethod = [&](SymbolCell* symbol, const char* name, NativeFunction fn, int arity) {
        proto->putOwn(symbol, rt.makeFunction(fromUtf8(name), std::move(fn), arity), PropWritable | PropConfigurable);
    };
    symbolMethod(rt.sym.match.get(), "[Symbol.match]", symbolMatch, 1);
    symbolMethod(rt.sym.matchAll.get(), "[Symbol.matchAll]", symbolMatchAll, 1);
    symbolMethod(rt.sym.replace.get(), "[Symbol.replace]", symbolReplace, 2);
    symbolMethod(rt.sym.search.get(), "[Symbol.search]", symbolSearch, 1);
    symbolMethod(rt.sym.split.get(), "[Symbol.split]", symbolSplit, 2);

    DynObject* iteratorProto = rt.newObject(rt.iteratorProto.get());
    rt.regExpStringIteratorProto.reset(iteratorProto, true);
    iteratorProto->putOwn(rt.sym.toStringTag.get(), rt.str("RegExp String Iterator"), PropConfigurable);
    rt.method(iteratorProto, "next", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynRegExpIterator* it = thisStringIterator(ci);
        if (it->done)
            return iterResult(rt2, Value::undefined(), true);
        const Value keep = ci.thisValue();
        Value match = regExpExec(rt2, it->regexp, it->input);
        if (match.isNull()) {
            it->done = true;
            return iterResult(rt2, Value::undefined(), true);
        }
        if (!it->global) {
            it->done = true;
            return iterResult(rt2, match, false);
        }
        if (toStringValue(rt2, getIndex(rt2, match, 0)).empty()) {
            RegExpState& state = stateOf(rt2);
            const double at = toLength(rt2, getProperty(rt2, it->regexp, state.lastIndex.get()));
            setOrThrow(rt2, it->regexp, state.lastIndex.get(), Value(double(advanceIndex(asString(it->input)->text(), at, it->pairs))));
        }
        return iterResult(rt2, match, false);
    }, 0);

    // What the short ways assume, as it stands now.
    static const char* const kWatchedNames[RegExpState::kWatched] = {"exec", "flags", "hasIndices", "global", "ignoreCase", "multiline", "dotAll", "unicode", "unicodeSets", "sticky"};
    for (int i = 0; i < RegExpState::kWatched; ++i) {
        StringCell* key = rt.intern(kWatchedNames[i]);
        state.watchedKeys[i].reset(key);
        state.watched[i] = proto->findOwn(key)->value;
    }

    installStringMethods(rt);
    installLegacyStatics(rt, ctor);
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
