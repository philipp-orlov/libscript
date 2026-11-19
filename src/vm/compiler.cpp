// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// AST to register bytecode (ls-jsc.md, C-3). Expressions are compiled towards a destination register the caller chose,
// so `x = a + b` for a local `x` is one `Add`. A variable a nested function reads or writes lives in a `Box`, held in its
// register; every other local is just its register. Top-level declarations are properties of the global object.
//
// Not compiled yet, and refused with `NotSupported`: async generators and `for await`, BigInt literals, `with`, `super()`
// and `new.target` inside arrows, and computed names of `super[...]` reads.
#include "script/internal/stack_guard.hpp"
#include "script/internal/dyn/parser.hpp"
#include "script/internal/vm/compiler.hpp"
#include "script/internal/vm/operators.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "script/internal/vm/convert.hpp"
#include "script/internal/vm/object.hpp"

namespace script {
namespace internal {
namespace vm {

namespace {

using dyn::Node;
using dyn::NK;
using dyn::Tok;
using dyn::VarKind;
using dyn::PropKind;

[[noreturn]] void unsupported(const Node& n, const char* what)
{
    throw Error(ErrorCode::NotSupported, std::string("vm core: ") + what + " is not supported yet (line " + std::to_string(n.line) + ")");
}

[[noreturn]] void syntaxError(const Node& n, const std::string& what)
{
    throw Error(ErrorCode::SyntaxError, what + " (line " + std::to_string(n.line) + ")");
}

enum class BKind : std::uint8_t
{
    Var,
    Let,
    Const,
    Param,
    Function,
    Class,
    FnName,
};

/// The words strict code may not use as names; a test on the length first, since this runs for every name in strict code.
inline bool isStrictReserved(const String& s) noexcept
{
    switch (s.size()) {
        case 3: return s == u"let";
        case 5: return s == u"yield";
        case 6: return s == u"public" || s == u"static";
        case 7: return s == u"package" || s == u"private";
        case 9: return s == u"interface" || s == u"protected";
        case 10: return s == u"implements";
        default: return false;
    }
}

struct Binding
{
    String name;
    std::uint16_t reg = 0;
    bool boxed = false;
    BKind kind = BKind::Var;
    bool tdz = false;          ///< `LoadUninit` at scope entry: a read before the declaration needs a check
    bool initialized = false;  ///< the declaration has run, on the path being compiled
};

struct NameHash
{
    std::size_t operator()(const String& s) const noexcept { return std::hash<std::u16string_view>()(s); }
};

struct Scope
{
    std::vector<Binding> bindings;
    std::uint16_t baseTop = 0;

    /// The first binding of this name. A scope of many names (a bundle's module function) is looked up through an index
    /// that takes in the bindings added since the last lookup; bindings are only ever appended.
    Binding* find(const String& name)
    {
        if (bindings.size() <= kScan) {
            for (Binding& b : bindings)
                if (b.name == name)
                    return &b;
            return nullptr;
        }
        for (; indexed < bindings.size(); ++indexed)
            index.emplace(bindings[indexed].name, std::uint32_t(indexed));
        const auto it = index.find(name);
        return it == index.end() ? nullptr : &bindings[it->second];
    }

private:
    static constexpr std::size_t kScan = 16;
    std::unordered_map<String, std::uint32_t, NameHash> index;
    std::size_t indexed = 0;
};

struct FusionOverflow
{
};

struct Label
{
    std::vector<std::uint32_t> patches;
    std::vector<std::uint32_t> patches16;  ///< fused compare-and-jumps
    std::int64_t target = -1;
};

struct TryCtx
{
    const Node* finallyBody = nullptr;
    bool closesIterator = false;  ///< a `for...of` loop: leaving it early closes its iterator
    std::uint16_t iterReg = 0;  ///< null for the catch part: nothing to run on the way out
    std::uint32_t start = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> holes;
};

struct JumpCtx
{
    std::vector<String> labels;
    bool isLoop = false;
    bool isSwitch = false;
    Label* breakTo = nullptr;
    Label* continueTo = nullptr;
    std::size_t tryDepth = 0;
    std::size_t breakDepth = 0;  ///< the `tries` a `break` leaves from: a `for...of` closes its iterator on the way out
};

struct Fn
{
    Fn* parent = nullptr;
    CodeBlock* block = nullptr;
    std::set<String> captured;
    std::vector<Scope> scopes;
    std::uint16_t top = 1;
    bool isArrow = false;
    bool isProgram = false;
    bool isEval = false;       ///< the text of a direct `eval`: arrow-like, with a completion value
    String evalEnvName;        ///< a sloppy function with a direct `eval` in it: the hidden variable that holds its eval object
    bool isMethodFn = false;   ///< a method, accessor, class constructor or field initialiser: `super.x` is allowed in it
    bool evalOuter = false;    ///< the stand-in for the variables around a direct `eval`
    bool dynamicVars = false;  ///< sloppy eval code: its `var`s and functions go to the caller's eval object or the global
    bool completion = false;   ///< expression statements leave their value in `resultReg`
    bool strict = false;
    std::size_t varScope = 0;  ///< the scope `var` and the function's own names live in
    std::uint16_t resultReg = 0;
    std::vector<JumpCtx> jumps;
    std::vector<TryCtx> tries;
    std::map<String, std::uint16_t> upvalIndex;
    std::map<Key, std::uint16_t> atomIndex;
    std::map<std::uint64_t, std::uint32_t> constIndex;
};

bool isFunctionNode(const Node& n) noexcept
{
    return n.kind == NK::FunctionExpr || n.kind == NK::ArrowExpr;
}

/// The names a function's nested functions mention, and whether an arrow inside it reads `this`: those variables
/// live in boxes. A name used by any nested function counts, whichever scope declares it, which over-approximates.
///
/// Every function's compile asks this of its body, so what a nested function contributes is kept (`CaptureCache`, by node,
/// as views of the tree's own names) and a function nested d deep is walked a few times, not d times.
using NameViews = std::vector<std::u16string_view>;

struct CaptureCache
{
    std::unordered_map<const Node*, NameViews> below[2];  ///< by the arrow chain the node is reached with
};

void capturedWalk(const Node& n, NameViews& out, bool nested, bool arrowChain, CaptureCache& cache, bool self);

void collectPatternNames(const Node& n, std::vector<String>& out);
void collectVars(const Node& n, std::vector<String>& out);

/// The names function `n` declares in its own function-level scope, which every reference inside it (a nested function's
/// too) finds there and never in the code around: its simple parameters, its own name when it is a function expression, and
/// the `var`s, function declarations, classes and top-level `let`/`const` of its body. False when a parameter is a pattern
/// or has a default, whose expressions run before the body's names exist.
bool declaredByFunction(const Node& n, std::vector<String>& out)
{
    for (const dyn::NodePtr& p : n.list)
        if (p->kind != NK::Identifier)
            return false;
    for (const dyn::NodePtr& p : n.list)
        out.push_back(p->text);
    if (n.kind == NK::FunctionExpr && n.namedExpr && !n.text.empty())
        out.push_back(n.text);
    if (!n.d || (n.d->kind != NK::Block && n.d->kind != NK::Program))
        return true;
    collectVars(*n.d, out);
    for (const dyn::NodePtr& s : n.d->list) {
        if (s->kind == NK::FunctionDecl || s->kind == NK::ClassDecl) {
            out.push_back(s->text);
        } else if (s->kind == NK::VarDecl && s->varKind != VarKind::Var) {
            for (const dyn::NodePtr& d : s->list)
                collectPatternNames(*d->a, out);
        }
    }
    return true;
}

/// The names a node at nested depth contributes, with everything inside it.
const NameViews& capturedBelow(const Node& n, bool arrowChain, CaptureCache& cache)
{
    auto& slot = cache.below[arrowChain ? 1 : 0];
    if (const auto it = slot.find(&n); it != slot.end())
        return it->second;
    NameViews names;
    capturedWalk(n, names, true, arrowChain, cache, true);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    // What the function declares for itself is not captured from outside, whichever scope of its body mentions it.
    if (isFunctionNode(n)) {
        std::vector<String> own;
        if (declaredByFunction(n, own) && !own.empty()) {
            std::vector<std::u16string_view> gone;
            for (const String& name : own)
                gone.emplace_back(name.data(), name.size());
            std::sort(gone.begin(), gone.end());
            names.erase(std::remove_if(names.begin(), names.end(),
                                       [&](std::u16string_view name) { return std::binary_search(gone.begin(), gone.end(), name); }),
                        names.end());
        }
    }
    names.shrink_to_fit();
    return slot.emplace(&n, std::move(names)).first->second;  // references into an unordered_map survive rehashing
}

void capturedWalk(const Node& n, NameViews& out, bool nested, bool arrowChain, CaptureCache& cache, bool self)
{
    checkNativeStack();
    const bool boundary = isFunctionNode(n) || n.kind == NK::FieldDef || n.kind == NK::StaticBlock;
    // A boundary node adds nothing itself and its children are nested either way, so the summary serves at any depth.
    if (boundary && !self) {
        const NameViews& names = capturedBelow(n, arrowChain, cache);
        out.insert(out.end(), names.begin(), names.end());
        return;
    }
    if (n.kind == NK::Identifier && nested)
        out.push_back(n.text);
    if ((n.kind == NK::ThisExpr || n.kind == NK::SuperExpr) && nested && arrowChain) {
        out.push_back(u"this");
        out.push_back(u"%bound");
    }
    if (n.kind == NK::NewTarget && nested && arrowChain)
        out.push_back(u"%newtarget");
    // A direct `eval` can name any variable around it, so none of them may live in a bare register.
    if (n.kind == NK::Call && n.a && n.a->kind == NK::Identifier && n.a->text == u"eval") {
        out.push_back(u"%all");
        out.push_back(u"%newtarget");
    }
    // `super()` in an arrow function needs the constructor around it and its new.target.
    if (n.kind == NK::Call && n.a && n.a->kind == NK::SuperExpr && nested && arrowChain) {
        out.push_back(u"%callee");
        out.push_back(u"%newtarget");
    }
    bool childNested = nested, childArrow = arrowChain;
    if (isFunctionNode(n)) {
        childNested = true;
        childArrow = arrowChain && n.kind == NK::ArrowExpr;
    } else if (n.kind == NK::FieldDef || n.kind == NK::StaticBlock) {
        // Run by a function of their own, with `this` of their own.
        childNested = true;
        childArrow = false;
    }
    for (const dyn::NodePtr* child : {&n.a, &n.b, &n.c, &n.d})
        if (*child)
            capturedWalk(**child, out, childNested, childArrow, cache, false);
    for (const dyn::NodePtr& child : n.list)
        capturedWalk(*child, out, childNested, childArrow, cache, false);
}

void collectCaptured(const Node& n, std::set<String>& out, CaptureCache& cache)
{
    NameViews names;
    capturedWalk(n, names, false, true, cache, false);
    for (std::u16string_view name : names)
        out.emplace_hint(out.end(), name);
}

/// The names `var` declares in a statement list, looking through blocks but not into functions.
void collectVars(const Node& n, std::vector<String>& out);

void collectPatternNames(const Node& n, std::vector<String>& out)
{
    checkNativeStack();
    switch (n.kind) {
        case NK::Identifier: out.push_back(n.text); break;
        case NK::AssignPattern: collectPatternNames(*n.a, out); break;
        case NK::RestElement: collectPatternNames(*n.a, out); break;
        case NK::ArrayPattern:
            for (const dyn::NodePtr& e : n.list)
                if (e && e->kind != NK::Hole)
                    collectPatternNames(*e, out);
            break;
        case NK::ObjectPattern:
            for (const dyn::NodePtr& p : n.list) {
                if (p->kind == NK::RestElement)
                    collectPatternNames(*p->a, out);
                else if (p->b)
                    collectPatternNames(*p->b, out);
            }
            break;
        default: break;
    }
}

void collectVars(const Node& n, std::vector<String>& out)
{
    checkNativeStack();
    switch (n.kind) {
        case NK::VarDecl:
            if (n.varKind == VarKind::Var)
                for (const dyn::NodePtr& d : n.list)
                    collectPatternNames(*d->a, out);
            break;
        case NK::Block:
        case NK::Program:
        case NK::SwitchCase:
            for (const dyn::NodePtr& s : n.list)
                collectVars(*s, out);
            break;
        case NK::Switch:
            for (const dyn::NodePtr& s : n.list)
                collectVars(*s, out);
            break;
        case NK::If:
            collectVars(*n.b, out);
            if (n.c)
                collectVars(*n.c, out);
            break;
        case NK::For:
            if (n.a)
                collectVars(*n.a, out);
            if (n.d)
                collectVars(*n.d, out);
            break;
        case NK::ForIn:
        case NK::ForOf:
            if (n.a && n.varKind == VarKind::Var)
                collectPatternNames(*n.a, out);
            collectVars(*n.d, out);
            break;
        case NK::While:
        case NK::DoWhile:
        case NK::Labeled:
            collectVars(*n.d, out);
            break;
        case NK::Try:
            collectVars(*n.a, out);
            if (n.c)
                collectVars(*n.c, out);
            if (n.d)
                collectVars(*n.d, out);
            break;
        default: break;
    }
}

/// Whether `arguments` is read by the code of `n`, in arrow functions too, which have none of their own.
bool containsArguments(const Node& n)
{
    checkNativeStack();
    if (n.kind == NK::FunctionExpr)
        return false;
    if (n.kind == NK::Identifier && n.text == u"arguments")
        return true;
    for (const dyn::NodePtr* child : {&n.a, &n.b, &n.c, &n.d})
        if (*child && containsArguments(**child))
            return true;
    for (const dyn::NodePtr& child : n.list)
        if (child && containsArguments(*child))
            return true;
    return false;
}

// Annex B.3.3: a function declared in a block of sloppy code is also a `var` of the function around it, unless a `let`,
// `const` or class of an enclosing block (or the top of the function) would clash with that `var`.
void annexBScan(const Node& s, const std::set<String>& lexical, std::set<String>& out);

void annexBList(const std::vector<dyn::NodePtr>& list, const std::set<String>& outer, std::set<String>& out, bool top)
{
    std::set<String> inner = outer;
    for (const dyn::NodePtr& s : list) {
        if ((s->kind == NK::VarDecl && s->varKind != VarKind::Var)) {
            std::vector<String> names;
            for (const dyn::NodePtr& d : s->list)
                collectPatternNames(*d->a, names);
            inner.insert(names.begin(), names.end());
        } else if (s->kind == NK::ClassDecl || (!top && s->kind == NK::FunctionDecl)) {
            inner.insert(s->text);
        }
    }
    if (!top)
        for (const dyn::NodePtr& s : list)
            if (s->kind == NK::FunctionDecl && s->a && !s->a->isGenerator && !s->a->isAsync && !outer.count(s->text))
                out.insert(s->text);
    for (const dyn::NodePtr& s : list)
        annexBScan(*s, inner, out);
}

void annexBSub(const dyn::NodePtr& body, const std::set<String>& lexical, std::set<String>& out)
{
    if (!body)
        return;
    if (body->kind == NK::FunctionDecl) {
        // `if (x) function f() {}`: a block of its own.
        if (body->a && !body->a->isGenerator && !body->a->isAsync && !lexical.count(body->text))
            out.insert(body->text);
        return;
    }
    annexBScan(*body, lexical, out);
}

void annexBScan(const Node& s, const std::set<String>& lexical, std::set<String>& out)
{
    checkNativeStack();
    switch (s.kind) {
        case NK::Block: annexBList(s.list, lexical, out, false); break;
        case NK::If:
            annexBSub(s.b, lexical, out);
            annexBSub(s.c, lexical, out);
            break;
        case NK::For:
        case NK::ForIn:
        case NK::ForOf: {
            std::set<String> inner = lexical;
            std::vector<String> names;
            if (s.kind == NK::For && s.a && s.a->kind == NK::VarDecl && s.a->varKind != VarKind::Var)
                for (const dyn::NodePtr& d : s.a->list)
                    collectPatternNames(*d->a, names);
            if (s.kind != NK::For && s.a && s.varKind != VarKind::None && s.varKind != VarKind::Var)
                collectPatternNames(*s.a, names);
            inner.insert(names.begin(), names.end());
            annexBSub(s.d, inner, out);
            break;
        }
        case NK::While:
        case NK::DoWhile:
        case NK::Labeled: annexBSub(s.d, lexical, out); break;
        case NK::Try:
            annexBSub(s.a, lexical, out);
            annexBSub(s.c, lexical, out);
            annexBSub(s.d, lexical, out);
            break;
        case NK::Switch: {
            std::set<String> inner = lexical;
            for (const dyn::NodePtr& c : s.list)
                for (const dyn::NodePtr& st : c->list) {
                    if (st->kind == NK::VarDecl && st->varKind != VarKind::Var) {
                        std::vector<String> names;
                        for (const dyn::NodePtr& d : st->list)
                            collectPatternNames(*d->a, names);
                        inner.insert(names.begin(), names.end());
                    } else if (st->kind == NK::ClassDecl || st->kind == NK::FunctionDecl) {
                        inner.insert(st->text);
                    }
                }
            for (const dyn::NodePtr& c : s.list)
                for (const dyn::NodePtr& st : c->list) {
                    if (st->kind == NK::FunctionDecl && st->a && !st->a->isGenerator && !st->a->isAsync && !lexical.count(st->text))
                        out.insert(st->text);
                    annexBScan(*st, inner, out);
                }
            break;
        }
        default: break;
    }
}

/// The names of the functions in blocks of `body` that are `var`s too, for sloppy code.
void annexBNames(const std::vector<dyn::NodePtr>& body, std::set<String>& out)
{
    std::set<String> top;
    annexBList(body, top, out, true);
}

bool mentions(const Node& n, const String& name)
{
    checkNativeStack();
    if (n.kind == NK::Identifier && n.text == name)
        return true;
    for (const dyn::NodePtr* child : {&n.a, &n.b, &n.c, &n.d})
        if (*child && mentions(**child, name))
            return true;
    for (const dyn::NodePtr& child : n.list)
        if (child && mentions(*child, name))
            return true;
    return false;
}

/// Marks the names of `declAt` that statement `at` of a list mentions where they can be read before they are initialised.
/// A nested function is looked at through the names it mentions (`capturedBelow`), which are kept for each one.
void markEarlyReads(const Node& n, std::size_t at, bool hoisted,
                    const std::unordered_map<std::u16string_view, std::size_t>& declAt,
                    std::unordered_set<std::u16string_view>& early, CaptureCache& cache)
{
    checkNativeStack();
    const auto mark = [&](std::u16string_view name) {
        const auto it = declAt.find(name);
        if (it != declAt.end() && (at <= it->second || hoisted))
            early.insert(it->first);
    };
    if (isFunctionNode(n) || n.kind == NK::FieldDef || n.kind == NK::StaticBlock) {
        for (std::u16string_view name : capturedBelow(n, true, cache))
            mark(name);
        return;
    }
    if (n.kind == NK::Identifier)
        mark(n.text);
    for (const dyn::NodePtr* child : {&n.a, &n.b, &n.c, &n.d})
        if (*child)
            markEarlyReads(**child, at, hoisted, declAt, early, cache);
    for (const dyn::NodePtr& child : n.list)
        if (child)
            markEarlyReads(*child, at, hoisted, declAt, early, cache);
}

/// The `let` and `const` names of a statement list that need a hole at scope entry because something can read them before
/// they are initialised: text before the declaration, its own initialiser, or a function declaration, which is made at scope
/// entry. One walk over the list, however many names it declares.
std::unordered_set<std::u16string_view> earlyReadNames(const std::vector<dyn::NodePtr>& statements,
                                                       const std::vector<std::pair<String, std::size_t>>& names,
                                                       CaptureCache& cache)
{
    std::unordered_set<std::u16string_view> early;
    if (names.empty())
        return early;
    std::unordered_map<std::u16string_view, std::size_t> declAt;
    for (const auto& [name, at] : names)
        declAt.emplace(std::u16string_view(name), at);
    for (std::size_t i = 0; i < statements.size(); ++i)
        markEarlyReads(*statements[i], i, statements[i]->kind == NK::FunctionDecl, declAt, early, cache);
    return early;
}

/// Whether evaluating a node can assign to a variable of this function.
bool hasAssignment(const Node& n)
{
    checkNativeStack();
    if (n.kind == NK::Assign || n.kind == NK::Update)
        return true;
    if (isFunctionNode(n))
        return false;
    for (const dyn::NodePtr* child : {&n.a, &n.b, &n.c, &n.d})
        if (*child && hasAssignment(**child))
            return true;
    for (const dyn::NodePtr& child : n.list)
        if (child && hasAssignment(*child))
            return true;
    return false;
}

/// Whether evaluating a node can assign to the variable `name` of this function by an assignment or update in the node
/// itself (an assignment to a member, or to another name, cannot).
bool assignsName(const Node& n, const String& name)
{
    checkNativeStack();
    if (n.kind == NK::Assign || n.kind == NK::Update) {
        if (!n.a || n.a->kind != NK::Member && n.a->kind != NK::Identifier)
            return true;
        if (n.a->kind == NK::Identifier && n.a->text == name)
            return true;
    }
    if (isFunctionNode(n))
        return false;
    for (const dyn::NodePtr* child : {&n.a, &n.b, &n.c, &n.d})
        if (*child && assignsName(**child, name))
            return true;
    for (const dyn::NodePtr& child : n.list)
        if (child && assignsName(*child, name))
            return true;
    return false;
}

/// Whether the value of `first`, evaluated into a register it may share with a local, is still that value after `later`
/// runs: a name only an assignment in `later` itself can change (a local that a closure writes is in a box, and is read
/// into a register of its own).
bool stableAcross(const Node& first, const Node& later)
{
    return first.kind == NK::Identifier ? !assignsName(later, first.text) : !hasAssignment(later);
}

/// Whether a direct `eval` call is in this function's own code: its parameters and body, not a function inside them.
bool containsDirectEval(const Node& n)
{
    checkNativeStack();
    if (n.kind == NK::Call && n.a && n.a->kind == NK::Identifier && n.a->text == u"eval")
        return true;
    if (isFunctionNode(n) || n.kind == NK::FieldDef || n.kind == NK::StaticBlock)
        return false;
    for (const dyn::NodePtr* child : {&n.a, &n.b, &n.c, &n.d})
        if (*child && containsDirectEval(**child))
            return true;
    for (const dyn::NodePtr& child : n.list)
        if (child && containsDirectEval(*child))
            return true;
    return false;
}

bool isEvalEnvName(const String& name) { return name.size() > 9 && name.compare(0, 9, u"%evalenv#") == 0; }

/// Whether `arguments` is read in a function's body or in an arrow inside it (not in another function, which has its own).
bool usesArguments(const Node& n)
{
    checkNativeStack();
    if (n.kind == NK::Identifier && n.text == u"arguments")
        return true;
    if (n.kind == NK::FunctionExpr)
        return false;
    for (const dyn::NodePtr* child : {&n.a, &n.b, &n.c, &n.d})
        if (*child && usesArguments(**child))
            return true;
    for (const dyn::NodePtr& child : n.list)
        if (child && usesArguments(*child))
            return true;
    return false;
}

bool usesSuper(const Node& n)
{
    checkNativeStack();
    if (n.kind == NK::SuperExpr)
        return true;
    if (n.kind == NK::FunctionExpr)
        return false;
    for (const dyn::NodePtr* child : {&n.a, &n.b, &n.c, &n.d})
        if (*child && usesSuper(**child))
            return true;
    for (const dyn::NodePtr& child : n.list)
        if (child && usesSuper(*child))
            return true;
    return false;
}

/// Whether the member or call chain `n` has an optional link (`?.`) in it.
bool hasOptionalLink(const Node& n)
{
    checkNativeStack();
    for (const Node* p = &n; p; p = p->a.get()) {
        if (p->optional)
            return true;
        if (p->kind != NK::Member && p->kind != NK::Call)
            return false;
    }
    return false;
}

bool hasUseStrict(const Node& body)
{
    for (const dyn::NodePtr& s : body.list) {
        if (s->kind != NK::ExprStatement || !s->a || s->a->kind != NK::StringLit)
            return false;
        if (s->a->text == u"use strict")
            return true;
    }
    return false;
}

/// Whether evaluating a node can neither change a variable nor call anything.
bool isPure(const Node& n) noexcept
{
    switch (n.kind) {
        case NK::NumberLit:
        case NK::StringLit:
        case NK::BoolLit:
        case NK::NullLit:
        case NK::Identifier:
        case NK::ThisExpr: return true;
        case NK::FunctionExpr:
        case NK::ArrowExpr: return true;
        default: return false;
    }
}

class Compiler
{
public:
    Compiler(VM& vm, std::string_view file, std::shared_ptr<const std::string> source, bool lazy = false)
        : vm_(vm), ctx_(vm.ctx), file_(fromUtf8(file)), source_(std::move(source)), lazy_(lazy)
    {
    }

