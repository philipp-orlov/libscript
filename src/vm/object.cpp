// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <cstdlib>
#include "script/internal/vm/bytecode.hpp"
#include "script/internal/vm/object.hpp"
#include "script/internal/vm/jit.hpp"

#include <cstring>

#include "script/internal/heap.hpp"

namespace script {
namespace internal {
namespace vm {

// ------------------------------------------------------------------ creation ---

JSObject* VM::newObject(Shape* root, std::uint8_t aux)
{
    auto* o = static_cast<JSObject*>(allocCell(JSObject::bytesFor(aux), Kind::Object, ObjClass::Plain, aux));
    o->h.shape = root->id;
    o->slots = nullptr;
    return o;
}

JSObject* VM::newObject(JSObject* proto, std::uint8_t aux)
{
    return newObject(rootShapeFor(proto), aux);
}

BufferState::~BufferState()
{
    if (data) {
        std::free(data);
        data = nullptr;
    }
}

void BufferState::refreshViews() noexcept
{
    for (JSTypedArray* t : views) {
        const std::size_t esz = kElemSize[std::size_t(t->kind)];
        t->data = data ? data + t->byteOffset : nullptr;
        if (detached || t->byteOffset > size) {
            t->length = 0;
        } else if (t->tracking) {
            t->length = std::uint32_t((size - t->byteOffset) / esz);
        } else {
            t->length = t->byteOffset + std::size_t(t->fixedLength) * esz <= size ? t->fixedLength : 0;
        }
    }
}

void forgetView(JSTypedArray* view) noexcept
{
    if (!isObject(view->buffer))
        return;
    Rare* r = asObject(view->buffer)->rare();
    if (!r || !r->ext)
        return;
    auto& views = static_cast<BufferState*>(r->ext)->views;
    for (std::size_t i = 0; i < views.size(); ++i)
        if (views[i] == view) {
            views[i] = views.back();
            views.pop_back();
            return;
        }
}

JSObject* VM::newArrayBuffer(std::size_t bytes, std::size_t maxBytes, bool resizable)
{
    const std::size_t capacity = resizable ? maxBytes : bytes;
    if (capacity > (std::size_t(1) << 31)) {
        throwError(ErrorCode::RangeError, "Array buffer allocation failed");
        return nullptr;
    }
    ctx.reserve(capacity);
    auto* state = new BufferState();
    state->vm = this;
    state->resizable = resizable;
    state->capacity = capacity;
    state->data = static_cast<std::uint8_t*>(std::calloc(capacity ? capacity : 1, 1));
    if (!state->data) {
        delete state;
        throwError(ErrorCode::RangeError, "Array buffer allocation failed");
        return nullptr;
    }
    state->size = bytes;
    JSObject* o = newObject(protos.arrayBuffer);
    o->h.cls = std::uint8_t(ObjClass::ArrayBuffer);
    o->rareOf(*this).ext = state;
    return o;
}

JSTypedArray* VM::newTypedArray(JSObject* proto, ElemKind kind, JSObject* buffer, std::uint32_t byteOffset, std::uint32_t length)
{
    Shape* root = rootShapeFor(proto);
    auto* t = static_cast<JSTypedArray*>(allocCell(sizeof(JSTypedArray), Kind::Object, ObjClass::TypedArray, 0));
    t->h.shape = root->id;
    t->slots = nullptr;
    auto* state = static_cast<BufferState*>(buffer->rare()->ext);
    t->data = state->data + byteOffset;
    t->length = length;
    t->kind = kind;
    t->byteOffset = byteOffset;
    t->fixedLength = length;
    t->tracking = false;
    t->buffer = toJSValue(buffer);
    detail::incRef(reinterpret_cast<CellPtr>(buffer));
    state->views.push_back(t);
    return t;
}

JSArray* VM::newArray(JSObject* proto, std::uint32_t length, std::uint32_t capacity)
{
    Shape* root = rootShapeFor(proto);
    if (capacity < length)
        capacity = length;
    auto* a = static_cast<JSArray*>(allocCell(sizeof(JSArray), Kind::Object, ObjClass::Array, 0));
    a->h.shape = root->id;
    a->h.flags |= cellflags::kAllNumbers;
    a->slots = nullptr;
    a->elems = a->small;
    a->length = 0;
    a->packed = 0;
    a->capacity = JSArray::kSmall;
    try {
        a->reserve(*this, capacity);
    } catch (...) {
        a->h.rc = 0;
        destroyNow(reinterpret_cast<CellPtr>(a));
        throw;
    }
    if (length) {
        for (std::uint32_t i = 0; i < length; ++i)
            a->elems[i] = JSValue::hole();
        a->length = length;
        a->h.flags &= std::uint8_t(~cellflags::kAllNumbers);
    }
    return a;
}

Box* VM::newBox(JSValue value)
{
    auto* b = static_cast<Box*>(allocCell(sizeof(Box), Kind::Box, ObjClass::Plain));
    b->value = value;
    return b;
}

GetterSetter* VM::newGetterSetter(JSValue getter, JSValue setter)
{
    auto* g = static_cast<GetterSetter*>(allocCell(sizeof(GetterSetter), Kind::GetterSetter, ObjClass::Plain));
    g->getter = getter;
    g->setter = setter;
    return g;
}

CodeBlock* VM::newCodeBlock(Key name, std::uint32_t length, bool constructor)
{
    auto* code = ::new (PoolAlloc<CodeBlock>().allocate(1)) CodeBlock();
    code->length = length;
    code->constructor = constructor;
    code->name = name;
    if (name)
        detail::incRef(name);
    code->uid = nextCodeUid_++;
    codeByUid_.emplace(code->uid, code);
    return code;
}

void VM::releaseCode(CodeBlock* code) noexcept
{
    if (--code->refs != 0)
        return;
    if (code->name)
        detail::decRef(code->name);
    for (JSValue v : code->consts)
        release(v);
    for (Key k : code->atoms)
        detail::decRef(k);
    for (const InlineCache& c : code->caches)
        if (c.validity)
            releaseValidity(c.validity);
    for (const PolyCache& p : code->poly)
        for (std::uint8_t i = 0; i < p.n; ++i)
            if (p.e[i].validity)
                releaseValidity(p.e[i].validity);
    for (const SuperCache& s : code->superCaches)
        if (s.validity)
            releaseValidity(s.validity);
    for (CodeBlock* child : code->children)
        releaseCode(child);
    if (code->tier2) {
        releaseTier2(code->tier2, this);
        code->tier2 = nullptr;
    }
    if (code->tier1) {
        releaseTier1(code->tier1, this);
        code->tier1 = nullptr;
    }
    codeByUid_.erase(code->uid);
    if (CodeBlock* from = code->inlinedFrom) {
        if (from->selfInlined == code)
            from->selfInlined = nullptr;
        releaseCode(from);
    }
    code->~CodeBlock();
    PoolAlloc<CodeBlock>().deallocate(code, 1);
}

JSFunction* VM::newFunction(JSObject* proto, CodeBlock* code, Box* const* upvalues, std::uint32_t n)
{
    const std::size_t bytes = JSFunction::bytesFor(n);
    if (bytes > mem::LocalHeap::kMaxSmall)
        throw Error(ErrorCode::RangeError, "a closure captures too many variables");
    // Generator and async functions inherit from prototypes of their own kind.
    if (code->flags & CodeBlock::kGenerator) {
        JSObject* kind = (code->flags & CodeBlock::kAsync)
                             ? ((code->flags & CodeBlock::kAsyncGen) ? protos.asyncGeneratorFunction : protos.asyncFunction)
                             : protos.generatorFunction;
        if (kind)
            proto = kind;
    }
    Shape* root = rootShapeFor(proto);
    auto* f = static_cast<JSFunction*>(allocCell(bytes, Kind::Object, ObjClass::Function, 0));
    f->h.shape = root->id;
    f->slots = nullptr;
    f->code = code;
    ++code->refs;
    f->flags = JSFunction::kLazyProps | (code->constructor ? JSFunction::kConstructor : 0u);
    f->nUpvals = n;
    for (std::uint32_t i = 0; i < n; ++i) {
        f->upvals()[i] = upvalues[i];
        detail::incRef(reinterpret_cast<CellPtr>(upvalues[i]));
    }
    return f;
}

JSFunction* VM::newNativeFunction(JSObject* proto, FastNative fn, void* data, Key name, std::uint32_t length)
{
    Shape* root = rootShapeFor(proto);
    auto* f = static_cast<JSFunction*>(allocCell(sizeof(JSFunction), Kind::Object, ObjClass::Function, 0));
    f->h.shape = root->id;
    f->slots = nullptr;
    f->code = nullptr;
    f->flags = JSFunction::kLazyProps | JSFunction::kNative;
    f->nUpvals = 0;
    try {
        Rare& r = f->rareOf(*this);
        r.native = fn;
        r.nativeData = data;
        r.fnLength = length;
        r.fnName = name;
        if (name)
            detail::incRef(name);
    } catch (...) {
        f->h.rc = 0;
        destroyNow(reinterpret_cast<CellPtr>(f));
        throw;
    }
    return f;
}

// ------------------------------------------------------------- slots and rare ---

void JSObject::growSlots(VM& vm, std::uint32_t capacity)
{
    const std::uint32_t old = slotCapacity();
    auto* header = static_cast<SlotsHeader*>(vm.allocPayload(sizeof(SlotsHeader) + 8u * capacity));
    header->rare = slots ? slotsHeader()->rare : nullptr;
    header->capacity = capacity;
    header->pad = 0;
    auto* fresh = reinterpret_cast<JSValue*>(header + 1);
    if (old)
        std::memcpy(fresh, slots, 8u * old);
    for (std::uint32_t i = old; i < capacity; ++i)
        fresh[i] = JSValue::undefined();
    if (slots)
        vm.freePayload(slotsHeader(), sizeof(SlotsHeader) + 8u * old);
    slots = fresh;
    // Compiled code holds the addresses of the global object's slots while the epoch stays.
    if (this == vm.global)
        ++vm.globalEpoch;
}

void JSObject::reserveSlot(VM& vm, std::uint32_t index)
{
    if (index < h.aux)
        return;
    const std::uint32_t needed = index - h.aux + 1;
    const std::uint32_t have = slotCapacity();
    if (needed <= have)
        return;
    growSlots(vm, have == 0 ? (needed < 4 ? 4 : needed) : (needed > have * 2 ? needed : have * 2));
}

Rare& JSObject::rareOf(VM& vm)
{
    if (h.flags & cellflags::kHasRare)
        return *slotsHeader()->rare;
    if (!slots)
        growSlots(vm, 0);
    auto* r = ::new (vm.allocPayload(sizeof(Rare))) Rare();
    slotsHeader()->rare = r;
    h.flags |= cellflags::kHasRare;
    return *r;
}

// ------------------------------------------------------------------ prototype ---

bool JSObject::wouldCycle(VM& vm, JSObject* next) const noexcept
{
    for (JSObject* p = next; p; p = p->proto(vm))
        if (p == this)
            return true;
    return false;
}

void JSObject::toDictionary(VM& vm)
{
    if (this == vm.global)
        ++vm.globalEpoch;
    Shape* s = shapeOf(vm);
    if (s->isDictionary())
        return;
    Rare& r = rareOf(vm);
    auto* d = ::new (PoolAlloc<Dictionary>().allocate(1)) Dictionary();
    Shape* ds = nullptr;
    try {
        d->entries.reserve(s->count + 1);
        for (std::uint32_t i = 0; i < s->count; ++i)
            d->entries.push_back(s->entryAt(i));
        d->live = s->count;
        d->rebuild();
        ds = vm.newShape();
        for (const Entry& e : d->entries)
            detail::incRef(e.key);
    } catch (...) {
        d->~Dictionary();
        PoolAlloc<Dictionary>().deallocate(d, 1);
        throw;
    }
    ds->flags = Shape::kDictionary;
    ds->proto = s->proto;
    r.dict = d;
    h.shape = ds->id;
}

void JSObject::dictionaryChanging(VM& vm)
{
    Shape* s = shapeOf(vm);
    if (!(s->flags & Shape::kCachedDict))
        return;
    Shape* n = vm.newShape();
    n->flags = Shape::kDictionary;
    n->proto = s->proto;
    h.shape = n->id;
    vm.freeShape(s);
    Dictionary& d = *rare()->dict;
    d.misses = 0;
    if (d.changes < 255)
        ++d.changes;
}

bool JSObject::cacheDictionary(VM& vm) noexcept
{
    Shape* s = shapeOf(vm);
    if (s->flags & Shape::kCachedDict)
        return true;
    // A dictionary used as a map, keys coming and going, would spend an id each time: after a few, it is read uncached.
    constexpr std::uint32_t kBudget = 1u << 14;
    Dictionary& d = *rare()->dict;
    if (d.changes >= 4 || vm.cachedDictionaries >= kBudget)
        return false;
    if (++d.misses < 8)
        return false;
    ++vm.cachedDictionaries;
    s->flags |= Shape::kCachedDict;
    return true;
}

void JSObject::setProto(VM& vm, JSObject* next)
{
    if (next == proto(vm))
        return;
    materialize(vm);
    Shape* s = shapeOf(vm);
    if (!s->isDictionary() && s->count > kMaxFastProperties / 2) {
        toDictionary(vm);
        s = shapeOf(vm);
    }
    if (s->isDictionary()) {
        vm.rootShapeFor(next);  // makes `next` a prototype that something holds a count of, for as long as the VM lives
        dictionaryChanging(vm);
        s = shapeOf(vm);
        s->proto = next;
    } else {
        Shape* t = vm.rootShapeFor(next);
        for (std::uint32_t i = 0; t && i < s->count; ++i) {
            const Entry e = s->entryAt(i);
            t = t->after(vm, e.key, e.attrs);
        }
        if (!t) {
            toDictionary(vm);
            shapeOf(vm)->proto = next;
        } else {
            h.shape = t->id;
        }
    }
    if (next)
        next->h.flags |= cellflags::kIsProto;
    if (isProto())
        vm.invalidateChain(this);
}

// ------------------------------------------------------------------ properties ---

std::uint32_t JSObject::propertyCount(VM& vm) noexcept
{
    materialize(vm);
    Shape* s = shapeOf(vm);
    return s->isDictionary() ? rare()->dict->live : s->count;
}

JSValue* JSObject::findOwn(VM& vm, Key key, std::uint8_t* attrs)
{
    materialize(vm);
    Shape* s = shapeOf(vm);
    if (s->isDictionary()) {
        const Dictionary& d = *rare()->dict;
        const std::int32_t i = d.find(key);
        if (i < 0)
            return nullptr;
        if (attrs)
            *attrs = d.entries[i].attrs;
        return slotAt(std::uint32_t(i));
    }
    const std::int32_t i = s->indexOf(key, attrs);
    return i < 0 ? nullptr : slotAt(std::uint32_t(i));
}

void JSObject::addProperty(VM& vm, Key key, JSValue value, std::uint8_t attrs)
{
    Shape* s = shapeOf(vm);
    if (s->isDictionary()) {
        Dictionary& d = *rare()->dict;
        if (key == vm.atoms.symHasInstance)
            vm.hasInstanceSeen = true;
        const std::uint32_t index = std::uint32_t(d.entries.size());
        reserveSlot(vm, index);
        d.add(key, attrs);
        detail::incRef(key);  // a dictionary entry holds a count of its key; a shape holds its own
        *slotAt(index) = value;
    } else {
        Shape* next = s->count < kMaxFastProperties ? s->after(vm, key, attrs) : nullptr;
        if (!next) {
            toDictionary(vm);
            addProperty(vm, key, value, attrs);
            return;
        }
        if (s->count >= h.aux && !isProto() && cls() == ObjClass::Plain) {
            // Past the slots in place: `new` gives the next objects of this tree more (an add then stays a cached one).
            const std::uint32_t want = s->count + 1;
            const std::uint8_t hint = want <= 8 ? 8 : want <= 16 ? 16 : 24;
            if (s->root && s->root->slotHint < hint)
                s->root->slotHint = hint;
        }
        reserveSlot(vm, s->count);
        *slotAt(s->count) = value;
        h.shape = next->id;
    }
    if (isProto())
        vm.invalidateChain(this);
}

void JSObject::putOwn(VM& vm, Key key, JSValue value, std::uint8_t attrs)
{
    materialize(vm);
    std::uint8_t old = 0;
    if (JSValue* slot = findOwn(vm, key, &old)) {
        if (old != attrs) {
            setAttrs(vm, key, attrs);
            slot = findOwn(vm, key);
        }
        const JSValue previous = *slot;
        *slot = value;
        release(previous);
        return;
    }
    addProperty(vm, key, value, attrs);
}

bool JSObject::setAttrs(VM& vm, Key key, std::uint8_t attrs)
{
    if (this == vm.global)
        ++vm.globalEpoch;
    materialize(vm);
    Shape* s = shapeOf(vm);
    if (s->isDictionary()) {
        const std::int32_t i = rare()->dict->find(key);
        if (i < 0)
            return false;
        if (rare()->dict->entries[i].attrs != attrs)
            dictionaryChanging(vm);
        rare()->dict->entries[i].attrs = attrs;
    } else {
        const std::int32_t i = s->indexOf(key);
        if (i < 0)
            return false;
        if (s->entryAt(std::uint32_t(i)).attrs == attrs)
            return true;
        Shape* t = std::uint32_t(i) == s->count - 1 ? s->withLastAttrs(vm, attrs) : nullptr;
        if (t) {
            h.shape = t->id;
        } else {
            toDictionary(vm);
            rare()->dict->entries[std::uint32_t(i)].attrs = attrs;
        }
    }
    if (isProto())
        vm.invalidateChain(this);
    return true;
}

void JSObject::compactDictionary(VM& vm)
{
    Dictionary& d = *rare()->dict;
    std::uint32_t to = 0;
    for (std::uint32_t from = 0; from < d.entries.size(); ++from) {
        if (!d.entries[from].key)
            continue;
        if (to != from) {
            *slotAt(to) = *slotAt(from);
            d.entries[to] = d.entries[from];
        }
        ++to;
    }
    for (std::uint32_t i = to; i < d.entries.size(); ++i)
        *slotAt(i) = JSValue::undefined();
    d.entries.resize(to);
    d.rebuild();
    (void)vm;
}

bool JSObject::deleteOwn(VM& vm, Key key)
{
    if (this == vm.global)
        ++vm.globalEpoch;
    materialize(vm);
    Shape* s = shapeOf(vm);
    JSValue removed;
    if (s->isDictionary()) {
        Dictionary& d = *rare()->dict;
        const std::int32_t i = d.find(key);
        if (i < 0)
            return true;
        if (!(d.entries[i].attrs & attr::kConfigurable))
            return false;
        dictionaryChanging(vm);
        removed = *slotAt(std::uint32_t(i));
        *slotAt(std::uint32_t(i)) = JSValue::undefined();
        d.erase(std::uint32_t(i));
        const std::size_t holes = d.entries.size() - d.live;
        if (holes >= 16 && holes * 2 >= d.live)
            compactDictionary(vm);
        detail::decRef(key);
    } else {
        std::uint8_t attrs = 0;
        const std::int32_t i = s->indexOf(key, &attrs);
        if (i < 0)
            return true;
        if (!(attrs & attr::kConfigurable))
            return false;
        if (std::uint32_t(i) == s->count - 1) {
            removed = *slotAt(std::uint32_t(i));
            *slotAt(std::uint32_t(i)) = JSValue::undefined();
            h.shape = s->parent->id;
        } else {
            toDictionary(vm);
            return deleteOwn(vm, key);
        }
    }
    release(removed);
    if (isProto())
        vm.invalidateChain(this);
    return true;
}

// ---------------------------------------------------------------- functions ---

void JSObject::materialize(VM& vm)
{
    if (h.cls != std::uint8_t(ObjClass::Function))
        return;
    auto* f = static_cast<JSFunction*>(this);
    if (!(f->flags & JSFunction::kLazyProps))
        return;
    f->flags &= ~JSFunction::kLazyProps;

    std::uint32_t length;
    Key name;
    if (f->flags & JSFunction::kNative) {
        Rare* r = rare();
        length = r ? r->fnLength : 0;
        name = r ? r->fnName : nullptr;
    } else {
        length = f->code->length;
        Rare* r = rare();
        name = r && r->fnName ? r->fnName : f->code->name;
    }
    constexpr std::uint8_t kReadOnlyConfigurable = attr::kConfigurable;
    putOwn(vm, vm.atoms.length, JSValue::int32(std::int32_t(length)), kReadOnlyConfigurable);
    Key text = name ? name : vm.internKey(StringView());  // either way a count for the property to take
    if (name)
        detail::incRef(text);
    putOwn(vm, vm.atoms.name, JSValue::cell(text), kReadOnlyConfigurable);
    const bool script = !(f->flags & JSFunction::kNative);
    const bool plainAsync = script && (f->code->flags & CodeBlock::kAsync) && !(f->code->flags & CodeBlock::kAsyncGen);
    if (script && (f->code->flags & CodeBlock::kGenerator) && !plainAsync) {
        // A generator function's `prototype` is what its generators inherit from; it has no `constructor`.
        JSObject* prototype = vm.newObject((f->code->flags & CodeBlock::kAsyncGen) ? vm.protos.asyncGenerator : vm.protos.generator);
        putOwn(vm, vm.atoms.prototype, toJSValue(prototype), attr::kWritable);
    } else if ((f->flags & JSFunction::kConstructor) && !(f->flags & JSFunction::kNative)) {
        JSObject* prototype = vm.newObject(vm.protos.object);
        Handle keep = Handle::adopt(toJSValue(prototype));
        // The one cycle a function makes, and only for a function that is asked about its properties.
        detail::incRef(reinterpret_cast<CellPtr>(f));
        prototype->putOwn(vm, vm.atoms.constructor, JSValue::cell(reinterpret_cast<CellPtr>(f)),
                          attr::kWritable | attr::kConfigurable);
        putOwn(vm, vm.atoms.prototype, keep.take(), attr::kWritable);
    }
}

// -------------------------------------------------------------------- arrays ---

void JSArray::reserve(VM& vm, std::uint32_t wanted)
{
    if (wanted <= capacity)
        return;
    std::uint64_t grown = capacity < 4 ? 4 : std::uint64_t(capacity) * 2;
    if (grown < wanted)
        grown = wanted;
    if (grown > 0xFFFFFFFFull)
        grown = 0xFFFFFFFFull;
    if (grown * 8 > (std::uint64_t(1) << 32))
        throw Error(ErrorCode::OutOfMemory, "array too large");
    auto* fresh = static_cast<JSValue*>(vm.allocPayload(8u * std::size_t(grown)));
    if (length)
        std::memcpy(fresh, elems, 8u * length);
    if (ownsElems())
        vm.freePayload(elems, 8u * capacity);
    elems = fresh;
    capacity = std::uint32_t(grown);
}

void JSArray::append(VM& vm, JSValue value)
{
    if (length == capacity) {
        try {
            reserve(vm, length + 1);
        } catch (...) {
            release(value);
            throw;
        }
    }
    if (!value.isNumber())
        h.flags &= std::uint8_t(~cellflags::kAllNumbers);
    elems[length++] = value;
    noteStore(length - 1, value);
}

void JSArray::appendAll(VM& vm, const JSValue* values, std::uint32_t n)
{
    reserve(vm, length + n);
    for (std::uint32_t i = 0; i < n; ++i) {
        retain(values[i]);
        if (!values[i].isNumber())
            h.flags &= std::uint8_t(~cellflags::kAllNumbers);
        elems[length + i] = values[i];
    }
    length += n;
    repack(length - n);
}

bool JSArray::setAt(VM& vm, std::uint32_t index, JSValue value)
{
    if (index < length) {
        const JSValue old = elems[index];
        elems[index] = value;
        if (!value.isNumber())
            h.flags &= std::uint8_t(~cellflags::kAllNumbers);
        noteStore(index, value);
        release(old);
        return true;
    }
    if (index - length > kMaxDenseGap) {
        release(value);
        return false;
    }
    try {
        resize(vm, index + 1);
    } catch (...) {
        release(value);
        throw;
    }
    elems[index] = value;
    if (!value.isNumber())
        h.flags &= std::uint8_t(~cellflags::kAllNumbers);
    noteStore(index, value);
    return true;
}

void JSArray::resize(VM& vm, std::uint32_t newLength)
{
    if (newLength > length) {
        reserve(vm, newLength);
        for (std::uint32_t i = length; i < newLength; ++i)
            elems[i] = JSValue::hole();
        h.flags &= std::uint8_t(~cellflags::kAllNumbers);
        length = newLength;
        return;
    }
    // Shorten first, then let go: a release can run a destructor that looks at this array.
    const std::uint32_t old = length;
    length = newLength;
    if (packed > newLength)
        packed = newLength;
    for (std::uint32_t i = newLength; i < old; ++i)
        release(elems[i]);
}

void JSArray::removeRange(VM& vm, std::uint32_t from, std::uint32_t count)
{
    (void)vm;
    if (from >= length)
        return;
    if (count > length - from)
        count = length - from;
    if (!count)
        return;
    // Keep the removed ones aside while the gap closes, so a destructor sees a consistent array.
    Vec<JSValue> removed(elems + from, elems + from + count);
    std::memmove(elems + from, elems + from + count, 8u * (length - from - count));
    length -= count;
    repack(from);
    for (JSValue v : removed)
        release(v);
}

void JSArray::insertGap(VM& vm, std::uint32_t at, std::uint32_t count)
{
    if (at > length)
        at = length;
    if (!count)
        return;
    reserve(vm, length + count);
    std::memmove(elems + at + count, elems + at, 8u * (length - at));
    for (std::uint32_t i = 0; i < count; ++i)
        elems[at + i] = JSValue::hole();
    length += count;
    if (packed > at)
        packed = at;
    h.flags &= std::uint8_t(~cellflags::kAllNumbers);
}

JSValue JSArray::pop(VM& vm)
{
    (void)vm;
    if (!length)
        return JSValue::undefined();
    --length;
    if (packed > length)
        packed = length;
    return elems[length];
}

}  // namespace vm
}  // namespace internal
}  // namespace script
