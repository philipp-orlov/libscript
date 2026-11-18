// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// RegExp over the shared matching engine (src/dyn/regexp.cpp), and the String methods that hand over to it.
#include <algorithm>
#include <map>
#include <mutex>

#include "script/internal/dyn/regexp.hpp"
#include <cstring>

#include "builtins_util.hpp"

namespace script {
namespace internal {
namespace vm {
namespace bi {

namespace regex = dyn::regex;

namespace {

struct RegExpData : NativeState
{
    std::shared_ptr<const regex::Program> program;
    String source;
    unsigned flags = 0;
    bool legacy = true;  ///< made by the RegExp constructor itself, so it may be recompiled
    regex::Matcher matcher;
};

std::shared_ptr<const regex::Program> programForShared(StringView source, unsigned flags, std::string& error);

/// The compiled program for a pattern: a literal in a loop makes a RegExp every time round, so a small direct-mapped cache of
/// this thread's answers is looked at first, which neither allocates nor locks.
std::shared_ptr<const regex::Program> programFor(StringView source, unsigned flags, std::string& error)
{
    struct Recent
    {
        std::u16string source;
        unsigned flags = 0;
        std::shared_ptr<const regex::Program> program;
    };
    static constexpr std::size_t kRecent = 64;
    thread_local Recent recent[kRecent];
    Recent& r = recent[(std::hash<StringView>()(source) ^ flags * 0x9E3779B1u) & (kRecent - 1)];
    if (r.program && r.flags == flags && StringView(r.source) == source)
        return r.program;
    auto program = programForShared(source, flags, error);
    if (program && source.size() <= 256) {
        r.source.assign(source);
        r.flags = flags;
        r.program = program;
    }
    return program;
}

std::shared_ptr<const regex::Program> programForShared(StringView source, unsigned flags, std::string& error)
{
    static std::mutex lock;
    static std::map<std::u16string, std::shared_ptr<const regex::Program>> cache;
    std::u16string key;
    key.push_back(char16_t(flags));
    key.append(source);
    {
        std::lock_guard<std::mutex> guard(lock);
        auto it = cache.find(key);
        if (it != cache.end())
            return it->second;
    }
    auto program = regex::compile(source, flags, error);
    if (!program)
        return nullptr;
    std::lock_guard<std::mutex> guard(lock);
    if (cache.size() >= 512)
        cache.clear();
    cache.emplace(std::move(key), program);
    return program;
}

RegExpData* dataOf(JSValue v)
{
    if (isObject(v) && asObject(v)->cls() == ObjClass::RegExp)
        if (Rare* r = asObject(v)->rare())
            return static_cast<RegExpData*>(r->ext);
    return nullptr;
}

RegExpData* thisRegExp(VM& vm, JSValue self, const char* method)
{
    if (RegExpData* d = dataOf(self))
        return d;
    typeError(vm, std::string("RegExp.prototype.") + method + " requires that 'this' be a RegExp object");
    return nullptr;
}

/// Set up `o` as a RegExp for `pattern` and `flags`; false with an exception pending.
bool initialize(VM& vm, JSObject* o, StringView pattern, StringView flagText)
{
    unsigned bits = 0;
    if (!regex::parseFlags(flagText, bits)) {
        vm.throwError(ErrorCode::SyntaxError, "Invalid regular expression flags");
        return false;
    }
    std::string error;
    auto program = programFor(pattern, bits, error);
    if (!program) {
        vm.throwError(ErrorCode::SyntaxError, "Invalid regular expression: /" + toUtf8(pattern) + "/: " + error);
        return false;
    }
    auto* data = new RegExpData();
    data->program = std::move(program);
    data->source = String(pattern);
    data->flags = bits;
    o->h.cls = std::uint8_t(ObjClass::RegExp);
    Rare& rare = o->rareOf(vm);
    delete rare.ext;
    rare.ext = data;
    if (!o->findOwn(vm, vm.atoms.lastIndex)) {
        o->putOwn(vm, vm.atoms.lastIndex, JSValue::int32(0), attr::kWritable);
        return true;
    }
    return setProperty(vm, toJSValue(o), vm.atoms.lastIndex, JSValue::int32(0), true);
}

/// ToLength of a value; false with an exception pending.
bool toLengthOf(VM& vm, JSValue v, double* out)
{
    double d = 0;
    if (!toNumber(vm, v, &d))
        return false;
    d = d != d ? 0 : std::trunc(d);
    *out = d <= 0 ? 0 : std::min(d, 9007199254740991.0);
    return true;
}

/// `ToLength(rx.lastIndex)`; false with an exception pending.
bool readLastIndex(VM& vm, JSObject* o, double* out)
{
    JSValue* slot = o->findOwn(vm, vm.atoms.lastIndex);
    return toLengthOf(vm, slot ? *slot : JSValue::undefined(), out);
}

/// `rx.lastIndex = v`, throwing when it cannot be written; false with the exception pending.
bool writeLastIndex(VM& vm, JSObject* o, double v)
{
    return setProperty(vm, toJSValue(o), vm.atoms.lastIndex, JSValue::boxNumber(v), true);
}

/// The capture bounds of the last successful match.
struct Bounds
{
    static constexpr std::size_t kInline = 20;  // a match with up to nine groups needs no allocation
    std::int32_t small[kInline];
    std::vector<std::int32_t> big;
    std::size_t n = 0;
    std::size_t size() const { return n; }
    std::int32_t* data() { return big.empty() ? small : big.data(); }
    const std::int32_t* data() const { return big.empty() ? small : big.data(); }
    std::int32_t operator[](std::size_t i) const { return data()[i]; }
    void assign(const std::int32_t* from, std::size_t count)
    {
        n = count;
        if (count <= kInline) {
            big.clear();
            for (std::size_t i = 0; i < count; ++i)
                small[i] = from[i];
        } else {
            big.assign(from, from + count);
        }
    }
};

struct Match
{
    Bounds bounds;  // two per group
    std::size_t groups() const { return bounds.size() / 2; }
    std::int32_t start() const { return bounds[0]; }
    std::int32_t end() const { return bounds[1]; }
};

bool runExec(RegExpData* d, StringView s, std::size_t start, Match* out)
{
    if (!regex::exec(*d->program, s, start, d->matcher))
        return false;
    const std::size_t n = regex::groupCount(*d->program);
    out->bounds.assign(d->matcher.registers.data(), 2 * n);
    return true;
}

JSValue captureString(VM& vm, StringView s, const Match& m, std::size_t g)
{
    if (m.bounds[2 * g] < 0)
        return JSValue::undefined();
    return makeString(vm, s.substr(std::size_t(m.bounds[2 * g]), std::size_t(m.bounds[2 * g + 1] - m.bounds[2 * g])));
}

/// The `groups` object of a match, or undefined when the pattern names none. Owned.
JSValue namedGroupsObject(VM& vm, RegExpData* d, StringView s, const Match& m)
{
    const auto& names = regex::namedGroups(*d->program);
    if (names.empty())
        return JSValue::undefined();
    JSObject* g = vm.newObject(static_cast<JSObject*>(nullptr));
    Handle keep = Handle::adopt(toJSValue(g));
    for (const regex::NamedGroup& ng : names) {
        JSValue v = JSValue::undefined();
        for (std::uint32_t group : ng.groups)
            if (m.bounds[2 * group] >= 0) {
                v = captureString(vm, s, m, group);
                break;
            }
        Key k = vm.internKey(StringView(ng.name));
        g->putOwn(vm, k, v, attr::kDefault);
        detail::decRef(k);
    }
    return keep.take();
}

/// The array a match makes. Owned.
JSValue matchArray(VM& vm, RegExpData* d, JSValue input, StringView s, const Match& m)
{
    JSArray* a = vm.newArray(vm.protos.array, 0, std::uint32_t(m.groups()));
    Handle keep = Handle::adopt(toJSValue(a));
    for (std::size_t g = 0; g < m.groups(); ++g)
        a->append(vm, captureString(vm, s, m, g));
    a->putOwn(vm, vm.atoms.index, JSValue::int32(m.start()), attr::kDefault);
    retain(input);
    a->putOwn(vm, vm.atoms.input, input, attr::kDefault);
    a->putOwn(vm, vm.atoms.groups, namedGroupsObject(vm, d, s, m), attr::kDefault);
    if (d->flags & regex::kHasIndices) {
        JSArray* idx = vm.newArray(vm.protos.array, 0, std::uint32_t(m.groups()));
        Handle idxKeep = Handle::adopt(toJSValue(idx));
        for (std::size_t g = 0; g < m.groups(); ++g) {
            if (m.bounds[2 * g] < 0) {
                idx->append(vm, JSValue::undefined());
                continue;
            }
            JSArray* pair = vm.newArray(vm.protos.array, 0, 2);
            pair->append(vm, JSValue::int32(m.bounds[2 * g]));
            pair->append(vm, JSValue::int32(m.bounds[2 * g + 1]));
            idx->append(vm, toJSValue(pair));
        }
        const auto& names = regex::namedGroups(*d->program);
        JSValue groupsValue = JSValue::undefined();
        if (!names.empty()) {
            JSObject* g = vm.newObject(static_cast<JSObject*>(nullptr));
            groupsValue = toJSValue(g);
            for (const regex::NamedGroup& ng : names) {
                JSValue v = JSValue::undefined();
                for (std::uint32_t group : ng.groups)
                    if (m.bounds[2 * group] >= 0) {
                        v = idx->at(group);
                        break;
                    }
                retain(v);
                Key k = vm.internKey(StringView(ng.name));
                g->putOwn(vm, k, v, attr::kDefault);
                detail::decRef(k);
            }
        }
        idx->putOwn(vm, vm.atoms.groups, groupsValue, attr::kDefault);
        retain(idxKeep.get());
        a->putOwn(vm, vm.atoms.indices, idxKeep.get(), attr::kDefault);
    }
    return keep.take();
}

/// What `RegExp.lastMatch` and its kin report: a match of a RegExp made by the constructor itself replaces them, and one of a
/// subclass instance makes them throw.
void noteLegacyBounds(VM& vm, const RegExpData* d, JSValue text, const std::int32_t* bounds, std::size_t groups)
{
    if (!d->legacy) {
        vm.legacyInvalid = true;
        return;
    }
    retain(text);
    release(vm.legacyInput);
    vm.legacyInput = text;
    vm.legacyInvalid = false;
    vm.legacyGroups = std::uint8_t(std::min<std::size_t>(groups, 10));
    for (std::size_t i = 0; i < 2u * vm.legacyGroups; ++i)
        vm.legacyBounds[i] = bounds[i];
}

void noteLegacyMatch(VM& vm, const RegExpData* d, JSValue text, const Match& m)
{
    noteLegacyBounds(vm, d, text, m.bounds.data(), m.groups());
}

/// RegExpBuiltinExec: a match array, null, or the exception sentinel.
JSValue builtinExec(VM& vm, JSValue re, JSValue input)
{
    RegExpData* d = dataOf(re);
    JSObject* o = asObject(re);
    Handle text = Handle::adopt(toStringValue(vm, input));
    if (text.get().isException())
        return JSValue::exception();
    StringView s = asString(text.get())->text();
    const bool useIndex = (d->flags & (regex::kGlobal | regex::kSticky)) != 0;
    double last = 0;
    if (!readLastIndex(vm, o, &last))
        return JSValue::exception();
    if (!useIndex)
        last = 0;
    if (last > double(s.size())) {
        if (useIndex && !writeLastIndex(vm, o, 0))
            return JSValue::exception();
        return JSValue::null();
    }
    Match m;
    if (!runExec(d, s, std::size_t(last), &m)) {
        if (useIndex && !writeLastIndex(vm, o, 0))
            return JSValue::exception();
        return JSValue::null();
    }
    if (useIndex && !writeLastIndex(vm, o, m.end()))
        return JSValue::exception();
    noteLegacyMatch(vm, d, text.get(), m);
    return matchArray(vm, d, text.get(), s, m);
}

/// `builtinExec` for a caller that wants to know only whether it matched: no match array, no capture strings, no vector of
/// bounds (the matcher's registers are read where they are). 1 a match, 0 none, -1 an exception.
int builtinTest(VM& vm, JSValue re, JSValue input)
{
    RegExpData* d = dataOf(re);
    JSObject* o = asObject(re);
    Handle text = Handle::adopt(toStringValue(vm, input));
    if (text.get().isException())
        return -1;
    StringView s = asString(text.get())->text();
    const bool useIndex = (d->flags & (regex::kGlobal | regex::kSticky)) != 0;
    double last = 0;
    if (!readLastIndex(vm, o, &last))
        return -1;
    if (!useIndex)
        last = 0;
    if (last > double(s.size()) || !regex::exec(*d->program, s, std::size_t(last), d->matcher)) {
        if (useIndex && !writeLastIndex(vm, o, 0))
            return -1;
        return 0;
    }
    if (useIndex && !writeLastIndex(vm, o, d->matcher.registers[1]))
        return -1;
    noteLegacyBounds(vm, d, text.get(), d->matcher.registers.data(), regex::groupCount(*d->program));
    return 1;
}

std::size_t advance(StringView s, std::size_t i, bool unicode)
{
    if (unicode && i + 1 < s.size() && s[i] >= 0xd800 && s[i] <= 0xdbff && s[i + 1] >= 0xdc00 && s[i + 1] <= 0xdfff)
        return i + 2;
    return i + 1;
}

String escapeSource(const String& source)
{
    if (source.empty())
        return u"(?:)";
    String out;
    bool inClass = false;
    for (std::size_t i = 0; i < source.size(); ++i) {
        const char16_t c = source[i];
        if (c == u'\\' && i + 1 < source.size()) {
            out.push_back(c);
            out.push_back(source[++i]);
            continue;
        }
        if (c == u'[')
            inClass = true;
        else if (c == u']')
            inClass = false;
        if (c == u'/' && !inClass)
            out += u"\\/";
        else if (c == u'\n')
            out += u"\\n";
        else if (c == u'\r')
            out += u"\\r";
        else if (c == 0x2028)
            out += u"\\u2028";
        else if (c == 0x2029)
            out += u"\\u2029";
        else
            out.push_back(c);
    }
    return out;
}

String flagString(unsigned f)
{
    String out;
    if (f & regex::kHasIndices) out.push_back(u'd');
    if (f & regex::kGlobal) out.push_back(u'g');
    if (f & regex::kIgnoreCase) out.push_back(u'i');
    if (f & regex::kMultiline) out.push_back(u'm');
    if (f & regex::kDotAll) out.push_back(u's');
    if (f & regex::kUnicode) out.push_back(u'u');
    if (f & regex::kUnicodeSets) out.push_back(u'v');
    if (f & regex::kSticky) out.push_back(u'y');
    return out;
}

// ------------------------------------------------------------- the generic path ---
// What the specification does with any object that has `exec`, `flags` and `lastIndex`. A plain RegExp whose prototype
// is as it was made takes the shorter way below.

struct Name
{
    Key key;
    Name(VM& vm, const char* text) : key(vm.internKey(std::string_view(text))) {}
    Name(const Name&) = delete;
    Name& operator=(const Name&) = delete;
    ~Name() { detail::decRef(key); }
};

bool sameValueOf(JSValue a, JSValue b)
{
    if (a.isNumber() && b.isNumber()) {
        const double x = a.asNumber(), y = b.asNumber();
        if (x != x && y != y)
            return true;
        return x == y && std::signbit(x) == std::signbit(y);
    }
    return sameValueZero(a, b);
}

/// IsRegExp: an object with a truthy `Symbol.match`, or else one that is a RegExp. False with `*ok` false on an exception.
bool isRegExpArg(VM& vm, JSValue v, bool* ok)
{
    *ok = true;
    if (!isObject(v))
        return false;
    Handle matcher = Handle::adopt(getProperty(vm, v, vm.atoms.symMatch));
    if (matcher.get().isException()) {
        *ok = false;
        return false;
    }
    if (!matcher.get().isUndefined())
        return toBoolean(matcher.get());
    return dataOf(v) != nullptr;
}

/// The text of `flags` for these flag bits, in the order the getter gives them (the bits are in that order).
String flagLetters(unsigned flags)
{
    String out;
    for (unsigned bit = 0; bit < 8; ++bit)
        if (flags & (1u << bit))
            out.push_back(u"dgimsuvy"[bit]);
    return out;
}

/// Where a recorded property of the RegExp prototype or constructor is now: at the index it had while its owner keeps the shape
/// it had, or, in a dictionary, while the entry at that index is still its key; else found again, and remembered.
JSValue* recordedSlot(VM& vm, VM::RegExpSlot& s)
{
    JSObject* o = s.owner;
    if (o->h.shape == s.shape)
        return o->slotAt(s.index);
    Shape* shape = o->shapeOf(vm);
    if (shape->isDictionary()) {
        const Dictionary& d = *o->rare()->dict;
        if (s.index < d.entries.size() && d.entries[s.index].key == s.key)
            return o->slotAt(s.index);
        const std::int32_t i = d.find(s.key);
        if (i < 0)
            return nullptr;
        s.index = std::uint32_t(i);
        return o->slotAt(s.index);
    }
    const std::int32_t i = shape->indexOf(s.key, nullptr);
    if (i < 0)
        return nullptr;
    s.shape = o->h.shape;
    s.index = std::uint32_t(i);
    return o->slotAt(s.index);
}

/// A plain RegExp made by this engine, with nothing on it or its prototype that a match could notice.
bool pristine(VM& vm, JSValue rx)
{
    RegExpData* d = dataOf(rx);
    if (!d)
        return false;
    JSObject* o = asObject(rx);
    if (o->proto(vm) != vm.protos.regexp)
        return false;
    Rare* r = o->rare();
    const Shape* own = vm.shapeById(o->h.shape);
    // Its one property is `lastIndex`, a writable number: the last a shape of one property added is that property.
    if (!r || r->dict || own->count != 1 || own->key != vm.atoms.lastIndex || own->attrs != attr::kWritable ||
        !o->slotAt(0)->isNumber())
        return false;
    for (VM::RegExpSlot& s : vm.regexpSlots) {
        JSValue* now = recordedSlot(vm, s);
        if (!now || now->bits != s.value.bits)
            return false;
    }
    return true;
}

/// `ToLength(rx.lastIndex)`, read as the language does.
bool getLastIndex(VM& vm, JSValue rx, double* out)
{
    Handle v = Handle::adopt(getProperty(vm, rx, vm.atoms.lastIndex));
    if (v.get().isException())
        return false;
    return toLengthOf(vm, v.get(), out);
}

/// The text of `rx.flags`; false with an exception pending.
bool flagsOf(VM& vm, JSValue rx, String* out)
{
    if (pristine(vm, rx)) {
        *out = flagLetters(dataOf(rx)->flags);
        return true;
    }
    Name flags(vm, "flags");
    Handle v = Handle::adopt(getProperty(vm, rx, flags.key));
    if (v.get().isException())
        return false;
    Handle text = Handle::adopt(toStringValue(vm, v.get()));
    if (text.get().isException())
        return false;
    *out = String(asString(text.get())->text());
    return true;
}

/// RegExpExec: `rx.exec(S)`, or the built-in when `rx` has none.
JSValue regExpExec(VM& vm, JSValue rx, JSValue S)
{
    if (pristine(vm, rx))
        return builtinExec(vm, rx, S);
    Handle fn = Handle::adopt(getProperty(vm, rx, vm.atoms.exec));
    if (fn.get().isException())
        return fn.get();
    if (isFunction(fn.get())) {
        if (fn.get().bits == vm.regexpExec.bits && dataOf(rx))
            return builtinExec(vm, rx, S);
        JSValue r = vm.call(fn.get(), rx, &S, 1);
        if (r.isException())
            return r;
        if (!isObject(r) && !r.isNull()) {
            release(r);
            return typeError(vm, "the result of exec must be an object or null");
        }
        return r;
    }
    if (!dataOf(rx))
        return typeError(vm, "RegExp.prototype.exec requires that 'this' be a RegExp object");
    return builtinExec(vm, rx, S);
}

/// `ToString(result[0])`.
bool matchedText(VM& vm, JSValue result, Handle* out)
{
    Handle first = Handle::adopt(getIndex(vm, result, 0));
    if (first.get().isException())
        return false;
    *out = Handle::adopt(toStringValue(vm, first.get()));
    return !out->get().isException();
}

/// After an empty match of a global search: `rx.lastIndex` moves on by one character.
bool stepPastEmpty(VM& vm, JSValue rx, StringView s, bool unicode)
{
    double at = 0;
    if (!getLastIndex(vm, rx, &at))
        return false;
    const double next = at >= double(s.size()) ? at + 1 : double(advance(s, std::size_t(at), unicode));
    return writeLastIndex(vm, asObject(rx), next);
}

NATIVE(regexpCtor)
{
    UNUSED();
    const JSValue pattern = arg(args, argc, 0), flags = arg(args, argc, 1);
    const bool called = vm.newTarget.isUndefined();
    bool ok = true;
    const bool patternIsRegExp = isRegExpArg(vm, pattern, &ok);
    if (!ok)
        return JSValue::exception();
    if (called && patternIsRegExp && flags.isUndefined()) {
        Name ctorName(vm, "constructor");
        Handle pc = Handle::adopt(getProperty(vm, pattern, ctorName.key));
        if (pc.get().isException())
            return pc.get();
        if (sameValueOf(vm.regexpCtor, pc.get())) {
            retain(pattern);
            return pattern;
        }
    }
    Handle sourceValue, flagValue;
    if (RegExpData* d = dataOf(pattern)) {
        sourceValue = Handle::adopt(makeString(vm, StringView(d->source)));
        flagValue = flags.isUndefined() ? Handle::adopt(makeString(vm, StringView(flagString(d->flags)))) : Handle::retained(flags);
    } else if (patternIsRegExp) {
        Name source(vm, "source");
        sourceValue = Handle::adopt(getProperty(vm, pattern, source.key));
        if (sourceValue.get().isException())
            return sourceValue.get();
        if (flags.isUndefined()) {
            Name flagsName(vm, "flags");
            flagValue = Handle::adopt(getProperty(vm, pattern, flagsName.key));
            if (flagValue.get().isException())
                return flagValue.get();
        } else {
            flagValue = Handle::retained(flags);
        }
    } else {
        sourceValue = Handle::retained(pattern);
        flagValue = Handle::retained(flags);
    }
    JSObject* o;
    Handle keep;
    if (called) {
        o = vm.newObject(vm.protos.regexp);
        keep = Handle::adopt(toJSValue(o));
    } else {
        o = asObject(self);
        keep = Handle::retained(self);
    }
    String source, flagText;
    if (!sourceValue.get().isUndefined()) {
        JSValue t = toStringValue(vm, sourceValue.get());
        if (t.isException())
            return t;
        source = String(asString(t)->text());
        release(t);
    }
    if (!flagValue.get().isUndefined()) {
        JSValue t = toStringValue(vm, flagValue.get());
        if (t.isException())
            return t;
        flagText = String(asString(t)->text());
        release(t);
    }
    if (!initialize(vm, o, StringView(source), StringView(flagText)))
        return JSValue::exception();
    dataOf(keep.get())->legacy = called || sameValueOf(vm.newTarget, vm.regexpCtor);
    return keep.take();
}

NATIVE(regexpCompile)
{
    UNUSED();
    if (!dataOf(self))
        return typeError(vm, "RegExp.prototype.compile requires that 'this' be a RegExp object");
    // Only a RegExp made by the RegExp constructor itself may be recompiled: a subclass would lose what it was made as.
    if (!dataOf(self)->legacy)
        return typeError(vm, "RegExp.prototype.compile called on an incompatible receiver");
    const JSValue pattern = arg(args, argc, 0), flags = arg(args, argc, 1);
    String source, flagText;
    if (RegExpData* d = dataOf(pattern)) {
        if (!flags.isUndefined())
            return typeError(vm, "flags must be undefined when the pattern is a RegExp");
        source = d->source;
        flagText = flagString(d->flags);
    } else {
        if (!pattern.isUndefined()) {
            JSValue t = toStringValue(vm, pattern);
            if (t.isException())
                return t;
            source = String(asString(t)->text());
            release(t);
        }
        if (!flags.isUndefined()) {
            JSValue t = toStringValue(vm, flags);
            if (t.isException())
                return t;
            flagText = String(asString(t)->text());
            release(t);
        }
    }
    if (!initialize(vm, asObject(self), StringView(source), StringView(flagText)))
        return JSValue::exception();
    dataOf(self)->legacy = true;
    retain(self);
    return self;
}

NATIVE(regexpExec)
{
    UNUSED();
    if (!thisRegExp(vm, self, "exec"))
        return JSValue::exception();
    return builtinExec(vm, self, arg(args, argc, 0));
}

NATIVE(regexpTest)
{
    UNUSED();
    if (!isObject(self))
        return typeError(vm, "RegExp.prototype.test requires that 'this' be an Object");
    Handle text = Handle::adopt(toStringValue(vm, arg(args, argc, 0)));
    if (text.get().isException())
        return JSValue::exception();
    if (pristine(vm, self)) {
        const int hit = builtinTest(vm, self, text.get());
        return hit < 0 ? JSValue::exception() : JSValue::boolean(hit != 0);
    }
    JSValue r = regExpExec(vm, self, text.get());
    if (r.isException())
        return r;
    const bool hit = !r.isNull();
    release(r);
    return JSValue::boolean(hit);
}

NATIVE(regexpToString)
{
    UNUSED();
    if (!isObject(self))
        return typeError(vm, "RegExp.prototype.toString requires that 'this' be an Object");
    Name source(vm, "source"), flags(vm, "flags");
    Handle src = Handle::adopt(getProperty(vm, self, source.key));
    if (src.get().isException())
        return src.get();
    Handle srcText = Handle::adopt(toStringValue(vm, src.get()));
    if (srcText.get().isException())
        return srcText.get();
    Handle fl = Handle::adopt(getProperty(vm, self, flags.key));
    if (fl.get().isException())
        return fl.get();
    Handle flText = Handle::adopt(toStringValue(vm, fl.get()));
    if (flText.get().isException())
        return flText.get();
    return makeString(vm, StringView(u"/" + String(asString(srcText.get())->text()) + u"/" + String(asString(flText.get())->text())));
}

constexpr std::intptr_t kFlagsGetter = 1 << 20;

NATIVE(regexpFlagGetter)
{
    UNUSED();
    const std::intptr_t which = reinterpret_cast<std::intptr_t>(data);  // 0 source, kFlagsGetter flags, else the bit
    if (!isObject(self))
        return typeError(vm, "RegExp.prototype getter requires that 'this' be an Object");
    if (which == kFlagsGetter) {
        if (pristine(vm, self))
            return makeString(vm, StringView(flagLetters(dataOf(self)->flags)));
        // Read through the flag properties, so a subclass or an object of its own is heard.
        struct Flag
        {
            const char* name;
            char16_t letter;
        };
        String out;
        for (const Flag& f : {Flag{"hasIndices", u'd'}, Flag{"global", u'g'}, Flag{"ignoreCase", u'i'}, Flag{"multiline", u'm'},
                              Flag{"dotAll", u's'}, Flag{"unicode", u'u'}, Flag{"unicodeSets", u'v'}, Flag{"sticky", u'y'}}) {
            Name key(vm, f.name);
            Handle v = Handle::adopt(getProperty(vm, self, key.key));
            if (v.get().isException())
                return v.get();
            if (toBoolean(v.get()))
                out.push_back(f.letter);
        }
        return makeString(vm, StringView(out));
    }
    RegExpData* d = dataOf(self);
    if (!d) {
        if (asObject(self) == vm.protos.regexp)
            return which == 0 ? makeString(vm, std::string_view("(?:)")) : JSValue::undefined();
        return typeError(vm, "RegExp.prototype getter requires that 'this' be a RegExp object");
    }
    if (which == 0)
        return makeString(vm, StringView(escapeSource(d->source)));
    return JSValue::boolean((d->flags & unsigned(which)) != 0);
}

// ----------------------------------------------------------------- replace ---

/// GetSubstitution: `$1`, `$<name>`, `$&` and the rest.
void expand(String& out, StringView templ, StringView s, const Match& m, RegExpData* d)
{
    for (std::size_t i = 0; i < templ.size(); ++i) {
        const char16_t c = templ[i];
        if (c != u'$' || i + 1 >= templ.size()) {
            out.push_back(c);
            continue;
        }
        const char16_t n = templ[i + 1];
        if (n == u'$') {
            out.push_back(u'$');
            ++i;
        } else if (n == u'&') {
            out += s.substr(std::size_t(m.start()), std::size_t(m.end() - m.start()));
            ++i;
        } else if (n == u'`') {
            out += s.substr(0, std::size_t(m.start()));
            ++i;
        } else if (n == u'\'') {
            out += s.substr(std::size_t(m.end()));
            ++i;
        } else if (n >= u'0' && n <= u'9') {
            std::size_t g = n - u'0';
            std::size_t used = 1;
            if (i + 2 < templ.size() && templ[i + 2] >= u'0' && templ[i + 2] <= u'9') {
                const std::size_t two = g * 10 + (templ[i + 2] - u'0');
                if (two >= 1 && two < m.groups()) {
                    g = two;
                    used = 2;
                }
            }
            if (g >= 1 && g < m.groups()) {
                if (m.bounds[2 * g] >= 0)
                    out += s.substr(std::size_t(m.bounds[2 * g]), std::size_t(m.bounds[2 * g + 1] - m.bounds[2 * g]));
                i += used;
            } else {
                out.push_back(c);
            }
        } else if (n == u'<' && d && !regex::namedGroups(*d->program).empty()) {
            const std::size_t close = templ.find(u'>', i + 2);
            if (close == StringView::npos) {
                out.push_back(c);
                continue;
            }
            const StringView name = templ.substr(i + 2, close - i - 2);
            for (const regex::NamedGroup& ng : regex::namedGroups(*d->program))
                if (StringView(ng.name) == name)
                    for (std::uint32_t g : ng.groups)
                        if (m.bounds[2 * g] >= 0) {
                            out += s.substr(std::size_t(m.bounds[2 * g]), std::size_t(m.bounds[2 * g + 1] - m.bounds[2 * g]));
                            break;
                        }
            i = close;
        } else {
            out.push_back(c);
        }
    }
}

/// GetSubstitution over captures that are values: what the generic path has instead of match bounds.
bool substitute(VM& vm, String& out, StringView matched, StringView s, std::size_t position, const Vec<Handle>& captures,
                JSValue named, StringView templ)
{
    const std::size_t m = captures.size();
    const std::size_t tail = std::min(position + matched.size(), s.size());
    for (std::size_t i = 0; i < templ.size(); ++i) {
        const char16_t c = templ[i];
        if (c != u'$' || i + 1 >= templ.size()) {
            out.push_back(c);
            continue;
        }
        const char16_t n = templ[i + 1];
        if (n == u'$') {
            out.push_back(u'$');
            ++i;
        } else if (n == u'&') {
            out += matched;
            ++i;
        } else if (n == u'`') {
            out += s.substr(0, std::min(position, s.size()));
            ++i;
        } else if (n == u'\'') {
            out += s.substr(tail);
            ++i;
        } else if (n >= u'0' && n <= u'9') {
            const bool two = i + 2 < templ.size() && templ[i + 2] >= u'0' && templ[i + 2] <= u'9';
            std::size_t index = two ? std::size_t(n - u'0') * 10 + (templ[i + 2] - u'0') : std::size_t(n - u'0');
            std::size_t digits = two ? 2 : 1;
            if (two && index > m) {
                digits = 1;
                index = std::size_t(n - u'0');
            }
            if (index >= 1 && index <= m) {
                const JSValue cap = captures[index - 1].get();
                if (isString(cap))
                    out += asString(cap)->text();
                i += digits;
            } else {
                out.push_back(c);
            }
        } else if (n == u'<') {
            const std::size_t close = named.isUndefined() ? StringView::npos : templ.find(u'>', i + 2);
            if (close == StringView::npos) {
                out.push_back(c);
                continue;
            }
            Key key = vm.internKey(templ.substr(i + 2, close - i - 2));
            Handle v = Handle::adopt(getProperty(vm, named, key));
            detail::decRef(key);
            if (v.get().isException())
                return false;
            if (!v.get().isUndefined()) {
                Handle text = Handle::adopt(toStringValue(vm, v.get()));
                if (text.get().isException())
                    return false;
                out += asString(text.get())->text();
            }
            i = close;
        } else {
            out.push_back(c);
        }
    }
    return true;
}

/// Symbol.replace for a plain RegExp: the matches first, then what replaces them.
JSValue fastReplace(VM& vm, JSValue self, RegExpData* d, JSValue textValue, JSValue replacement, bool functional, JSValue templValue)
{
    JSObject* o = asObject(self);
    StringView s = asString(textValue)->text();
    const bool global = (d->flags & regex::kGlobal) != 0, sticky = (d->flags & regex::kSticky) != 0;
    const bool unicode = (d->flags & (regex::kUnicode | regex::kUnicodeSets)) != 0;
    std::size_t at = 0;
    if (global) {
        if (!writeLastIndex(vm, o, 0))
            return JSValue::exception();
    } else if (sticky) {
        double last = 0;
        if (!readLastIndex(vm, o, &last))
            return JSValue::exception();
        at = last > double(s.size()) ? s.size() + 1 : std::size_t(last);
    }
    std::vector<Match> found;
    for (;;) {
        Match m;
        if (at > s.size() || !runExec(d, s, at, &m)) {
            if ((sticky || global) && !writeLastIndex(vm, o, 0))
                return JSValue::exception();
            break;
        }
        const std::size_t ms = std::size_t(m.start()), me = std::size_t(m.end());
        found.push_back(m);
        if (!global) {
            if (sticky && !writeLastIndex(vm, o, double(me)))
                return JSValue::exception();
            break;
        }
        at = me == ms ? advance(s, me, unicode) : me;
    }
    if (!found.empty())
        noteLegacyMatch(vm, d, textValue, found.back());
    String out;
    std::size_t copied = 0;
    for (const Match& m : found) {
        const std::size_t ms = std::size_t(m.start()), me = std::size_t(m.end());
        if (ms < copied)
            continue;
        out += s.substr(copied, ms - copied);
        if (functional) {
            Vec<Handle> callArgs;
            for (std::size_t g = 0; g < m.groups(); ++g)
                callArgs.push_back(Handle::adopt(captureString(vm, s, m, g)));
            callArgs.push_back(Handle::adopt(JSValue::int32(std::int32_t(ms))));
            callArgs.push_back(Handle::retained(textValue));
            if (!regex::namedGroups(*d->program).empty())
                callArgs.push_back(Handle::adopt(namedGroupsObject(vm, d, s, m)));
            std::vector<JSValue> raw;
            for (const Handle& h : callArgs)
                raw.push_back(h.get());
            JSValue r = vm.call(replacement, JSValue::undefined(), raw.data(), std::uint32_t(raw.size()));
            if (r.isException())
                return r;
            JSValue rs = toStringValue(vm, r);
            release(r);
            if (rs.isException())
                return rs;
            out += asString(rs)->text();
            release(rs);
        } else {
            expand(out, asString(templValue)->text(), s, m, d);
        }
        copied = me;
    }
    if (copied < s.size())
        out += s.substr(copied);
    return makeString(vm, StringView(out));
}

JSValue genericReplace(VM& vm, JSValue rx, JSValue S, JSValue replaceValue, bool functional, JSValue templValue)
{
    StringView s = asString(S)->text();
    String flags;
    if (!flagsOf(vm, rx, &flags))
        return JSValue::exception();
    const bool global = flags.find(u'g') != String::npos;
    const bool unicode = flags.find(u'u') != String::npos || flags.find(u'v') != String::npos;
    if (global && !writeLastIndex(vm, asObject(rx), 0))
        return JSValue::exception();
    Vec<Handle> results;
    for (;;) {
        JSValue r = regExpExec(vm, rx, S);
        if (r.isException())
            return r;
        if (r.isNull())
            break;
        results.push_back(Handle::adopt(r));
        if (!global)
            break;
        Handle matched;
        if (!matchedText(vm, r, &matched))
            return JSValue::exception();
        if (asString(matched.get())->length == 0 && !stepPastEmpty(vm, rx, s, unicode))
            return JSValue::exception();
    }
    Name lengthName(vm, "length"), indexName(vm, "index"), groupsName(vm, "groups");
    String out;
    std::size_t next = 0;
    for (const Handle& rh : results) {
        const JSValue result = rh.get();
        Handle lengthValue = Handle::adopt(getProperty(vm, result, lengthName.key));
        if (lengthValue.get().isException())
            return lengthValue.get();
        double resultLength = 0;
        if (!toLengthOf(vm, lengthValue.get(), &resultLength))
            return JSValue::exception();
        const double nCaptures = std::max(resultLength - 1, 0.0);
        Handle matched;
        if (!matchedText(vm, result, &matched))
            return JSValue::exception();
        const StringView matchedView = asString(matched.get())->text();
        Handle indexValue = Handle::adopt(getProperty(vm, result, indexName.key));
        if (indexValue.get().isException())
            return indexValue.get();
        double position = 0;
        if (!toNumber(vm, indexValue.get(), &position))
            return JSValue::exception();
        position = position != position ? 0 : std::trunc(position);
        position = std::max(0.0, std::min(position, double(s.size())));
        Vec<Handle> captures;
        for (double n = 1; n <= nCaptures; ++n) {
            Handle cap = Handle::adopt(getIndex(vm, result, std::uint32_t(n)));
            if (cap.get().isException())
                return cap.get();
            if (!cap.get().isUndefined()) {
                cap = Handle::adopt(toStringValue(vm, cap.get()));
                if (cap.get().isException())
                    return cap.get();
            }
            captures.push_back(std::move(cap));
        }
        Handle named = Handle::adopt(getProperty(vm, result, groupsName.key));
        if (named.get().isException())
            return named.get();
        String replacement;
        if (functional) {
            std::vector<JSValue> callArgs;
            callArgs.push_back(matched.get());
            for (const Handle& c : captures)
                callArgs.push_back(c.get());
            callArgs.push_back(JSValue::boxNumber(position));
            callArgs.push_back(S);
            if (!named.get().isUndefined())
                callArgs.push_back(named.get());
            JSValue r = vm.call(replaceValue, JSValue::undefined(), callArgs.data(), std::uint32_t(callArgs.size()));
            if (r.isException())
                return r;
            Handle rs = Handle::adopt(toStringValue(vm, r));
            release(r);
            if (rs.get().isException())
                return rs.get();
            replacement = String(asString(rs.get())->text());
        } else {
            if (!named.get().isUndefined()) {
                named = Handle::adopt(toObject(vm, named.get()));
                if (named.get().isException())
                    return named.get();
            }
            if (!substitute(vm, replacement, matchedView, s, std::size_t(position), captures, named.get(), asString(templValue)->text()))
                return JSValue::exception();
        }
        if (std::size_t(position) >= next) {
            out += s.substr(next, std::size_t(position) - next);
            out += replacement;
            next = std::size_t(position) + matchedView.size();
        }
    }
    if (next < s.size())
        out += s.substr(next);
    return makeString(vm, StringView(out));
}

NATIVE(regexpReplace)
{
    UNUSED();
    if (!isObject(self))
        return typeError(vm, "RegExp.prototype[Symbol.replace] requires that 'this' be an Object");
    Handle text = Handle::adopt(toStringValue(vm, arg(args, argc, 0)));
    if (text.get().isException())
        return JSValue::exception();
    const JSValue replacement = arg(args, argc, 1);
    const bool functional = isFunction(replacement);
    Handle templ;
    if (!functional) {
        templ = Handle::adopt(toStringValue(vm, replacement));
        if (templ.get().isException())
            return JSValue::exception();
    }
    if (pristine(vm, self))
        return fastReplace(vm, self, dataOf(self), text.get(), replacement, functional, templ.get());
    return genericReplace(vm, self, text.get(), replacement, functional, templ.get());
}

NATIVE(regexpMatch)
{
    UNUSED();
    if (!isObject(self))
        return typeError(vm, "RegExp.prototype[Symbol.match] requires that 'this' be an Object");
    Handle text = Handle::adopt(toStringValue(vm, arg(args, argc, 0)));
    if (text.get().isException())
        return JSValue::exception();
    String flags;
    if (!flagsOf(vm, self, &flags))
        return JSValue::exception();
    if (flags.find(u'g') == String::npos)
        return regExpExec(vm, self, text.get());
    const bool unicode = flags.find(u'u') != String::npos || flags.find(u'v') != String::npos;
    StringView s = asString(text.get())->text();
    if (!writeLastIndex(vm, asObject(self), 0))
        return JSValue::exception();
    JSArray* out = vm.newArray(vm.protos.array);
    Handle keep = Handle::adopt(toJSValue(out));
    for (;;) {
        JSValue r = regExpExec(vm, self, text.get());
        if (r.isException())
            return r;
        if (r.isNull())
            return out->length == 0 ? JSValue::null() : keep.take();
        Handle result = Handle::adopt(r);
        Handle matched;
        if (!matchedText(vm, r, &matched))
            return JSValue::exception();
        retain(matched.get());
        out->append(vm, matched.get());
        if (asString(matched.get())->length == 0 && !stepPastEmpty(vm, self, s, unicode))
            return JSValue::exception();
    }
}

NATIVE(regexpSearch)
{
    UNUSED();
    if (!isObject(self))
        return typeError(vm, "RegExp.prototype[Symbol.search] requires that 'this' be an Object");
    Handle text = Handle::adopt(toStringValue(vm, arg(args, argc, 0)));
    if (text.get().isException())
        return JSValue::exception();
    Handle previous = Handle::adopt(getProperty(vm, self, vm.atoms.lastIndex));
    if (previous.get().isException())
        return previous.get();
    if (!sameValueOf(previous.get(), JSValue::int32(0)) && !setProperty(vm, self, vm.atoms.lastIndex, JSValue::int32(0), true))
        return JSValue::exception();
    JSValue r = regExpExec(vm, self, text.get());
    if (r.isException())
        return r;
    Handle result = Handle::adopt(r);
    Handle current = Handle::adopt(getProperty(vm, self, vm.atoms.lastIndex));
    if (current.get().isException())
        return current.get();
    if (!sameValueOf(current.get(), previous.get()) && !setProperty(vm, self, vm.atoms.lastIndex, previous.get(), true))
        return JSValue::exception();
    if (result.get().isNull())
        return JSValue::int32(-1);
    return getProperty(vm, result.get(), vm.atoms.index);
}

/// Symbol.split for a plain RegExp: searches ahead for the next match rather than trying every position.
JSValue fastSplit(VM& vm, RegExpData* d, StringView s, double limit)
{
    JSArray* out = vm.newArray(vm.protos.array);
    Handle keep = Handle::adopt(toJSValue(out));
    if (limit == 0)
        return keep.take();
    // The engine matches at or after a position; a sticky pattern would match only at it, so run a copy without that.
    RegExpData plain;
    RegExpData* e = d;
    if (d->flags & regex::kSticky) {
        std::string error;
        plain.program = programFor(StringView(d->source), d->flags & ~unsigned(regex::kSticky), error);
        plain.flags = d->flags & ~unsigned(regex::kSticky);
        e = &plain;
    }
    const bool unicode = (d->flags & (regex::kUnicode | regex::kUnicodeSets)) != 0;
    Match m;
    if (s.empty()) {
        if (!runExec(e, s, 0, &m) || m.end() != 0)
            out->append(vm, makeString(vm, s));
        return keep.take();
    }
    std::size_t p = 0, q = 0;
    while (q < s.size()) {
        if (!runExec(e, s, q, &m))
            break;
        const std::size_t ms = std::size_t(m.start());
        const std::size_t end = std::min<std::size_t>(std::size_t(m.end()), s.size());
        if (ms >= s.size())
            break;
        if (end == p && ms == p) {
            q = advance(s, ms, unicode);
            continue;
        }
        if (end == ms && ms == p) {
            q = advance(s, ms, unicode);
            continue;
        }
        out->append(vm, makeString(vm, s.substr(p, ms - p)));
        if (double(out->length) == limit)
            return keep.take();
        for (std::size_t g = 1; g < m.groups(); ++g) {
            out->append(vm, captureString(vm, s, m, g));
            if (double(out->length) == limit)
                return keep.take();
        }
        p = end;
        q = p == ms ? advance(s, p, unicode) : p;
    }
    out->append(vm, makeString(vm, s.substr(p)));
    return keep.take();
}

NATIVE(regexpSplit)
{
    UNUSED();
    if (!isObject(self))
        return typeError(vm, "RegExp.prototype[Symbol.split] requires that 'this' be an Object");
    Handle text = Handle::adopt(toStringValue(vm, arg(args, argc, 0)));
    if (text.get().isException())
        return JSValue::exception();
    StringView s = asString(text.get())->text();
    if (pristine(vm, self)) {
        double limit = 4294967295.0;
        if (!arg(args, argc, 1).isUndefined()) {
            double l;
            if (!toNumber(vm, args[1], &l))
                return JSValue::exception();
            limit = double(toUint32(l));
        }
        return fastSplit(vm, dataOf(self), s, limit);
    }
    Handle ctor = Handle::adopt(speciesConstructor(vm, self, vm.regexpCtor));
    if (ctor.get().isException())
        return ctor.get();
    String flags;
    if (!flagsOf(vm, self, &flags))
        return JSValue::exception();
    const bool unicode = flags.find(u'u') != String::npos || flags.find(u'v') != String::npos;
    const String newFlags = flags.find(u'y') != String::npos ? flags : flags + u"y";
    Handle flagsValue = Handle::adopt(makeString(vm, StringView(newFlags)));
    const JSValue ctorArgs[2] = {self, flagsValue.get()};
    Handle splitter = Handle::adopt(vm.construct(ctor.get(), ctorArgs, 2));
    if (splitter.get().isException())
        return splitter.get();
    double limit = 4294967295.0;
    if (!arg(args, argc, 1).isUndefined()) {
        double l;
        if (!toNumber(vm, args[1], &l))
            return JSValue::exception();
        limit = double(toUint32(l));
    }
    JSArray* out = vm.newArray(vm.protos.array);
    Handle keep = Handle::adopt(toJSValue(out));
    if (limit == 0)
        return keep.take();
    if (s.empty()) {
        JSValue z = regExpExec(vm, splitter.get(), text.get());
        if (z.isException())
            return z;
        const bool matched = !z.isNull();
        release(z);
        if (!matched)
            out->append(vm, makeString(vm, s));
        return keep.take();
    }
    std::size_t p = 0, q = 0;
    Name lengthName(vm, "length");
    while (q < s.size()) {
        if (!writeLastIndex(vm, asObject(splitter.get()), double(q)))
            return JSValue::exception();
        JSValue z = regExpExec(vm, splitter.get(), text.get());
        if (z.isException())
            return z;
        if (z.isNull()) {
            q = advance(s, q, unicode);
            continue;
        }
        Handle result = Handle::adopt(z);
        double e = 0;
        if (!getLastIndex(vm, splitter.get(), &e))
            return JSValue::exception();
        e = std::min(e, double(s.size()));
        if (std::size_t(e) == p) {
            q = advance(s, q, unicode);
            continue;
        }
        out->append(vm, makeString(vm, s.substr(p, q - p)));
        if (double(out->length) == limit)
            return keep.take();
        p = std::size_t(e);
        Handle lengthValue = Handle::adopt(getProperty(vm, z, lengthName.key));
        if (lengthValue.get().isException())
            return lengthValue.get();
        double count = 0;
        if (!toLengthOf(vm, lengthValue.get(), &count))
            return JSValue::exception();
        for (double i = 1; i < count; ++i) {
            JSValue cap = getIndex(vm, z, std::uint32_t(i));
            if (cap.isException())
                return cap;
            out->append(vm, cap);
            if (double(out->length) == limit)
                return keep.take();
        }
        q = p;
    }
    out->append(vm, makeString(vm, s.substr(p)));
    return keep.take();
}

struct MatchAllIter : IteratorState
{
    JSValue re, text;
    bool global, unicode;
    bool done = false;
    MatchAllIter(JSValue r, JSValue t, bool g, bool u) : re(r), text(t), global(g), unicode(u)
    {
        retain(re);
        retain(text);
    }
    ~MatchAllIter() override { clear_(); }
    void clear(VM&) noexcept override { clear_(); }
    void clear_() noexcept
    {
        release(re);
        release(text);
        re = text = JSValue::undefined();
    }
    void values(Vec<JSValue>& out) const override
    {
        out.push_back(re);
        out.push_back(text);
    }
    int step(VM& vm, JSValue* out) override
    {
        if (done)
            return 0;
        JSValue r = regExpExec(vm, re, text);
        if (r.isException()) {
            done = true;
            return -1;
        }
        if (r.isNull()) {
            done = true;
            return 0;
        }
        if (!global) {
            done = true;
        } else {
            Handle matched;
            if (!matchedText(vm, r, &matched) || (asString(matched.get())->length == 0 && !stepPastEmpty(vm, re, asString(text)->text(), unicode))) {
                release(r);
                done = true;
                return -1;
            }
        }
        *out = r;
        return 1;
    }
};

NATIVE(regexpMatchAll)
{
    UNUSED();
    if (!isObject(self))
        return typeError(vm, "RegExp.prototype[Symbol.matchAll] requires that 'this' be an Object");
    Handle text = Handle::adopt(toStringValue(vm, arg(args, argc, 0)));
    if (text.get().isException())
        return JSValue::exception();
    Handle ctor = Handle::adopt(speciesConstructor(vm, self, vm.regexpCtor));
    if (ctor.get().isException())
        return ctor.get();
    String flags;
    if (!flagsOf(vm, self, &flags))
        return JSValue::exception();
    Handle flagsValue = Handle::adopt(makeString(vm, StringView(flags)));
    const JSValue ctorArgs[2] = {self, flagsValue.get()};
    Handle matcher = Handle::adopt(vm.construct(ctor.get(), ctorArgs, 2));
    if (matcher.get().isException())
        return matcher.get();
    double last = 0;
    if (!getLastIndex(vm, self, &last))
        return JSValue::exception();
    if (!setProperty(vm, matcher.get(), vm.atoms.lastIndex, JSValue::boxNumber(last), true))
        return JSValue::exception();
    return newNativeIterator(vm, new MatchAllIter(matcher.get(), text.get(), flags.find(u'g') != String::npos,
                                                  flags.find(u'u') != String::npos || flags.find(u'v') != String::npos),
                             kRegExpStringIterator);
}

// ------------------------------------------------------------------ String ---

/// `String.prototype.match`, `search` and `matchAll`: a pattern that is not a RegExp is made into one.
NATIVE(stringMatchOrSearch)
{
    UNUSED();
    if (self.isNullish())
        return typeError(vm, "String.prototype method called on null or undefined");
    const int mode = int(reinterpret_cast<std::intptr_t>(data));  // 0 match, 1 search, 2 matchAll
    const JSValue pattern = arg(args, argc, 0);
    Key symbol = mode == 0 ? vm.atoms.symMatch : mode == 1 ? vm.atoms.symSearch : vm.atoms.symMatchAll;
    if (mode == 2 && isObject(pattern)) {
        bool ok = true;
        if (isRegExpArg(vm, pattern, &ok)) {
            Name flagsName(vm, "flags");
            Handle flags = Handle::adopt(getProperty(vm, pattern, flagsName.key));
            if (flags.get().isException())
                return flags.get();
            if (flags.get().isNullish())
                return typeError(vm, "String.prototype.matchAll called with a RegExp whose flags are missing");
            Handle text = Handle::adopt(toStringValue(vm, flags.get()));
            if (text.get().isException())
                return text.get();
            if (asString(text.get())->text().find(u'g') == StringView::npos)
                return typeError(vm, "String.prototype.matchAll called with a non-global RegExp argument");
        } else if (!ok) {
            return JSValue::exception();
        }
    }
    bool handled = false;
    JSValue r = callSymbolMethod(vm, pattern, symbol, self, nullptr, 0, &handled);
    if (handled)
        return r;
    Handle text = Handle::adopt(toStringValue(vm, self));
    if (text.get().isException())
        return JSValue::exception();
    // A pattern that is not a RegExp is made into one.
    String source;
    if (!pattern.isUndefined()) {
        JSValue pt = toStringValue(vm, pattern);
        if (pt.isException())
            return pt;
        source = String(asString(pt)->text());
        release(pt);
    }
    JSObject* re = vm.newObject(vm.protos.regexp);
    Handle reKeep = Handle::adopt(toJSValue(re));
    if (!initialize(vm, re, StringView(source), mode == 2 ? StringView(u"g") : StringView()))
        return JSValue::exception();
    JSValue method = getProperty(vm, reKeep.get(), symbol);
    if (method.isException())
        return method;
    Handle methodKeep = Handle::adopt(method);
    const JSValue callArgs[1] = {text.get()};
    return vm.call(method, reKeep.get(), callArgs, 1);
}

}  // namespace

JSValue callSymbolMethod(VM& vm, JSValue pattern, Key symbol, JSValue string, const JSValue* extra, std::uint32_t extraCount,
                         bool* handled)
{
    *handled = false;
    if (!isObject(pattern))
        return JSValue::undefined();
    JSValue method = getProperty(vm, pattern, symbol);
    if (method.isException()) {
        *handled = true;
        return method;
    }
    Handle keep = Handle::adopt(method);
    if (method.isNullish())
        return JSValue::undefined();
    if (!isFunction(method)) {
        *handled = true;
        return typeError(vm, "Symbol method is not a function");
    }
    *handled = true;
    JSValue callArgs[3] = {string, extraCount > 0 ? extra[0] : JSValue::undefined(), extraCount > 1 ? extra[1] : JSValue::undefined()};
    return vm.call(method, pattern, callArgs, 1 + extraCount);
}

bool isRegExpValue(VM& vm, JSValue v, bool* ok)
{
    return isRegExpArg(vm, v, ok);
}

JSValue newRegExp(VM& vm, StringView pattern, StringView flags)
{
    JSObject* o = vm.newObject(vm.protos.regexp);
    Handle keep = Handle::adopt(toJSValue(o));
    if (!initialize(vm, o, pattern, flags))
        return JSValue::exception();
    return keep.take();
}

void installRegExp(VM& vm)
{
    JSObject* proto = vm.newObject(vm.protos.object);
    vm.setIntrinsic(&vm.protos.regexp, proto);
    release(toJSValue(proto));
    JSFunction* ctor = constructor(vm, "RegExp", regexpCtor, 2, proto);
    vm.regexpCtor = toJSValue(ctor);
    retain(vm.regexpCtor);
    JSFunction* exec = method(vm, proto, "exec", regexpExec, 1);
    vm.regexpExec = toJSValue(exec);
    retain(vm.regexpExec);
    method(vm, proto, "compile", regexpCompile, 2);
    method(vm, proto, "test", regexpTest, 1);
    method(vm, proto, "toString", regexpToString, 0);
    symbolMethod(vm, proto, vm.atoms.symMatch, "[Symbol.match]", regexpMatch, 1);
    symbolMethod(vm, proto, vm.atoms.symMatchAll, "[Symbol.matchAll]", regexpMatchAll, 1);
    symbolMethod(vm, proto, vm.atoms.symReplace, "[Symbol.replace]", regexpReplace, 2);
    symbolMethod(vm, proto, vm.atoms.symSearch, "[Symbol.search]", regexpSearch, 1);
    symbolMethod(vm, proto, vm.atoms.symSplit, "[Symbol.split]", regexpSplit, 2);
    struct Getter
    {
        const char* name;
        std::intptr_t which;
    };
    for (const Getter& g : {Getter{"dotAll", regex::kDotAll}, Getter{"flags", kFlagsGetter}, Getter{"global", regex::kGlobal},
                            Getter{"hasIndices", regex::kHasIndices}, Getter{"ignoreCase", regex::kIgnoreCase},
                            Getter{"multiline", regex::kMultiline}, Getter{"source", 0}, Getter{"sticky", regex::kSticky},
                            Getter{"unicode", regex::kUnicode}, Getter{"unicodeSets", regex::kUnicodeSets}}) {
        Key key = vm.internKey(std::string_view(g.name));
        Key fnName = vm.internKey(std::string_view(std::string("get ") + g.name));
        JSFunction* getter = vm.newNativeFunction(vm.protos.function, regexpFlagGetter, reinterpret_cast<void*>(g.which), fnName, 0);
        Handle keep = Handle::adopt(toJSValue(getter));
        defineAccessor(vm, proto, key, toJSValue(getter), JSValue::undefined(), attr::kConfigurable);
        detail::decRef(key);
        detail::decRef(fnName);
    }
    JSObject* sp = vm.protos.string;
    method(vm, sp, "match", stringMatchOrSearch, 1, reinterpret_cast<void*>(0));
    method(vm, sp, "search", stringMatchOrSearch, 1, reinterpret_cast<void*>(1));
    method(vm, sp, "matchAll", stringMatchOrSearch, 1, reinterpret_cast<void*>(2));
}

/// The getters of `RegExp.$1` to `RegExp.$9` (data 1 to 9), `input` (0), `lastMatch` (10), `lastParen` (11), `leftContext` (12) and
/// `rightContext` (13).
NATIVE(regexpLegacyGet)
{
    UNUSED();
    if (self.bits != vm.regexpCtor.bits)
        return typeError(vm, "RegExp legacy static accessors require the RegExp constructor as the receiver");
    if (vm.legacyInvalid)
        return typeError(vm, "RegExp legacy static properties are not available after a subclass match");
    const int which = int(reinterpret_cast<std::intptr_t>(data));
    if (which == 0) {
        if (isString(vm.legacyInput)) {
            retain(vm.legacyInput);
            return vm.legacyInput;
        }
        return makeString(vm, StringView());
    }
    if (!isString(vm.legacyInput))
        return makeString(vm, StringView());
    const StringView text = asString(vm.legacyInput)->text();
    auto slice = [&](std::int32_t from, std::int32_t to) {
        return makeString(vm, text.substr(std::size_t(from), std::size_t(to - from)));
    };
    auto group = [&](std::size_t g) {
        if (g >= vm.legacyGroups || vm.legacyBounds[2 * g] < 0)
            return makeString(vm, StringView());
        return slice(vm.legacyBounds[2 * g], vm.legacyBounds[2 * g + 1]);
    };
    if (which <= 9)
        return group(std::size_t(which));
    switch (which) {
        case 10: return group(0);
        case 11: return vm.legacyGroups > 1 ? group(vm.legacyGroups - 1u) : makeString(vm, StringView());
        case 12: return slice(0, vm.legacyBounds[0]);
        default: return slice(vm.legacyBounds[1], std::int32_t(text.size()));
    }
}

NATIVE(regexpLegacyInputSet)
{
    UNUSED();
    if (self.bits != vm.regexpCtor.bits)
        return typeError(vm, "RegExp legacy static accessors require the RegExp constructor as the receiver");
    JSValue text = toStringValue(vm, arg(args, argc, 0));
    if (text.isException())
        return text;
    release(vm.legacyInput);
    vm.legacyInput = text;
    return JSValue::undefined();
}

/// `RegExp.escape(string)`: the text as a pattern that matches just it.
NATIVE(regexpEscape)
{
    UNUSED();
    const JSValue v = arg(args, argc, 0);
    if (!isString(v))
        return typeError(vm, "RegExp.escape requires a string");
    const StringView text = asString(v)->text();
    String out;
    auto hex = [&](std::uint32_t value, int digits) {
        static const char16_t kDigits[] = u"0123456789abcdef";
        for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4)
            out.push_back(kDigits[(value >> shift) & 15]);
    };
    for (std::size_t i = 0; i < text.size();) {
        std::uint32_t cp = text[i];
        std::size_t width = 1;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (text[i + 1] - 0xDC00);
            width = 2;
        }
        const bool alnum = (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
        if (i == 0 && alnum) {
            out += u"\\x";
            hex(cp, 2);
        } else if (cp < 128 && std::strchr("^$\\.*+?()[]{}|/", int(cp)) && cp != 0) {
            out.push_back(u'\\');
            out.push_back(char16_t(cp));
        } else if (cp == '\t' || cp == '\n' || cp == 0x0B || cp == '\f' || cp == '\r') {
            out.push_back(u'\\');
            out.push_back(cp == '\t' ? u't' : cp == '\n' ? u'n' : cp == 0x0B ? u'v' : cp == '\f' ? u'f' : u'r');
        } else if ((cp < 128 && cp != 0 && std::strchr(",-=<>#&!%:;@~'`\"", int(cp))) || cp == ' ' || cp == 0xA0 || cp == 0xFEFF ||
                   cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000 ||
                   cp == 0x2028 || cp == 0x2029 || (cp >= 0xD800 && cp <= 0xDFFF)) {
            if (cp <= 0xFF) {
                out += u"\\x";
                hex(cp, 2);
            } else {
                out += u"\\u";
                hex(cp, 4);
            }
        } else {
            out += text.substr(i, width);
        }
        i += width;
    }
    return makeString(vm, StringView(out));
}

void installRegExpLegacy(VM& vm)
{
    JSObject* ctor = asObject(vm.regexpCtor);
    method(vm, ctor, "escape", regexpEscape, 1);
    struct Spec
    {
        const char* name;
        const char* alias;
        int which;
    };
    static const Spec specs[] = {{"input", "$_", 0}, {"$1", nullptr, 1}, {"$2", nullptr, 2}, {"$3", nullptr, 3},
                                 {"$4", nullptr, 4}, {"$5", nullptr, 5}, {"$6", nullptr, 6}, {"$7", nullptr, 7},
                                 {"$8", nullptr, 8}, {"$9", nullptr, 9}, {"lastMatch", "$&", 10}, {"lastParen", "$+", 11},
                                 {"leftContext", "$`", 12}, {"rightContext", "$'", 13}};
    for (const Spec& spec : specs) {
        for (const char* name : {spec.name, spec.alias}) {
            if (!name)
                continue;
            Key key = vm.internKey(std::string_view(name));
            Key getName = vm.internKey(std::string_view(std::string("get ") + name));
            JSFunction* getter = vm.newNativeFunction(vm.protos.function, regexpLegacyGet, reinterpret_cast<void*>(std::intptr_t(spec.which)), getName, 0);
            Handle getKeep = Handle::adopt(toJSValue(getter));
            JSValue setter = JSValue::undefined();
            Handle setKeep;
            if (spec.which == 0) {
                Key setName = vm.internKey(std::string_view(std::string("set ") + name));
                JSFunction* fn = vm.newNativeFunction(vm.protos.function, regexpLegacyInputSet, nullptr, setName, 1);
                setKeep = Handle::adopt(toJSValue(fn));
                setter = setKeep.get();
                detail::decRef(setName);
            }
            defineAccessor(vm, ctor, key, toJSValue(getter), setter, attr::kConfigurable);
            detail::decRef(key);
            detail::decRef(getName);
        }
    }
}

/// What a plain RegExp counts on staying as it is, taken once everything that goes on the prototype and the constructor is there.
void recordRegExpOriginals(VM& vm)
{
    installRegExpLegacy(vm);
    JSObject* proto = vm.protos.regexp;
    JSObject* ctor = asObject(vm.regexpCtor);
    const char* const names[] = {"exec", "constructor", "flags", "global", "ignoreCase", "multiline", "dotAll", "unicode",
                                 "unicodeSets", "sticky", "hasIndices"};
    auto add = [&](JSObject* owner, Key key) {
        JSValue* slot = owner->findOwn(vm, key);
        if (!slot)
            return;
        retain(*slot);
        retain(toJSValue(owner));
        detail::incRef(key);
        Shape* shape = owner->shapeOf(vm);
        const bool fixed = !shape->isDictionary();
        const std::int32_t index = fixed ? shape->indexOf(key, nullptr) : owner->rare()->dict->find(key);
        vm.regexpSlots.push_back(VM::RegExpSlot{owner, key, *slot, fixed ? owner->h.shape : VM::kNoShape, std::uint32_t(index)});
    };
    for (const char* name : names) {
        Key key = vm.internKey(std::string_view(name));
        add(proto, key);
        detail::decRef(key);
    }
    add(ctor, vm.atoms.symSpecies);
    for (Key key : {vm.atoms.symMatch, vm.atoms.symReplace, vm.atoms.symSplit, vm.atoms.symMatchAll, vm.atoms.symSearch})
        add(proto, key);
}

}  // namespace bi
}  // namespace vm
}  // namespace internal
}  // namespace script