    /// Fill `stub` from `n`, the function parsed again from its text.
    void resume(const Node& n, CodeBlock* stub);

    CodeBlock* program(const Node& program);
    CodeBlock* evalProgram(const Node& program, const std::vector<String>& names, bool strict, bool home)
    {
        evalHome_ = home;
        return evalOnce(program, names, strict);
    }

private:
    VM& vm_;
    ContextImpl& ctx_;
    String file_;
    std::shared_ptr<const std::string> source_;
    Fn* fn_ = nullptr;
    std::uint32_t line_ = 0;
    Label* chain_ = nullptr;
    bool chainUsed_ = false;
    bool noFuse_ = false;
    bool lazy_ = false;                ///< functions that qualify are left as stubs (`lazyStub`)
    std::unordered_set<const Node*> calledAtOnce_;  ///< function expressions called where they stand: compiled at once
    bool discard_ = false;  ///< the expression about to be compiled has its value thrown away
    CaptureCache captures_;  ///< what each nested function captures, by node, for the whole tree being compiled

    // ---------------------------------------------------------------- emit ---
    std::uint32_t pc() const noexcept { return std::uint32_t(fn_->block->code.size()); }

    void emit(Op op, std::uint32_t a = 0, std::uint32_t b = 0, std::uint32_t c = 0)
    {
        if (a > 0xFFFF || b > 0xFFFF || c > 0xFFFF)
            throw Error(ErrorCode::NotSupported, "vm core: function too large (operand over 65535)");
        fn_->block->code.push_back(Ins::make(op, std::uint16_t(a), std::uint16_t(b), std::uint16_t(c)));
        fn_->block->lines.push_back(line_);
    }

    /// A property instruction and the word after it that names its cache.
    void emitCached(Op op, std::uint32_t a, std::uint32_t b, std::uint32_t c)
    {
        emit(op, a, b, c);
        std::uint32_t id = kNoCache;
        if (fn_->block->caches.size() < kNoCache) {
            id = std::uint32_t(fn_->block->caches.size());
            fn_->block->caches.emplace_back();
        }
        emit(Op::Cache, id);
    }

    /// `op a, b` and a jump to `label`, in one instruction: the distance has 16 bits, and a function with a jump that
    /// is longer is compiled again without these.
    void emitFusedJump(Op op, std::uint32_t a, std::uint32_t b, Label& label)
    {
        const std::uint32_t at = pc();
        emit(op, a, b, 0);
        if (label.target >= 0)
            patch16(at, label.target);
        else
            label.patches16.push_back(at);
    }
    void patch16(std::uint32_t at, std::int64_t target)
    {
        const std::int64_t offset = target - (at + 1);
        if (offset < -32768 || offset > 32767)
            throw FusionOverflow();
        fn_->block->code[at].c = std::uint16_t(std::int16_t(offset));
    }

    /// The class's field initialisers run on `self`: a call of the function that holds them, when there is one (an ordinary
    /// call, which compiled code makes and can inline).
    void fieldInit(std::uint16_t self)
    {
        const std::uint16_t at = fn_->top;
        const std::uint16_t f = temp();
        temp();
        Label none;
        emit(Op::FieldInit, f);
        emitJump(Op::JUndefined, f, none);
        emit(Op::Mov, f + 1u, self);
        emit(Op::Call, f, f, 0);
        bind(none);
        fn_->top = at;
    }

    std::uint32_t emitJump(Op op, std::uint32_t reg, Label& label)
    {
        const std::uint32_t at = pc();
        emit(op, reg);
        if (label.target >= 0)
            fn_->block->code[at].setJump(std::int32_t(label.target - (at + 1)));
        else
            label.patches.push_back(at);
        return at;
    }
    void jump(Label& label) { emitJump(Op::Jmp, 0, label); }
    void bind(Label& label)
    {
        label.target = pc();
        for (std::uint32_t at : label.patches)
            fn_->block->code[at].setJump(std::int32_t(label.target - (at + 1)));
        label.patches.clear();
        for (std::uint32_t at : label.patches16)
            patch16(at, label.target);
        label.patches16.clear();
    }

    std::uint16_t temp()
    {
        if (fn_->top >= 0xFFF0)
            throw Error(ErrorCode::NotSupported, "vm core: function needs too many registers");
        const std::uint16_t r = fn_->top++;
        if (fn_->top > fn_->block->nregs)
            fn_->block->nregs = fn_->top;
        return r;
    }
    std::uint16_t temps(std::uint32_t n)
    {
        const std::uint16_t first = fn_->top;
        for (std::uint32_t i = 0; i < n; ++i)
            temp();
        return first;
    }

    /// Whether `atom(name)` has an index to give.
    bool roomForName(const String& name)
    {
        if (fn_->block->atoms.size() < 0xFFFF)
            return true;
        Key key = vm_.internKey(name);
        const bool known = fn_->atomIndex.count(key) != 0;
        detail::decRef(key);
        return known;
    }
    std::uint16_t atom(const String& name)
    {
        Key key = vm_.internKey(name);  // a count the block keeps
        // A name the source spells: there are only as many as the source has, so objects keyed by it may grow shapes
        // freely (Shape::after limits the transitions of names made at run time).
        if (key->kind == detail::CellKind::String)
            static_cast<StringCell*>(key)->markInSource();
        auto it = fn_->atomIndex.find(key);
        if (it != fn_->atomIndex.end()) {
            detail::decRef(key);
            return it->second;
        }
        const std::size_t index = fn_->block->atoms.size();
        if (index > 0xFFFF)
            throw Error(ErrorCode::NotSupported, "vm core: too many names in one function");
        fn_->block->atoms.push_back(key);
        fn_->atomIndex[key] = std::uint16_t(index);
        return std::uint16_t(index);
    }
    /// The index of a constant in the block's table: up to 2^32 for LoadK (`emitLoadK`), up to 65535 for the
    /// instructions that name a constant in one operand (`constant`).
    std::uint32_t constantIndex(JSValue owned)
    {
        auto it = fn_->constIndex.find(owned.bits);
        if (it != fn_->constIndex.end()) {
            release(owned);
            return it->second;
        }
        const std::size_t index = fn_->block->consts.size();
        if (index > 0xFFFFFFFEu)
            throw Error(ErrorCode::NotSupported, "vm core: too many constants in one function");
        fn_->block->consts.push_back(owned);
        fn_->constIndex[owned.bits] = std::uint32_t(index);
        return std::uint32_t(index);
    }
    std::uint16_t constant(JSValue owned)
    {
        const std::uint32_t index = constantIndex(owned);
        if (index > 0xFFFF)
            throw Error(ErrorCode::NotSupported, "vm core: too many constants in one function");
        return std::uint16_t(index);
    }
    /// LoadK names its constant with two operands, low half first.
    void emitLoadK(std::uint32_t dst, JSValue owned)
    {
        const std::uint32_t index = constantIndex(owned);
        emit(Op::LoadK, dst, index & 0xFFFF, index >> 16);
    }

    void loadNumber(std::uint16_t dst, double d)
    {
        if (d == double(std::int16_t(d)) && !(d == 0 && std::signbit(d)))
            emit(Op::LoadInt, dst, std::uint16_t(std::int16_t(d)));
        else
            emitLoadK(dst, (JSValue::boxNumber(d)));
    }
    void loadString(std::uint16_t dst, const String& text)
    {
        emitLoadK(dst, (fromValue(vm_, ctx_.str(text))));
    }

    // ---------------------------------------------------------------- scopes ---
    Binding* findInScopes(Fn& fn, const String& name)
    {
        for (std::size_t i = fn.scopes.size(); i-- > 0;)
            if (Binding* b = fn.scopes[i].find(name))
                return b;
        return nullptr;
    }

    void pushScope()
    {
        fn_->scopes.emplace_back();
        fn_->scopes.back().baseTop = fn_->top;
    }
    void popScope()
    {
        fn_->top = fn_->scopes.back().baseTop;
        fn_->scopes.pop_back();
    }

    bool isCaptured(const String& name) const
    {
        return fn_->captured.count(name) != 0 || fn_->captured.count(String(u"%all")) != 0;
    }

    Binding& declare(const String& name, BKind kind)
    {
        Scope& scope = fn_->scopes.back();
        if (Binding* b = scope.find(name))
            return *b;  // a second `var` or function of the same name is the first one
        Binding b;
        b.name = name;
        b.kind = kind;
        b.reg = temp();
        b.boxed = isCaptured(name);
        scope.bindings.push_back(b);
        return scope.bindings.back();
    }

    /// What a name means from here: a binding of this function, one of an enclosing function (an upvalue), or a global.
    struct Ref
    {
        enum Kind { Local, Upval, Global } kind = Global;
        Binding* binding = nullptr;  ///< for Local
        std::uint16_t upval = 0;
        bool checked = false;  ///< a read needs the hole check
        bool isConst = false;
        bool isFnName = false;  ///< a named function expression's own name: stores are ignored (an error in strict code)
    };

    Ref resolve(const String& name)
    {
        return resolveIn(*fn_, name);
    }

    Ref resolveIn(Fn& fn, const String& name)
    {
        Ref r;
        if (Binding* b = findInScopes(fn, name)) {
            r.kind = Ref::Local;
            r.binding = b;
            r.isConst = b->kind == BKind::Const;
            r.isFnName = b->kind == BKind::FnName;
            r.checked = b->tdz && !b->initialized;
            return r;
        }
        if (!fn.parent || (fn.isProgram))
            return r;
        // `this` of an arrow is its parent's; every other name is looked for outwards.
        Ref outer = resolveIn(*fn.parent, name);
        if (outer.kind == Ref::Global)
            return r;
        auto it = fn.upvalIndex.find(name);
        std::uint16_t index;
        if (it != fn.upvalIndex.end()) {
            index = it->second;
        } else {
            UpvalDesc d;
            if (outer.kind == Ref::Local) {
                if (!outer.binding->boxed)
                    throw Error(ErrorCode::NotSupported, "vm core: internal error: an unboxed variable is captured");
                d.index = outer.binding->reg;
                d.fromParentLocal = true;
            } else {
                d.index = outer.upval;
                d.fromParentLocal = false;
            }
            index = std::uint16_t(fn.block->upvals.size());
            fn.block->upvals.push_back(d);
            fn.upvalIndex[name] = index;
        }
        r.kind = Ref::Upval;
        r.upval = index;
        r.isConst = outer.isConst;
        r.isFnName = outer.isFnName;
        r.checked = outer.checked;
        return r;
    }

    // ------------------------------------------------------ eval variables ---
    /// The eval objects a name that is not declared anywhere may live on, innermost first: one for each sloppy function around
    /// this code that has a direct `eval` in it, and the ones of the code a direct `eval` runs in.
    std::vector<Ref> evalEnvRefs()
    {
        std::vector<Ref> refs;
        for (Fn* f = fn_; f; f = f->parent) {
            if (!f->evalEnvName.empty())
                refs.push_back(resolve(f->evalEnvName));
            if (f->evalOuter)
                for (const Binding& b : f->scopes.back().bindings)
                    if (isEvalEnvName(b.name))
                        refs.push_back(resolve(b.name));
        }
        return refs;
    }
    /// The eval object of `e` in a register of its own.
    std::uint16_t loadEnv(const Ref& e)
    {
        const std::uint16_t t = temp();
        emit(e.kind == Ref::Local ? Op::GetBox : Op::GetUpval, t, e.kind == Ref::Local ? e.binding->reg : e.upval);
        return t;
    }

    // -------------------------------------------------------------- variables ---
    /// Read `name` into `dst` (or a register of its own, when `dst` is negative and the variable lives in one).
    std::uint16_t loadVar(const String& name, int dst, const Node& at, bool forTypeof = false)
    {
        Ref r = resolve(name);
        if (r.kind == Ref::Local && !r.binding->boxed) {
            if (r.checked) {
                const std::uint16_t d = dst >= 0 ? std::uint16_t(dst) : r.binding->reg;
                emit(Op::CheckInit, r.binding->reg, atom(name));
                if (dst >= 0 && dst != r.binding->reg)
                    emit(Op::Mov, d, r.binding->reg);
                return d;
            }
            if (dst >= 0 && dst != r.binding->reg) {
                emit(Op::Mov, std::uint16_t(dst), r.binding->reg);
                return std::uint16_t(dst);
            }
            return r.binding->reg;
        }
        const std::uint16_t d = dst >= 0 ? std::uint16_t(dst) : temp();
        if (r.kind == Ref::Local) {
            emit(Op::GetBox, d, r.binding->reg);
            if (r.checked)
                emit(Op::CheckInit, d, atom(name));
        } else if (r.kind == Ref::Upval) {
            emit(Op::GetUpval, d, r.upval);
            if (r.checked)
                emit(Op::CheckInit, d, atom(name));
        } else {
            if (name == u"undefined") {
                emit(Op::LoadUndef, d);
            } else {
                const std::vector<Ref> envs = evalEnvRefs();
                Label done;
                const std::uint16_t mark = fn_->top;
                for (const Ref& e : envs) {
                    const std::uint16_t env = loadEnv(e), flag = temp();
                    emit(Op::EnvHas, flag, env, atom(name));
                    Label next;
                    emitJump(Op::JFalse, flag, next);
                    emit(Op::EnvGet, d, env, atom(name));
                    jump(done);
                    bind(next);
                    fn_->top = mark;
                }
                emitCached(forTypeof ? Op::GetGlobalOrUndef : Op::GetGlobal, d, atom(name), 0);
                bind(done);
            }
        }
        (void)at;
        return d;
    }

    /// Store `src` in the variable `name`; `init` for the statement that declares it.
    void storeVar(const String& name, std::uint16_t src, bool init, const Node& at)
    {
        Ref r = resolve(name);
        if (!init && r.isFnName) {
            if (fn_->strict)
                emit(Op::ThrowError, std::uint32_t(ErrorCode::TypeError), constant(fromValue(vm_, ctx_.str("Assignment to constant variable."))));
            return;
        }
        if (!init && r.isConst) {
            emit(Op::ThrowError, std::uint32_t(ErrorCode::TypeError), constant(fromValue(vm_, ctx_.str("Assignment to constant variable."))));
            return;
        }
        if (r.kind == Ref::Local) {
            Binding& b = *r.binding;
            if (!init && r.checked) {
                // Assigning to a `let` still in its dead zone is an error.
                const std::uint16_t t = temp();
                if (b.boxed)
                    emit(Op::GetBox, t, b.reg);
                else
                    emit(Op::Mov, t, b.reg);
                emit(Op::CheckInit, t, atom(name));
                fn_->top--;
            }
            if (b.boxed)
                emit(Op::PutBox, b.reg, src);
            else if (b.reg != src)
                emit(Op::Mov, b.reg, src);
            if (init)
                b.initialized = true;
        } else if (r.kind == Ref::Upval) {
            if (r.checked) {
                const std::uint16_t t = temp();
                emit(Op::GetUpval, t, r.upval);
                emit(Op::CheckInit, t, atom(name));
                fn_->top--;
            }
            emit(Op::PutUpval, r.upval, src);
        } else {
            const std::vector<Ref> envs = evalEnvRefs();
            const std::uint16_t mark = fn_->top;
            if (init && fn_->dynamicVars && !envs.empty()) {
                // A function or `var` of sloppy eval code goes to the eval object of the function that called it.
                const std::uint16_t env = loadEnv(envs.front());
                emit(Op::DefEnv, env, atom(name), src);
                fn_->top = mark;
            } else if (init) {
                emit(Op::DefGlobal, atom(name), src, fn_->dynamicVars ? 1 : 0);
            } else {
                Label done;
                for (const Ref& e : envs) {
                    const std::uint16_t env = loadEnv(e), flag = temp();
                    emit(Op::EnvHas, flag, env, atom(name));
                    Label next;
                    emitJump(Op::JFalse, flag, next);
                    emit(Op::EnvSet, env, atom(name), src);
                    jump(done);
                    bind(next);
                    fn_->top = mark;
                }
                emitCached(Op::PutGlobal, atom(name), src, 0);
                bind(done);
            }
        }
        (void)at;
    }

    // ----------------------------------------------------------- expressions ---
    std::uint16_t expr(const Node& n, int dst = -1);
    std::uint16_t exprTemp(const Node& n)
    {
        const std::uint16_t d = temp();
        expr(n, d);
        return d;
    }
    /// A register holding the value, which may be a local's own register: not to be kept across code that can assign.
    std::uint16_t exprAny(const Node& n) { return expr(n, -1); }
    std::uint16_t dest(int dst) { return dst >= 0 ? std::uint16_t(dst) : temp(); }
    /// For constructs that write their destination before they are done (a literal, a condition): through a temporary
    /// unless the caller gave a register that nothing else reads.

    std::uint16_t member(const Node& n, int dst);
    std::uint16_t call(const Node& n, int dst, bool isNew);
    std::uint16_t chainRoot(const Node& n, int dst, bool isNew);
    std::uint16_t unary(const Node& n, int dst);
    std::uint16_t update(const Node& n, int dst, bool discard);
    std::uint16_t binary(const Node& n, int dst);
    std::uint16_t binaryStep(const Node& n, int dst, int left);
    std::uint16_t logical(const Node& n, int dst);
    std::uint16_t assign(const Node& n, int dst, bool unused = false);
    std::uint16_t conditional(const Node& n, int dst);
    std::uint16_t objectLit(const Node& n, int dst);
    std::uint16_t arrayLit(const Node& n, int dst);
    std::uint16_t templateLit(const Node& n, int dst);
    std::uint16_t function(const Node& n, const String& inferredName, int dst, bool exactName = false);
    void condJump(const Node& cond, Label& whenFalse);
    void condJumpTrue(const Node& cond, Label& whenTrue);
    bool isAnonymousFunction(const Node& n) const
    {
        return isFunctionNode(n) && n.text.empty() && !n.isMethod;
    }
    std::uint16_t exprNamed(const Node& n, int dst, const String& name)
    {
        if (isAnonymousFunction(n))
            return function(n, name, dst);
        if (n.kind == NK::ClassExpr && n.text.empty())
            return classExpr(n, dst, name);
        return expr(n, dst);
    }

    // ------------------------------------------------------------ statements ---
    void statement(const Node& n);
    void statements(const std::vector<dyn::NodePtr>& list);
    void blockScoped(const std::vector<dyn::NodePtr>& list, bool newScope);
    void declareLexical(const std::vector<dyn::NodePtr>& list);
    void hoistFunctions(const std::vector<dyn::NodePtr>& list);
    void annexBCopy(const Node& n);
    void branch(const Node& n);
    void varDecl(const Node& n);
    void ifStatement(const Node& n);
    void forStatement(const Node& n);
    void forInOf(const Node& n, bool isOf);
    void whileStatement(const Node& n);
    void doWhile(const Node& n);
    void returnStatement(const Node& n);
    void breakContinue(const Node& n, bool isBreak);
    void tryStatement(const Node& n);
    void switchStatement(const Node& n);
    void labeled(const Node& n);
    void loopBody(const Node& body, Label& breakL, Label& continueL, std::vector<String> labels);
    void emitFinallyFor(std::size_t fromDepth);
    void bindPattern(const Node& target, std::uint16_t value, bool init, VarKind kind);
    void bindTarget(const Node& target, std::uint16_t value, bool init, VarKind kind);
    void closeTry(TryCtx& ctx, std::uint32_t end, std::uint32_t target, std::uint16_t reg, bool isCatch = false);

    std::vector<String> pendingLabels_;
    bool evalHome_ = false;  ///< compiling eval code that was called from inside a method
    int classDepth_ = 0;  ///< inside a class body, all of which is strict
    /// While a parameter's default is worked out: the parameters not yet initialised, which may not be read.
    struct ParamTdz
    {
        const Fn* fn = nullptr;
        std::map<String, std::size_t> index;
        std::size_t current = 0;
    } paramTdz_;
    bool loopGuard_ = false;  ///< the next loop body sits inside the iterator-closing guard of a `for...of`

