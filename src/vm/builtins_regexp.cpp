// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// RegExp over the shared matching engine (src/dyn/regexp.cpp), and the String methods that hand over to it.
#include <algorithm>
#include <map>
#include <mutex>

#include "script/internal/dyn/regexp.hpp"
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
    regex::Matcher matcher;
};

std::shared_ptr<const regex::Program> programFor(StringView source, unsigned flags, std::string& error)
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
    if (!o->findOwn(vm, vm.atoms.lastIndex))
        o->putOwn(vm, vm.atoms.lastIndex, JSValue::int32(0), attr::kWritable);
    else
        *o->findOwn(vm, vm.atoms.lastIndex) = JSValue::int32(0);
    return true;
}

double readLastIndex(VM& vm, JSObject* o)
{
    JSValue* slot = o->findOwn(vm, vm.atoms.lastIndex);
    if (!slot)
        return 0;
    double d = 0;
    if (!toNumber(vm, *slot, &d))
        return -1;
    return d != d ? 0 : std::trunc(d);
}

void writeLastIndex(VM& vm, JSObject* o, double v)
{
    JSValue* slot = o->findOwn(vm, vm.atoms.lastIndex);
    if (slot) {
        const JSValue old = *slot;
        *slot = JSValue::boxNumber(v);
        release(old);
    }
}

/// The capture bounds of the last successful match.
struct Match
{
    std::vector<std::int32_t> bounds;  // two per group
    std::size_t groups() const { return bounds.size() / 2; }
    std::int32_t start() const { return bounds[0]; }
    std::int32_t end() const { return bounds[1]; }
};

bool runExec(RegExpData* d, StringView s, std::size_t start, Match* out)
{
    if (!regex::exec(*d->program, s, start, d->matcher))
        return false;
    const std::size_t n = regex::groupCount(*d->program);
    out->bounds.assign(d->matcher.registers.begin(), d->matcher.registers.begin() + 2 * n);
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
        retain(idxKeep.get());
        a->putOwn(vm, vm.atoms.indices, idxKeep.get(), attr::kDefault);
    }
    return keep.take();
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
    double last = readLastIndex(vm, o);
    if (last < 0)
        return JSValue::exception();
    if (!useIndex)
        last = 0;
    if (last > double(s.size())) {
        if (useIndex)
            writeLastIndex(vm, o, 0);
        return JSValue::null();
    }
    Match m;
    if (!runExec(d, s, std::size_t(last), &m)) {
        if (useIndex)
            writeLastIndex(vm, o, 0);
        return JSValue::null();
    }
    if (useIndex)
        writeLastIndex(vm, o, m.end());
    return matchArray(vm, d, text.get(), s, m);
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