    CodeBlock* compileFunction(const Node& n, const String& name);
    CodeBlock* compileFunctionOnce(const Node& n, const String& name, std::uint32_t extraFlags);
    CodeBlock* functionHeader(const Node& n, const String& name, std::uint32_t extraFlags);
    void functionCode(const Node& n, CodeBlock* block, std::uint32_t extraFlags, const LazyInfo* resuming = nullptr);
    bool lazyEligible(const Node& n, const NameViews& names) const;
    CodeBlock* lazyStub(const Node& n, const String& name);
    std::uint32_t pendingFlags_ = 0;  ///< flags for the next function compiled: a class constructor's
    std::uint16_t classExpr(const Node& cls, int dst, const String& inferredName, bool innerBinding = true);
    CodeBlock* compileInitFunction(const std::vector<const Node*>& items, bool isStatic);
    CodeBlock* compileInitOnce(const std::vector<const Node*>& items, bool isStatic);
    void storeThis(std::uint16_t src);
    static String computedKeyName(const Node& field)
    {
        return u"%key" + fromUtf8(std::to_string(reinterpret_cast<std::uintptr_t>(&field)));
    }
    /// A method of an object literal that mentions `super` looks the prototype up from the literal.
    void homeFor(const Node& value, std::uint16_t fnReg, std::uint16_t object)
    {
        if (value.kind == NK::FunctionExpr && value.isMethod && value.d && usesSuper(*value.d))
            emit(Op::SetHome, fnReg, object);
    }
    /// The function that owns `this` reads it from register 0 even when an arrow captured it: only a derived constructor,
    /// where `super()` writes it, goes through the box.
    bool thisInRegister()
    {
        return !fn_->isArrow && (!thisBindsLate() || !findInScopes(*fn_, String(u"this")));
    }
    /// A register holding `this`: register 0, or a copy of the captured one inside an arrow.
    std::uint16_t thisRegister()
    {
        checkThisBound(Node(NK::ThisExpr));
        if (thisInRegister())
            return 0;
        const std::uint16_t t = temp();
        loadVar(String(u"this"), t, Node(NK::ThisExpr));
        return t;
    }
    /// In a derived constructor, and the arrows inside it, `this` is not there until `super()` has returned.
    bool thisBindsLate() const
    {
        const Fn* f = fn_;
        while (f->isArrow && f->parent)
            f = f->parent;
        return !f->isArrow && !f->isProgram && (f->block->flags & CodeBlock::kDerived);
    }
    void checkThisBound(const Node& at)
    {
        if (!thisBindsLate())
            return;
        const std::uint16_t mark = fn_->top;
        const std::uint16_t flag = loadVar(String(u"%bound"), -1, at);
        emit(Op::CheckBound, flag);
        fn_->top = mark;
    }
    /// Return `r`; a derived constructor checks what it returns first.
    void emitReturn(std::uint16_t r)
    {
        if (!fn_->isArrow && (fn_->block->flags & CodeBlock::kDerived)) {
            const std::uint16_t mark = fn_->top;
            const std::uint16_t flag = loadVar(String(u"%bound"), -1, Node(NK::ThisExpr));
            emit(Op::CheckDerivedRet, r, flag);
            fn_->top = mark;
            // An arrow function may have made `this`, which lives in its box.
            if (Binding* b = findInScopes(*fn_, String(u"this")))
                emit(Op::GetBox, 0, b->reg);
        }
        emit(Op::Ret, r);
    }
    // ---------------------------------------------------------- early errors ---
    void checkDeclarations(const std::vector<dyn::NodePtr>& statements, bool functionTop, const std::vector<String>* params = nullptr);
    void checkDeclarations(const std::vector<const Node*>& statements, bool functionTop, const std::vector<String>* params = nullptr);
    void checkBindingName(const String& name, const Node& at);
    void checkParams(const Node& fn, std::vector<String>& names);
    void checkClassMembers(const Node& cls);
    CodeBlock* programOnce(const Node& program);
    CodeBlock* evalOnce(const Node& program, const std::vector<String>& names, bool strict);
    void emitEvalScope();
    void functionBody(const Node& n, CodeBlock* block);
};


// ======================================================================= expressions ===

std::uint16_t Compiler::expr(const Node& n, int dst)
{
    checkNativeStack();
    const bool discard = discard_;
    discard_ = false;
    const std::uint32_t savedLine = line_;
    if (n.line)
        line_ = n.line;
    struct Restore
    {
        std::uint32_t& line;
        std::uint32_t saved;
        ~Restore() { line = saved; }
    } restore{line_, savedLine};

    switch (n.kind) {
        case NK::NumberLit: {
            if (n.legacyOctal && fn_->strict)
                syntaxError(n, "Octal literals are not allowed in strict mode");
            const std::uint16_t d = dest(dst);
            loadNumber(d, n.number);
            return d;
        }
        case NK::StringLit: {
            if (n.legacyOctal && fn_->strict)
                syntaxError(n, "Octal escape sequences are not allowed in strict mode");
            const std::uint16_t d = dest(dst);
            loadString(d, n.text);
            return d;
        }
        case NK::BoolLit: {
            const std::uint16_t d = dest(dst);
            emit(n.number != 0 ? Op::LoadTrue : Op::LoadFalse, d);
            return d;
        }
        case NK::NullLit: {
            const std::uint16_t d = dest(dst);
            emit(Op::LoadNull, d);
            return d;
        }
        case NK::Identifier:
            if (paramTdz_.fn == fn_) {
                const auto it = paramTdz_.index.find(n.text);
                if (it != paramTdz_.index.end() && it->second >= paramTdz_.current) {
                    const std::uint16_t d = dest(dst);
                    emit(Op::ThrowError, std::uint32_t(ErrorCode::ReferenceError),
                         constant(fromValue(vm_, ctx_.str(StringView(u"Cannot access '" + n.text + u"' before initialization")))));
                    emit(Op::LoadUndef, d);
                    return d;
                }
            }
            if (n.isPrivate && resolve(n.text).kind == Ref::Global)
                syntaxError(n, "Private field '" + toUtf8(n.text) + "' must be declared in an enclosing class");
            if (n.isPrivate)
                fn_->block->flags |= CodeBlock::kPrivate;
            if (fn_->strict && !n.isPrivate)
                if (isStrictReserved(n.text))
                    syntaxError(n, "Unexpected strict mode reserved word");
            // A field initialiser has no `arguments`, and eval code in one may not name it.
            if (fn_->isEval && n.text == u"arguments" && resolve(n.text).kind == Ref::Global && resolve(String(u"%noargs")).kind != Ref::Global)
                syntaxError(n, "'arguments' is not allowed in class field initializer or static initialization block");
            return loadVar(n.text, dst, n);
        case NK::ThisExpr: {
            checkThisBound(n);
            if (fn_->isEval && resolve(String(u"this")).kind == Ref::Global)
                return loadVar(String(u"globalThis"), dst, n);
            if (!thisInRegister())
                return loadVar(String(u"this"), dst, n);
            if (dst >= 0 && dst != 0) {
                emit(Op::Mov, std::uint16_t(dst), 0);
                return std::uint16_t(dst);
            }
            return 0;
        }
        case NK::TemplateLit: return templateLit(n, dst);
        case NK::ArrayLit: return arrayLit(n, dst);
        case NK::ObjectLit: return objectLit(n, dst);
        case NK::FunctionExpr:
        case NK::ArrowExpr: return function(n, n.text, dst);
        case NK::Unary: return unary(n, dst);
        case NK::Update: return update(n, dst, discard);
        case NK::Binary: return binary(n, dst);
        case NK::Logical: return logical(n, dst);
        case NK::Assign: return assign(n, dst, discard);
        case NK::Conditional: return conditional(n, dst);
        case NK::Call: return chainRoot(n, dst, false);
        case NK::NewExpr: return call(n, dst, true);
        case NK::Member: return chainRoot(n, dst, false);
        case NK::Sequence: {
            const std::uint16_t mark = fn_->top;
            for (std::size_t i = 0; i + 1 < n.list.size(); ++i) {
                expr(*n.list[i], -1);
                fn_->top = mark;
            }
            return expr(*n.list.back(), dst);
        }
        case NK::SuperExpr: {
            // Only as the object of `super.x = v` and the like, which store on `this`.
            checkThisBound(n);
            const std::uint16_t d = dest(dst);
            if (!thisInRegister())
                loadVar(String(u"this"), d, n);
            else
                emit(Op::Mov, d, 0);
            return d;
        }
        case NK::Spread: unsupported(n, "spread");
        case NK::Yield: {
            if (!(fn_->block->flags & CodeBlock::kGenerator))
                syntaxError(n, "Yield expression not allowed in formal parameter or static block");
            const std::uint16_t mark = fn_->top;
            const std::uint16_t d = dest(dst);
            const std::uint16_t inner = fn_->top;
            const bool asyncGen = (fn_->block->flags & CodeBlock::kAsyncGen) != 0;
            if (n.prefix && asyncGen) {
                // `yield*` in an async generator: the inner iterator's results are awaited, and what comes out is each value.
                const std::uint16_t src = exprAny(*n.a);
                const std::uint16_t it = temps(2);
                const std::uint16_t recv = temps(2);
                const std::uint16_t res = temp();
                const std::uint16_t flag = temp();
                const std::uint16_t v = temp();
                Label top, finished, noReturn, end, proceed;
                emit(Op::AsyncIterInit, it, src);
                emit(Op::LoadUndef, recv);
                loadNumber(std::uint16_t(recv + 1), 0);
                bind(top);
                emit(Op::DelegateCallAsync, res, it, recv);
                emitJump(Op::JHole, res, noReturn);
                emit(Op::Await, res, res);
                emit(Op::DelegateDone, flag, res);
                emitJump(Op::JTrue, flag, finished);
                // An iterator without `throw` was closed and its answer awaited: that is the TypeError.
                loadNumber(flag, 3);
                emit(Op::StrictEq, flag, std::uint16_t(recv + 1), flag);
                emitJump(Op::JFalse, flag, proceed);
                emit(Op::ThrowError, std::uint32_t(ErrorCode::TypeError), constant(fromValue(vm_, ctx_.str("The iterator does not provide a 'throw' method"))));
                bind(proceed);
                emitCached(Op::GetProp, v, res, atom(String(u"value")));
                emit(Op::YieldDelegate, recv, v, 1);
                jump(top);
                bind(finished);
                emitCached(Op::GetProp, v, res, atom(String(u"value")));
                loadNumber(flag, 2);
                emit(Op::StrictEq, flag, std::uint16_t(recv + 1), flag);
                emitJump(Op::JFalse, flag, end);
                emit(Op::Await, v, v);
                emitFinallyFor(0);
                emitReturn(v);
                bind(noReturn);
                emit(Op::Mov, v, recv);
                emit(Op::Await, v, v);
                emitFinallyFor(0);
                emitReturn(v);
                bind(end);
                emit(Op::Mov, d, v);
            } else if (n.prefix) {
                // `yield*`: each resumption goes to the inner iterator's next, throw or return, whose result is given out as
                // it is; when it says it is done its value is the value of the expression (or, for a return, the return).
                const std::uint16_t src = exprAny(*n.a);
                const std::uint16_t it = temps(2);
                const std::uint16_t recv = temps(2);
                const std::uint16_t res = temp();
                const std::uint16_t flag = temp();
                const std::uint16_t v = temp();
                Label top, finished, noReturn, end;
                emit(Op::GetIteratorObj, it, src);
                emit(Op::LoadUndef, recv);
                loadNumber(std::uint16_t(recv + 1), 0);
                bind(top);
                emit(Op::DelegateCall, res, it, recv);
                emitJump(Op::JHole, res, noReturn);
                emit(Op::DelegateDone, flag, res);
                emitJump(Op::JTrue, flag, finished);
                emit(Op::YieldDelegate, recv, res);
                jump(top);
                bind(finished);
                emitCached(Op::GetProp, v, res, atom(String(u"value")));
                loadNumber(flag, 2);
                emit(Op::StrictEq, flag, std::uint16_t(recv + 1), flag);
                emitJump(Op::JFalse, flag, end);
                emitFinallyFor(0);
                emitReturn(v);
                bind(noReturn);
                emit(Op::Mov, v, recv);
                emitFinallyFor(0);
                emitReturn(v);
                bind(end);
                emit(Op::Mov, d, v);
            } else {
                std::uint16_t v;
                if (n.a) {
                    v = exprAny(*n.a);
                } else {
                    v = temp();
                    emit(Op::LoadUndef, v);
                }
                // In an async generator the value is awaited first.
                if (asyncGen)
                    emit(Op::Await, v, v);
                emit(Op::Yield, d, v);
            }
            fn_->top = inner;
            (void)mark;
            return d;
        }
        case NK::Await: {
            if (!(fn_->block->flags & CodeBlock::kAsync))
                syntaxError(n, "await is only valid in async functions");
            const std::uint16_t mark = fn_->top;
            const std::uint16_t d = dest(dst);
            const std::uint16_t inner = fn_->top;
            const std::uint16_t v = exprAny(*n.a);
            emit(Op::Await, d, v);
            fn_->top = inner;
            (void)mark;
            return d;
        }
        case NK::ClassExpr: return classExpr(n, dst, String());
        case NK::BigIntLit: {
            bool ok = false;
            JSValue v = bigintFromText(vm_, n.text, &ok);
            if (!ok)
                syntaxError(n, "Invalid BigInt literal");
            const std::uint16_t d = dest(dst);
            emitLoadK(d, (v));
            return d;
        }
        case NK::RegExpLit: {
            const std::uint16_t d = dest(dst);
            const std::uint16_t pattern = constant(fromValue(vm_, ctx_.str(n.text)));
            const std::uint16_t flags = constant(fromValue(vm_, ctx_.str(n.extra().strs[0])));
            emit(Op::NewRegExp, d, pattern, flags);
            return d;
        }
        case NK::NewTarget: {
            if (fn_->isProgram)
                syntaxError(n, "new.target expression is not allowed here");
            if (fn_->isArrow) {
                if (resolve(String(u"%newtarget")).kind == Ref::Global)
                    syntaxError(n, "new.target expression is not allowed here");
                return loadVar(String(u"%newtarget"), dst, n);
            }
            const std::uint16_t d = dest(dst);
            emit(Op::NewTarget, d);
            return d;
        }
        case NK::TemplateObject: {
            // One frozen array for the place in the source, with its `raw` twin.
            auto frozenArray = [&](const std::vector<String>& items) {
                JSArray* a = vm_.newArray(vm_.protos.array, 0, std::uint32_t(items.size()));
                JSValue handle = toJSValue(a);
                for (const String& item : items)
                    a->append(vm_, fromValue(vm_, ctx_.str(item)));
                a->h.flags |= cellflags::kFrozen | cellflags::kNotExtensible;
                a->packed = 0;
                return handle;
            };
            JSValue cooked = frozenArray(n.extra().strs);
            JSValue raw = frozenArray(n.extra().raws);
            {
                Key rawKey = vm_.internKey(std::string_view("raw"));
                asObject(cooked)->putOwn(vm_, rawKey, raw, 0);
                detail::decRef(rawKey);
            }
            const std::uint16_t d = dest(dst);
            emitLoadK(d, (cooked));
            return d;
        }
        default: unsupported(n, "this expression");
    }
}

std::uint16_t Compiler::templateLit(const Node& n, int dst)
{
    const std::vector<String>& strs = n.extra().strs;
    const std::uint16_t mark = fn_->top;
    const std::uint16_t t = temp();
    loadString(t, strs.empty() ? String() : strs[0]);
    for (std::size_t i = 0; i < n.list.size(); ++i) {
        const std::uint16_t inner = fn_->top;
        const std::uint16_t v = exprAny(*n.list[i]);
        const std::uint16_t s = temp();
        emit(Op::ToString, s, v);
        emit(Op::Add, t, t, s);
        if (i + 1 < strs.size() && !strs[i + 1].empty()) {
            loadString(s, strs[i + 1]);
            emit(Op::Add, t, t, s);
        }
        fn_->top = inner;
    }
    if (dst >= 0) {
        emit(Op::Mov, std::uint16_t(dst), t);
        fn_->top = mark;
        return std::uint16_t(dst);
    }
    fn_->top = mark + 1;
    // The register `t` was the first temp at `mark`; it is the result.
    return t;
}

std::uint16_t Compiler::arrayLit(const Node& n, int dst)
{
    const std::uint16_t mark = fn_->top;
    const std::uint16_t t = temp();
    emit(Op::NewArray, t, std::uint32_t(std::min<std::size_t>(n.list.size(), 0xFFFF)));
    for (const dyn::NodePtr& e : n.list) {
        const std::uint16_t inner = fn_->top;
        if (e->kind == NK::Hole) {
            const std::uint16_t h = temp();
            emit(Op::LoadHole, h);
            emit(Op::ArrayPush, t, h);
        } else if (e->kind == NK::Spread) {
            const std::uint16_t src = exprAny(*e->a);
            const std::uint16_t it = temps(2);
            const std::uint16_t v = temp();
            Label top, done;
            emit(Op::IterInit, it, src);
            bind(top);
            emit(Op::IterNext, v, it, 0);
            emitJump(Op::JHole, v, done);
            emit(Op::ArrayPush, t, v);
            jump(top);
            bind(done);
        } else {
            const std::uint16_t v = exprAny(*e);
            emit(Op::ArrayPush, t, v);
        }
        fn_->top = inner;
    }
    if (dst >= 0) {
        emit(Op::Mov, std::uint16_t(dst), t);
        fn_->top = mark;
        return std::uint16_t(dst);
    }
    fn_->top = mark + 1;
    return t;
}

std::uint16_t Compiler::objectLit(const Node& n, int dst)
{
    const std::uint16_t mark = fn_->top;
    const std::uint16_t t = temp();
    // Room for the literal's properties, an odd count so that the cell is a multiple of 16 bytes (4.4).
    std::uint32_t inlineSlots = std::max<std::size_t>(3, n.list.size());
    inlineSlots = std::min<std::uint32_t>(inlineSlots | 1u, 15);
    emit(Op::NewObject, t, inlineSlots);
    for (const dyn::NodePtr& p : n.list) {
        const std::uint16_t inner = fn_->top;
        line_ = p->line ? p->line : line_;
        if (p->propKind == PropKind::Spread) {
            const std::uint16_t v = exprAny(*p->a);
            emit(Op::SpreadObj, t, v);
            fn_->top = inner;
            continue;
        }
        if (p->setsProto) {
            const std::uint16_t v = exprAny(*p->b);
            emit(Op::SetProto, t, v);
            fn_->top = inner;
            continue;
        }
        if (p->computed) {
            const std::uint16_t k = exprTemp(*p->a);
            // The key becomes a property key before the value is worked out.
            emit(Op::ToKey, k, k);
            if (p->propKind == PropKind::Getter || p->propKind == PropKind::Setter) {
                const std::uint16_t f = temp();
                function(*p->b, String(), f);
                homeFor(*p->b, f, t);
                emit(p->propKind == PropKind::Getter ? Op::InitGetterElem : Op::InitSetterElem, t, k, f);
            } else {
                const std::uint16_t v = temp();
                if (isAnonymousFunction(*p->b))
                    function(*p->b, String(), v);
                else
                    expr(*p->b, v);
                homeFor(*p->b, v, t);
                emit(Op::InitElem, t, k, v);
                if ((isFunctionNode(*p->b) || p->b->kind == NK::ClassExpr) && p->b->text.empty())
                    emit(Op::SetFnName, v, k);
            }
        } else {
            const String name = p->text;
            if (p->propKind == PropKind::Getter || p->propKind == PropKind::Setter) {
                const std::uint16_t f = temp();
                function(*p->b, (p->propKind == PropKind::Getter ? u"get " : u"set ") + name, f, true);
                homeFor(*p->b, f, t);
                emit(p->propKind == PropKind::Getter ? Op::InitGetter : Op::InitSetter, t, atom(name), f);
            } else if (!roomForName(name)) {
                // The function's names are used up (an object literal of tens of thousands of keys): the key as a
                // constant in a register.
                const std::uint16_t k = temp();
                loadString(k, name);
                const std::uint16_t v = temp();
                exprNamed(*p->b, v, name);
                homeFor(*p->b, v, t);
                emit(Op::InitElem, t, k, v);
            } else {
                const std::uint16_t v = temp();
                exprNamed(*p->b, v, name);
                homeFor(*p->b, v, t);
                emitCached(Op::InitProp, t, atom(name), v);
            }
        }
        fn_->top = inner;
    }
    if (dst >= 0) {
        emit(Op::Mov, std::uint16_t(dst), t);
        fn_->top = mark;
        return std::uint16_t(dst);
    }
    fn_->top = mark + 1;
    return t;
}

std::uint16_t Compiler::function(const Node& n, const String& inferredName, int dst, bool exactName)
{
    const String& childName = n.text.empty() || exactName ? inferredName : n.text;
    CodeBlock* child = lazyStub(n, childName);
    if (!child)
        child = compileFunction(n, childName);
    const std::size_t index = fn_->block->children.size();
    fn_->block->children.push_back(child);
    const std::uint16_t d = dest(dst);
    emit(Op::Closure, d, std::uint32_t(index));
    if (n.kind == NK::ArrowExpr && usesSuper(*n.d)) {
        // `super.x` inside the arrow means what it means in the method around it.
        const std::uint16_t h = temp();
        emit(Op::GetHome, h);
        emit(Op::SetHome, d, h);
        fn_->top = h;
    }
    return d;
}

void Compiler::storeThis(std::uint16_t src)
{
    if (fn_->isArrow) {
        storeVar(String(u"this"), src, true, Node(NK::ThisExpr));
        return;
    }
    if (src != 0)
        emit(Op::Mov, 0, src);
    if (Binding* b = findInScopes(*fn_, String(u"this"))) {
        if (b->boxed)
            emit(Op::PutBox, b->reg, 0);
        else
            emit(Op::Mov, b->reg, 0);
    }
}

void Compiler::checkClassMembers(const Node& n)
{
    bool hasConstructor = false;
    for (const dyn::NodePtr& m : n.list) {
        if ((m->kind == NK::FieldDef && m->b && containsArguments(*m->b)) || (m->kind == NK::StaticBlock && m->d && containsArguments(*m->d)))
            syntaxError(*m, "'arguments' is not allowed in class field initializer or static initialization block");
        if ((m->kind != NK::MethodDef && m->kind != NK::FieldDef) || m->computed || m->isPrivate || !m->a)
            continue;
        const bool isCtorName = m->a->text == u"constructor";
        if (m->kind == NK::FieldDef) {
            if (isCtorName || (m->isStatic && m->a->text == u"prototype"))
                syntaxError(*m, "Classes may not have a field named '" + toUtf8(m->a->text) + "'");
            continue;
        }
        if (m->isStatic && m->a->text == u"prototype")
            syntaxError(*m, "Classes may not have a static property named 'prototype'");
        if (m->propKind == PropKind::Constructor) {
            if (hasConstructor)
                syntaxError(*m, "A class may only have one constructor");
            hasConstructor = true;
            if (m->b->isGenerator || m->b->isAsync)
                syntaxError(*m, "Class constructor may not be a generator or async");
        } else if (isCtorName && !m->isStatic) {
            syntaxError(*m, "Class constructor may not be an accessor, a generator or async");
        }
    }
    // Private names: one declaration each, except a getter and a setter of the same kind of member.
    std::map<String, int> kinds;  // 1 getter, 2 setter, 4 anything else, 8 static
    for (const dyn::NodePtr& m : n.list) {
        if ((m->kind != NK::MethodDef && m->kind != NK::FieldDef) || !m->isPrivate)
            continue;
        const String& pn = m->a->text;
        if (pn == u"#constructor")
            syntaxError(*m, "Classes may not have a private field named '#constructor'");
        const int kind = m->kind == NK::MethodDef && m->propKind == PropKind::Getter ? 1
                         : m->kind == NK::MethodDef && m->propKind == PropKind::Setter ? 2 : 4;
        const auto seen = kinds.find(pn);
        if (seen != kinds.end() && (kind == 4 || (seen->second & 7 & kind) || (seen->second & 4) || bool(seen->second & 8) != m->isStatic))
            syntaxError(*m, "Identifier '" + toUtf8(pn) + "' has already been declared");
        kinds[pn] |= kind | (m->isStatic ? 8 : 0);
    }
}

std::uint16_t Compiler::classExpr(const Node& cls, int dst, const String& inferredName, bool innerBinding)
{
    const String name = cls.text.empty() ? inferredName : cls.text;
    const bool derived = cls.a != nullptr;
    struct ClassDepth
    {
        int& depth;
        explicit ClassDepth(int& d) : depth(d) { ++depth; }
        ~ClassDepth() { --depth; }
    } classDepth(classDepth_);
    checkClassMembers(cls);
    const Node* ctor = nullptr;
    for (const dyn::NodePtr& m : cls.list)
        if (m->kind == NK::MethodDef && m->propKind == PropKind::Constructor && !m->declared)
            ctor = m.get();
    for (const dyn::NodePtr& m : cls.list) {
        if ((m->kind == NK::MethodDef || m->kind == NK::FieldDef) && !m->computed && m->a && m->a->kind != NK::Identifier &&
            m->a->kind != NK::StringLit && m->a->kind != NK::NumberLit)
            unsupported(*m, "this class member name");
    }

    const std::uint16_t d = dest(dst);
    const std::uint16_t inner = fn_->top;
    const bool scoped = innerBinding && !cls.text.empty();
    // The class's private names, each a binding of the scope the class's functions see; the fields and the methods
    // that use them are other functions, so each is captured.
    std::vector<String> privateNames;
    for (const dyn::NodePtr& m : cls.list)
        if (m->isPrivate && !m->declared && m->a && std::find(privateNames.begin(), privateNames.end(), m->a->text) == privateNames.end())
            privateNames.push_back(m->a->text);
    const bool anyScope = scoped || !privateNames.empty();
    if (anyScope)
        pushScope();
    if (scoped) {
        Binding& b = declare(cls.text, BKind::Const);
        b.initialized = true;
        emit(Op::LoadUndef, b.reg);
        if (b.boxed)
            emit(Op::NewBox, b.reg);
    }
    for (const String& pn : privateNames) {
        fn_->captured.insert(pn);
        Binding& b = declare(pn, BKind::Const);
        b.initialized = true;
        emit(Op::NewPrivateName, b.reg, atom(pn));
        if (b.boxed)
            emit(Op::NewBox, b.reg);
    }
    const std::uint16_t f = temp();
    std::uint16_t sup = 0;
    if (derived) {
        sup = temp();
        expr(*cls.a, sup);
    }

    // The constructor: the one written, or the one the class gets without it.
    dyn::NodePtr made;
    const Node* ctorFn = nullptr;
    if (ctor) {
        ctorFn = ctor->b.get();
    } else {
        made = std::make_unique<Node>(NK::FunctionExpr, cls.line);
        made->isMethod = true;
        made->d = std::make_unique<Node>(NK::Block, cls.line);
        if (derived) {
            dyn::NodePtr rest = std::make_unique<Node>(NK::RestElement, cls.line);
            rest->a = std::make_unique<Node>(NK::Identifier, cls.line);
            rest->a->text = u"args";
            made->list.push_back(std::move(rest));
            dyn::NodePtr call = std::make_unique<Node>(NK::Call, cls.line);
            call->a = std::make_unique<Node>(NK::SuperExpr, cls.line);
            dyn::NodePtr spread = std::make_unique<Node>(NK::Spread, cls.line);
            spread->a = std::make_unique<Node>(NK::Identifier, cls.line);
            spread->a->text = u"args";
            spread->declared = true;  // the rest array is passed on as it is, not iterated
            call->list.push_back(std::move(spread));
            dyn::NodePtr stmt = std::make_unique<Node>(NK::ExprStatement, cls.line);
            stmt->a = std::move(call);
            made->d->list.push_back(std::move(stmt));
        }
        ctorFn = made.get();
    }
    if (ctorFn->isAsync || ctorFn->isGenerator)
        unsupported(*ctorFn, "this constructor");
    pendingFlags_ = std::uint32_t(CodeBlock::kClassConstructor) | (derived ? std::uint32_t(CodeBlock::kDerived) : 0u);
    CodeBlock* ctorCode = compileFunction(*ctorFn, name);
    if (cls.srcEnd) {
        // A class's text is its constructor's.
        ctorCode->srcStart = cls.srcStart;
        ctorCode->srcEnd = cls.srcEnd;
    }
    {
        const std::size_t index = fn_->block->children.size();
        fn_->block->children.push_back(ctorCode);
        emit(Op::NewClass, f, std::uint32_t(index), derived ? std::uint32_t(sup) + 1 : 0);
    }

    const std::uint16_t proto = temp();
    emitCached(Op::GetProp, proto, f, atom(String(u"prototype")));

    std::vector<const Node*> instanceFields, staticItems;
    for (const dyn::NodePtr& m : cls.list) {
        if (m->declared)
            continue;
        if (m->kind == NK::FieldDef) {
            if (m->computed) {
                // The key is worked out here, once, in the order the class is written.
                const String holder = computedKeyName(*m);
                fn_->captured.insert(holder);
                Binding& hb = declare(holder, BKind::Const);
                hb.initialized = true;
                const std::uint16_t at = fn_->top;
                const std::uint16_t t = temp();
                expr(*m->a, t);
                emit(Op::ToKey, t, t);
                emit(Op::Mov, hb.reg, t);
                if (hb.boxed)
                    emit(Op::NewBox, hb.reg);
                fn_->top = at;
            }
            (m->isStatic ? staticItems : instanceFields).push_back(m.get());
            continue;
        }
        if (m->kind == NK::StaticBlock) {
            staticItems.push_back(m.get());
            continue;
        }
        if (m->kind != NK::MethodDef || m->propKind == PropKind::Constructor)
            continue;
        const std::uint16_t target = m->isStatic ? f : proto;
        if (m->isPrivate) {
            // Made once here; each instance (or the class, if static) gets it where its fields are set up.
            const String& pn = m->a->text;
            const String holder = pn + (m->propKind == PropKind::Getter ? u" get" : m->propKind == PropKind::Setter ? u" set" : u" method");
            fn_->captured.insert(holder);
            Binding& hb = declare(holder, BKind::Const);
            hb.initialized = true;
            const std::uint16_t at = fn_->top;
            const std::uint16_t t = temp();
            function(*m->b, m->propKind == PropKind::Getter ? u"get " + pn : m->propKind == PropKind::Setter ? u"set " + pn : pn, t, true);
            emit(Op::SetHome, t, target);
            emit(Op::Mov, hb.reg, t);
            if (hb.boxed)
                emit(Op::NewBox, hb.reg);
            fn_->top = at;
            (m->isStatic ? staticItems : instanceFields).push_back(m.get());
            continue;
        }
        const std::uint16_t at = fn_->top;
        const Op plain = m->propKind == PropKind::Getter ? (m->computed ? Op::DefGetterElem : Op::DefGetter)
                         : m->propKind == PropKind::Setter ? (m->computed ? Op::DefSetterElem : Op::DefSetter)
                                                           : (m->computed ? Op::DefMethodElem : Op::DefMethod);
        if (m->computed) {
            const std::uint16_t k = exprTemp(*m->a);
            const std::uint16_t t = temp();
            function(*m->b, String(), t);
            emit(plain, target, k, t);
        } else {
            const std::uint16_t t = temp();
            function(*m->b, m->propKind == PropKind::Getter ? u"get " + m->a->text : m->propKind == PropKind::Setter ? u"set " + m->a->text : m->a->text, t, true);
            emit(plain, target, atom(m->a->text), t);
        }
        fn_->top = at;
    }
    if (!instanceFields.empty()) {
        const std::uint16_t at = fn_->top;
        const std::uint16_t t = temp();
        CodeBlock* init = compileInitFunction(instanceFields, false);
        const std::size_t index = fn_->block->children.size();
        fn_->block->children.push_back(init);
        emit(Op::Closure, t, std::uint32_t(index));
        emit(Op::SetHome, t, proto);
        emit(Op::SetFieldInit, f, t);
        fn_->top = at;
    }
    if (scoped)
        storeVar(cls.text, f, true, cls);
    if (!staticItems.empty()) {
        const std::uint16_t at = fn_->top;
        const std::uint16_t base = temps(2);
        CodeBlock* init = compileInitFunction(staticItems, true);
        const std::size_t index = fn_->block->children.size();
        fn_->block->children.push_back(init);
        emit(Op::Closure, base, std::uint32_t(index));
        emit(Op::SetHome, base, f);
        emit(Op::Mov, base + 1, f);
        emit(Op::Call, base, base, 0);
        fn_->top = at;
    }
    emit(Op::Mov, d, f);
    if (anyScope)
        popScope();
    fn_->top = inner;
    return d;
}

CodeBlock* Compiler::compileInitFunction(const std::vector<const Node*>& items, bool isStatic)
{
    const bool saved = noFuse_;
    for (;;) {
        try {
            CodeBlock* block = compileInitOnce(items, isStatic);
            noFuse_ = saved;
            return block;
        } catch (const FusionOverflow&) {
            if (noFuse_) {
                noFuse_ = saved;
                throw;
            }
            noFuse_ = true;
        } catch (...) {
            noFuse_ = saved;
            throw;
        }
    }
}

/// What runs a class's fields (and, for a static one, its static blocks): a method-like function whose `this` is the
/// instance, or the class.
CodeBlock* Compiler::compileInitOnce(const std::vector<const Node*>& items, bool isStatic)
{
    CodeBlock* block = vm_.newCodeBlock(nullptr, 0, false);
    block->flags |= CodeBlock::kStrict;
    block->file = file_;
    Fn child;
    child.isMethodFn = true;
    child.parent = fn_;
    child.block = block;
    child.strict = true;
    for (const Node* item : items) {
        if (item->kind == NK::FieldDef) {
            if (item->b)
                collectCaptured(*item->b, child.captured, captures_);
        } else if (item->kind == NK::StaticBlock) {
            collectCaptured(*item->d, child.captured, captures_);
        }
    }
    Fn* saved = fn_;
    const std::uint32_t savedLine = line_;
    Label* savedChain = chain_;
    chain_ = nullptr;
    std::vector<String> savedLabels;
    savedLabels.swap(pendingLabels_);
    fn_ = &child;
    try {
        fn_->top = 1;
        block->nregs = 1;
        pushScope();
        if (isCaptured(String(u"this"))) {
            Binding& b = declare(String(u"this"), BKind::Var);
            emit(Op::Mov, b.reg, 0);
            if (b.boxed)
                emit(Op::NewBox, b.reg);
        }
        if (isCaptured(String(u"%newtarget"))) {
            Binding& b = declare(String(u"%newtarget"), BKind::Var);
            emit(Op::LoadUndef, b.reg);
            if (b.boxed)
                emit(Op::NewBox, b.reg);
        }
        if (isCaptured(String(u"%all"))) {
            Binding& b = declare(String(u"%noargs"), BKind::Var);
            emit(Op::LoadUndef, b.reg);
            if (b.boxed)
                emit(Op::NewBox, b.reg);
        }
        for (const Node* item : items) {
            if (item->kind != NK::StaticBlock)
                continue;
            std::vector<String> vars;
            collectVars(*item->d, vars);
            for (const String& v : vars) {
                Binding& b = declare(v, BKind::Var);
                if (b.boxed)
                    emit(Op::NewBox, b.reg);
            }
        }
        // Private methods go on first, then fields and blocks in the order they were written.
        std::vector<const Node*> ordered;
        for (const Node* item : items)
            if (item->kind == NK::MethodDef)
                ordered.push_back(item);
        for (const Node* item : items)
            if (item->kind != NK::MethodDef)
                ordered.push_back(item);
        for (const Node* item : ordered) {
            line_ = item->line;
            const std::uint16_t mark = fn_->top;
            if (item->kind == NK::MethodDef) {
                const String& pn = item->a->text;
                const String holder = pn + (item->propKind == PropKind::Getter ? u" get" : item->propKind == PropKind::Setter ? u" set" : u" method");
                const std::uint16_t sym = temp();
                loadVar(pn, sym, *item);
                const std::uint16_t fnReg = temp();
                loadVar(holder, fnReg, *item);
                emit(item->propKind == PropKind::Getter ? Op::DefPrivGetter : item->propKind == PropKind::Setter ? Op::DefPrivSetter : Op::DefPrivMethod,
                     0, sym, fnReg);
            } else if (item->kind == NK::StaticBlock) {
                statement(*item->d);
            } else if (item->isPrivate) {
                const std::uint16_t sym = temp();
                loadVar(item->a->text, sym, *item);
                const std::uint16_t v = temp();
                if (item->b)
                    exprNamed(*item->b, v, item->a->text);
                else
                    emit(Op::LoadUndef, v);
                emit(Op::DefPrivate, 0, sym, v);
                fn_->block->flags |= CodeBlock::kPrivate;
            } else if (item->computed) {
                const std::uint16_t key = temp();
                loadVar(computedKeyName(*item), key, *item);
                const std::uint16_t v = temp();
                if (item->b)
                    expr(*item->b, v);
                else
                    emit(Op::LoadUndef, v);
                emit(Op::InitElem, 0, key, v);
                if (item->b && (isFunctionNode(*item->b) || item->b->kind == NK::ClassExpr) && item->b->text.empty())
                    emit(Op::SetFnName, v, key);
            } else {
                const String key = item->a->text;
                const std::uint16_t v = temp();
                if (item->b)
                    exprNamed(*item->b, v, key);
                else
                    emit(Op::LoadUndef, v);
                if (isStatic) {
                    emit(Op::InitProp, 0, atom(key), v);
                    emit(Op::Cache, kNoCache);
                } else {
                    emitCached(Op::InitProp, 0, atom(key), v);
                }
            }
            fn_->top = mark;
        }
        const std::uint16_t r = temp();
        emit(Op::LoadUndef, r);
        emit(Op::Ret, r);
        fn_->scopes.pop_back();
    } catch (...) {
        fn_ = saved;
        line_ = savedLine;
        chain_ = savedChain;
        pendingLabels_.swap(savedLabels);
        vm_.releaseCode(block);
        throw;
    }
    fn_ = saved;
    line_ = savedLine;
    chain_ = savedChain;
    pendingLabels_.swap(savedLabels);
    return block;
}

std::uint16_t Compiler::unary(const Node& n, int dst)
{
    const std::uint16_t mark = fn_->top;
    switch (n.op) {
        case Tok::Bang:
        case Tok::Minus:
        case Tok::Plus:
        case Tok::Tilde: {
            if (n.op == Tok::Minus && n.a->kind == NK::NumberLit) {
                const std::uint16_t d = dest(dst);
                loadNumber(d, -n.a->number);
                return d;
            }
            const std::uint16_t v = exprAny(*n.a);
            fn_->top = mark;
            const std::uint16_t d = dest(dst);
            emit(n.op == Tok::Bang ? Op::Not : n.op == Tok::Minus ? Op::Neg : n.op == Tok::Plus ? Op::ToNumber : Op::BitNot, d, v);
            return d;
        }
        case Tok::TypeOf: {
            std::uint16_t v;
            if (n.a->kind == NK::Identifier)
                v = loadVar(n.a->text, -1, *n.a, /*forTypeof=*/true);
            else
                v = exprAny(*n.a);
            fn_->top = mark;
            const std::uint16_t d = dest(dst);
            emit(Op::TypeOf, d, v);
            return d;
        }
        case Tok::Void: {
            exprAny(*n.a);
            fn_->top = mark;
            const std::uint16_t d = dest(dst);
            emit(Op::LoadUndef, d);
            return d;
        }
        case Tok::Delete: {
            const Node& target = *n.a;
            if (target.kind == NK::Identifier && fn_->strict)
                syntaxError(n, "Delete of an unqualified identifier in strict mode.");
            if (target.kind == NK::Member && target.computed && target.b && target.b->kind == NK::Identifier && target.b->isPrivate)
                syntaxError(n, "Private fields can not be deleted");
            if (target.kind == NK::Member) {
                const std::uint16_t o = exprTemp(*target.a);
                if (target.computed) {
                    const std::uint16_t k = exprAny(*target.b);
                    fn_->top = mark;
                    const std::uint16_t d = dest(dst);
                    emit(Op::DelElem, d, o, k);
                    return d;
                }
                fn_->top = mark;
                const std::uint16_t d = dest(dst);
                emit(Op::DelProp, d, o, atom(target.text));
                return d;
            }
            if (target.kind == NK::Identifier && resolve(target.text).kind == Ref::Global) {
                // An eval-declared variable can be deleted; so can a configurable property of the global object.
                const std::vector<Ref> envs = evalEnvRefs();
                const std::uint16_t d = dst >= 0 ? std::uint16_t(dst) : temp();
                const std::uint16_t inner = fn_->top;
                Label done;
                for (const Ref& e : envs) {
                    const std::uint16_t env = loadEnv(e), flag = temp();
                    emit(Op::EnvHas, flag, env, atom(target.text));
                    Label next;
                    emitJump(Op::JFalse, flag, next);
                    emit(Op::DelProp, d, env, atom(target.text));
                    jump(done);
                    bind(next);
                    fn_->top = inner;
                }
                emit(Op::DelGlobal, d, atom(target.text));
                bind(done);
                fn_->top = dst >= 0 ? mark : std::uint16_t(d + 1);
                return d;
            }
            if (target.kind != NK::Identifier)
                exprAny(target);
            fn_->top = mark;
            const std::uint16_t d = dest(dst);
            emit(target.kind == NK::Identifier ? Op::LoadFalse : Op::LoadTrue, d);
            return d;
        }
        default: unsupported(n, "this unary operator");
    }
}

namespace {

Op binaryOp(Tok t, bool* ok)
{
    *ok = true;
    switch (t) {
        case Tok::Plus: return Op::Add;
        case Tok::Minus: return Op::Sub;
        case Tok::Star: return Op::Mul;
        case Tok::Slash: return Op::Div;
        case Tok::Percent: return Op::Mod;
        case Tok::StarStar: return Op::Exp;
        case Tok::Amp: return Op::BitAnd;
        case Tok::Pipe: return Op::BitOr;
        case Tok::Caret: return Op::BitXor;
        case Tok::Shl: return Op::Shl;
        case Tok::Shr: return Op::Shr;
        case Tok::UShr: return Op::UShr;
        case Tok::Lt: return Op::Lt;
        case Tok::Gt: return Op::Gt;
        case Tok::Le: return Op::Le;
        case Tok::Ge: return Op::Ge;
        case Tok::EqEq: return Op::Eq;
        case Tok::NotEq: return Op::Ne;
        case Tok::EqEqEq: return Op::StrictEq;
        case Tok::NotEqEq: return Op::StrictNe;
        case Tok::In: return Op::In;
        case Tok::InstanceOf: return Op::InstanceOf;
        default: *ok = false; return Op::Nop;
    }
}

Op compoundOp(Tok t, bool* ok)
{
    switch (t) {
        case Tok::PlusAssign: return binaryOp(Tok::Plus, ok);
        case Tok::MinusAssign: return binaryOp(Tok::Minus, ok);
        case Tok::StarAssign: return binaryOp(Tok::Star, ok);
        case Tok::SlashAssign: return binaryOp(Tok::Slash, ok);
        case Tok::PercentAssign: return binaryOp(Tok::Percent, ok);
        case Tok::StarStarAssign: return binaryOp(Tok::StarStar, ok);
        case Tok::ShlAssign: return binaryOp(Tok::Shl, ok);
        case Tok::ShrAssign: return binaryOp(Tok::Shr, ok);
        case Tok::UShrAssign: return binaryOp(Tok::UShr, ok);
        case Tok::AmpAssign: return binaryOp(Tok::Amp, ok);
        case Tok::PipeAssign: return binaryOp(Tok::Pipe, ok);
        case Tok::CaretAssign: return binaryOp(Tok::Caret, ok);
        default: *ok = false; return Op::Nop;
    }
}

/// The form of an arithmetic or bitwise operator that takes a small integer constant, or Nop.
Op immediateOp(Op op) noexcept
{
    switch (op) {
        case Op::Mul: return Op::MulI;
        case Op::Mod: return Op::ModI;
        case Op::BitAnd: return Op::BitAndI;
        case Op::BitOr: return Op::BitOrI;
        case Op::BitXor: return Op::BitXorI;
        case Op::Shl: return Op::ShlI;
        case Op::Shr: return Op::ShrI;
        case Op::UShr: return Op::UShrI;
        default: return Op::Nop;
    }
}

Op immediateJump(Op op) noexcept
{
    switch (op) {
        case Op::JLt: return Op::JLtI;
        case Op::JLe: return Op::JLeI;
        case Op::JGt: return Op::JGtI;
        case Op::JGe: return Op::JGeI;
        case Op::JNotLt: return Op::JNotLtI;
        case Op::JNotLe: return Op::JNotLeI;
        case Op::JNotGt: return Op::JNotGtI;
        case Op::JNotGe: return Op::JNotGeI;
        case Op::JStrictNe: return Op::JStrictNeI;
        case Op::JStrictEq: return Op::JStrictEqI;
        default: return Op::Nop;
    }
}

/// 1 when a test is a literal that is always true (`true`, a non-zero number), 0 when always false, -1 otherwise.
int constantTest(const Node& n) noexcept
{
    if (n.kind == NK::BoolLit)
        return n.number != 0 ? 1 : 0;
    if (n.kind == NK::NumberLit)
        return n.number != 0 && n.number == n.number ? 1 : 0;
    return -1;
}

bool smallInt(const Node& n) noexcept
{
    return n.kind == NK::NumberLit && n.number == double(std::int16_t(n.number)) && !(n.number == 0 && std::signbit(n.number));
}

}  // namespace

std::uint16_t Compiler::binary(const Node& n, int dst)
{
    if (n.a->kind != NK::Binary)
        return binaryStep(n, dst, -1);
    // `a + b + c + ...` leans to the left: a loop over the spine keeps a long chain off the native stack.
    std::vector<const Node*> spine;
    for (const Node* cur = &n; cur->kind == NK::Binary; cur = cur->a.get())
        spine.push_back(cur);
    const std::uint32_t savedLine = line_;
    int result = -1;
    for (std::size_t i = spine.size(); i-- > 0;) {
        if (spine[i]->line)
            line_ = spine[i]->line;
        result = binaryStep(*spine[i], i == 0 ? dst : -1, result);
    }
    line_ = savedLine;
    return std::uint16_t(result);
}

/// One binary operator; `left`, when not -1, is the register that already holds the left operand.
std::uint16_t Compiler::binaryStep(const Node& n, int dst, int left)
{
    bool ok;
    const Op op = binaryOp(n.op, &ok);
    if (!ok)
        unsupported(n, "this binary operator");
    const std::uint16_t mark = left >= 0 ? std::uint16_t(left) : fn_->top;
    if (left >= 0)
        fn_->top = std::uint16_t(left + 1);
    auto leftOperand = [&](bool keep) -> std::uint16_t {
        if (left >= 0)
            return std::uint16_t(left);
        return keep ? exprAny(*n.a) : exprTemp(*n.a);
    };
    if ((op == Op::Add || op == Op::Sub) && smallInt(*n.b)) {
        const std::uint16_t a = leftOperand(true);
        fn_->top = mark;
        const std::uint16_t d = dest(dst);
        const int imm = int(n.b->number);
        emit(op == Op::Add ? Op::AddI : Op::SubI, d, a, std::uint16_t(std::int16_t(imm)));
        return d;
    }
    if (smallInt(*n.b) && immediateOp(op) != Op::Nop) {
        const std::uint16_t a = leftOperand(true);
        fn_->top = mark;
        const std::uint16_t d = dest(dst);
        emit(immediateOp(op), d, a, std::uint16_t(std::int16_t(int(n.b->number))));
        return d;
    }
    const std::uint16_t a = leftOperand(stableAcross(*n.a, *n.b));
    const std::uint16_t b = exprAny(*n.b);
    fn_->top = mark;
    const std::uint16_t d = dest(dst);
    emit(op, d, a, b);
    return d;
}

std::uint16_t Compiler::logical(const Node& n, int dst)
{
    const std::uint16_t mark = fn_->top;
    const std::uint16_t t = temp();
    expr(*n.a, t);
    Label end;
    emitJump(n.op == Tok::AndAnd ? Op::JFalse : n.op == Tok::OrOr ? Op::JTrue : Op::JNotNullish, t, end);
    expr(*n.b, t);
    bind(end);
    if (dst >= 0) {
        emit(Op::Mov, std::uint16_t(dst), t);
        fn_->top = mark;
        return std::uint16_t(dst);
    }
    fn_->top = mark + 1;
    return t;
}

std::uint16_t Compiler::conditional(const Node& n, int dst)
{
    const std::uint16_t mark = fn_->top;
    const std::uint16_t t = temp();
    Label elseL, end;
    condJump(*n.a, elseL);
    expr(*n.b, t);
    jump(end);
    bind(elseL);
    expr(*n.c, t);
    bind(end);
    if (dst >= 0) {
        emit(Op::Mov, std::uint16_t(dst), t);
        fn_->top = mark;
        return std::uint16_t(dst);
    }
    fn_->top = mark + 1;
    return t;
}

void Compiler::condJumpTrue(const Node& cond, Label& whenTrue)
{
    checkNativeStack();
    // `while (true)`: no value made and nothing tested.
    if (const int k = constantTest(cond); k >= 0) {
        if (k)
            emitJump(Op::Jmp, 0, whenTrue);
        return;
    }
    // Only the jumps matter in a test: `&&`, `||` and `!` become control flow, no value made.
    if (cond.kind == NK::Logical && cond.op == Tok::AndAnd) {
        Label no;
        condJump(*cond.a, no);
        condJumpTrue(*cond.b, whenTrue);
        bind(no);
        return;
    }
    if (cond.kind == NK::Logical && cond.op == Tok::OrOr) {
        condJumpTrue(*cond.a, whenTrue);
        condJumpTrue(*cond.b, whenTrue);
        return;
    }
    if (cond.kind == NK::Unary && cond.op == Tok::Bang) {
        condJump(*cond.a, whenTrue);
        return;
    }
    if (cond.kind == NK::Binary && !noFuse_ && (cond.op == Tok::EqEq || cond.op == Tok::NotEq) && cond.b->kind == NK::NullLit) {
        // `x == null` and `x != null`: one test of the value, no boolean.
        const std::uint16_t mark = fn_->top;
        const std::uint16_t a = exprAny(*cond.a);
        emitJump(cond.op == Tok::EqEq ? Op::JNullish : Op::JNotNullish, a, whenTrue);
        fn_->top = mark;
        return;
    }
    if (cond.kind == NK::Binary && !noFuse_) {
        Op fused = Op::Nop;
        switch (cond.op) {
            case Tok::Lt: fused = Op::JLt; break;
            case Tok::Le: fused = Op::JLe; break;
            case Tok::Gt: fused = Op::JGt; break;
            case Tok::Ge: fused = Op::JGe; break;
            case Tok::EqEqEq: fused = Op::JStrictEq; break;
            case Tok::NotEqEq: fused = Op::JStrictNe; break;
            case Tok::EqEq: fused = Op::JEq; break;
            case Tok::NotEq: fused = Op::JNe; break;
            default: break;
        }
        if (fused != Op::Nop) {
            const std::uint16_t mark = fn_->top;
            if (smallInt(*cond.b) && immediateJump(fused) != Op::Nop) {
                const std::uint16_t a = exprAny(*cond.a);
                emitFusedJump(immediateJump(fused), a, std::uint16_t(std::int16_t(int(cond.b->number))), whenTrue);
                fn_->top = mark;
                return;
            }
            const std::uint16_t a = isPure(*cond.b) || (cond.a->kind == NK::Identifier && stableAcross(*cond.a, *cond.b))
                                        ? exprAny(*cond.a) : exprTemp(*cond.a);
            const std::uint16_t b = exprAny(*cond.b);
            emitFusedJump(fused, a, b, whenTrue);
            fn_->top = mark;
            return;
        }
    }
    const std::uint16_t mark = fn_->top;
    const std::uint16_t r = exprAny(cond);
    emitJump(Op::JTrue, r, whenTrue);
    fn_->top = mark;
}

void Compiler::condJump(const Node& cond, Label& whenFalse)
{
    checkNativeStack();
    if (const int k = constantTest(cond); k >= 0) {
        if (!k)
            emitJump(Op::Jmp, 0, whenFalse);
        return;
    }
    if (cond.kind == NK::Logical && cond.op == Tok::AndAnd) {
        condJump(*cond.a, whenFalse);
        condJump(*cond.b, whenFalse);
        return;
    }
    if (cond.kind == NK::Logical && cond.op == Tok::OrOr) {
        Label yes;
        condJumpTrue(*cond.a, yes);
        condJump(*cond.b, whenFalse);
        bind(yes);
        return;
    }
    if (cond.kind == NK::Unary && cond.op == Tok::Bang) {
        condJumpTrue(*cond.a, whenFalse);
        return;
    }
    if (cond.kind == NK::Binary && !noFuse_ && (cond.op == Tok::EqEq || cond.op == Tok::NotEq) && cond.b->kind == NK::NullLit) {
        // `x == null` and `x != null`: one test of the value, no boolean.
        const std::uint16_t mark = fn_->top;
        const std::uint16_t a = exprAny(*cond.a);
        emitJump(cond.op == Tok::EqEq ? Op::JNotNullish : Op::JNullish, a, whenFalse);
        fn_->top = mark;
        return;
    }
    if (cond.kind == NK::Binary && !noFuse_) {
        Op fused = Op::Nop;
        switch (cond.op) {
            case Tok::Lt: fused = Op::JNotLt; break;
            case Tok::Le: fused = Op::JNotLe; break;
            case Tok::Gt: fused = Op::JNotGt; break;
            case Tok::Ge: fused = Op::JNotGe; break;
            case Tok::EqEqEq: fused = Op::JStrictNe; break;
            case Tok::NotEqEq: fused = Op::JStrictEq; break;
            case Tok::EqEq: fused = Op::JNe; break;
            case Tok::NotEq: fused = Op::JEq; break;
            default: break;
        }
        if (fused != Op::Nop) {
            const std::uint16_t mark = fn_->top;
            if (smallInt(*cond.b) && immediateJump(fused) != Op::Nop) {
                const std::uint16_t a = exprAny(*cond.a);
                emitFusedJump(immediateJump(fused), a, std::uint16_t(std::int16_t(int(cond.b->number))), whenFalse);
                fn_->top = mark;
                return;
            }
            const std::uint16_t a = isPure(*cond.b) || (cond.a->kind == NK::Identifier && stableAcross(*cond.a, *cond.b))
                                        ? exprAny(*cond.a) : exprTemp(*cond.a);
            const std::uint16_t b = exprAny(*cond.b);
            emitFusedJump(fused, a, b, whenFalse);
            fn_->top = mark;
            return;
        }
    }
    const std::uint16_t mark = fn_->top;
    const std::uint16_t r = exprAny(cond);
    emitJump(Op::JFalse, r, whenFalse);
    fn_->top = mark;
}

std::uint16_t Compiler::update(const Node& n, int dst, bool discard)
{
    const std::uint16_t mark = fn_->top;
    const Node& target = *n.a;
    const Op step = n.op == Tok::PlusPlus ? Op::Inc : Op::Dec;
    if (target.kind == NK::Identifier) {
        Ref r = resolve(target.text);
        if (r.kind == Ref::Local && !r.binding->boxed && !r.checked && !r.isConst && !r.isFnName) {
            const std::uint16_t reg = r.binding->reg;
            if (n.prefix || discard) {
                emit(step, reg, reg);
                if (dst >= 0 && dst != reg) {
                    emit(Op::Mov, std::uint16_t(dst), reg);
                    return std::uint16_t(dst);
                }
                return reg;
            }
            const std::uint16_t d = dest(dst);
            emit(Op::ToNumeric, d, reg);
            emit(step, reg, d);
            return d;
        }
        const std::uint16_t old = temp();
        loadVar(target.text, old, target);
        emit(Op::ToNumeric, old, old);
        const std::uint16_t fresh = temp();
        emit(step, fresh, old);
        storeVar(target.text, fresh, false, target);
        fn_->top = mark;
        const std::uint16_t d = dest(dst);
        emit(Op::Mov, d, n.prefix ? fresh : old);
        return d;
    }
    if (target.kind == NK::Member) {
        // Nothing after the key's evaluation can assign a local: the object and the key need no copies unless the key
        // assigns the object's name.
        const std::uint16_t o = !target.computed || stableAcross(*target.a, *target.b) ? exprAny(*target.a) : exprTemp(*target.a);
        std::uint16_t k = 0;
        if (target.computed)
            k = exprAny(*target.b);
        const std::uint16_t old = temp();
        if (target.computed)
            emit(Op::GetElem, old, o, k);
        else
            emitCached(Op::GetProp, old, o, atom(target.text));
        emit(Op::ToNumeric, old, old);
        const std::uint16_t fresh = temp();
        emit(step, fresh, old);
        if (target.computed)
            emit(Op::PutElem, o, k, fresh);
        else
            emitCached(Op::PutProp, o, atom(target.text), fresh);
        fn_->top = mark;
        const std::uint16_t d = dest(dst);
        emit(Op::Mov, d, n.prefix ? fresh : old);
        return d;
    }
    syntaxError(n, "Invalid left-hand side expression in " + std::string(n.prefix ? "prefix" : "postfix") + " operation");
}

std::uint16_t Compiler::assign(const Node& n, int dst, bool unused)
{
    const std::uint16_t mark = fn_->top;
    const Node& target = *n.a;
    bool isCompound;
    const Op op = compoundOp(n.op, &isCompound);
    const bool isLogical = n.op == Tok::AndAssign || n.op == Tok::OrAssign || n.op == Tok::NullishAssign;

    auto finish = [&](std::uint16_t value) -> std::uint16_t {
        if (unused && dst < 0) {
            // Nobody looks at the value.
            fn_->top = mark;
            return value >= mark ? mark : value;
        }
        if (dst >= 0) {
            if (dst != value)
                emit(Op::Mov, std::uint16_t(dst), value);
            fn_->top = mark;
            return std::uint16_t(dst);
        }
        // The value lives in a temporary above `mark`; keep one register for it.
        if (value >= mark) {
            if (value != mark)
                emit(Op::Mov, mark, value);
            fn_->top = mark;
            temp();
            return mark;
        }
        fn_->top = mark;
        return value;
    };

    if (target.kind == NK::ArrayPattern || target.kind == NK::ObjectPattern) {
        if (n.op != Tok::Assign)
            syntaxError(n, "Invalid destructuring assignment target");
        const std::uint16_t v = exprTemp(*n.b);
        bindPattern(target, v, false, VarKind::None);
        return finish(v);
    }

    if (target.kind == NK::Identifier) {
        if (n.op == Tok::Assign) {
            Ref r = resolve(target.text);
            if (r.kind == Ref::Local && !r.binding->boxed && !r.checked && !r.isConst && !r.isFnName &&
                (n.b->kind == NK::NumberLit || n.b->kind == NK::StringLit || n.b->kind == NK::Binary ||
                 n.b->kind == NK::Call || n.b->kind == NK::Identifier || n.b->kind == NK::Unary ||
                 n.b->kind == NK::Member || n.b->kind == NK::FunctionExpr || n.b->kind == NK::ArrowExpr ||
                 n.b->kind == NK::BoolLit || n.b->kind == NK::NullLit || n.b->kind == NK::NewExpr)) {
                const std::uint16_t reg = r.binding->reg;
                exprNamed(*n.b, reg, target.text);
                return finish(reg);
            }
            const std::uint16_t v = isAnonymousFunction(*n.b) ? function(*n.b, target.text, -1) : exprAny(*n.b);
            storeVar(target.text, v, false, target);
            return finish(v);
        }
        if (isLogical) {
            const std::uint16_t t = temp();
            loadVar(target.text, t, target);
            Label end;
            emitJump(n.op == Tok::AndAssign ? Op::JFalse : n.op == Tok::OrAssign ? Op::JTrue : Op::JNotNullish, t, end);
            exprNamed(*n.b, t, target.text);
            storeVar(target.text, t, false, target);
            bind(end);
            return finish(t);
        }
        if (!isCompound)
            unsupported(n, "this assignment operator");
        {
            // A local no other function can see, updated in place: no copy out and back.
            Ref r = resolve(target.text);
            if (r.kind == Ref::Local && !r.binding->boxed && !r.checked && !r.isConst && !r.isFnName && !hasAssignment(*n.b)) {
                const std::uint16_t reg = r.binding->reg;
                if ((op == Op::Add || op == Op::Sub) && smallInt(*n.b))
                    emit(op == Op::Add ? Op::AddI : Op::SubI, reg, reg, std::uint16_t(std::int16_t(n.b->number)));
                else if (smallInt(*n.b) && immediateOp(op) != Op::Nop)
                    emit(immediateOp(op), reg, reg, std::uint16_t(std::int16_t(n.b->number)));
                else {
                    const std::uint16_t v = exprAny(*n.b);
                    emit(op, reg, reg, v);
                }
                return finish(reg);
            }
        }
        const std::uint16_t old = temp();
        loadVar(target.text, old, target);
        const std::uint16_t v = exprAny(*n.b);
        emit(op, old, old, v);
        storeVar(target.text, old, false, target);
        return finish(old);
    }

    if (target.kind == NK::Member) {
        // A local the right side cannot assign needs no copy.
        const bool computed = target.computed;
        const bool objectStable = stableAcross(*target.a, *n.b) && (!computed || stableAcross(*target.a, *target.b));
        const bool keyStable = !computed || stableAcross(*target.b, *n.b);
        const bool stable = objectStable && keyStable;
        const std::uint16_t o = objectStable ? exprAny(*target.a) : exprTemp(*target.a);
        std::uint16_t k = 0;
        if (computed)
            k = keyStable ? exprAny(*target.b) : exprTemp(*target.b);
        const std::uint16_t nameAtom = computed ? 0 : atom(target.text);
        auto put = [&](std::uint16_t value) {
            if (computed)
                emit(Op::PutElem, o, k, value);
            else
                emitCached(Op::PutProp, o, nameAtom, value);
        };
        auto get = [&](std::uint16_t into) {
            if (computed)
                emit(Op::GetElem, into, o, k);
            else
                emitCached(Op::GetProp, into, o, nameAtom);
        };
        if (n.op == Tok::Assign) {
            const std::uint16_t v = stable ? exprAny(*n.b) : temp();
            if (!stable)
                expr(*n.b, v);
            put(v);
            return finish(v);
        }
        const std::uint16_t t = temp();
        get(t);
        if (isLogical) {
            Label end;
            emitJump(n.op == Tok::AndAssign ? Op::JFalse : n.op == Tok::OrAssign ? Op::JTrue : Op::JNotNullish, t, end);
            expr(*n.b, t);
            put(t);
            bind(end);
            return finish(t);
        }
        if (!isCompound)
            unsupported(n, "this assignment operator");
        const std::uint16_t v = exprAny(*n.b);
        emit(op, t, t, v);
        put(t);
        return finish(t);
    }
    syntaxError(n, "Invalid left-hand side in assignment");
}

// Member access and calls share the optional-chain machinery: the outermost link of a chain owns the label that every
// `?.` in it jumps to when its object is null or undefined, and the whole chain then yields undefined.
std::uint16_t Compiler::chainRoot(const Node& n, int dst, bool isNew)
{
    const bool root = chain_ == nullptr;
    Label shortCircuit;
    Label* saved = chain_;
    const bool savedUsed = chainUsed_;
    if (root) {
        chain_ = &shortCircuit;
        chainUsed_ = false;
    }
    const std::uint16_t mark = fn_->top;
    // Without a `?.` in it nothing can short-circuit, so the result goes straight to where it is wanted.
    const bool direct = dst >= 0 && !hasOptionalLink(n);
    const std::uint16_t t = direct ? std::uint16_t(dst) : temp();
    if (n.kind == NK::Call)
        call(n, t, isNew);
    else
        member(n, t);
    const bool used = chainUsed_;
    if (root) {
        chain_ = saved;
        chainUsed_ = savedUsed;
        if (used) {
            Label end;
            jump(end);
            bind(shortCircuit);
            emit(Op::LoadUndef, t);
            bind(end);
        }
    }
    if (direct) {
        fn_->top = mark;
        return std::uint16_t(dst);
    }
    if (dst >= 0) {
        emit(Op::Mov, std::uint16_t(dst), t);
        fn_->top = mark;
        return std::uint16_t(dst);
    }
    fn_->top = mark + 1;
    return t;
}

std::uint16_t Compiler::member(const Node& n, int dst)
{
    if (n.a->kind == NK::SuperExpr) {
        // `super.x` needs a method around it: a program, or eval code that runs outside any function, has none.
        {
            const Fn* holder = fn_;
            while (holder && holder->isArrow)
                holder = holder->parent;
            if (holder && (holder->evalOuter ? !evalHome_ : (holder->isProgram || !holder->isMethodFn)))
                syntaxError(n, "'super' keyword unexpected here");
        }
        const std::uint16_t at = fn_->top;
        if (n.computed) {
            const std::uint16_t self = thisRegister();
            const std::uint16_t key = exprAny(*n.b);
            fn_->top = at;
            const std::uint16_t d = dest(dst);
            emit(Op::GetSuperElem, d, key, self);
            return d;
        }
        const std::uint16_t d = dest(dst);
        const std::uint16_t inner = fn_->top;
        emit(Op::GetSuperProp, d, atom(n.text), thisRegister());
        fn_->top = inner;
        return d;
    }
    const std::uint16_t mark = fn_->top;
    const bool constantKey = n.computed && n.b->kind == NK::StringLit;
    const bool indexAssigns = n.computed && !constantKey &&
                              (n.a->kind == NK::Identifier ? assignsName(*n.b, n.a->text) : hasAssignment(*n.b));
    const std::uint16_t o = indexAssigns ? exprTemp(*n.a) : exprAny(*n.a);
    if (n.optional) {
        emitJump(Op::JNullish, o, *chain_);
        chainUsed_ = true;
    }
    if (n.computed && !constantKey) {
        Label* savedChain = chain_;
        chain_ = nullptr;
        const std::uint16_t k = exprAny(*n.b);
        chain_ = savedChain;
        fn_->top = mark;
        const std::uint16_t d = dest(dst);
        emit(Op::GetElem, d, o, k);
        return d;
    }
    fn_->top = mark;
    const std::uint16_t d = dest(dst);
    if (n.bindMethod) {
        emit(Op::GetPropBound, d, o, atom(constantKey ? n.b->text : n.text));
        return d;
    }
    emitCached(Op::GetProp, d, o, atom(constantKey ? n.b->text : n.text));
    return d;
}

std::uint16_t Compiler::call(const Node& n, int dst, bool isNew)
{
    const std::uint16_t mark = fn_->top;
    const std::uint16_t base = temps(2);
    const Node& callee = *n.a;
    {
        // A function expression called where it stands runs at once: it is compiled with what is around it.
        const Node* target = &callee;
        if (target->kind == NK::Member && !target->computed && (target->text == u"call" || target->text == u"apply"))
            target = target->a.get();
        if (target && target->kind == NK::FunctionExpr)
            calledAtOnce_.insert(target);
    }
    const bool superCall = callee.kind == NK::SuperExpr;
    if (superCall && (isNew || (fn_->isArrow ? resolve(String(u"%callee")).kind == Ref::Global
                                              : !((fn_->block->flags & CodeBlock::kClassConstructor) && (fn_->block->flags & CodeBlock::kDerived)))))
        syntaxError(n, "'super' keyword unexpected here");
    if (!isNew && callee.kind == NK::Member && callee.a->kind == NK::SuperExpr) {
        const std::uint16_t at = fn_->top;
        const std::uint16_t self = thisRegister();
        if (callee.computed) {
            const std::uint16_t key = exprAny(*callee.b);
            emit(Op::GetSuperElem, base, key, self);
        } else {
            emit(Op::GetSuperProp, base, atom(callee.text), self);
        }
        emit(Op::Mov, base + 1, self);
        fn_->top = at;
    } else if (superCall && fn_->isArrow) {
        const std::uint16_t at = fn_->top;
        const std::uint16_t calleeFn = loadVar(String(u"%callee"), -1, n);
        emit(Op::GetSuperCtorOf, base, calleeFn);
        fn_->top = at;
        loadVar(String(u"this"), base + 1, n);
        const std::uint16_t target = loadVar(String(u"%newtarget"), -1, n);
        emit(Op::SetNewTarget, target);
        fn_->top = at;
    } else if (superCall) {
        emit(Op::GetSuperCtor, base);
        emit(Op::Mov, base + 1, 0);
    } else if (!isNew && callee.kind == NK::Member) {
        expr(*callee.a, base + 1);
        if (callee.optional) {
            emitJump(Op::JNullish, base + 1, *chain_);
            chainUsed_ = true;
        }
        if (callee.computed) {
            Label* savedChain = chain_;
            chain_ = nullptr;
            const std::uint16_t k = exprAny(*callee.b);
            chain_ = savedChain;
            emit(Op::GetElem, base, base + 1, k);
            fn_->top = base + 2;
        } else {
            emitCached(Op::GetProp, base, base + 1, atom(callee.text));
        }
    } else {
        Label* savedChain = chain_;
        // `a?.b()` is one chain with the call inside it; the callee of a plain call or `new` is its own.
        if (callee.kind != NK::Member && callee.kind != NK::Call)
            chain_ = nullptr;
        expr(callee, base);
        chain_ = savedChain;
        emit(Op::LoadUndef, base + 1);
    }
    if (n.optional) {
        emitJump(Op::JNullish, base, *chain_);
        chainUsed_ = true;
    }
    Label* savedChain = chain_;
    chain_ = nullptr;
    bool spread = false;
    for (const dyn::NodePtr& arg : n.list)
        spread = spread || arg->kind == NK::Spread;
    std::uint32_t argc = std::uint32_t(n.list.size());
    if (spread) {
        // The arguments are collected into an array, which the call spreads over its window.
        const std::uint16_t arr = temp();
        if (n.list.size() == 1 && n.list[0]->declared) {
            expr(*n.list[0]->a, arr);
        } else {
            emit(Op::NewArray, arr, 0);
        }
        for (const dyn::NodePtr& arg : n.list) {
            if (arg->declared)
                continue;
            const std::uint16_t inner = fn_->top;
            if (arg->kind == NK::Spread) {
                const std::uint16_t src = exprAny(*arg->a);
                const std::uint16_t it = temps(2);
                const std::uint16_t v = temp();
                Label top, done;
                emit(Op::IterInit, it, src);
                bind(top);
                emit(Op::IterNext, v, it, 0);
                emitJump(Op::JHole, v, done);
                emit(Op::ArrayPush, arr, v);
                jump(top);
                bind(done);
            } else {
                const std::uint16_t v = exprAny(*arg);
                emit(Op::ArrayPush, arr, v);
            }
            fn_->top = inner;
        }
        argc = 0;
    } else {
        for (const dyn::NodePtr& arg : n.list) {
            const std::uint16_t r = temp();
            expr(*arg, r);
        }
    }
    chain_ = savedChain;
    const std::uint16_t d = dst >= 0 ? std::uint16_t(dst) : base;
    if (!isNew && callee.kind == NK::Identifier && callee.text == u"eval" && resolve(String(u"eval")).kind == Ref::Global)
        emitEvalScope();
    if (superCall && thisBindsLate()) {
        const std::uint16_t at = fn_->top;
        const std::uint16_t flag = loadVar(String(u"%bound"), -1, n);
        emit(Op::FreshThisIfBound, std::uint16_t(base + 1), flag);
        fn_->top = at;
    }
    emit(spread ? (superCall ? Op::SuperSpread : isNew ? Op::NewSpread : Op::CallSpread)
                : (superCall ? Op::SuperCall : isNew ? Op::New : Op::Call),
         d, base, argc);
    if (superCall && thisBindsLate()) {
        // A second `super()` runs the parent constructor again and is an error once it has returned.
        const std::uint16_t at = fn_->top;
        const std::uint16_t flag = loadVar(String(u"%bound"), -1, n);
        emit(Op::CheckUnbound, flag);
        fn_->top = at;
    }
    if (superCall) {
        // The constructor's `this` is what the parent made; the fields of this class are initialised now.
        const std::uint16_t at = fn_->top;
        storeThis(d);
        if (thisBindsLate()) {
            const std::uint16_t t = temp();
            emit(Op::LoadTrue, t);
            storeVar(String(u"%bound"), t, true, n);
            fn_->top = at;
        }
        if (fn_->isArrow) {
            const std::uint16_t calleeFn = loadVar(String(u"%callee"), -1, n);
            const std::uint16_t self = loadVar(String(u"this"), -1, n);
            emit(Op::InitFieldsOf, calleeFn, self);
            fn_->top = at;
        } else {
            fieldInit(d);
        }
    }
    fn_->top = mark;
    if (dst < 0) {
        fn_->top = mark + 1;
        if (fn_->top > fn_->block->nregs)
            fn_->block->nregs = fn_->top;
    }
    return d;
}

// ======================================================================= statements ===

void Compiler::statements(const std::vector<dyn::NodePtr>& list)
{
    for (const dyn::NodePtr& s : list)
        statement(*s);
}

/// Declare the `let`, `const` and `class` names of a statement list in the current scope, and emit what a binding needs at
/// scope entry: a hole where a read can precede the declaration, and a fresh box where a closure captures it.
void Compiler::declareLexical(const std::vector<dyn::NodePtr>& list)
{
    std::vector<std::pair<String, std::size_t>> lexical;
    for (std::size_t i = 0; i < list.size(); ++i)
        if (list[i]->kind == NK::VarDecl && list[i]->varKind != VarKind::Var) {
            std::vector<String> names;
            for (const dyn::NodePtr& d : list[i]->list)
                collectPatternNames(*d->a, names);
            for (String& name : names)
                lexical.emplace_back(std::move(name), i);
        }
    const std::unordered_set<std::u16string_view> early = earlyReadNames(list, lexical, captures_);
    for (std::size_t i = 0; i < list.size(); ++i) {
        const Node& s = *list[i];
        if (s.kind == NK::ClassDecl) {
            Binding& b = declare(s.text, BKind::Let);
            b.tdz = true;
            b.initialized = false;
            emit(Op::LoadUninit, b.reg);
            if (b.boxed)
                emit(Op::NewBox, b.reg);
            continue;
        }
        if (s.kind != NK::VarDecl || s.varKind == VarKind::Var)
            continue;
        std::vector<String> names;
        for (const dyn::NodePtr& d : s.list)
            collectPatternNames(*d->a, names);
        for (const String& name : names) {
            Binding& b = declare(name, s.varKind == VarKind::Const ? BKind::Const : BKind::Let);
            b.tdz = early.count(std::u16string_view(name)) != 0;
            b.initialized = false;
            if (b.tdz)
                emit(Op::LoadUninit, b.reg);
            if (b.boxed)
                emit(Op::NewBox, b.reg);
        }
    }
}

/// Function declarations are made when their scope is entered.
void Compiler::hoistFunctions(const std::vector<dyn::NodePtr>& list)
{
    const bool global = (fn_->isProgram && fn_->scopes.size() == 1) || (fn_->dynamicVars && fn_->scopes.size() == 1);
    // Declare every name first, so that a function can see its siblings; then make each closure.
    if (!global) {
        for (const dyn::NodePtr& s : list)
            if (s->kind == NK::FunctionDecl) {
                Binding& b = declare(s->text, BKind::Function);
                if (b.boxed && b.kind == BKind::Function)
                    emit(Op::NewBox, b.reg);
            }
    }
    for (const dyn::NodePtr& s : list) {
        if (s->kind != NK::FunctionDecl)
            continue;
        const std::uint16_t mark = fn_->top;
        const std::uint16_t t = function(*s->a, s->text, -1);
        storeVar(s->text, t, true, *s);
        fn_->top = mark;
    }
}

/// A name declared twice in one scope, or both lexically and by a `var` that passes through it. At the top of a function or
/// program a function declaration counts as a `var`; in a block it is lexical, and only plain ones may repeat in code that
/// is not strict.
void Compiler::checkDeclarations(const std::vector<dyn::NodePtr>& statements, bool functionTop, const std::vector<String>* params)
{
    std::vector<const Node*> plain;
    plain.reserve(statements.size());
    for (const dyn::NodePtr& s : statements)
        plain.push_back(s.get());
    checkDeclarations(plain, functionTop, params);
}

void Compiler::checkDeclarations(const std::vector<const Node*>& statements, bool functionTop, const std::vector<String>* params)
{
    if (statements.empty())
        return;
    std::set<String> lexical, plainFunctions, vars;
    const auto declareName = [&](const String& name, const Node& at) {
        if (!lexical.insert(name).second)
            syntaxError(at, "Identifier '" + toUtf8(name) + "' has already been declared");
    };
    for (const Node* s : statements) {
        std::vector<String> names;
        if (s->kind == NK::VarDecl && s->varKind != VarKind::Var) {
            for (const dyn::NodePtr& d : s->list)
                if (d->a)
                    collectPatternNames(*d->a, names);
            for (const String& name : names)
                declareName(name, *s);
        } else if (s->kind == NK::ClassDecl) {
            declareName(s->text, *s);
        } else if (s->kind == NK::FunctionDecl) {
            if (functionTop) {
                vars.insert(s->text);
            } else if (!fn_->strict && s->a && !s->a->isGenerator && !s->a->isAsync) {
                if (!plainFunctions.insert(s->text).second)
                    continue;
                if (lexical.count(s->text))
                    syntaxError(*s, "Identifier '" + toUtf8(s->text) + "' has already been declared");
                lexical.insert(s->text);
            } else {
                declareName(s->text, *s);
            }
        } else {
            collectVars(*s, names);
            vars.insert(names.begin(), names.end());
        }
    }
    for (const String& name : vars)
        if (lexical.count(name))
            syntaxError(*statements.front(), "Identifier '" + toUtf8(name) + "' has already been declared");
    if (params)
        for (const String& name : *params)
            if (lexical.count(name))
                syntaxError(*statements.front(), "Identifier '" + toUtf8(name) + "' has already been declared");
}

void Compiler::checkBindingName(const String& name, const Node& at)
{
    if (!fn_->strict)
        return;
    if (name == u"eval" || name == u"arguments")
        syntaxError(at, "Unexpected eval or arguments in strict mode");
    if (isStrictReserved(name))
        syntaxError(at, "Unexpected strict mode reserved word");
}

/// The names the parameters bind; a duplicate is an error in strict code, with a non-simple list, in an arrow and in a method.
void Compiler::checkParams(const Node& fn, std::vector<String>& names)
{
    bool simple = true;
    for (const dyn::NodePtr& p : fn.list) {
        simple = simple && p->kind == NK::Identifier;
        collectPatternNames(*p, names);
    }
    if (!simple && fn.d && hasUseStrict(*fn.d))
        syntaxError(fn, "Illegal 'use strict' directive in function with non-simple parameter list");
    std::set<String> seen;
    for (const String& name : names) {
        checkBindingName(name, fn);
        if (!seen.insert(name).second && (fn_->strict || !simple || fn_->isArrow || fn.isMethod))
            syntaxError(fn, "Duplicate parameter name not allowed in this context");
    }
    if (!fn.text.empty() && fn.kind == NK::FunctionExpr && !fn.isMethod)
        checkBindingName(fn.text, fn);
}

/// A function declared in a block, once evaluated, is copied to the `var` of the same name outside it (Annex B.3.3).
void Compiler::annexBCopy(const Node& n)
{
    if (fn_->strict || !n.a || n.a->isGenerator || n.a->isAsync || fn_->scopes.size() <= fn_->varScope + 1)
        return;
    const String& name = n.text;
    Binding* own = nullptr;
    for (Binding& b : fn_->scopes.back().bindings)
        if (b.name == name && b.kind == BKind::Function)
            own = &b;
    if (!own)
        return;
    for (std::size_t i = fn_->scopes.size() - 1; i-- > fn_->varScope;)
        for (const Binding& b : fn_->scopes[i].bindings)
            if (b.name == name && (i > fn_->varScope || b.kind == BKind::Let || b.kind == BKind::Const || b.kind == BKind::Class))
                return;
    const std::uint16_t mark = fn_->top;
    const std::uint16_t t = temp();
    loadVar(name, t, n);
    std::vector<Scope> hidden(std::make_move_iterator(fn_->scopes.begin() + std::ptrdiff_t(fn_->varScope) + 1),
                              std::make_move_iterator(fn_->scopes.end()));
    fn_->scopes.erase(fn_->scopes.begin() + std::ptrdiff_t(fn_->varScope) + 1, fn_->scopes.end());
    storeVar(name, t, false, n);
    for (Scope& s : hidden)
        fn_->scopes.push_back(std::move(s));
    fn_->top = mark;
}

void Compiler::blockScoped(const std::vector<dyn::NodePtr>& list, bool newScope)
{
    if (newScope) {
        if (!list.empty())
            checkDeclarations(list, false);
        pushScope();
    }
    declareLexical(list);
    hoistFunctions(list);
    statements(list);
    if (newScope)
        popScope();
}

void Compiler::statement(const Node& n)
{
    checkNativeStack();
    if (n.line)
        line_ = n.line;
    const std::uint16_t mark = fn_->top;
    switch (n.kind) {
        case NK::Empty: break;
        case NK::FunctionDecl: annexBCopy(n); break;
        case NK::ExprStatement:
            if (fn_->isProgram || fn_->completion) {
                expr(*n.a, fn_->resultReg);
            } else {
                discard_ = true;
                expr(*n.a, -1);
                discard_ = false;
            }
            break;
        case NK::VarDecl: varDecl(n); break;
        case NK::Block: blockScoped(n.list, true); break;
        case NK::If: ifStatement(n); break;
        case NK::For: forStatement(n); break;
        case NK::ForIn: forInOf(n, false); break;
        case NK::ForOf: forInOf(n, true); break;
        case NK::While: whileStatement(n); break;
        case NK::DoWhile: doWhile(n); break;
        case NK::ReturnStmt: returnStatement(n); break;
        case NK::BreakStmt: breakContinue(n, true); break;
        case NK::ContinueStmt: breakContinue(n, false); break;
        case NK::ThrowStmt: {
            const std::uint16_t r = exprAny(*n.a);
            emit(Op::Throw, r);
            break;
        }
        case NK::Try: tryStatement(n); break;
        case NK::Switch: switchStatement(n); break;
        case NK::Labeled: labeled(n); break;
        case NK::ClassDecl: {
            const std::uint16_t v = classExpr(*n.a, -1, n.text);
            storeVar(n.text, v, true, n);
            break;
        }
        default: unsupported(n, "this statement");
    }
    fn_->top = mark;
}

void Compiler::varDecl(const Node& n)
{
    for (const dyn::NodePtr& d : n.list) {
        const Node& target = *d->a;
        const bool lexical = n.varKind != VarKind::Var;
        if (target.kind == NK::Identifier) {
            const std::uint16_t mark = fn_->top;
            if (d->b) {
                Ref r = resolve(target.text);
                const NK k = d->b->kind;
                if (r.kind == Ref::Local && !r.binding->boxed && !r.isFnName &&
                    (k == NK::NumberLit || k == NK::StringLit || k == NK::Binary || k == NK::Call || k == NK::Identifier ||
                     k == NK::Unary || k == NK::Member || k == NK::FunctionExpr || k == NK::ArrowExpr || k == NK::BoolLit ||
                     k == NK::NullLit || k == NK::NewExpr || k == NK::ThisExpr)) {
                    // Straight into the variable's register.
                    exprNamed(*d->b, r.binding->reg, target.text);
                    r.binding->initialized = true;
                    fn_->top = mark;
                    continue;
                }
                const std::uint16_t v = exprNamed(*d->b, -1, target.text);
                storeVar(target.text, v, lexical, target);
            } else if (lexical) {
                const std::uint16_t v = temp();
                emit(Op::LoadUndef, v);
                storeVar(target.text, v, true, target);
            }
            fn_->top = mark;
        } else {
            if (!d->b)
                syntaxError(*d, "Missing initializer in destructuring declaration");
            const std::uint16_t mark = fn_->top;
            const std::uint16_t v = exprTemp(*d->b);
            bindPattern(target, v, lexical, n.varKind);
            fn_->top = mark;
        }
    }
}

void Compiler::bindTarget(const Node& target, std::uint16_t value, bool init, VarKind kind)
{
    checkNativeStack();
    if (target.kind == NK::Identifier) {
        storeVar(target.text, value, init, target);
    } else if (target.kind == NK::ArrayPattern || target.kind == NK::ObjectPattern) {
        bindPattern(target, value, init, kind);
    } else if (target.kind == NK::Member) {
        const std::uint16_t mark = fn_->top;
        const std::uint16_t o = exprTemp(*target.a);
        if (target.computed) {
            const std::uint16_t k = exprAny(*target.b);
            emit(Op::PutElem, o, k, value);
        } else {
            emitCached(Op::PutProp, o, atom(target.text), value);
        }
        fn_->top = mark;
    } else {
        syntaxError(target, "Invalid destructuring assignment target");
    }
}

void Compiler::bindPattern(const Node& target, std::uint16_t value, bool init, VarKind kind)
{
    checkNativeStack();
    const std::uint16_t mark = fn_->top;
    if (target.kind == NK::ObjectPattern) {
        // Reading a property of null or undefined is the TypeError a destructuring of it should raise.
        // The names taken so far, for a rest element that gets the others.
        const std::uint16_t taken = temp();
        bool needTaken = false;
        for (const dyn::NodePtr& p : target.list)
            needTaken = needTaken || p->kind == NK::RestElement;
        if (needTaken)
            emit(Op::NewArray, taken, 0);
        for (const dyn::NodePtr& p : target.list) {
            if (p->kind == NK::RestElement) {
                const std::uint16_t inner = fn_->top;
                const std::uint16_t v = temp();
                emit(Op::ObjRest, v, value, taken);
                bindTarget(*p->a, v, init, kind);
                fn_->top = inner;
                continue;
            }
            const std::uint16_t inner = fn_->top;
            if (needTaken) {
                const std::uint16_t k = temp();
                if (p->computed)
                    expr(*p->a, k);
                else
                    loadString(k, p->text);
                emit(Op::ArrayPush, taken, k);
            }
            const std::uint16_t v = temp();
            if (p->computed) {
                const std::uint16_t k = exprAny(*p->a);
                emit(Op::GetElem, v, value, k);
            } else {
                emitCached(Op::GetProp, v, value, atom(p->text));
            }
            const Node* t = p->b.get();
            if (t->kind == NK::AssignPattern) {
                Label has;
                emitJump(Op::JNotUndefined, v, has);
                exprNamed(*t->b, v, t->a->kind == NK::Identifier ? t->a->text : String());
                bind(has);
                t = t->a.get();
            }
            bindTarget(*t, v, init, kind);
            fn_->top = inner;
        }
    } else {
        const std::uint16_t it = temps(2);
        emit(Op::IterInit, it, value);
        for (const dyn::NodePtr& e : target.list) {
            const std::uint16_t inner = fn_->top;
            if (e->kind == NK::Hole) {
                const std::uint16_t v = temp();
                emit(Op::IterNext, v, it, 1);
                fn_->top = inner;
                continue;
            }
            if (e->kind == NK::RestElement) {
                const std::uint16_t r = temp();
                emit(Op::IterRest, r, it);
                bindTarget(*e->a, r, init, kind);
                fn_->top = inner;
                continue;
            }
            const std::uint16_t v = temp();
            emit(Op::IterNext, v, it, 1);
            const Node* t = e.get();
            if (t->kind == NK::AssignPattern) {
                Label has;
                emitJump(Op::JNotUndefined, v, has);
                exprNamed(*t->b, v, t->a->kind == NK::Identifier ? t->a->text : String());
                bind(has);
                t = t->a.get();
            }
            bindTarget(*t, v, init, kind);
            fn_->top = inner;
        }
        // An iterator the pattern did not use up is closed.
        emit(Op::IterClose, it, 0);
    }
    fn_->top = mark;
}

/// A branch of an `if`: a function declaration there (sloppy code only) is as if in a block of its own.
void Compiler::branch(const Node& n)
{
    if (n.kind != NK::FunctionDecl) {
        statement(n);
        return;
    }
    if (fn_->strict || n.a->isGenerator || n.a->isAsync)
        syntaxError(n, "In strict mode code, functions can only be declared at top level or inside a block");
    pushScope();
    Binding& b = declare(n.text, BKind::Function);
    if (b.boxed)
        emit(Op::NewBox, b.reg);
    const std::uint16_t mark = fn_->top;
    const std::uint16_t t = function(*n.a, n.text, -1);
    storeVar(n.text, t, true, n);
    fn_->top = mark;
    annexBCopy(n);
    popScope();
}

void Compiler::ifStatement(const Node& n)
{
    Label elseL, end;
    condJump(*n.a, elseL);
    branch(*n.b);
    if (n.c) {
        jump(end);
        bind(elseL);
        branch(*n.c);
        bind(end);
    } else {
        bind(elseL);
    }
}

void Compiler::loopBody(const Node& body, Label& breakL, Label& continueL, std::vector<String> labels)
{
    JumpCtx ctx;
    ctx.labels = std::move(labels);
    ctx.isLoop = true;
    ctx.breakTo = &breakL;
    ctx.continueTo = &continueL;
    ctx.tryDepth = fn_->tries.size();
    ctx.breakDepth = loopGuard_ ? ctx.tryDepth - 1 : ctx.tryDepth;
    loopGuard_ = false;
    fn_->jumps.push_back(std::move(ctx));
    statement(body);
    fn_->jumps.pop_back();
}

void Compiler::whileStatement(const Node& n)
{
    std::vector<String> labels;
    labels.swap(pendingLabels_);
    // The test is made once before the loop and once at the bottom of each pass, so a pass has one jump, not two.
    Label body, cont, end;
    condJump(*n.a, end);
    bind(body);
    loopBody(*n.d, end, cont, std::move(labels));
    bind(cont);
    condJumpTrue(*n.a, body);
    bind(end);
}

void Compiler::doWhile(const Node& n)
{
    std::vector<String> labels;
    labels.swap(pendingLabels_);
    Label top, cont, end;
    bind(top);
    loopBody(*n.d, end, cont, std::move(labels));
    bind(cont);
    condJumpTrue(*n.a, top);
    bind(end);
}

void Compiler::forStatement(const Node& n)
{
    std::vector<String> labels;
    labels.swap(pendingLabels_);
    pushScope();
    std::vector<Binding*> perIteration;
    const bool lexical = n.a && n.a->kind == NK::VarDecl && n.a->varKind != VarKind::Var;
    if (lexical) {
        std::vector<dyn::NodePtr> holder;  // declareLexical wants a statement list; this is one statement
        std::vector<String> names;
        for (const dyn::NodePtr& d : n.a->list)
            collectPatternNames(*d->a, names);
        for (const String& name : names) {
            Binding& b = declare(name, n.a->varKind == VarKind::Const ? BKind::Const : BKind::Let);
            b.tdz = false;
            if (b.boxed)
                emit(Op::NewBox, b.reg);
        }
    }
    if (n.a) {
        if (n.a->kind == NK::VarDecl)
            varDecl(*n.a);
        else {
            const std::uint16_t mark = fn_->top;
            // The parser keeps an expression here as a statement.
            expr(n.a->kind == NK::ExprStatement ? *n.a->a : *n.a, -1);
            fn_->top = mark;
        }
    }
    Label top, cont, end;
    if (n.b)
        condJump(*n.b, end);
    bind(top);
    loopBody(*n.d, end, cont, std::move(labels));
    bind(cont);
    // A closure made in one iteration keeps that iteration's variable: the next one starts from a copy.
    if (lexical)
        for (Binding& b : fn_->scopes.back().bindings)
            if (b.boxed)
                emit(Op::CopyBox, b.reg);
    if (n.c) {
        const std::uint16_t mark = fn_->top;
        discard_ = true;
        expr(*n.c, -1);
        discard_ = false;
        fn_->top = mark;
    }
    if (n.b)
        condJumpTrue(*n.b, top);
    else
        jump(top);
    bind(end);
    popScope();
}

void Compiler::forInOf(const Node& n, bool isOf)
{
    checkNativeStack();
    if (n.isAsync && !(fn_->block->flags & CodeBlock::kAsync))
        unsupported(n, "for await here");
    std::vector<String> labels;
    labels.swap(pendingLabels_);
    const std::uint16_t mark = fn_->top;
    const std::uint16_t src = exprTemp(*n.b);
    const std::uint16_t it = temps(2);
    if (isOf && n.isAsync) {
        emit(Op::AsyncIterInit, it, src);
    } else if (isOf) {
        emit(Op::IterInit, it, src);
    } else {
        const std::uint16_t keys = temp();
        emit(Op::KeysOf, keys, src);
        emit(Op::IterInit, it, keys);
        fn_->top--;
    }
    Label top, end;
    bind(top);
    const std::uint16_t v = temp();
    if (n.isAsync) {
        emit(Op::AsyncIterNext, v, it);
        emit(Op::Await, v, v);
        emit(Op::IterUnpack, v, v);
    } else {
        emit(Op::IterNext, v, it, 0);
    }
    emitJump(Op::JHole, v, end);
    // From here an early way out (`break`, `return`, an exception) closes the iterator.
    TryCtx guard;
    if (isOf) {
        guard.closesIterator = true;
        guard.iterReg = it;
        guard.start = pc();
        fn_->tries.push_back(std::move(guard));
        loopGuard_ = true;
    }
    pushScope();
    const Node& left = *n.a;
    if (n.varKind != VarKind::None) {
        // The parser keeps the declaration's kind on the loop and the target as its head.
        const bool lexical = n.varKind != VarKind::Var;
        if (lexical) {
            std::vector<String> names;
            collectPatternNames(left, names);
            for (const String& name : names) {
                Binding& b = declare(name, n.varKind == VarKind::Const ? BKind::Const : BKind::Let);
                if (b.boxed)
                    emit(Op::NewBox, b.reg);
            }
        }
        bindTarget(left, v, lexical, n.varKind);
    } else {
        bindTarget(left, v, false, VarKind::None);
    }
    loopBody(*n.d, end, top, std::move(labels));
    popScope();
    if (isOf) {
        TryCtx done = std::move(fn_->tries.back());
        fn_->tries.pop_back();
        const std::uint32_t bodyEnd = pc();
        jump(top);
        // An exception out of the body: close the iterator, then throw it again.
        const std::uint32_t excStart = pc();
        const std::uint16_t exc = temp();
        closeTry(done, bodyEnd, excStart, exc);
        emit(Op::IterClose, it, 1);
        emit(Op::Throw, exc);
    } else {
        jump(top);
    }
    bind(end);
    fn_->top = mark;
}

void Compiler::emitFinallyFor(std::size_t fromDepth)
{
    // Leaving `tries` from the innermost one out to `fromDepth`: each finally block runs where the jump is, outside the
    // range its own try protects, and inside the ranges of the tries around it.
    std::vector<TryCtx> saved = std::move(fn_->tries);
    fn_->tries.clear();
    for (std::size_t i = saved.size(); i-- > fromDepth;) {
        // While a finally copy runs, only the tries outside it are active.
        fn_->tries.assign(std::make_move_iterator(saved.begin()), std::make_move_iterator(saved.begin() + i));
        const Node* body = saved[i].finallyBody;
        if (body || saved[i].closesIterator) {
            const std::uint32_t start = pc();
            if (body)
                statement(*body);
            else
                emit(Op::IterClose, saved[i].iterReg, 0);
            const std::uint32_t endPc = pc();
            for (std::size_t j = i; j < saved.size(); ++j)
                saved[j].holes.push_back({start, endPc});
        }
        // Put the tries back, as the copy may have grown their holes.
        for (std::size_t j = 0; j < i; ++j)
            saved[j] = std::move(fn_->tries[j]);
        fn_->tries.clear();
    }
    fn_->tries = std::move(saved);
}

void Compiler::returnStatement(const Node& n)
{
    if (fn_->isProgram || fn_->isEval)
        syntaxError(n, "Illegal return statement");
    const std::uint16_t mark = fn_->top;
    std::uint16_t r;
    if (n.a) {
        r = exprTemp(*n.a);
    } else {
        r = temp();
        emit(Op::LoadUndef, r);
    }
    if (fn_->block->flags & CodeBlock::kAsyncGen)
        emit(Op::Await, r, r);
    emitFinallyFor(0);
    emitReturn(r);
    fn_->top = mark;
}

void Compiler::breakContinue(const Node& n, bool isBreak)
{
    JumpCtx* target = nullptr;
    for (std::size_t i = fn_->jumps.size(); i-- > 0;) {
        JumpCtx& c = fn_->jumps[i];
        if (!n.text.empty()) {
            if (std::find(c.labels.begin(), c.labels.end(), n.text) != c.labels.end() && (isBreak || c.isLoop)) {
                target = &c;
                break;
            }
        } else if (isBreak ? (c.isLoop || c.isSwitch) : c.isLoop) {
            target = &c;
            break;
        }
    }
    if (!target)
        syntaxError(n, isBreak ? "Illegal break statement" : "Illegal continue statement");
    emitFinallyFor(isBreak ? target->breakDepth : target->tryDepth);
    jump(isBreak ? *target->breakTo : *target->continueTo);
}

void Compiler::labeled(const Node& n)
{
    pendingLabels_.push_back(n.text);
    const Node& body = *n.d;
    const bool isLoop = body.kind == NK::For || body.kind == NK::ForIn || body.kind == NK::ForOf || body.kind == NK::While ||
                        body.kind == NK::DoWhile;
    if (isLoop || body.kind == NK::Labeled || body.kind == NK::Switch) {
        // The loop or switch takes the labels, so `continue label` and `break label` find it.
        statement(body);
        pendingLabels_.clear();
        return;
    }
    Label end;
    JumpCtx ctx;
    ctx.labels.swap(pendingLabels_);
    ctx.breakTo = &end;
    ctx.tryDepth = fn_->tries.size();
    ctx.breakDepth = ctx.tryDepth;
    fn_->jumps.push_back(std::move(ctx));
    statement(body);
    fn_->jumps.pop_back();
    bind(end);
}

void Compiler::switchStatement(const Node& n)
{
    std::vector<String> labels;
    labels.swap(pendingLabels_);
    const std::uint16_t mark = fn_->top;
    const std::uint16_t disc = exprTemp(*n.a);
    pushScope();
    std::vector<dyn::NodePtr> all;
    // The cases share one block scope: gather their declarations the way a block's are.
    std::vector<const Node*> bodies;
    for (const dyn::NodePtr& c : n.list)
        for (const dyn::NodePtr& s : c->list)
            bodies.push_back(s.get());
    checkDeclarations(bodies, false);
    // `declareLexical` and `hoistFunctions` take owning lists; walk the cases by hand with the same rules.
    for (const Node* s : bodies) {
        if (s->kind == NK::ClassDecl) {
            Binding& b = declare(s->text, BKind::Let);
            b.tdz = true;
            emit(Op::LoadUninit, b.reg);
            if (b.boxed)
                emit(Op::NewBox, b.reg);
        }
        if (s->kind == NK::VarDecl && s->varKind != VarKind::Var) {
            std::vector<String> names;
            for (const dyn::NodePtr& d : s->list)
                collectPatternNames(*d->a, names);
            for (const String& name : names) {
                Binding& b = declare(name, s->varKind == VarKind::Const ? BKind::Const : BKind::Let);
                b.tdz = true;
                emit(Op::LoadUninit, b.reg);
                if (b.boxed)
                    emit(Op::NewBox, b.reg);
            }
        }
    }
    for (const Node* s : bodies) {
        if (s->kind == NK::FunctionDecl) {
            Binding& b = declare(s->text, BKind::Function);
            if (b.boxed)
                emit(Op::NewBox, b.reg);
            const std::uint16_t m2 = fn_->top;
            const std::uint16_t t = function(*s->a, s->text, -1);
            storeVar(s->text, t, true, *s);
            fn_->top = m2;
        }
    }
    std::vector<Label> caseLabels(n.list.size());
    Label end;
    int defaultIndex = -1;
    for (std::size_t i = 0; i < n.list.size(); ++i) {
        const Node& c = *n.list[i];
        if (!c.a) {
            defaultIndex = int(i);
            continue;
        }
        const std::uint16_t m2 = fn_->top;
        if (!noFuse_) {
            // One compare-and-jump per case: no boolean made, and a case read from a variable is compared where it is.
            if (smallInt(*c.a)) {
                emitFusedJump(Op::JStrictEqI, disc, std::uint16_t(std::int16_t(int(c.a->number))), caseLabels[i]);
            } else {
                const std::uint16_t t = exprAny(*c.a);
                emitFusedJump(Op::JStrictEq, disc, t, caseLabels[i]);
            }
            fn_->top = m2;
            continue;
        }
        const std::uint16_t t = temp();
        expr(*c.a, t);
        emit(Op::StrictEq, t, disc, t);
        emitJump(Op::JTrue, t, caseLabels[i]);
        fn_->top = m2;
    }
    jump(defaultIndex >= 0 ? caseLabels[std::size_t(defaultIndex)] : end);
    JumpCtx ctx;
    ctx.labels = std::move(labels);
    ctx.isSwitch = true;
    ctx.breakTo = &end;
    ctx.tryDepth = fn_->tries.size();
    ctx.breakDepth = ctx.tryDepth;
    fn_->jumps.push_back(std::move(ctx));
    for (std::size_t i = 0; i < n.list.size(); ++i) {
        bind(caseLabels[i]);
        // Control reaches a case by a jump, not from the case above it: what an earlier case declared has not run.
        for (Binding& b : fn_->scopes.back().bindings)
            if (b.tdz)
                b.initialized = false;
        statements(n.list[i]->list);
    }
    fn_->jumps.pop_back();
    bind(end);
    popScope();
    fn_->top = mark;
}

void Compiler::closeTry(TryCtx& ctx, std::uint32_t end, std::uint32_t target, std::uint16_t reg, bool isCatch)
{
    // The protected range with the finally copies cut out of it.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> holes = ctx.holes;
    std::sort(holes.begin(), holes.end());
    std::uint32_t from = ctx.start;
    for (const auto& h : holes) {
        if (h.first > from)
            fn_->block->handlers.push_back(Handler{from, h.first, target, reg, isCatch});
        from = std::max(from, h.second);
    }
    if (end > from)
        fn_->block->handlers.push_back(Handler{from, end, target, reg, isCatch});
}

void Compiler::tryStatement(const Node& n)
{
    const std::uint16_t mark = fn_->top;
    const bool hasCatch = n.c != nullptr;
    const Node* finallyBody = n.d.get();
    const std::uint16_t catchReg = temp();
    const std::uint16_t finallyReg = temp();

    if (finallyBody) {
        TryCtx f;
        f.finallyBody = finallyBody;
        f.start = pc();
        fn_->tries.push_back(std::move(f));
    }
    const std::size_t finallyIndex = fn_->tries.size() - 1;

    Label afterCatch, finNormal, end;
    if (hasCatch) {
        TryCtx c;
        c.start = pc();
        fn_->tries.push_back(std::move(c));
        statement(*n.a);
        TryCtx done = std::move(fn_->tries.back());
        fn_->tries.pop_back();
        const std::uint32_t tryEnd = pc();
        jump(afterCatch);
        const std::uint32_t catchStart = pc();
        closeTry(done, tryEnd, catchStart, catchReg, true);
        // The catch clause: its parameter, then its body.
        pushScope();
        if (n.b) {
            if (n.b->kind == NK::Identifier) {
                Binding& b = declare(n.b->text, BKind::Let);
                b.initialized = true;
                if (b.boxed) {
                    emit(Op::Mov, b.reg, catchReg);
                    emit(Op::NewBox, b.reg);
                } else {
                    emit(Op::Mov, b.reg, catchReg);
                }
            } else {
                std::vector<String> names;
                collectPatternNames(*n.b, names);
                for (const String& name : names) {
                    Binding& b = declare(name, BKind::Let);
                    if (b.boxed)
                        emit(Op::NewBox, b.reg);
                }
                bindPattern(*n.b, catchReg, true, VarKind::Let);
            }
        }
        statement(*n.c);
        popScope();
        bind(afterCatch);
    } else {
        statement(*n.a);
    }

    if (finallyBody) {
        TryCtx f = std::move(fn_->tries.back());
        fn_->tries.pop_back();
        (void)finallyIndex;
        const std::uint32_t protectedEnd = pc();
        // Normal completion: the finally block runs, and control goes on.
        statement(*finallyBody);
        jump(end);
        // An exception out of the try or the catch: run the finally block, then throw it again.
        const std::uint32_t excStart = pc();
        closeTry(f, protectedEnd, excStart, finallyReg);
        statement(*finallyBody);
        emit(Op::Throw, finallyReg);
        bind(end);
    } else {
        (void)finNormal;
    }
    fn_->top = mark;
}

// ======================================================================= functions ===

CodeBlock* Compiler::compileFunction(const Node& n, const String& name)
{
    const bool saved = noFuse_;
    noFuse_ = false;  // a function inside one that needed it does not: its jumps are its own
    const std::uint32_t extra = pendingFlags_;
    pendingFlags_ = 0;
    for (;;) {
        try {
            CodeBlock* block = compileFunctionOnce(n, name, extra);
            noFuse_ = saved;
            return block;
        } catch (const FusionOverflow&) {
            if (noFuse_) {
                noFuse_ = saved;
                throw;
            }
            noFuse_ = true;
        } catch (...) {
            noFuse_ = saved;
            throw;
        }
    }
}

CodeBlock* Compiler::functionHeader(const Node& n, const String& name, std::uint32_t extraFlags)
{
    const bool arrow = n.kind == NK::ArrowExpr;
    Key nameKey = name.empty() ? nullptr : vm_.internKey(StringView(name));
    CodeBlock* block = vm_.newCodeBlock(nameKey, 0, !arrow && !n.isGenerator && !n.isAsync && (!n.isMethod || (extraFlags & CodeBlock::kClassConstructor)));
    if (n.isGenerator || n.isAsync)
        block->flags |= CodeBlock::kGenerator;
    if (n.isAsync)
        block->flags |= CodeBlock::kAsync;
    if (n.isAsync && n.isGenerator)
        block->flags |= CodeBlock::kAsyncGen;
    if (nameKey)
        detail::decRef(nameKey);  // `newCodeBlock` took its own count
    block->flags |= extraFlags;
    block->file = file_;
    block->source = source_;
    block->srcStart = n.srcStart;
    block->srcEnd = n.srcEnd;
    // Parameters before a rest parameter arrive in registers of their own; the rest are taken from the arguments array.
    std::size_t fixedParams = n.list.size();
    for (std::size_t i = 0; i < n.list.size(); ++i)
        if (n.list[i]->kind == NK::RestElement) {
            fixedParams = i;
            break;
        }
    block->nparams = std::uint16_t(fixedParams);
    for (const dyn::NodePtr& p : n.list) {
        if (p->kind == NK::AssignPattern || p->kind == NK::RestElement)
            break;
        ++block->length;
    }
    return block;
}

CodeBlock* Compiler::compileFunctionOnce(const Node& n, const String& name, std::uint32_t extraFlags)
{
    CodeBlock* block = functionHeader(n, name, extraFlags);
    try {
        functionCode(n, block, extraFlags);
    } catch (...) {
        vm_.releaseCode(block);
        throw;
    }
    return block;
}

/// The code of function `n` in `block`, whose header is made; the function around it is `fn_`.
void Compiler::functionCode(const Node& n, CodeBlock* block, std::uint32_t extraFlags, const LazyInfo* resuming)
{
    const bool arrow = n.kind == NK::ArrowExpr;
    Fn child;
    child.isMethodFn = n.isMethod || (extraFlags & CodeBlock::kClassConstructor) != 0;
    child.parent = fn_;
    child.block = block;
    child.isArrow = arrow;
    child.strict = fn_->strict || (n.d && hasUseStrict(*n.d)) || (extraFlags & CodeBlock::kClassConstructor) != 0 || classDepth_ > 0;
    if (child.strict)
        block->flags |= CodeBlock::kStrict;
    if (arrow)
        block->flags |= CodeBlock::kArrow;
    if (resuming) {
        // The closures of this function exist: their upvalues are in this order.
        for (std::size_t i = 0; i < resuming->upvalNames.size(); ++i)
            child.upvalIndex[String(resuming->upvalNames[i].data(), resuming->upvalNames[i].size())] = std::uint16_t(i);
    }
    collectCaptured(*n.d, child.captured, captures_);
    for (const dyn::NodePtr& p : n.list)
        collectCaptured(*p, child.captured, captures_);

    Fn* saved = fn_;
    const std::uint32_t savedLine = line_;
    Label* savedChain = chain_;
    chain_ = nullptr;
    std::vector<String> savedLabels;
    savedLabels.swap(pendingLabels_);
    fn_ = &child;
    try {
        functionBody(n, block);
    } catch (...) {
        fn_ = saved;
        line_ = savedLine;
        chain_ = savedChain;
        pendingLabels_.swap(savedLabels);
        throw;
    }
    fn_ = saved;
    line_ = savedLine;
    chain_ = savedChain;
    pendingLabels_.swap(savedLabels);
}

/// Whether `n`, with the names `names` it and what it contains mention, is left to compile when it is first called: a plain
/// function in a big program, whose names mean the same then as now. Nothing in a class, which brings private names and
/// `super`, and nothing near a direct `eval`. Compiling later also reports what the language forbids later, so a small
/// function (and every program of the conformance suites) is compiled at once.
bool Compiler::lazyEligible(const Node& n, const NameViews& names) const
{
    std::size_t kProgramBytes = 64 * 1024, kFunctionBytes = 128;
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    // SCRIPT_VM_LAZY=1: every function that qualifies, whatever the sizes, so the test suites run through the stubs.
    if (const char* t = std::getenv("SCRIPT_VM_LAZY"); t && *t == '1')
        kProgramBytes = kFunctionBytes = 0;
    else if (t && *t == '0')
        return false;  // SCRIPT_VM_LAZY=0: none
#endif
    if (!lazy_)
        return false;
    const bool arrow = n.kind == NK::ArrowExpr;
    // An arrow with a body in braces is parsed again from its text; one with an expression for a body can end where the
    // context decides (`in` in a `for` head), and one that names `super` needs the method around it.
    if ((n.kind != NK::FunctionExpr && !arrow) || (arrow && (n.arrowExprBody || usesSuper(*n.d))) || n.isMethod ||
        n.propKind == PropKind::Constructor || n.propKind == PropKind::Getter || n.propKind == PropKind::Setter || !n.d || classDepth_ != 0 || pendingFlags_ != 0)
        return false;
    if (!source_ || source_->size() < kProgramBytes || n.srcEnd > source_->size() || n.srcEnd <= n.srcStart || n.srcEnd < n.srcStart + kFunctionBytes)
        return false;
    if (calledAtOnce_.count(&n))
        return false;
    for (const Fn* f = fn_; f; f = f->parent)
        if (f->isEval || f->evalOuter || f->dynamicVars || !f->evalEnvName.empty())
            return false;
    for (std::u16string_view name : names)
        if (!name.empty() && name[0] == u'%' && name != u"%newtarget")
            return false;  // `%all` (a direct `eval` inside), `%callee` and the hidden names of classes and derived constructors
    return true;
}

/// A function left to compile: its header and what makes a closure of it. It captures every variable around it that its
/// text names, and the body may then use fewer. Null when it does not qualify.
CodeBlock* Compiler::lazyStub(const Node& n, const String& name)
{
    if (!lazy_)
        return nullptr;
    // An arrow's `this`, `arguments` and `new.target` are those of the function around it.
    const NameViews& names = capturedBelow(n, n.kind == NK::ArrowExpr, captures_);
    if (!lazyEligible(n, names))
        return nullptr;
    std::vector<UpvalDesc> descs;
    auto info = std::make_unique<LazyInfo>();
    for (std::u16string_view view : names) {
        const String key(view.data(), view.size());
        const Ref r = resolve(key);
        if (r.kind == Ref::Global)
            continue;
        UpvalDesc d;
        if (r.kind == Ref::Local) {
            if (!r.binding->boxed)
                return nullptr;  // not a variable a closure can capture: compile it at once, which says what is wrong
            d.index = r.binding->reg;
            d.fromParentLocal = true;
        } else {
            d.index = r.upval;
            d.fromParentLocal = false;
        }
        descs.push_back(d);
        info->upvalNames.emplace_back(view);
        info->upvalBits.push_back(std::uint8_t((r.isConst ? 1 : 0) | (r.isFnName ? 2 : 0) | (r.checked ? 4 : 0)));
    }
    info->line = n.line;
    info->isDeclaration = !n.namedExpr && !n.text.empty();
    info->isArrow = n.kind == NK::ArrowExpr;
    info->strictOuter = fn_->strict;
    CodeBlock* block = functionHeader(n, name, 0);
    for (const UpvalDesc& d : descs)
        block->upvals.push_back(d);
    block->flags |= CodeBlock::kLazy;
    block->lazy = info.release();
    return block;
}

void Compiler::resume(const Node& n, CodeBlock* stub)
{
    const LazyInfo& info = *stub->lazy;
    // The function around it, as the stub saw it: one box for each name it captured.
    Fn outer;
    outer.isProgram = true;
    outer.strict = info.strictOuter;
    outer.scopes.emplace_back();
    for (std::size_t i = 0; i < info.upvalNames.size(); ++i) {
        Binding b;
        b.name = String(info.upvalNames[i].data(), info.upvalNames[i].size());
        b.reg = std::uint16_t(i);
        b.boxed = true;
        const std::uint8_t bits = info.upvalBits[i];
        b.kind = (bits & 1) ? BKind::Const : (bits & 2) ? BKind::FnName : BKind::Var;
        b.tdz = (bits & 4) != 0;
        b.initialized = false;
        outer.scopes.back().bindings.push_back(b);
    }
    fn_ = &outer;
    const std::size_t upvals = stub->upvals.size();
    for (;;) {
        // Into a block of its own: a function with a far jump is compiled again without fused jumps, from nothing.
        CodeBlock* made = functionHeader(n, String(), 0);
        for (const UpvalDesc& d : stub->upvals)
            made->upvals.push_back(d);
        const bool savedFuse = noFuse_;
        try {
            functionCode(n, made, 0, &info);
        } catch (const FusionOverflow&) {
            vm_.releaseCode(made);
            if (noFuse_) {
                noFuse_ = savedFuse;
                fn_ = nullptr;
                throw;
            }
            noFuse_ = true;
            continue;
        } catch (...) {
            vm_.releaseCode(made);
            fn_ = nullptr;
            throw;
        }
        fn_ = nullptr;
        if (made->upvals.size() != upvals) {
            vm_.releaseCode(made);
            throw Error(ErrorCode::NotSupported, "vm core: internal error: a function that was left to compile captured more than it said");
        }
        stub->code.swap(made->code);
        stub->consts.swap(made->consts);
        stub->atoms.swap(made->atoms);
        stub->children.swap(made->children);
        stub->evalScopes.swap(made->evalScopes);
        stub->evalHome.swap(made->evalHome);
        stub->handlers.swap(made->handlers);
        stub->lines.swap(made->lines);
        stub->caches.swap(made->caches);
        stub->nregs = made->nregs;
        stub->flags |= made->flags;
        stub->flags &= ~std::uint32_t(CodeBlock::kLazy);
        vm_.releaseCode(made);
        delete stub->lazy;
        stub->lazy = nullptr;
        return;
    }
}

void Compiler::functionBody(const Node& n, CodeBlock* block)
{
    const bool arrow = fn_->isArrow;
    const std::size_t nparams = n.list.size();
    const std::size_t fixed = block->nparams;
    const bool hasRest = fixed < nparams;
    const bool needsArgs = hasRest || (!arrow && (usesArguments(*n.d) || fn_->captured.count(String(u"%all"))));
    // `this`, the fixed parameters, then (if any) the arguments array.
    fn_->top = std::uint16_t(1 + fixed + (needsArgs ? 1 : 0));
    block->nregs = fn_->top;
    // A rest parameter and no `arguments`: the call makes the rest array itself, in the register after the parameters.
    const bool restOnly = hasRest && !(!arrow && (usesArguments(*n.d) || fn_->captured.count(String(u"%all"))));
    if (needsArgs) {
        block->flags |= CodeBlock::kNeedsArgs;
        if (restOnly)
            block->flags |= CodeBlock::kRestOnly;
        fn_->scopes.reserve(0);
    }

    // A named function expression sees its own name; the scope is outside the function's own so a parameter or variable
    // of the same name wins.
    if (n.kind == NK::FunctionExpr && n.namedExpr && !n.text.empty()) {
        pushScope();
        Binding& b = declare(n.text, BKind::FnName);
        emit(Op::LoadCallee, b.reg);
        if (b.boxed)
            emit(Op::NewBox, b.reg);
    }
    pushScope();
    if (needsArgs && !arrow) {
        // `arguments` is the array the call made, in the register after the parameters.
        Scope& scope = fn_->scopes.back();
        Binding b;
        b.name = String(u"arguments");
        b.kind = BKind::Var;
        b.reg = std::uint16_t(1 + fixed);
        b.boxed = isCaptured(b.name);
        scope.bindings.push_back(b);
        if (b.boxed)
            emit(Op::NewBox, b.reg);
    }
    // `this` for the arrows inside.
    if (!arrow && isCaptured(String(u"this"))) {
        Binding& b = declare(String(u"this"), BKind::Var);
        emit(Op::Mov, b.reg, 0);
        if (b.boxed)
            emit(Op::NewBox, b.reg);
    }
    if (!arrow && fn_->captured.count(String(u"%newtarget"))) {
        Binding& b = declare(String(u"%newtarget"), BKind::Var);
        emit(Op::NewTarget, b.reg);
        if (b.boxed)
            emit(Op::NewBox, b.reg);
    }
    if (!arrow && fn_->captured.count(String(u"%callee"))) {
        Binding& b = declare(String(u"%callee"), BKind::Var);
        emit(Op::LoadCallee, b.reg);
        if (b.boxed)
            emit(Op::NewBox, b.reg);
    }
    if (!fn_->strict) {
        bool hasEval = containsDirectEval(*n.d);
        for (const dyn::NodePtr& p : n.list)
            hasEval = hasEval || containsDirectEval(*p);
        if (hasEval) {
            // The variables a direct `eval` in here declares go to this object, which the code inside reaches by name.
            fn_->evalEnvName = u"%evalenv#" + fromUtf8(std::to_string(++vm_.evalEnvSerial));
            fn_->captured.insert(fn_->evalEnvName);
            Binding& b = declare(fn_->evalEnvName, BKind::Var);
            emit(Op::NewEnv, b.reg);
            if (b.boxed)
                emit(Op::NewBox, b.reg);
        }
    }
    if (block->flags & CodeBlock::kDerived) {
        // Whether `super()` has returned.
        Binding& b = declare(String(u"%bound"), BKind::Var);
        emit(Op::LoadUndef, b.reg);
        if (b.boxed)
            emit(Op::NewBox, b.reg);
    }
    if ((block->flags & CodeBlock::kClassConstructor) && !(block->flags & CodeBlock::kDerived))
        fieldInit(0);
    // Parameters: the first registers after `this`.
    std::vector<Binding*> simple;
    for (std::size_t i = 0; i < fixed; ++i) {
        const Node& p = *n.list[i];
        const Node* target = p.kind == NK::AssignPattern ? p.a.get() : &p;
        if (target->kind == NK::Identifier) {
            Scope& scope = fn_->scopes.back();
            Binding b;
            b.name = target->text;
            b.kind = BKind::Param;
            b.reg = std::uint16_t(i + 1);
            b.boxed = isCaptured(b.name);
            bool duplicate = false;
            for (Binding& e : scope.bindings)
                if (e.name == b.name) {
                    // `function f(a, a)`: the last one wins.
                    e.reg = b.reg;
                    e.boxed = b.boxed;
                    duplicate = true;
                }
            if (!duplicate)
                scope.bindings.push_back(b);
        }
    }
    bool nonSimple = false;
    for (std::size_t i = 0; i < fixed; ++i)
        nonSimple = nonSimple || n.list[i]->kind != NK::Identifier;
    paramTdz_ = ParamTdz();
    if (nonSimple) {
        paramTdz_.fn = fn_;
        for (std::size_t i = 0; i < fixed; ++i) {
            std::vector<String> names;
            collectPatternNames(n.list[i]->kind == NK::AssignPattern ? *n.list[i]->a : *n.list[i], names);
            for (const String& name : names)
                paramTdz_.index.emplace(name, i);
        }
    }
    // With a default or a pattern among the parameters, a closure or an `eval` in one of them can reach a parameter that
    // comes after it, so every captured parameter has its box from the start.
    std::vector<bool> earlyBox(fixed, false);
    if (nonSimple) {
        for (std::size_t i = 0; i < fixed; ++i) {
            const Node& p = *n.list[i];
            const Node* target = p.kind == NK::AssignPattern ? p.a.get() : &p;
            if (target->kind != NK::Identifier)
                continue;
            Binding* b = findInScopes(*fn_, target->text);
            if (b && b->reg == std::uint16_t(i + 1) && b->boxed) {
                emit(Op::NewBox, b->reg);
                earlyBox[i] = true;
            }
        }
    }
    for (std::size_t i = 0; i < fixed; ++i) {
        const Node& p = *n.list[i];
        const std::uint16_t reg = std::uint16_t(i + 1);
        const Node* target = &p;
        if (p.kind == NK::AssignPattern) {
            Label has;
            paramTdz_.current = i;
            if (earlyBox[i]) {
                const std::uint16_t mark = fn_->top;
                const std::uint16_t t = temp();
                emit(Op::GetBox, t, reg);
                emitJump(Op::JNotUndefined, t, has);
                exprNamed(*p.b, t, p.a->kind == NK::Identifier ? p.a->text : String());
                emit(Op::PutBox, reg, t);
                fn_->top = mark;
            } else {
                emitJump(Op::JNotUndefined, reg, has);
                exprNamed(*p.b, reg, p.a->kind == NK::Identifier ? p.a->text : String());
            }
            bind(has);
            target = p.a.get();
        }
        paramTdz_.current = i + 1;
        if (target->kind == NK::Identifier) {
            Binding* b = findInScopes(*fn_, target->text);
            if (b && b->reg == reg && b->boxed && !earlyBox[i])
                emit(Op::NewBox, reg);
        } else {
            std::vector<String> names;
            collectPatternNames(*target, names);
            for (const String& name : names) {
                Binding& b = declare(name, BKind::Let);
                b.initialized = true;
                if (b.boxed)
                    emit(Op::NewBox, b.reg);
            }
            bindPattern(*target, reg, true, VarKind::Let);
        }
    }

    paramTdz_ = ParamTdz();
    if (hasRest) {
        const Node& rest = *n.list[fixed];
        const Node& target = *rest.a;
        std::vector<String> names;
        collectPatternNames(target, names);
        for (const String& name : names) {
            Binding& b = declare(name, BKind::Let);
            b.initialized = true;
            if (b.boxed)
                emit(Op::NewBox, b.reg);
        }
        const std::uint16_t mark = fn_->top;
        const std::uint16_t v = temp();
        if (restOnly)
            emit(Op::Mov, v, std::uint16_t(1 + fixed));
        else
            emit(Op::RestSlice, v, std::uint16_t(1 + fixed), std::uint32_t(fixed));
        bindTarget(target, v, true, VarKind::Let);
        fn_->top = mark;
    }

    {
        std::vector<String> paramNames;
        checkParams(n, paramNames);
        checkDeclarations(n.d->list, true, &paramNames);
    }
    // `var` names and function declarations of the whole body, then the block's own `let`s.
    std::vector<String> vars;
    collectVars(*n.d, vars);
    fn_->varScope = fn_->scopes.size() - 1;
    if (!fn_->strict) {
        std::set<String> blockFunctions;
        annexBNames(n.d->list, blockFunctions);
        vars.insert(vars.end(), blockFunctions.begin(), blockFunctions.end());
    }
    for (const String& v : vars) {
        const bool known = findInScopes(*fn_, v) && fn_->scopes.back().bindings.end() !=
                                                       std::find_if(fn_->scopes.back().bindings.begin(), fn_->scopes.back().bindings.end(),
                                                                    [&](const Binding& b) { return b.name == v; });
        if (known)
            continue;
        Binding& b = declare(v, BKind::Var);
        if (b.boxed)
            emit(Op::NewBox, b.reg);
    }
    declareLexical(n.d->list);
    hoistFunctions(n.d->list);
    // The parameters and the declarations are done when a generator is called, not when it is first resumed.
    if ((block->flags & CodeBlock::kGenerator) && (!(block->flags & CodeBlock::kAsync) || (block->flags & CodeBlock::kAsyncGen)))
        emit(Op::GenStart);
    statements(n.d->list);
    const std::uint16_t r = temp();
    emit(Op::LoadUndef, r);
    emitReturn(r);
    if (n.kind == NK::FunctionExpr && n.namedExpr && !n.text.empty())
        fn_->scopes.pop_back();
    fn_->scopes.pop_back();
}

/// Tell a direct `eval` call about the variables around it: what `SetEvalScope` names is a table in the code block.
void Compiler::emitEvalScope()
{
    std::vector<String> names;
    for (Fn* f = fn_; f; f = f->parent)
        for (const Scope& scope : f->scopes)
            for (const Binding& b : scope.bindings)
                if (std::find(names.begin(), names.end(), b.name) == names.end())
                    names.push_back(b.name);
    Vec<EvalVar> vars;
    for (const String& name : names) {
        const Ref r = resolve(name);
        if (r.kind == Ref::Global || (r.kind == Ref::Local && !r.binding->boxed))
            continue;
        EvalVar v;
        v.atom = atom(name);
        v.local = r.kind == Ref::Local;
        v.index = v.local ? r.binding->reg : r.upval;
        vars.push_back(v);
    }
    fn_->block->evalScopes.push_back(std::move(vars));
    {
        const Fn* holder = fn_;
        while (holder && holder->isArrow)
            holder = holder->parent;
        // In eval code, whatever its caller allowed.
        fn_->block->evalHome.push_back(holder && (holder->isMethodFn || (holder->evalOuter && evalHome_)) ? 1 : 0);
    }
    emit(Op::SetEvalScope, std::uint16_t(fn_->block->evalScopes.size() - 1));
}

CodeBlock* Compiler::evalOnce(const Node& program, const std::vector<String>& names, bool strict)
{
    CodeBlock* block = vm_.newCodeBlock(nullptr, 0, false);
    block->file = file_;
    block->source = source_;
    block->flags |= CodeBlock::kArrow;
    // The caller's variables, each a box the eval code reaches as an upvalue.
    Fn outer;
    outer.isProgram = true;
    outer.evalOuter = true;
    outer.scopes.emplace_back();
    for (std::size_t i = 0; i < names.size(); ++i) {
        Binding b;
        b.name = names[i];
        b.reg = std::uint16_t(i);
        b.boxed = true;
        outer.scopes.back().bindings.push_back(b);
    }
    Fn top;
    top.parent = &outer;
    top.block = block;
    top.isArrow = true;
    top.isEval = true;
    top.completion = true;
    top.strict = strict || hasUseStrict(program);
    if (top.strict)
        block->flags |= CodeBlock::kStrict;
    collectCaptured(program, top.captured, captures_);
    fn_ = &top;
    try {
        top.top = 1;
        block->nregs = 1;
        pushScope();
        top.resultReg = temp();
        emit(Op::LoadUndef, top.resultReg);
        std::vector<String> vars;
        collectVars(program, vars);
        top.varScope = fn_->scopes.size() - 1;
        if (!top.strict) {
            std::set<String> blockFunctions;
            annexBNames(program.list, blockFunctions);
            vars.insert(vars.end(), blockFunctions.begin(), blockFunctions.end());
        }
        // Sloppy eval code declares its `var`s and functions where its caller does: on the eval object of the function that
        // called it, or on the global object when it was called from global code.
        top.dynamicVars = !top.strict;
        const std::vector<Ref> callerEnvs = top.dynamicVars ? evalEnvRefs() : std::vector<Ref>();
        for (const String& v : vars) {
            // `var x` of a name the caller has is that variable.
            if (std::find(names.begin(), names.end(), v) != names.end() ||
                (findInScopes(*fn_, v) && fn_->scopes.back().bindings.end() != std::find_if(fn_->scopes.back().bindings.begin(), fn_->scopes.back().bindings.end(), [&](const Binding& b) { return b.name == v; })))
                continue;
            if (top.dynamicVars) {
                if (!callerEnvs.empty()) {
                    const std::uint16_t mark = fn_->top;
                    const std::uint16_t env = loadEnv(callerEnvs.front());
                    emit(Op::DeclEnv, env, atom(v));
                    fn_->top = mark;
                } else {
                    emit(Op::DeclGlobal, atom(v), 1);
                }
                continue;
            }
            Binding& b = declare(v, BKind::Var);
            if (b.boxed)
                emit(Op::NewBox, b.reg);
        }
        declareLexical(program.list);
        hoistFunctions(program.list);
        statements(program.list);
        emit(Op::Ret, top.resultReg);
        popScope();
    } catch (...) {
        fn_ = nullptr;
        vm_.releaseCode(block);
        throw;
    }
    fn_ = nullptr;
    return block;
}

CodeBlock* Compiler::program(const Node& program)
{
    noFuse_ = false;
    for (;;) {
        try {
            return programOnce(program);
        } catch (const FusionOverflow&) {
            if (noFuse_)
                throw;
            noFuse_ = true;
        }
    }
}

CodeBlock* Compiler::programOnce(const Node& program)
{
    CodeBlock* block = vm_.newCodeBlock(nullptr, 0, false);
    block->file = file_;
    block->flags |= CodeBlock::kProgram;
    Fn top;
    top.block = block;
    top.isProgram = true;
    top.strict = hasUseStrict(program);
    if (top.strict)
        block->flags |= CodeBlock::kStrict;
    collectCaptured(program, top.captured, captures_);
    fn_ = &top;
    try {
        top.top = 1;
        block->nregs = 1;
        pushScope();
        top.resultReg = temp();
        emit(Op::LoadUndef, top.resultReg);
        // `this` for arrows at the top of a program.
        if (isCaptured(String(u"this"))) {
            Binding& b = declare(String(u"this"), BKind::Var);
            emit(Op::Mov, b.reg, 0);
            if (b.boxed)
                emit(Op::NewBox, b.reg);
        }
        checkDeclarations(program.list, true);
        // Declarations of the top level are properties of the global object.
        std::vector<String> vars;
        collectVars(program, vars);
        top.varScope = fn_->scopes.size() - 1;
        if (!top.strict) {
            std::set<String> blockFunctions;
            annexBNames(program.list, blockFunctions);
            vars.insert(vars.end(), blockFunctions.begin(), blockFunctions.end());
        }
        for (const String& v : vars)
            emit(Op::DeclGlobal, atom(v));
        for (const dyn::NodePtr& s : program.list) {
            if (s->kind == NK::FunctionDecl)
                emit(Op::DeclGlobal, atom(s->text));
        }
        hoistFunctions(program.list);
        statements(program.list);
        emit(Op::Ret, top.resultReg);
        popScope();
    } catch (...) {
        fn_ = nullptr;
        vm_.releaseCode(block);
        throw;
    }
    fn_ = nullptr;
    return block;
}

}  // namespace

CodeBlock* compileEval(VM& vm, const dyn::Node& program, std::string_view file, std::shared_ptr<const std::string> source,
                       const std::vector<String>& names, bool strict, bool home)
{
    Compiler compiler(vm, file, std::move(source));
    return compiler.evalProgram(program, names, strict, home);
}

CodeBlock* compileProgram(VM& vm, const dyn::Node& program, std::string_view file, std::shared_ptr<const std::string> source, bool lazy)
{
    Compiler compiler(vm, file, std::move(source), lazy);
    return compiler.program(program);
}

void compileLazy(VM& vm, CodeBlock* stub)
{
    dyn::Parser parser(*stub->source, toUtf8(stub->file), /*typescript=*/false);
    const dyn::NodePtr node = stub->lazy->isArrow ? parser.parseArrowAt(stub->srcStart, stub->lazy->line)
                                                   : parser.parseFunctionAt(stub->srcStart, stub->lazy->line, stub->lazy->isDeclaration);
    Compiler compiler(vm, toUtf8(stub->file), stub->source, /*lazy=*/true);
    compiler.resume(*node, stub);
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (std::getenv("SCRIPT_VM_DUMP")) {
        std::fprintf(stderr, "lazy compiled (upvals %zu):\n", stub->upvals.size());
        std::fputs(dump(*stub).c_str(), stderr);
    }
#endif
}

}  // namespace vm
}  // namespace internal
}  // namespace script