NATIVE(regexpCtor)
{
    UNUSED();
    const JSValue pattern = arg(args, argc, 0), flags = arg(args, argc, 1);
    String source, flagText;
    if (RegExpData* d = dataOf(pattern)) {
        source = d->source;
        flagText = flags.isUndefined() ? flagString(d->flags) : String();
    } else if (!pattern.isUndefined()) {
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
    JSObject* o;
    Handle keep;
    if (!vm.newTarget.isUndefined() && isObject(self)) {
        o = asObject(self);
        retain(self);
        keep = Handle::adopt(self);
    } else {
        o = vm.newObject(vm.protos.regexp);
        keep = Handle::adopt(toJSValue(o));
    }
    if (!initialize(vm, o, StringView(source), StringView(flagText)))
        return JSValue::exception();
    return keep.take();
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
    if (!thisRegExp(vm, self, "test"))
        return JSValue::exception();
    JSValue r = builtinExec(vm, self, arg(args, argc, 0));
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
    if (RegExpData* d = dataOf(self))
        return makeString(vm, StringView(u"/" + escapeSource(d->source) + u"/" + flagString(d->flags)));
    return typeError(vm, "RegExp.prototype.toString requires that 'this' be a RegExp object");
}

NATIVE(regexpFlagGetter)
{
    UNUSED();
    const std::intptr_t which = reinterpret_cast<std::intptr_t>(data);  // 0 source, 1 flags, else the bit
    RegExpData* d = dataOf(self);
    if (!d) {
        if (isObject(self) && asObject(self) == vm.protos.regexp)
            return which == 0 ? makeString(vm, std::string_view("(?:)")) : which == 1 ? makeString(vm, std::string_view()) : JSValue::undefined();
        return typeError(vm, "RegExp.prototype getter requires that 'this' be a RegExp object");
    }
    if (which == 0)
        return makeString(vm, StringView(escapeSource(d->source)));
    if (which == 1)
        return makeString(vm, StringView(flagString(d->flags)));
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

NATIVE(regexpReplace)
{
    UNUSED();
    RegExpData* d = thisRegExp(vm, self, "[Symbol.replace]");
    if (!d)
        return JSValue::exception();
    JSObject* o = asObject(self);
    Handle keep = Handle::retained(self);
    Handle text = Handle::adopt(toStringValue(vm, arg(args, argc, 0)));
    if (text.get().isException())
        return JSValue::exception();
    StringView s = asString(text.get())->text();
    const JSValue replacement = arg(args, argc, 1);
    const bool functional = isFunction(replacement);
    Handle templ;
    if (!functional) {
        templ = Handle::adopt(toStringValue(vm, replacement));
        if (templ.get().isException())
            return JSValue::exception();
    }
    const bool global = (d->flags & regex::kGlobal) != 0, sticky = (d->flags & regex::kSticky) != 0;
    const bool unicode = (d->flags & (regex::kUnicode | regex::kUnicodeSets)) != 0;
    std::size_t at = 0;
    if (global) {
        writeLastIndex(vm, o, 0);
    } else if (sticky) {
        const double last = readLastIndex(vm, o);
        if (last < 0)
            return JSValue::exception();
        at = last > double(s.size()) ? s.size() + 1 : std::size_t(last);
    }
    String out;
    std::size_t copied = 0;
    for (;;) {
        Match m;
        if (at > s.size() || !runExec(d, s, at, &m)) {
            if (sticky && !global)
                writeLastIndex(vm, o, 0);
            break;
        }
        const std::size_t ms = std::size_t(m.start()), me = std::size_t(m.end());
        if (ms >= copied) {
            out += s.substr(copied, ms - copied);
            if (functional) {
                std::vector<Handle> callArgs;
                for (std::size_t g = 0; g < m.groups(); ++g)
                    callArgs.push_back(Handle::adopt(captureString(vm, s, m, g)));
                callArgs.push_back(Handle::adopt(JSValue::int32(std::int32_t(ms))));
                callArgs.push_back(text);
                const bool named = !regex::namedGroups(*d->program).empty();
                if (named)
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
                expand(out, asString(templ.get())->text(), s, m, d);
            }
            copied = me;
        }
        if (!global) {
            if (sticky)
                writeLastIndex(vm, o, double(me));
            break;
        }
        at = me == ms ? advance(s, me, unicode) : me;
    }
    if (copied < s.size())
        out += s.substr(copied);
    return makeString(vm, StringView(out));
}

NATIVE(regexpMatch)
{
    UNUSED();
    RegExpData* d = thisRegExp(vm, self, "[Symbol.match]");
    if (!d)
        return JSValue::exception();
    if (!(d->flags & regex::kGlobal))
        return builtinExec(vm, self, arg(args, argc, 0));
    JSObject* o = asObject(self);
    Handle text = Handle::adopt(toStringValue(vm, arg(args, argc, 0)));
    if (text.get().isException())
        return JSValue::exception();
    StringView s = asString(text.get())->text();
    const bool unicode = (d->flags & (regex::kUnicode | regex::kUnicodeSets)) != 0;
    writeLastIndex(vm, o, 0);
    JSArray* out = vm.newArray(vm.protos.array);
    Handle keep = Handle::adopt(toJSValue(out));
    std::size_t at = 0;
    Match m;
    while (at <= s.size() && runExec(d, s, at, &m)) {
        out->append(vm, captureString(vm, s, m, 0));
        at = m.start() == m.end() ? advance(s, std::size_t(m.end()), unicode) : std::size_t(m.end());
    }
    writeLastIndex(vm, o, 0);
    if (out->length == 0)
        return JSValue::null();
    return keep.take();
}

NATIVE(regexpSearch)
{
    UNUSED();
    RegExpData* d = thisRegExp(vm, self, "[Symbol.search]");
    if (!d)
        return JSValue::exception();
    Handle text = Handle::adopt(toStringValue(vm, arg(args, argc, 0)));
    if (text.get().isException())
        return JSValue::exception();
    StringView s = asString(text.get())->text();
    Match m;
    // Searches from the start whatever `lastIndex` says, and leaves it alone.
    if (d->flags & regex::kSticky ? !runExec(d, s, 0, &m) : !runExec(d, s, 0, &m))
        return JSValue::int32(-1);
    return JSValue::int32(m.start());
}

NATIVE(regexpSplit)
{
    UNUSED();
    RegExpData* d = thisRegExp(vm, self, "[Symbol.split]");
    if (!d)
        return JSValue::exception();
    Handle text = Handle::adopt(toStringValue(vm, arg(args, argc, 0)));
    if (text.get().isException())
        return JSValue::exception();
    StringView s = asString(text.get())->text();
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
        if (out->length == limit)
            return keep.take();
        for (std::size_t g = 1; g < m.groups(); ++g) {
            out->append(vm, captureString(vm, s, m, g));
            if (out->length == limit)
                return keep.take();
        }
        p = end;
        q = p == ms ? advance(s, p, unicode) : p;
    }
    out->append(vm, makeString(vm, s.substr(p)));
    return keep.take();
}

struct MatchAllIter : IteratorState
{
    JSValue re, text;
    bool done = false;
    MatchAllIter(JSValue r, JSValue t) : re(r), text(t)
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
    void values(std::vector<JSValue>& out) const override
    {
        out.push_back(re);
        out.push_back(text);
    }
    int step(VM& vm, JSValue* out) override
    {
        if (done)
            return 0;
        RegExpData* d = dataOf(re);
        JSValue r = builtinExec(vm, re, text);
        if (r.isException())
            return -1;
        if (r.isNull()) {
            done = true;
            return 0;
        }
        if (!(d->flags & regex::kGlobal)) {
            done = true;
        } else {
            JSValue first = static_cast<JSArray*>(asObject(r))->at(0);
            if (isString(first) && asString(first)->length == 0) {
                JSObject* o = asObject(re);
                const double last = readLastIndex(vm, o);
                writeLastIndex(vm, o, double(advance(asString(text)->text(), std::size_t(last), (d->flags & regex::kUnicode) != 0)));
            }
        }
        *out = r;
        return 1;
    }
};

NATIVE(regexpMatchAll)
{
    UNUSED();
    RegExpData* d = thisRegExp(vm, self, "[Symbol.matchAll]");
    if (!d)
        return JSValue::exception();
    Handle text = Handle::adopt(toStringValue(vm, arg(args, argc, 0)));
    if (text.get().isException())
        return JSValue::exception();
    // The iterator works on a copy, so the original's `lastIndex` stays as it was.
    JSObject* copy = vm.newObject(vm.protos.regexp);
    Handle copyKeep = Handle::adopt(toJSValue(copy));
    if (!initialize(vm, copy, StringView(d->source), StringView(flagString(d->flags))))
        return JSValue::exception();
    writeLastIndex(vm, copy, readLastIndex(vm, asObject(self)));
    return newNativeIterator(vm, new MatchAllIter(copyKeep.get(), text.get()));
}

// ------------------------------------------------------------------ String ---

bool isRegExpValue(JSValue v) { return dataOf(v) != nullptr; }

/// `String.prototype.match` and `search`: a pattern that is not a RegExp is made into one.
NATIVE(stringMatchOrSearch)
{
    UNUSED();
    if (self.isNullish())
        return typeError(vm, "String.prototype method called on null or undefined");
    const int mode = int(reinterpret_cast<std::intptr_t>(data));  // 0 match, 1 search, 2 matchAll
    const JSValue pattern = arg(args, argc, 0);
    Key symbol = mode == 0 ? vm.atoms.symMatch : mode == 1 ? vm.atoms.symSearch : vm.atoms.symMatchAll;
    Handle text = Handle::adopt(toStringValue(vm, self));
    if (text.get().isException())
        return JSValue::exception();
    if (mode == 2 && isRegExpValue(pattern) && !(dataOf(pattern)->flags & regex::kGlobal))
        return typeError(vm, "String.prototype.matchAll called with a non-global RegExp argument");
    bool handled = false;
    JSValue r = callSymbolMethod(vm, pattern, symbol, text.get(), nullptr, 0, &handled);
    if (handled)
        return r;
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
    (void)ctor;
    method(vm, proto, "exec", regexpExec, 1);
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
    for (const Getter& g : {Getter{"source", 0}, Getter{"flags", 1}, Getter{"global", regex::kGlobal},
                            Getter{"ignoreCase", regex::kIgnoreCase}, Getter{"multiline", regex::kMultiline},
                            Getter{"dotAll", regex::kDotAll}, Getter{"unicode", regex::kUnicode},
                            Getter{"unicodeSets", regex::kUnicodeSets}, Getter{"sticky", regex::kSticky},
                            Getter{"hasIndices", regex::kHasIndices}}) {
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

}  // namespace bi
}  // namespace vm
}  // namespace internal
}  // namespace script
