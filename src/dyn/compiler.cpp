// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Syntax tree to bytecode.
//
// One pass, with a cheap pre-pass per function that finds the names any nested
// function mentions. Those locals are boxed into `Box`s at their declaration,
// so a closure can capture them without the interpreter having to track and
// close open upvalues later.

#include <algorithm>
#include <set>
#include <unordered_map>
#include <string>
#include <map>
#include <vector>

#include "script/internal/dyn/bigint.hpp"
#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/compiler.hpp"
#include "script/internal/dyn/runtime.hpp"
#include "script/internal/dyn/numconv.hpp"
#include "script/internal/dyn/object.hpp"
#include "script/internal/stack_guard.hpp"

namespace script {
namespace internal {
namespace dyn {

namespace {

constexpr std::uint32_t kNoSlot = FunctionCode::kNoSlot;

struct Local
{
    String name;
    std::uint32_t slot = 0;
    int depth = 0;
    bool boxed = false;
    bool isConst = false;
};

struct LoopCtx
{
    std::vector<std::size_t> breaks;
    std::vector<std::size_t> continues;
    String label;
    bool continuable = true;
    std::size_t finallyDepth = 0;
};

/// A `finally` block that pending exits have to run on their way out.
struct FinallyCtx
{
    std::vector<std::size_t> sites;  ///< JumpSub operands waiting for `pc`
    /// Not a `finally` but a `for-of`, whose iterator at this place in the stack is closed by whatever jumps out of it.
    std::uint32_t iterDepth = kNoSlot;
    bool asyncIter = false;
};

struct FnState
{
    FnState* parent = nullptr;
    std::shared_ptr<FunctionCode> code;
    std::vector<Local> locals;
    std::vector<String> upvalNames;
    std::vector<bool> upvalConst;  ///< parallel to upvalNames: the binding is a `const`
    std::set<String> captured;      ///< names a nested function mentions
    std::set<const Node*> hoistedFuncs;  ///< function declarations `hoist` compiled
    std::vector<LoopCtx> loops;
    std::vector<FinallyCtx*> finallys;
    int depth = 0;
    std::uint32_t nextSlot = 0;
    std::uint32_t stmtDepth = 0;  ///< loop/switch temporaries live on the stack
    std::uint32_t retvalSlot = kNoSlot;
    std::uint32_t completionSlot = kNoSlot;
    std::uint32_t tempSlot = kNoSlot;
    bool isArrow = false;
    bool isGlobal = false;
    bool isDerivedCtor = false;
    bool isMethod = false;
    /// Something in here calls `eval` directly: every name is kept in a cell, where the code it compiles can find it.
    bool boxAll = false;
    bool isEval = false;
    std::unordered_map<std::uint64_t, std::uint32_t> numberConsts;
    std::unordered_map<String, std::uint32_t> stringConsts;
    std::unordered_map<const GCObject*, std::uint32_t> atomSlots;
};

enum class RefKind
{
    Local,
    Box,
    Upval,
    Global,
};

struct VarRef
{
    RefKind kind = RefKind::Global;
    std::uint32_t index = 0;
    bool isConst = false;
};

// ---- the pre-pass -------------------------------------------------------

/// Visit the direct children of `n`, `a`..`d` first and then `list`.
template <class F>
void forEachChild(const Node& n, F&& visit)
{
    for (const NodePtr* p : {&n.a, &n.b, &n.c, &n.d}) {
        if (*p)
            visit(**p);
    }
    for (const NodePtr& kid : n.list) {
        if (kid)
            visit(*kid);
    }
}

/// Every name mentioned inside `n`. `inArrow` tracks whether `this` still
/// refers to the function we are compiling.
void collectNames(const Node& n, std::set<String>& out, bool inArrow)
{
    checkNativeStack();
    switch (n.kind) {
        case NK::Identifier:
            out.insert(n.text);
            break;
        case NK::ThisExpr:
            if (inArrow)
                out.insert(ascii("this"));
            break;
        case NK::SuperExpr:
            if (inArrow) {
                out.insert(ascii("this"));
                out.insert(ascii(" ctor"));
                out.insert(ascii(" nt"));
            }
            break;
        case NK::NewTarget:
            if (inArrow)
                out.insert(ascii(" nt"));
            break;
        case NK::FunctionExpr:
            inArrow = false;
            break;
        default:
            break;
    }
    // A member access spells out a property name, not a variable name.
    if (n.kind == NK::Member) {
        if (n.a)
            collectNames(*n.a, out, inArrow);
        if (n.computed && n.b)
            collectNames(*n.b, out, inArrow);
        return;
    }
    if (n.kind == NK::Property && !n.computed) {
        if (n.b)
            collectNames(*n.b, out, inArrow);
        return;
    }
    forEachChild(n, [&](const Node& kid) { collectNames(kid, out, inArrow); });
}

/// Names captured by functions nested inside `n`, not entering `n` itself.
void collectCaptured(const Node& n, std::set<String>& out)
{
    checkNativeStack();
    if (n.kind == NK::FunctionExpr || n.kind == NK::ArrowExpr || n.kind == NK::ClassExpr) {
        collectNames(n, out, n.kind == NK::ArrowExpr);
        return;
    }
    forEachChild(n, [&](const Node& kid) { collectCaptured(kid, out); });
}

/// A call of `eval` by that name, here or in any function nested here.
bool containsDirectEval(const Node& n)
{
    checkNativeStack();
    if (n.kind == NK::Call && n.a && n.a->kind == NK::Identifier && n.a->text == u"eval")
        return true;
    bool found = false;
    forEachChild(n, [&](const Node& kid) { found = found || containsDirectEval(kid); });
    return found;
}

/// `var` and function-declaration names, which hoist to the top of the
/// enclosing function. Nested functions keep their own.
void collectHoisted(const Node& n, std::vector<String>& vars, std::vector<const Node*>& funcs,
                    bool topLevel)
{
    switch (n.kind) {
        case NK::FunctionExpr:
        case NK::ArrowExpr:
        case NK::ClassExpr:
            return;
        case NK::FunctionDecl:
            if (topLevel)
                funcs.push_back(&n);
            else
                vars.push_back(n.text);
            return;
        case NK::VarDecl:
            if (n.varKind == VarKind::Var) {
                for (const NodePtr& d : n.list) {
                    std::set<String> names;
                    if (d->a)
                        collectNames(*d->a, names, false);
                    vars.insert(vars.end(), names.begin(), names.end());
                }
            }
            return;
        default:
            break;
    }
    forEachChild(n, [&](const Node& kid) { collectHoisted(kid, vars, funcs, false); });
}

bool mentionsArguments(const Node& n)
{
    checkNativeStack();
    if (n.kind == NK::FunctionExpr)
        return false;
    if (n.kind == NK::Identifier && n.text == ascii("arguments"))
        return true;
    bool found = false;
    forEachChild(n, [&](const Node& kid) { found = found || mentionsArguments(kid); });
    return found;
}

bool chainHasOptional(const Node& n)
{
    checkNativeStack();
    if (n.optional)
        return true;
    if ((n.kind == NK::Member || n.kind == NK::Call) && n.a)
        return chainHasOptional(*n.a);
    return false;
}

bool hasSpread(const std::vector<NodePtr>& args)
{
    return std::any_of(args.begin(), args.end(),
                       [](const NodePtr& a) { return a->kind == NK::Spread; });
}

/// The `Keep` jump that short-circuits `&&`, `||` or `??` (and their
/// assignment forms) once the left operand decides the result.
Op shortCircuitJump(Tok op) noexcept
{
    switch (op) {
        case Tok::OrOr:
        case Tok::OrAssign:
            return Op::JumpIfTrueKeep;
        case Tok::QuestionQuestion:
        case Tok::NullishAssign:
            return Op::JumpIfNotNullishKeep;
        default:
            return Op::JumpIfFalseKeep;
    }
}

// ---- the compiler -------------------------------------------------------

class Compiler
{
public:
    void setSloppyThis(bool sloppy) noexcept { sloppyThis_ = sloppy; }

    /// Marks a region of source as strict for as long as the object lives.
    struct StrictScope
    {
        Compiler& compiler;
        bool active;
        StrictScope(Compiler& c, bool strict) : compiler(c), active(strict) { compiler.strictDepth_ += active ? 1 : 0; }
        ~StrictScope() { compiler.strictDepth_ -= active ? 1 : 0; }
        StrictScope(const StrictScope&) = delete;
        StrictScope& operator=(const StrictScope&) = delete;
    };

    /// Whether a function body or a program opens with a "use strict" directive.
    static bool hasUseStrict(const Node& body)
    {
        for (const NodePtr& s : body.list) {
            if (s->kind != NK::ExprStatement || !s->a || s->a->kind != NK::StringLit)
                return false;
            if (s->a->text == ascii("use strict"))
                return true;
        }
        return false;
    }


    Compiler(Runtime& rt, std::string sourceName)
        : rt_(rt), sourceName_(std::make_shared<std::string>(std::move(sourceName)))
    {}

    std::shared_ptr<FunctionCode> program(const Node& node)
    {
        FnState fs;
        fs.code = std::make_shared<FunctionCode>();
        fs.code->setName("<program>");
        fs.code->sourceName = sourceName_;
        fs.isGlobal = true;
        fn_ = &fs;
        const StrictScope strictProgram(*this, hasUseStrict(node));
        fs.code->sloppyThis = sloppyThis_ && strictDepth_ == 0;

        checkDeclarations(node.list, true);
        collectCaptured(node, fs.captured);
        fs.boxAll = containsDirectEval(node);
        if (fs.boxAll)
            fs.code->jitRefused = true;
        fs.completionSlot = hiddenSlot();
        emit(Op::PushUndefined);
        emitU(Op::SetLocal, fs.completionSlot);

        hoist(node, true);
        for (const NodePtr& s : node.list)
            stmt(*s);

        emitU(Op::GetLocal, fs.completionSlot);
        emit(Op::Return);
        return fs.code;
    }

    std::shared_ptr<FunctionCode> evalCode(const Node& node, const EvalScope& scope)
    {
        // The caller, as far as this code can tell: its cells as boxed locals, its upvalues as upvalues of its own.
        FnState outermost;
        FnState caller;
        caller.parent = &outermost;
        caller.code = std::make_shared<FunctionCode>();
        for (const EvalScope::Name& name : scope.names) {
            if (name.local) {
                Local local;
                local.name = name.name;
                local.slot = name.index;
                local.boxed = true;
                local.isConst = name.isConst;
                caller.locals.push_back(local);
            } else {
                if (caller.upvalNames.size() <= name.index) {
                    caller.upvalNames.resize(name.index + 1, ascii(" none"));
                    caller.upvalConst.resize(name.index + 1, false);
                }
                caller.upvalNames[name.index] = name.name;
                caller.upvalConst[name.index] = name.isConst;
            }
        }
        FnState fs;
        fs.parent = &caller;
        fs.code = std::make_shared<FunctionCode>();
        fs.code->setName("<eval>");
        fs.code->sourceName = sourceName_;
        fs.code->isArrow = true;
        fs.code->jitRefused = true;
        fs.isArrow = true;
        fs.isEval = true;
        fs.isGlobal = scope.global;
        fn_ = &fs;
        const StrictScope inherited(*this, scope.strict);
        const StrictScope strictProgram(*this, hasUseStrict(node));
        fs.code->sloppyThis = sloppyThis_ && strictDepth_ == 0;
        checkDeclarations(node.list, true);
        collectCaptured(node, fs.captured);
        fs.boxAll = containsDirectEval(node);
        fs.completionSlot = hiddenSlot();
        emit(Op::PushUndefined);
        emitU(Op::SetLocal, fs.completionSlot);
        hoist(node, true);
        for (const NodePtr& s : node.list)
            stmt(*s);
        emitU(Op::GetLocal, fs.completionSlot);
        emit(Op::Return);
        return fs.code;
    }

private:
    /// What a direct `eval` here can see.
    std::uint32_t evalScopeHere(std::uint32_t argc)
    {
        EvalScope scope;
        scope.argc = argc;
        scope.global = fn_->isGlobal;
        scope.strict = inStrictCode();
        std::set<String> seen;
        for (std::size_t i = fn_->locals.size(); i-- > 0;) {
            const Local& local = fn_->locals[i];
            if (!local.boxed || !seen.insert(local.name).second)
                continue;
            scope.names.push_back({local.name, true, local.isConst, local.slot});
        }
        for (FnState* outer = fn_->parent; outer; outer = outer->parent) {
            for (std::size_t i = outer->locals.size(); i-- > 0;) {
                const String name = outer->locals[i].name;
                if (!outer->locals[i].boxed || seen.count(name))
                    continue;
                const int up = resolveUpval(*fn_, name);
                if (up < 0)
                    continue;
                seen.insert(name);
                scope.names.push_back({name, false, fn_->upvalConst[std::size_t(up)], std::uint32_t(up)});
            }
        }
        fn_->code->evalScopes.push_back(std::move(scope));
        return std::uint32_t(fn_->code->evalScopes.size() - 1);
    }

    // ---- emission ----
    std::vector<std::uint8_t>& bytes() { return fn_->code->code; }
    std::uint32_t here() const { return std::uint32_t(fn_->code->code.size()); }

    void note(std::uint32_t line)
    {
        auto& lines = fn_->code->lines;
        if (line && (lines.empty() || lines.back().second != line))
            lines.emplace_back(here(), line);
    }
    void emit(Op op) { bytes().push_back(std::uint8_t(op)); }
    void operand(std::uint32_t v)
    {
        bytes().push_back(std::uint8_t(v));
        bytes().push_back(std::uint8_t(v >> 8));
        bytes().push_back(std::uint8_t(v >> 16));
        bytes().push_back(std::uint8_t(v >> 24));
    }
    /// An element access, with its byte of what it has met.
    void emitElem(Op op)
    {
        Vec<std::uint8_t>& saw = fn_->code->elemSaw;
        emitU(op, std::uint32_t(saw.size()));
        saw.push_back(0);
    }
    void emitU(Op op, std::uint32_t a)
    {
        emit(op);
        operand(a);
    }
    void emitU(Op op, std::uint32_t a, std::uint32_t b)
    {
        emit(op);
        operand(a);
        operand(b);
    }
    std::size_t emitJump(Op op)
    {
        emit(op);
        const std::size_t site = bytes().size();
        operand(0);
        return site;
    }
    void patchTo(std::size_t site, std::uint32_t target)
    {
        auto& c = bytes();
        c[site] = std::uint8_t(target);
        c[site + 1] = std::uint8_t(target >> 8);
        c[site + 2] = std::uint8_t(target >> 16);
        c[site + 3] = std::uint8_t(target >> 24);
    }
    void patchHere(std::size_t site) { patchTo(site, here()); }

    std::uint32_t constIndex(const Value& v)
    {
        // By exact bits for a number, so -0 stays -0; by text for a string. A big function has thousands.
        auto& consts = fn_->code->consts;
        if (v.type() == Type::Double) {
            auto [it, fresh] = fn_->numberConsts.emplace(detail::Access::u8(v), std::uint32_t(consts.size()));
            if (fresh)
                consts.push_back(v);
            return it->second;
        }
        if (StringCell* text = asString(v)) {
            auto [it, fresh] = fn_->stringConsts.emplace(String(text->text()), std::uint32_t(consts.size()));
            if (fresh)
                consts.push_back(v);
            return it->second;
        }
        for (std::uint32_t i = 0; i < consts.size(); ++i) {
            if (consts[i].type() == v.type() && strictEquals(consts[i], v))
                return i;
        }
        consts.push_back(v);
        return std::uint32_t(consts.size() - 1);
    }
    std::uint32_t atomIndex(GCObject* key)
    {
        auto& atoms = fn_->code->atoms;
        auto [it, fresh] = fn_->atomSlots.emplace(key, std::uint32_t(atoms.size()));
        if (fresh)
            atoms.emplace_back(key);
        return it->second;
    }
    std::uint32_t atomFor(StringView name)
    {
        StringCell* key = rt_.intern(name);
        key->inSource = true;
        return atomIndex(key);
    }
    /// The operand of an instruction with an inline cache.
    std::uint32_t siteOperand(StringView name)
    {
        const std::uint32_t atom = atomFor(name);
        if (atom > kAtomMask)
            throw Error(ErrorCode::CompileError, "a function refers to too many property names");
        std::uint32_t& count = fn_->code->siteCount;
        const std::uint32_t site = count < kNoSite ? count++ : kNoSite;
        return atom | (site << kSiteShift);
    }

    [[noreturn]] void fail(std::uint32_t line, const std::string& message)
    {
        throw Error(ErrorCode::SyntaxError,
                    *sourceName_ + ":" + std::to_string(line) + ": " + message);
    }

    // ---- scopes and variables ----
    std::uint32_t hiddenSlot()
    {
        const std::uint32_t slot = fn_->nextSlot++;
        fn_->code->numLocals = std::max(fn_->code->numLocals, slot + 1);
        return slot;
    }

    /// Declare a local in the current scope. A captured local lives in a
    /// cell, which is created here unless `makeCell` is false because the VM
    /// supplies it at frame entry.
    std::uint32_t declareLocal(const String& name, bool isConst, bool makeCell = true)
    {
        Local local;
        local.name = name;
        local.slot = fn_->nextSlot++;
        local.depth = fn_->depth;
        local.boxed = fn_->boxAll || fn_->captured.count(name) != 0;
        local.isConst = isConst;
        fn_->code->numLocals = std::max(fn_->code->numLocals, local.slot + 1);
        fn_->locals.push_back(local);
        if (local.boxed && makeCell)
            emitU(Op::NewCell, local.slot);
        return local.slot;
    }

    void beginScope() { ++fn_->depth; }
    void endScope()
    {
        --fn_->depth;
        while (!fn_->locals.empty() && fn_->locals.back().depth > fn_->depth)
            fn_->locals.pop_back();
    }

    static const Local* findLocal(const FnState& fs, StringView name)
    {
        for (std::size_t i = fs.locals.size(); i-- > 0;) {
            if (fs.locals[i].name == name)
                return &fs.locals[i];
        }
        return nullptr;
    }

    int resolveUpval(FnState& fs, StringView name)
    {
        if (!fs.parent)
            return -1;
        for (std::size_t i = 0; i < fs.upvalNames.size(); ++i) {
            if (fs.upvalNames[i] == name)
                return int(i);
        }
        if (const Local* local = findLocal(*fs.parent, name)) {
            if (!local->boxed)
                return -1;  // the pre-pass says nobody captures it
            fs.code->upvals.push_back(UpvalDesc{true, local->slot});
            fs.upvalNames.emplace_back(name);
            fs.upvalConst.push_back(local->isConst);
            return int(fs.upvalNames.size() - 1);
        }
        const int outer = resolveUpval(*fs.parent, name);
        if (outer < 0)
            return -1;
        fs.code->upvals.push_back(UpvalDesc{false, std::uint32_t(outer)});
        fs.upvalNames.emplace_back(name);
        fs.upvalConst.push_back(fs.parent->upvalConst[std::size_t(outer)]);
        return int(fs.upvalNames.size() - 1);
    }

    VarRef resolve(StringView name)
    {
        if (const Local* local = findLocal(*fn_, name))
            return {local->boxed ? RefKind::Box : RefKind::Local, local->slot, local->isConst};
        const int up = resolveUpval(*fn_, name);
        if (up >= 0)
            return {RefKind::Upval, std::uint32_t(up), fn_->upvalConst[std::size_t(up)]};
        return {RefKind::Global, atomFor(name), false};
    }

    void loadVar(StringView name)
    {
        const VarRef ref = resolve(name);
        switch (ref.kind) {
            case RefKind::Local: emitU(Op::GetLocal, ref.index); break;
            case RefKind::Box: emitU(Op::GetCell, ref.index); break;
            case RefKind::Upval: emitU(Op::GetUpval, ref.index); break;
            case RefKind::Global: emitU(Op::GetGlobal, ref.index); break;
        }
    }

    void storeVar(StringView name, std::uint32_t line)
    {
        const VarRef ref = resolve(name);
        if (ref.isConst)
            fail(line, "assignment to a constant: " + toUtf8(name));
        switch (ref.kind) {
            case RefKind::Local: emitU(Op::SetLocal, ref.index); break;
            case RefKind::Box: emitU(Op::SetCell, ref.index); break;
            case RefKind::Upval: emitU(Op::SetUpval, ref.index); break;
            case RefKind::Global: emitU(Op::SetGlobal, ref.index); break;
        }
    }

    // ---- hoisting ----
    void hoist(const Node& body, bool global)
    {
        std::vector<String> vars;
        std::vector<const Node*> funcs;
        for (const NodePtr& s : body.list)
            collectHoisted(*s, vars, funcs, true);

        for (const String& name : vars) {
            if (global) {
                emitU(Op::DefGlobal, atomFor(name), PropWritable | PropEnumerable);
            } else if (!findLocal(*fn_, name)) {
                const std::uint32_t slot = declareLocal(name, false);
                emit(Op::PushUndefined);
                emitU(fn_->locals.back().boxed ? Op::SetCell : Op::SetLocal, slot);
            }
        }
        // Every name this body binds has to exist before the first nested
        // function is compiled, or a reference from inside that function to a
        // sibling function, a `let`, a `const` or a `class` declared further
        // down would resolve as a global.
        if (!global) {
            for (const Node* f : funcs) {
                if (!findLocal(*fn_, f->text))
                    declareLocal(f->text, false);
            }
            predeclareLexical(body.list);
        }
        for (const Node* f : funcs) {
            fn_->hoistedFuncs.insert(f);
            if (global)
                emitU(Op::DefGlobal, atomFor(f->text), PropWritable | PropEnumerable);
            function(*f->a, f->text);
            storeVar(f->text, f->line);
        }
    }

    /// Give the `let`, `const` and `class` declarations directly inside a
    /// scope their slots at the top of it. `declarePattern` later finds the
    /// slot and stores into it instead of making a second one. A boxed slot
    /// gets its cell here, so a closure created above the declaration line
    /// shares the binding the declaration will initialise.
    void predeclareLexical(const std::vector<NodePtr>& statements)
    {
        for (const NodePtr& s : statements) {
            if (s->kind == NK::VarDecl && s->varKind != VarKind::Var) {
                for (const NodePtr& d : s->list) {
                    if (d->a)
                        predeclareTarget(*d->a, s->varKind == VarKind::Const);
                }
            } else if (s->kind == NK::ClassDecl && !s->text.empty()) {
                predeclareName(s->text, false);
            }
        }
    }

    void predeclareName(const String& name, bool isConst)
    {
        const Local* existing = findLocal(*fn_, name);
        if (!existing || existing->depth != fn_->depth)
            declareLocal(name, isConst);
    }

    void predeclareTarget(const Node& target, bool isConst)
    {
        switch (target.kind) {
            case NK::Identifier: predeclareName(target.text, isConst); return;
            case NK::AssignPattern:
            case NK::RestElement:
                if (target.a)
                    predeclareTarget(*target.a, isConst);
                return;
            case NK::ArrayPattern:
                for (const NodePtr& element : target.list)
                    predeclareTarget(*element, isConst);
                return;
            case NK::ObjectPattern:
                for (const NodePtr& prop : target.list) {
                    if (prop->kind == NK::RestElement)
                        predeclareTarget(*prop, isConst);
                    else if (prop->b)
                        predeclareTarget(*prop->b, isConst);
                }
                return;
            default: return;
        }
    }

    // ---- functions ----
    /// Compile a FunctionExpr or ArrowExpr and leave the closure on the stack.
    void function(const Node& node, StringView name)
    {
        FnState fs;
        fs.parent = fn_;
        fs.code = std::make_shared<FunctionCode>();
        fs.code->setName(toUtf8(name));
        fs.code->sourceName = sourceName_;
        fs.code->isArrow = node.kind == NK::ArrowExpr;
        fs.isArrow = fs.code->isArrow;
        fs.code->isClassConstructor = node.propKind == PropKind::Constructor;
        fs.code->isDerivedConstructor = node.isStatic;  // set by compileClass
        fs.isDerivedCtor = fs.code->isDerivedConstructor && fs.code->isClassConstructor;
        fs.code->arity = int(node.list.size());
        fs.code->isGenerator = node.isGenerator;
        fs.code->isAsync = node.isAsync;
        // A coroutine lives in a frame of its own, which is the interpreter's to resume.
        // And what `super()` binds `this` to is known only in the frame.
        if (node.isGenerator || node.isAsync || fs.isDerivedCtor)
            fs.code->jitRefused = true;
        const StrictScope strictBody(*this, node.d && hasUseStrict(*node.d));
        fs.code->sloppyThis = sloppyThis_ && strictDepth_ == 0;
        fs.isMethod = node.isMethod || fs.code->isClassConstructor;
        fs.code->isMethod = node.isMethod && !fs.code->isClassConstructor;
        std::vector<String> paramNames;
        if (earlyErrors()) {
            bool simple = true;
            for (const NodePtr& p : node.list) {
                simple = simple && p->kind == NK::Identifier;
                patternNames(*p, paramNames);
            }
            if (!simple && node.d && hasUseStrict(*node.d))
                fail(node.line, "'use strict' needs simple parameters");
            std::set<String> seen;
            for (const String& name : paramNames) {
                checkBindingName(name, node.line);
                if (!seen.insert(name).second && (inStrictCode() || !simple || fs.isArrow || node.isMethod))
                    fail(node.line, "a parameter is named twice: " + toUtf8(name));
            }
            if (!node.text.empty() && node.kind == NK::FunctionExpr && !node.isMethod)
                checkBindingName(node.text, node.line);
            if (node.d)
                checkDeclarations(node.d->list, true, &paramNames);
        }

        collectCaptured(*node.d, fs.captured);
        for (const NodePtr& p : node.list)
            collectCaptured(*p, fs.captured);
        fs.boxAll = containsDirectEval(*node.d);
        for (const NodePtr& p : node.list)
            fs.boxAll = fs.boxAll || containsDirectEval(*p);
        if (fs.boxAll)
            fs.code->jitRefused = true;

        FnState* saved = fn_;
        fn_ = &fs;
        // An async function's promise is rejected by what its parameters throw; a generator's are bound by the call.
        if (node.isAsync && !node.isGenerator)
            emit(Op::CoStart);

        // Parameters occupy the first slots; the VM writes them there directly
        // unless they are boxed or destructured. Nothing else may take a slot
        // before them.
        std::vector<std::uint32_t> plainParams;
        for (std::size_t i = 0; i < node.list.size(); ++i) {
            const Node& p = *node.list[i];
            if (p.kind == NK::RestElement) {
                fs.code->hasRest = true;
                fs.code->arity = int(i);
            }
            const std::uint32_t slot = fn_->nextSlot++;
            fs.code->numLocals = std::max(fs.code->numLocals, slot + 1);
            plainParams.push_back(slot);
        }

        // Arrow functions inside this one read `this` from a box. It is made
        // before the parameters are bound so a default value can use it.
        if (!fs.isArrow && (fs.boxAll || fs.captured.count(ascii("this")))) {
            const std::uint32_t slot = declareLocal(ascii("this"), false);
            emit(Op::PushThis);
            emitU(Op::SetCell, slot);
        }
        if (!fs.isArrow && (fs.boxAll || fs.captured.count(ascii(" ctor")))) {
            const std::uint32_t slot = declareLocal(ascii(" ctor"), false);
            emit(Op::PushCallee);
            emitU(Op::SetCell, slot);
        }
        if (!fs.isArrow && (fs.boxAll || fs.captured.count(ascii(" nt")))) {
            const std::uint32_t slot = declareLocal(ascii(" nt"), false);
            emit(Op::PushNewTarget);
            emitU(Op::SetCell, slot);
        }

        for (std::size_t i = 0; i < node.list.size(); ++i)
            bindParameter(*node.list[i], plainParams[i]);

        // A parameter called `arguments` is what the name means, and there is no object.
        std::vector<String> allParams;
        for (const NodePtr& p : node.list)
            patternNames(*p, allParams);
        const bool paramIsArguments = std::find(allParams.begin(), allParams.end(), ascii("arguments")) != allParams.end();
        if (!fs.isArrow && !paramIsArguments && (fs.boxAll || mentionsArguments(*node.d))) {
            // The VM stores the arguments array, boxed or not, at frame entry.
            const std::uint32_t slot = declareLocal(ascii("arguments"), false, false);
            fs.code->argumentsSlot = slot;
            fs.code->argumentsBoxed = fn_->locals.back().boxed;
        }

        // `function fact(n) { ... fact(n - 1) ... }` used as an expression:
        // the name is a binding inside the body, unless a parameter or a
        // hoisted declaration of the body already claims it.
        if (node.namedExpr && !node.text.empty() && !findLocal(*fn_, node.text)) {
            std::vector<String> vars;
            std::vector<const Node*> funcs;
            for (const NodePtr& s : node.d->list)
                collectHoisted(*s, vars, funcs, true);
            const bool claimed =
                std::find(vars.begin(), vars.end(), node.text) != vars.end() ||
                std::any_of(funcs.begin(), funcs.end(),
                            [&](const Node* f) { return f->text == node.text; });
            if (!claimed) {
                const std::uint32_t slot = declareLocal(node.text, false);
                emit(Op::PushCallee);
                emitU(fn_->locals.back().boxed ? Op::SetCell : Op::SetLocal, slot);
            }
        }

        hoist(*node.d, false);
        if (node.isGenerator)
            emit(Op::CoStart);
        for (const NodePtr& s : node.d->list)
            stmt(*s);
        if (fs.isDerivedCtor) {
            emit(Op::PushUndefined);
            emitFinalReturn();
        } else {
            emit(Op::ReturnUndefined);
        }

        fn_ = saved;
        fn_->code->children.push_back(fs.code);
        emitU(Op::Closure, std::uint32_t(fn_->code->children.size() - 1));
    }

    /// Give parameter slot `slot` a name, or unpack it into a pattern.
    void bindParameter(const Node& param, std::uint32_t slot)
    {
        const Node* target = &param;
        const Node* defaultValue = nullptr;
        if (target->kind == NK::RestElement)
            target = target->a.get();
        if (target->kind == NK::AssignPattern) {
            defaultValue = target->b.get();
            target = target->a.get();
        }

        if (target->kind == NK::Identifier && !defaultValue) {
            Local local;
            local.name = target->text;
            local.slot = slot;
            local.depth = fn_->depth;
            local.boxed = fn_->boxAll || fn_->captured.count(target->text) != 0;
            fn_->locals.push_back(local);
            if (local.boxed) {
                // The VM leaves the raw argument in the plain slot; move it
                // into a box the closures can share.
                emitU(Op::GetLocal, slot);
                emitU(Op::NewCell, slot);
                emitU(Op::SetCell, slot);
            }
            return;
        }

        emitU(Op::GetLocal, slot);
        if (defaultValue)
            emitDefault(*defaultValue, target->kind == NK::Identifier ? target->text : String());
        declarePattern(*target, VarKind::Let);
    }

    /// Replace the value on top of the stack with `value` when it is undefined.
    void emitDefault(const Node& value, const String& name = String())
    {
        emit(Op::Dup);
        emit(Op::PushUndefined);
        emit(Op::StrictEq);
        const std::size_t skip = emitJump(Op::JumpIfFalse);
        emit(Op::Pop);
        exprNamed(value, name);
        patchHere(skip);
    }

    /// An anonymous function or class takes the name of what it is assigned to.
    void exprNamed(const Node& value, const String& name)
    {
        if (!name.empty() && (value.kind == NK::FunctionExpr || value.kind == NK::ArrowExpr) && value.text.empty() && !value.isMethod) {
            function(value, name);
            return;
        }
        if (!name.empty() && value.kind == NK::ClassExpr && value.text.empty()) {
            classNameHint_ = name;
            classExpr(value);
            return;
        }
        expr(value);
    }

    /// Push `this`, from its box when an arrow function captured it.
    void loadThis()
    {
        loadThisUnchecked();
        // Ahead of `super()` there is none yet.
        const FnState* owner = fn_;
        while (owner->isArrow && owner->parent)
            owner = owner->parent;
        if (owner->isDerivedCtor)
            emit(Op::CheckThis);
    }
    void loadThisUnchecked()
    {
        if (findLocal(*fn_, ascii("this")) || fn_->isArrow)
            loadVar(ascii("this"));
        else
            emit(Op::PushThis);
    }
    void emitFinalReturn()
    {
        if (fn_->isDerivedCtor) {
            loadThisUnchecked();
            emit(Op::DerivedReturn);
        }
        emit(Op::Return);
    }

    // ---- statements ----
    void stmt(const Node& n)
    {
        checkNativeStack();
        note(n.line);
        switch (n.kind) {
            case NK::Empty:
                return;
            case NK::FieldInit: fieldInit(n); return;
            case NK::ExprStatement:
                expr(*n.a);
                // The value of a program is the value of the last expression
                // statement it evaluated, wherever that statement sits.
                if ((fn_->isGlobal || fn_->isEval) && fn_->completionSlot != kNoSlot) {
                    emit(Op::Dup);
                    emitU(Op::SetLocal, fn_->completionSlot);
                }
                emit(Op::Pop);
                return;
            case NK::VarDecl:
                varDecl(n);
                return;
            case NK::Block:
                if (!n.list.empty())
                    checkDeclarations(n.list, false);
                beginScope();
                predeclareLexical(n.list);
                for (const NodePtr& s : n.list)
                    stmt(*s);
                endScope();
                return;
            case NK::If: {
                expr(*n.a);
                const std::size_t toElse = emitJump(Op::JumpIfFalse);
                stmt(*n.b);
                if (n.c) {
                    const std::size_t done = emitJump(Op::Jump);
                    patchHere(toElse);
                    stmt(*n.c);
                    patchHere(done);
                } else {
                    patchHere(toElse);
                }
                return;
            }
            case NK::While:
                whileLoop(n, String());
                return;
            case NK::DoWhile:
                doLoop(n, String());
                return;
            case NK::For:
                forLoop(n, String());
                return;
            case NK::ForIn:
            case NK::ForOf:
                forEachLoop(n, String());
                return;
            case NK::Labeled:
                labeled(n);
                return;
            case NK::BreakStmt:
            case NK::ContinueStmt:
                jumpOut(n);
                return;
            case NK::ReturnStmt:
                emitReturn(n.a.get());
                return;
            case NK::ThrowStmt:
                expr(*n.a);
                emit(Op::Throw);
                return;
            case NK::Try:
                tryStatement(n);
                return;
            case NK::Switch:
                switchStatement(n);
                return;
            case NK::FunctionDecl:
                if (fn_->hoistedFuncs.count(&n))
                    return;
                if (!findLocal(*fn_, n.text) && !fn_->isGlobal)
                    declareLocal(n.text, false);
                function(*n.a, n.text);
                storeVar(n.text, n.line);
                return;
            case NK::ClassDecl:
                if (fn_->isGlobal && fn_->depth == 0)
                    emitU(Op::DefGlobal, atomFor(n.text), PropWritable | PropEnumerable);
                else if (!findLocal(*fn_, n.text))
                    declareLocal(n.text, false);
                classExpr(*n.a);
                storeVar(n.text, n.line);
                return;
            default:
                expr(n);
                emit(Op::Pop);
                return;
        }
    }

    void varDecl(const Node& n)
    {
        for (const NodePtr& d : n.list) {
            const Node& target = *d->a;
            const String name = target.kind == NK::Identifier ? target.text : String();
            if (n.varKind == VarKind::Var) {
                if (!d->b)
                    continue;  // already hoisted to undefined
                exprNamed(*d->b, name);
                assignPattern(target);
                emit(Op::Pop);
                continue;
            }
            if (d->b)
                exprNamed(*d->b, name);
            else
                emit(Op::PushUndefined);
            declarePattern(target, n.varKind);
        }
    }

    /// Introduce fresh bindings for `target`, consuming the value on top.
    void declarePattern(const Node& target, VarKind kind)
    {
        const bool isConst = kind == VarKind::Const;
        if (target.kind == NK::Identifier) {
            // What `let` and `const` declare in an `eval` stays in it.
            if (fn_->isGlobal && fn_->depth == 0 && !(fn_->isEval && kind != VarKind::Var)) {
                // Top-level bindings live on the global object so a REPL and
                // successive `eval` calls see each other's declarations.
                emitU(Op::DefGlobal, atomFor(target.text), PropWritable | PropEnumerable);
                emitU(Op::SetGlobal, atomFor(target.text));
                return;
            }
            // `predeclareLexical` usually made the slot at the top of the
            // scope so that a closure created above this line could capture it.
            const Local* existing = findLocal(*fn_, target.text);
            if (existing && existing->depth == fn_->depth) {
                emitU(existing->boxed ? Op::SetCell : Op::SetLocal, existing->slot);
                return;
            }
            const std::uint32_t slot = declareLocal(target.text, isConst);
            emitU(fn_->locals.back().boxed ? Op::SetCell : Op::SetLocal, slot);
            return;
        }
        destructure(target, kind, true);
    }

    /// Assign to an existing target, consuming the value on top and leaving it.
    void assignPattern(const Node& target)
    {
        switch (target.kind) {
            case NK::Identifier:
                emit(Op::Dup);
                storeVar(target.text, target.line);
                return;
            case NK::Member: {
                // stack: value  ->  obj value / obj key value
                expr(*target.a);
                if (target.computed) {
                    expr(*target.b);
                    emit(Op::Rot3);  // key obj value -> value key obj ... see below
                    emit(Op::Rot3);
                    emitElem(Op::SetElem);
                } else {
                    emit(Op::Swap);
                    emitU(Op::SetProp, siteOperand(target.text));
                }
                return;
            }
            case NK::ArrayPattern:
            case NK::ObjectPattern:
                emit(Op::Dup);
                destructure(target, VarKind::None, false);
                return;
            default:
                fail(target.line, "invalid assignment target");
        }
    }

    /// Unpack the value on top of the stack into `pattern`, consuming it.
    void destructure(const Node& pattern, VarKind kind, bool declaring)
    {
        if (pattern.kind == NK::Identifier || pattern.kind == NK::Member) {
            if (declaring)
                declarePattern(pattern, kind);
            else {
                assignPattern(pattern);
                emit(Op::Pop);
            }
            return;
        }
        if (pattern.kind == NK::AssignPattern) {
            emitDefault(*pattern.b, pattern.a->kind == NK::Identifier ? pattern.a->text : String());
            destructure(*pattern.a, kind, declaring);
            return;
        }
        if (pattern.kind == NK::ArrayPattern) {
            emit(Op::IterStart);
            for (const NodePtr& elem : pattern.list) {
                if (elem->kind == NK::Hole) {
                    emit(Op::IterNextOpt);
                    emit(Op::Pop);
                    continue;
                }
                if (elem->kind == NK::RestElement) {
                    emit(Op::IterRest);
                    destructure(*elem->a, kind, declaring);
                    continue;
                }
                emit(Op::IterNextOpt);
                destructure(*elem, kind, declaring);
            }
            emit(Op::IterClose);
            return;
        }
        if (pattern.kind == NK::ObjectPattern) {
            std::vector<const Node*> taken;
            for (const NodePtr& p : pattern.list) {
                if (p->kind == NK::RestElement) {
                    emit(Op::Dup);
                    emit(Op::NewArray);
                    operand(std::uint32_t(taken.size()));
                    for (const Node* k : taken) {
                        if (k->computed)
                            expr(*k->a);
                        else
                            emitU(Op::PushConst, constIndex(rt_.str(k->text)));
                        emit(Op::ArrayAppend);
                    }
                    emit(Op::ObjectRest);
                    destructure(*p->a, kind, declaring);
                    continue;
                }
                taken.push_back(p.get());
                emit(Op::Dup);
                if (p->computed) {
                    expr(*p->a);
                    emitElem(Op::GetElem);
                } else {
                    emitU(Op::GetProp, siteOperand(p->text));
                }
                destructure(*p->b, kind, declaring);
            }
            emit(Op::Pop);
            return;
        }
        fail(pattern.line, "unsupported binding pattern");
    }

    // ---- loops ----
    LoopCtx& pushLoop(const String& label, bool continuable)
    {
        fn_->loops.emplace_back();
        LoopCtx& loop = fn_->loops.back();
        loop.label = label;
        loop.continuable = continuable;
        loop.finallyDepth = fn_->finallys.size();
        return loop;
    }
    void popLoop(std::uint32_t breakTarget, std::uint32_t continueTarget)
    {
        LoopCtx loop = std::move(fn_->loops.back());
        fn_->loops.pop_back();
        for (std::size_t site : loop.breaks)
            patchTo(site, breakTarget);
        for (std::size_t site : loop.continues)
            patchTo(site, continueTarget);
    }

    void whileLoop(const Node& n, const String& label)
    {
        const std::uint32_t top = here();
        pushLoop(label, true);
        expr(*n.a);
        const std::size_t exit = emitJump(Op::JumpIfFalse);
        stmt(*n.d);
        emitU(Op::Jump, top);
        patchHere(exit);
        popLoop(here(), top);
    }

    void doLoop(const Node& n, const String& label)
    {
        const std::uint32_t top = here();
        pushLoop(label, true);
        stmt(*n.d);
        const std::uint32_t testPc = here();
        expr(*n.a);
        emitU(Op::JumpIfTrue, top);
        popLoop(here(), testPc);
    }

    void forLoop(const Node& n, const String& label)
    {
        beginScope();
        std::vector<std::uint32_t> perIteration;
        if (n.a) {
            const std::size_t before = fn_->locals.size();
            stmt(*n.a);
            if (n.a->kind == NK::VarDecl && n.a->varKind != VarKind::Var) {
                for (std::size_t i = before; i < fn_->locals.size(); ++i) {
                    if (fn_->locals[i].boxed)
                        perIteration.push_back(fn_->locals[i].slot);
                }
            }
        }
        const std::uint32_t top = here();
        pushLoop(label, true);
        std::size_t exit = 0;
        bool hasTest = false;
        if (n.b) {
            expr(*n.b);
            exit = emitJump(Op::JumpIfFalse);
            hasTest = true;
        }
        stmt(*n.d);
        const std::uint32_t continuePc = here();
        // Each iteration of a `let` loop gets its own binding, so a closure
        // made in one iteration does not see the next one's value.
        for (std::uint32_t slot : perIteration)
            emitU(Op::RenewCell, slot);
        if (n.c) {
            expr(*n.c);
            emit(Op::Pop);
        }
        emitU(Op::Jump, top);
        if (hasTest)
            patchHere(exit);
        popLoop(here(), continuePc);
        endScope();
    }

    void forEachLoop(const Node& n, const String& label)
    {
        const bool isOf = n.kind == NK::ForOf;
        const bool isAwait = isOf && n.isAsync;
        expr(*n.b);
        if (isAwait)
            emit(Op::AsyncIterStart);
        else if (isOf)
            emit(Op::IterStartLoop);
        else
            emit(Op::ForInStart);

        const std::uint32_t iterDepth = fn_->stmtDepth;
        fn_->stmtDepth += isOf ? 1 : 2;
        const std::uint32_t top = here();
        // What leaves the body other than by the loop's own `break` and `continue` closes the iterator.
        FinallyCtx closes;
        closes.iterDepth = iterDepth;
        closes.asyncIter = isAwait;
        if (isOf)
            fn_->finallys.push_back(&closes);
        pushLoop(label, true);
        std::size_t exit;
        if (isAwait) {
            emit(Op::AsyncIterCall);
            emit(Op::Await);
            exit = emitJump(Op::AsyncIterCheck);
        } else {
            exit = emitJump(isOf ? Op::IterNext : Op::ForInNext);
        }

        const std::uint32_t bodyStart = here();
        beginScope();
        if (n.varKind == VarKind::None)
            assignPattern(*n.a), emit(Op::Pop);
        else
            declarePattern(*n.a, n.varKind);
        stmt(*n.d);
        endScope();
        const std::uint32_t bodyEnd = here();
        emitU(Op::Jump, top);

        if (isOf)
            fn_->finallys.pop_back();
        const std::uint32_t breakTarget = here();
        if (isOf) {
            // A `break` closes it; running out does not.
            emitFinallyCall(closes);
            emit(Op::Pop);
        }
        const std::size_t afterBreak = isOf ? emitJump(Op::Jump) : 0;
        patchHere(exit);
        popLoop(breakTarget, top);
        fn_->stmtDepth -= isOf ? 1 : 2;
        if (isOf) {
            emit(Op::Pop);
            patchHere(afterBreak);
            const std::size_t skip = emitJump(Op::Jump);
            const std::uint32_t closePc = here();
            emit(Op::IterCloseThrow);
            patchHere(skip);
            fn_->code->handlers.push_back(Handler{bodyStart, bodyEnd, closePc, iterDepth + 1, HandlerKind::IterClose});
        } else {
            emitU(Op::PopN, 2);
        }
    }

    /// `yield e`: on being resumed by `return`, what `return` does here.
    void yieldExpr(const Node& n)
    {
        const bool isAsyncGen = fn_->code->isAsync;
        if (n.prefix) {
            yieldStar(n);
            return;
        }
        if (n.a)
            expr(*n.a);
        else
            emit(Op::PushUndefined);
        if (isAsyncGen)
            emit(Op::Await);
        const std::size_t returned = emitJump(Op::Yield);
        const std::size_t resumed = emitJump(Op::Jump);
        patchHere(returned);
        if (isAsyncGen)
            emit(Op::Await);
        emitReturnOfTop();
        patchHere(resumed);
    }

    void yieldStar(const Node& n)
    {
        const bool isAsyncGen = fn_->code->isAsync;
        expr(*n.a);
        emit(isAsyncGen ? Op::AsyncIterStart : Op::IterStart);
        emit(Op::DelegateStart);
        const std::uint32_t top = here();
        const std::size_t noMethod = emitJump(Op::DelegateCall);
        if (isAsyncGen)
            emit(Op::Await);
        const std::size_t done = emitJump(Op::DelegateCheck);
        emitU(Op::Jump, top);
        patchHere(done);
        const std::size_t returned = emitJump(Op::DelegateDone);
        const std::size_t resumed = emitJump(Op::Jump);
        patchHere(noMethod);
        if (isAsyncGen)
            emit(Op::Await);
        patchHere(returned);
        emitReturnOfTop();
        patchHere(resumed);
    }

    void labeled(const Node& n)
    {
        const Node& body = *n.d;
        switch (body.kind) {
            case NK::While: whileLoop(body, n.text); return;
            case NK::DoWhile: doLoop(body, n.text); return;
            case NK::For: forLoop(body, n.text); return;
            case NK::ForIn:
            case NK::ForOf: forEachLoop(body, n.text); return;
            default: break;
        }
        // A label on anything else only supports `break`.
        pushLoop(n.text, false);
        stmt(body);
        popLoop(here(), here());
    }

    void jumpOut(const Node& n)
    {
        const bool isBreak = n.kind == NK::BreakStmt;
        std::size_t at = fn_->loops.size();
        while (at-- > 0) {
            LoopCtx& loop = fn_->loops[at];
            if (!n.text.empty() && loop.label != n.text)
                continue;
            if (n.text.empty() && !isBreak && !loop.continuable)
                continue;
            if (!isBreak && !loop.continuable)
                fail(n.line, "'continue' must target a loop");
            for (std::size_t i = fn_->finallys.size(); i-- > loop.finallyDepth;)
                emitFinallyCall(*fn_->finallys[i]);
            const std::size_t site = emitJump(Op::Jump);
            (isBreak ? loop.breaks : loop.continues).push_back(site);
            return;
        }
        fail(n.line, n.text.empty() ? "'break' or 'continue' outside a loop"
                                    : "unknown label: " + toUtf8(n.text));
    }

    void emitFinallyCall(FinallyCtx& ctx)
    {
        if (ctx.iterDepth == kNoSlot) {
            ctx.sites.push_back(emitJump(Op::JumpSub));
        } else if (ctx.asyncIter) {
            emitU(Op::AsyncIterClose, ctx.iterDepth);
            emit(Op::Await);
            emit(Op::Pop);
        } else {
            emitU(Op::IterCloseAt, ctx.iterDepth);
        }
    }

    void emitReturn(const Node* argument)
    {
        if (argument)
            expr(*argument);
        else
            emit(Op::PushUndefined);
        // What an async generator returns is awaited.
        if (argument && fn_->code->isAsync && fn_->code->isGenerator)
            emit(Op::Await);
        emitReturnOfTop();
    }

    /// `return` of the value on the stack, through every `finally` it is inside.
    void emitReturnOfTop()
    {
        if (fn_->finallys.empty()) {
            emitFinalReturn();
            return;
        }
        if (fn_->retvalSlot == kNoSlot)
            fn_->retvalSlot = hiddenSlot();
        emitU(Op::SetLocal, fn_->retvalSlot);
        for (std::size_t i = fn_->finallys.size(); i-- > 0;)
            emitFinallyCall(*fn_->finallys[i]);
        emitU(Op::GetLocal, fn_->retvalSlot);
        emitFinalReturn();
    }

    void tryStatement(const Node& n)
    {
        FinallyCtx finallyCtx;
        const bool hasFinally = n.d != nullptr;
        const bool hasCatch = n.c != nullptr;
        const std::uint32_t stackDepth = fn_->stmtDepth;

        const std::uint32_t tryStart = here();
        if (hasFinally)
            fn_->finallys.push_back(&finallyCtx);
        stmt(*n.a);
        if (hasFinally)
            fn_->finallys.pop_back();
        const std::uint32_t tryEnd = here();

        if (hasFinally)
            emitFinallyCall(finallyCtx);
        const std::size_t doneFromTry = emitJump(Op::Jump);

        std::uint32_t catchPc = 0;
        std::size_t doneFromCatch = 0;
        if (hasCatch) {
            catchPc = here();
            beginScope();
            if (n.b)
                declarePattern(*n.b, VarKind::Let);
            else
                emit(Op::Pop);
            if (hasFinally)
                fn_->finallys.push_back(&finallyCtx);
            stmt(*n.c);
            if (hasFinally)
                fn_->finallys.pop_back();
            endScope();
            if (hasFinally)
                emitFinallyCall(finallyCtx);
            doneFromCatch = emitJump(Op::Jump);
        }

        std::uint32_t finallyPc = 0;
        if (hasFinally) {
            finallyPc = here();
            for (std::size_t site : finallyCtx.sites)
                patchTo(site, finallyPc);
            stmt(*n.d);
            emit(Op::EndFinally);
        }

        patchHere(doneFromTry);
        if (hasCatch)
            patchHere(doneFromCatch);

        // Registered last so that handlers of nested `try`s, which were added
        // while the body compiled, are found first.
        if (hasCatch)
            fn_->code->handlers.push_back(
                Handler{tryStart, tryEnd, catchPc, stackDepth, HandlerKind::Catch});
        if (hasFinally)
            fn_->code->handlers.push_back(
                Handler{tryStart, finallyPc, finallyPc, stackDepth, HandlerKind::Finally});
    }

    void switchStatement(const Node& n)
    {
        expr(*n.a);
        ++fn_->stmtDepth;
        pushLoop(String(), false);

        // The cases share one scope. Its bindings are made here, before the
        // dispatch jumps, so that every case body lands past the slot setup.
        beginScope();
        for (const NodePtr& c : n.list)
            predeclareLexical(c->list);

        std::vector<std::size_t> caseJumps;
        std::size_t defaultJump = 0;
        bool hasDefault = false;
        for (const NodePtr& c : n.list) {
            if (!c->a) {
                hasDefault = true;
                continue;
            }
            emit(Op::Dup);
            expr(*c->a);
            emit(Op::StrictEq);
            caseJumps.push_back(emitJump(Op::JumpIfTrue));
        }
        defaultJump = emitJump(Op::Jump);

        std::size_t index = 0;
        std::uint32_t defaultPc = 0;
        for (const NodePtr& c : n.list) {
            if (!c->a)
                defaultPc = here();
            else
                patchHere(caseJumps[index++]);
            for (const NodePtr& s : c->list)
                stmt(*s);
        }
        endScope();
        const std::uint32_t endPc = here();
        patchTo(defaultJump, hasDefault ? defaultPc : endPc);
        popLoop(endPc, endPc);
        --fn_->stmtDepth;
        emit(Op::Pop);
    }

    // ---- expressions ----
    void expr(const Node& n)
    {
        checkNativeStack();
        note(n.line);
        switch (n.kind) {
            case NK::NumberLit:
                if (n.legacyOctal && earlyErrors() && inStrictCode())
                    fail(n.line, "a number with a leading zero is not allowed in strict code");
                emitU(Op::PushConst, constIndex(Value(n.number)));
                return;
            case NK::StringLit:
                emitU(Op::PushConst, constIndex(rt_.str(n.text)));
                return;
            case NK::BoolLit:
                emit(n.number != 0 ? Op::PushTrue : Op::PushFalse);
                return;
            case NK::NullLit:
                emit(Op::PushNull);
                return;
            case NK::Identifier:
                checkStrictWord(n.text, n.line);
                if (n.isPrivate && std::none_of(privateScopes_.begin(), privateScopes_.end(), [&](const std::set<String>& names) { return names.count(n.text) != 0; }))
                    fail(n.line, "a private name is not declared in any class around it: " + toUtf8(n.text));
                if (n.text == ascii("undefined") && !findLocal(*fn_, n.text)) {
                    emit(Op::PushUndefined);
                    return;
                }
                loadVar(n.text);
                return;
            case NK::ThisExpr:
                loadThis();
                return;
            case NK::TemplateLit:
                templateLit(n);
                return;
            case NK::ArrayLit:
                arrayLit(n);
                return;
            case NK::ObjectLit:
                objectLit(n);
                return;
            case NK::FunctionExpr:
            case NK::ArrowExpr:
                function(n, n.text);
                return;
            case NK::ClassExpr:
                classExpr(n);
                return;
            case NK::Unary:
                unary(n);
                return;
            case NK::Update:
                update(n);
                return;
            case NK::Binary:
                if (n.op == Tok::In && n.a->isPrivate) {
                    expr(*n.b);
                    expr(*n.a);
                    emit(Op::PrivateIn);
                    return;
                }
                expr(*n.a);
                expr(*n.b);
                emit(binaryOp(n.op, n.line));
                return;
            case NK::Logical:
                logical(n);
                return;
            case NK::Conditional: {
                expr(*n.a);
                const std::size_t toElse = emitJump(Op::JumpIfFalse);
                expr(*n.b);
                const std::size_t done = emitJump(Op::Jump);
                patchHere(toElse);
                expr(*n.c);
                patchHere(done);
                return;
            }
            case NK::Assign:
                assign(n);
                return;
            case NK::Sequence:
                for (std::size_t i = 0; i < n.list.size(); ++i) {
                    expr(*n.list[i]);
                    if (i + 1 < n.list.size())
                        emit(Op::Pop);
                }
                return;
            case NK::NewExpr:
                newExpr(n);
                return;
            case NK::Member:
            case NK::Call: {
                if (!chainHasOptional(n)) {
                    std::vector<std::size_t> none;
                    chain(n, none);
                    return;
                }
                std::vector<std::size_t> exits;
                chain(n, exits);
                const std::size_t done = emitJump(Op::Jump);
                for (std::size_t site : exits)
                    patchHere(site);
                emit(Op::Pop);
                emit(Op::PushUndefined);
                patchHere(done);
                return;
            }
            case NK::SuperExpr:
                fail(n.line, "'super' is only valid in a member access or a call");
            case NK::TemplateObject: templateObject(n); return;
            case NK::BigIntLit: {
                Value value;
                bigint::parseString(rt_, n.text, value);
                fn_->code->consts.push_back(value);
                emitU(Op::PushConst, std::uint32_t(fn_->code->consts.size() - 1));
                return;
            }
            case NK::RegExpLit:
                fn_->code->consts.push_back(makeRegExpTemplate(rt_, n.text, n.strs[0]));
                emitU(Op::NewRegExp, std::uint32_t(fn_->code->consts.size() - 1));
                return;
            case NK::NewTarget:
                if (fn_->isArrow)
                    loadVar(ascii(" nt"));
                else
                    emit(Op::PushNewTarget);
                return;
            case NK::Yield: yieldExpr(n); return;
            case NK::Await:
                expr(*n.a);
                emit(Op::Await);
                return;
            case NK::Spread:
                fail(n.line, "unexpected '...'");
            default:
                fail(n.line, "unsupported expression");
        }
    }

    void templateLit(const Node& n)
    {
        emitU(Op::PushConst, constIndex(rt_.str(n.strs.empty() ? String() : n.strs[0])));
        for (std::size_t i = 0; i < n.list.size(); ++i) {
            expr(*n.list[i]);
            // A substitution is converted with ToString, so an object's
            // `toString` wins over its `valueOf`, unlike `'' + value`.
            emit(Op::ToString);
            emit(Op::Add);
            if (i + 1 < n.strs.size() && !n.strs[i + 1].empty()) {
                emitU(Op::PushConst, constIndex(rt_.str(n.strs[i + 1])));
                emit(Op::Add);
            }
        }
    }

    void arrayLit(const Node& n)
    {
        emitU(Op::NewArray, std::uint32_t(n.list.size()));
        for (const NodePtr& e : n.list) {
            if (e->kind == NK::Spread) {
                expr(*e->a);
                emit(Op::ArraySpread);
            } else if (e->kind == NK::Hole) {
                fn_->code->consts.push_back(holeValue());
                emitU(Op::PushConst, std::uint32_t(fn_->code->consts.size() - 1));
                emit(Op::ArrayAppend);
            } else {
                expr(*e);
                emit(Op::ArrayAppend);
            }
        }
    }

    void objectLit(const Node& n)
    {
        // How many properties to make room for.
        emitU(Op::NewObject, std::uint32_t(std::min<std::size_t>(n.list.size(), 32)));
        for (const NodePtr& p : n.list) {
            if (p->propKind == PropKind::Spread) {
                expr(*p->a);
                emit(Op::ObjectSpread);
                continue;
            }
            if (p->computed) {
                expr(*p->a);
                emit(Op::ToPropertyKey);
                expr(*p->b);
                if (p->propKind == PropKind::Getter || p->propKind == PropKind::Setter)
                    emitU(Op::DefAccessorElem, p->propKind == PropKind::Setter ? 1u : 0u);
                else if (p->propKind == PropKind::Method)
                    emit(Op::DefMethodElem);
                else
                    emit(Op::DefElem);
                continue;
            }
            if (p->propKind == PropKind::Init && p->text != u"__proto__")
                exprNamed(*p->b, p->text);
            else
                expr(*p->b);
            switch (p->propKind) {
                case PropKind::Getter: emitU(Op::DefGetter, atomFor(p->text)); break;
                case PropKind::Setter: emitU(Op::DefSetter, atomFor(p->text)); break;
                case PropKind::Method: emitU(Op::DefMethod, atomFor(p->text)); break;
                default: emitU(Op::DefField, atomFor(p->text)); break;
            }
        }
    }

    Op binaryOp(Tok op, std::uint32_t line)
    {
        switch (op) {
            case Tok::Plus: return Op::Add;
            case Tok::Minus: return Op::Sub;
            case Tok::Star: return Op::Mul;
            case Tok::Slash: return Op::Div;
            case Tok::Percent: return Op::Mod;
            case Tok::StarStar: return Op::Pow;
            case Tok::Amp: return Op::BitAnd;
            case Tok::Pipe: return Op::BitOr;
            case Tok::Caret: return Op::BitXor;
            case Tok::Shl: return Op::Shl;
            case Tok::Shr: return Op::Shr;
            case Tok::UShr: return Op::UShr;
            case Tok::EqEq: return Op::Eq;
            case Tok::NotEq: return Op::Ne;
            case Tok::EqEqEq: return Op::StrictEq;
            case Tok::NotEqEq: return Op::StrictNe;
            case Tok::Lt: return Op::Lt;
            case Tok::Le: return Op::Le;
            case Tok::Gt: return Op::Gt;
            case Tok::Ge: return Op::Ge;
            case Tok::In: return Op::In;
            case Tok::InstanceOf: return Op::InstanceOf;
            default: fail(line, "unsupported operator");
        }
    }

    void unary(const Node& n)
    {
        if (n.op == Tok::TypeOf && n.a->kind == NK::Identifier) {
            const VarRef ref = resolve(n.a->text);
            if (ref.kind == RefKind::Global) {
                emitU(Op::TypeofGlobal, ref.index);
                return;
            }
        }
        if (n.op == Tok::Delete) {
            const Node& target = *n.a;
            if (target.kind == NK::Member && target.b && target.b->isPrivate)
                fail(n.line, "a private member cannot be deleted");
            if (earlyErrors() && inStrictCode() && target.kind == NK::Identifier)
                fail(n.line, "strict code cannot delete a name");
            if (target.kind == NK::Member) {
                expr(*target.a);
                if (target.computed) {
                    expr(*target.b);
                    emit(Op::DeleteElem);
                } else {
                    emitU(Op::DeleteProp, atomFor(target.text));
                }
                return;
            }
            if (target.kind == NK::Identifier) {
                const VarRef ref = resolve(target.text);
                if (ref.kind == RefKind::Global) {
                    emitU(Op::DeleteGlobal, ref.index);
                    return;
                }
                emit(Op::PushFalse);
                return;
            }
            expr(target);
            emit(Op::Pop);
            emit(Op::PushTrue);
            return;
        }
        expr(*n.a);
        switch (n.op) {
            case Tok::Bang: emit(Op::Not); return;
            case Tok::Tilde: emit(Op::BitNot); return;
            case Tok::Minus: emit(Op::Neg); return;
            case Tok::Plus: emit(Op::Pos); return;
            case Tok::TypeOf: emit(Op::TypeOf); return;
            case Tok::Void: emit(Op::Void); return;
            default: fail(n.line, "unsupported unary operator");
        }
    }

    std::uint32_t temp()
    {
        if (fn_->tempSlot == kNoSlot)
            fn_->tempSlot = hiddenSlot();
        return fn_->tempSlot;
    }

    void update(const Node& n)
    {
        const Node& target = *n.a;
        const Op step = n.op == Tok::PlusPlus ? Op::Inc : Op::Dec;
        if (target.kind == NK::Identifier) {
            loadVar(target.text);
            emit(Op::ToNumber);
            if (!n.prefix)
                emit(Op::Dup);
            emit(step);
            if (n.prefix)
                emit(Op::Dup);
            storeVar(target.text, n.line);
            return;
        }
        if (target.kind != NK::Member)
            fail(n.line, "invalid increment target");

        // The result is parked in a scratch slot: the receiver sits under the
        // value, so it cannot be shuffled out of the way cheaply.
        const std::uint32_t slot = temp();
        expr(*target.a);
        if (target.computed) {
            expr(*target.b);
            emit(Op::Dup2);
            emitElem(Op::GetElem);
        } else {
            emit(Op::Dup);
            emitU(Op::GetProp, siteOperand(target.text));
        }
        emit(Op::ToNumber);
        if (!n.prefix) {
            emit(Op::Dup);
            emitU(Op::SetLocal, slot);
        }
        emit(step);
        if (n.prefix) {
            emit(Op::Dup);
            emitU(Op::SetLocal, slot);
        }
        if (target.computed)
            emitElem(Op::SetElem);
        else
            emitU(Op::SetProp, siteOperand(target.text));
        emit(Op::Pop);
        emitU(Op::GetLocal, slot);
    }

    void logical(const Node& n)
    {
        expr(*n.a);
        const std::size_t done = emitJump(shortCircuitJump(n.op));
        emit(Op::Pop);
        expr(*n.b);
        patchHere(done);
    }

    void assign(const Node& n)
    {
        if (n.op == Tok::Assign) {
            const Node& target = *n.a;
            if (target.kind == NK::ArrayPattern || target.kind == NK::ObjectPattern) {
                expr(*n.b);
                emit(Op::Dup);
                destructure(target, VarKind::None, false);
                return;
            }
            if (target.kind == NK::Identifier) {
                exprNamed(*n.b, target.text);
                emit(Op::Dup);
                storeVar(target.text, n.line);
                return;
            }
            if (target.kind != NK::Member)
                fail(n.line, "invalid assignment target");
            expr(*target.a);
            if (target.computed) {
                expr(*target.b);
                expr(*n.b);
                emitElem(Op::SetElem);
            } else {
                expr(*n.b);
                emitU(Op::SetProp, siteOperand(target.text));
            }
            return;
        }

        if (n.op == Tok::AndAssign || n.op == Tok::OrAssign || n.op == Tok::NullishAssign) {
            expr(*n.a);
            const std::size_t done = emitJump(shortCircuitJump(n.op));
            emit(Op::Pop);
            expr(*n.b);
            emit(Op::Dup);
            assignTo(*n.a, n.line);
            patchHere(done);
            return;
        }

        const Op op = compoundOp(n.op, n.line);
        const Node& target = *n.a;
        if (target.kind == NK::Identifier) {
            loadVar(target.text);
            expr(*n.b);
            emit(op);
            emit(Op::Dup);
            storeVar(target.text, n.line);
            return;
        }
        if (target.kind != NK::Member)
            fail(n.line, "invalid assignment target");
        expr(*target.a);
        if (target.computed) {
            expr(*target.b);
            emit(Op::Dup2);
            emitElem(Op::GetElem);
            expr(*n.b);
            emit(op);
            emitElem(Op::SetElem);
        } else {
            emit(Op::Dup);
            emitU(Op::GetProp, siteOperand(target.text));
            expr(*n.b);
            emit(op);
            emitU(Op::SetProp, siteOperand(target.text));
        }
    }

    /// Store the value on top of the stack into `target`, popping it.
    void assignTo(const Node& target, std::uint32_t line)
    {
        if (target.kind == NK::Identifier) {
            storeVar(target.text, line);
            return;
        }
        if (target.kind != NK::Member)
            fail(line, "invalid assignment target");
        expr(*target.a);
        if (target.computed) {
            expr(*target.b);
            emit(Op::Rot3);
            emit(Op::Rot3);
            emitElem(Op::SetElem);
            emit(Op::Pop);
        } else {
            emit(Op::Swap);
            emitU(Op::SetProp, siteOperand(target.text));
            emit(Op::Pop);
        }
    }

    Op compoundOp(Tok op, std::uint32_t line)
    {
        switch (op) {
            case Tok::PlusAssign: return Op::Add;
            case Tok::MinusAssign: return Op::Sub;
            case Tok::StarAssign: return Op::Mul;
            case Tok::SlashAssign: return Op::Div;
            case Tok::PercentAssign: return Op::Mod;
            case Tok::StarStarAssign: return Op::Pow;
            case Tok::ShlAssign: return Op::Shl;
            case Tok::ShrAssign: return Op::Shr;
            case Tok::UShrAssign: return Op::UShr;
            case Tok::AmpAssign: return Op::BitAnd;
            case Tok::PipeAssign: return Op::BitOr;
            case Tok::CaretAssign: return Op::BitXor;
            default: fail(line, "unsupported assignment operator");
        }
    }

    /// Compile a member/call spine, collecting the short-circuit exits of an
    /// optional chain.
    void chain(const Node& n, std::vector<std::size_t>& exits)
    {
        checkNativeStack();
        if (n.kind == NK::Member) {
            if (n.a->kind == NK::SuperExpr) {
                if (earlyErrors()) {
                    const FnState* owner = fn_;
                    while (owner->isArrow && owner->parent)
                        owner = owner->parent;
                    if (!owner->isMethod)
                        fail(n.line, "'super' is only valid in a method");
                }
                if (n.computed) {
                    expr(*n.b);
                    emit(Op::GetSuperElem);
                } else {
                    emitU(Op::GetSuper, atomFor(n.text));
                }
                return;
            }
            spine(*n.a, exits);
            if (n.optional)
                exits.push_back(emitJump(Op::JumpIfNullishKeep));
            if (n.computed) {
                expr(*n.b);
                emitElem(Op::GetElem);
            } else {
                emitU(n.bindMethod ? Op::GetPropBound : Op::GetProp, siteOperand(n.text));
            }
            return;
        }

        // A call.
        const Node& callee = *n.a;
        const bool spread = hasSpread(n.list);

        if (callee.kind == NK::SuperExpr) {
            if (earlyErrors()) {
                const FnState* owner = fn_;
                while (owner->isArrow && owner->parent)
                    owner = owner->parent;
                if (!owner->isDerivedCtor)
                    fail(n.line, "'super()' is only valid in the constructor of a derived class");
            }
            // `this` from here on is what the super constructor made. An arrow function has neither the constructor nor
            // `this` in its own frame.
            if (fn_->isArrow) {
                loadVar(ascii(" ctor"));
                loadVar(ascii(" nt"));
                loadThisUnchecked();
                pushArgs(n.list, spread);
                if (spread)
                    emit(Op::CallSuperViaSpread);
                else
                    emitU(Op::CallSuperVia, std::uint32_t(n.list.size()));
            } else {
                pushArgs(n.list, spread);
                if (spread)
                    emit(Op::CallSuperSpread);
                else
                    emitU(Op::CallSuper, std::uint32_t(n.list.size()));
            }
            if (findLocal(*fn_, ascii("this")) || fn_->isArrow) {
                emit(Op::Dup);
                storeThisBox();
            }
            return;
        }

        bool receiverPushed = false;
        if (callee.kind == NK::Member && callee.a->kind == NK::SuperExpr) {
            chain(callee, exits);
            loadThis();
            receiverPushed = true;
        } else if (callee.kind == NK::Member) {
            spine(*callee.a, exits);
            if (callee.optional)
                exits.push_back(emitJump(Op::JumpIfNullishKeep));
            if (callee.computed) {
                expr(*callee.b);
                emitElem(Op::GetElemDup);
            } else {
                emitU(Op::GetPropDup, siteOperand(callee.text));
            }
            receiverPushed = true;
        } else {
            spine(callee, exits);
        }
        if (n.optional) {
            // Park the receiver so the short-circuit exit leaves exactly one
            // value behind, like every other link in the chain.
            const std::uint32_t slot = temp();
            if (receiverPushed)
                emitU(Op::SetLocal, slot);
            exits.push_back(emitJump(Op::JumpIfNullishKeep));
            if (receiverPushed)
                emitU(Op::GetLocal, slot);
            else
                emit(Op::PushUndefined);
        } else if (!receiverPushed) {
            emit(Op::PushUndefined);
        }
        pushArgs(n.list, spread);
        if (spread)
            emit(Op::CallSpread);
        else if (callee.kind == NK::Identifier && callee.text == u"eval" && !receiverPushed && !n.optional)
            emitU(Op::DirectEval, evalScopeHere(std::uint32_t(n.list.size())));
        else
            emitU(Op::Call, std::uint32_t(n.list.size()));
    }

    void spine(const Node& n, std::vector<std::size_t>& exits)
    {
        checkNativeStack();
        if (n.kind == NK::Member || n.kind == NK::Call)
            chain(n, exits);
        else
            expr(n);
    }

    void pushArgs(const std::vector<NodePtr>& args, bool spread)
    {
        if (!spread) {
            for (const NodePtr& a : args)
                expr(*a);
            return;
        }
        emitU(Op::NewArray, std::uint32_t(args.size()));
        for (const NodePtr& a : args) {
            if (a->kind == NK::Spread) {
                expr(*a->a);
                emit(Op::ArraySpread);
            } else {
                expr(*a);
                emit(Op::ArrayAppend);
            }
        }
    }

    void newExpr(const Node& n)
    {
        expr(*n.a);
        const bool spread = hasSpread(n.list);
        pushArgs(n.list, spread);
        if (spread)
            emit(Op::NewSpread);
        else
            emitU(Op::New, std::uint32_t(n.list.size()));
    }

    // ---- classes ----
    /// A new function, of the given statements, that runs with `this` the object they are about.
    NodePtr initFunction(std::uint32_t line)
    {
        NodePtr fn = makeNode(NK::FunctionExpr, line);
        fn->isMethod = true;
        fn->d = makeNode(NK::Block, line);
        return fn;
    }

    /// What a field does to `this`, or for a private method, what makes it one of `this`'s.
    void fieldInit(const Node& n)
    {
        emit(Op::PushThis);
        if (n.propKind != PropKind::Init) {
            loadVar(n.text);
            loadVar(n.a->text);
            if (n.propKind == PropKind::Method)
                emit(Op::DefPrivateMethod);
            else
                emitU(Op::DefPrivateAccessor, n.propKind == PropKind::Setter ? 1u : 0u);
            emit(Op::Pop);
            return;
        }
        const auto value = [&](StringView name) {
            if (!n.b)
                emit(Op::PushUndefined);
            else if ((n.b->kind == NK::FunctionExpr || n.b->kind == NK::ArrowExpr) && n.b->text.empty())
                function(*n.b, name);
            else
                expr(*n.b);
        };
        if (n.isPrivate) {
            loadVar(n.text);
            value(n.text);
            emit(Op::DefPrivate);
        } else if (n.computed) {
            loadVar(n.a->text);
            value(ascii(""));
            emit(Op::DefElem);
        } else {
            value(n.text);
            emitU(Op::DefField, atomFor(n.text));
        }
        emit(Op::Pop);
    }

    /// One object for each place in the source, whatever the tag does with it: frozen, as is what `raw` gives.
    void templateObject(const Node& n)
    {
        const auto frozen = [&](const std::vector<String>& items) {
            DynArray* array = rt_.newArray(items.size());
            Value handle = detail::Access::adopt(Type::Object, array);
            for (const String& item : items)
                array->append(rt_.str(item));
            return handle;
        };
        Value cooked = frozen(n.strs);
        Value raw = frozen(n.raws);
        asObject(raw)->frozen = true;
        asObject(raw)->extensible = false;
        asObject(cooked)->putOwn(rt_.intern("raw"), raw, 0);
        asObject(cooked)->frozen = true;
        asObject(cooked)->extensible = false;
        fn_->code->consts.push_back(cooked);
        emitU(Op::PushConst, std::uint32_t(fn_->code->consts.size() - 1));
    }

    void storeThisBox()
    {
        const VarRef ref = resolve(ascii("this"));
        switch (ref.kind) {
            case RefKind::Box: emitU(Op::SetCell, ref.index); break;
            case RefKind::Upval: emitU(Op::SetUpval, ref.index); break;
            default: emitU(Op::SetLocal, ref.index); break;
        }
    }

    // ---- what a program may not say ----
    bool earlyErrors() const noexcept { return sloppyThis_; }
    bool inStrictCode() const noexcept { return strictDepth_ > 0; }

    static void patternNames(const Node& p, std::vector<String>& out)
    {
        checkNativeStack();
        switch (p.kind) {
            case NK::Identifier: out.push_back(p.text); return;
            case NK::AssignPattern:
            case NK::RestElement:
                if (p.a)
                    patternNames(*p.a, out);
                return;
            case NK::ArrayPattern:
                for (const NodePtr& e : p.list)
                    patternNames(*e, out);
                return;
            case NK::ObjectPattern:
                for (const NodePtr& e : p.list) {
                    if (e->kind == NK::RestElement)
                        patternNames(*e, out);
                    else if (e->b)
                        patternNames(*e->b, out);
                }
                return;
            default: return;
        }
    }

    void checkBindingName(const String& name, std::uint32_t line)
    {
        if (!earlyErrors() || !inStrictCode())
            return;
        if (name == ascii("eval") || name == ascii("arguments"))
            fail(line, "strict code cannot bind " + toUtf8(name));
        checkStrictWord(name, line);
    }

    void checkStrictWord(const String& name, std::uint32_t line)
    {
        if (!earlyErrors() || !inStrictCode())
            return;
        for (const char* word : {"implements", "interface", "let", "package", "private", "protected", "public", "static", "yield"}) {
            if (name == ascii(word))
                fail(line, toUtf8(name) + " is reserved in strict code");
        }
    }

    /// Names `var` declares in these statements, however deep, short of another function.
    static void varNames(const Node& n, std::vector<String>& out)
    {
        checkNativeStack();
        switch (n.kind) {
            case NK::FunctionExpr:
            case NK::ArrowExpr:
            case NK::ClassExpr:
            case NK::FunctionDecl:
            case NK::ClassDecl: return;
            case NK::VarDecl:
                if (n.varKind == VarKind::Var) {
                    for (const NodePtr& d : n.list) {
                        if (d->a)
                            patternNames(*d->a, out);
                    }
                }
                return;
            default: break;
        }
        forEachChild(n, [&](const Node& kid) { varNames(kid, out); });
    }

    /// A name declared twice in one scope, or both lexically and by a `var` that passes through it. At the top of a function
    /// or program a function declaration counts as a `var`; in a block it is lexical, and only plain ones may repeat in
    /// code that is not strict.
    void checkDeclarations(const std::vector<NodePtr>& statements, bool functionTop, const std::vector<String>* params = nullptr)
    {
        if (!earlyErrors())
            return;
        std::set<String> lexical;
        std::set<String> plainFunctions;
        std::set<String> vars;
        const auto declare = [&](const String& name, std::uint32_t line) {
            if (!lexical.insert(name).second)
                fail(line, "a name is declared twice: " + toUtf8(name));
        };
        for (const NodePtr& s : statements) {
            std::vector<String> names;
            if (s->kind == NK::VarDecl && s->varKind != VarKind::Var) {
                for (const NodePtr& d : s->list) {
                    if (d->a)
                        patternNames(*d->a, names);
                }
                for (const String& name : names)
                    declare(name, s->line);
            } else if (s->kind == NK::ClassDecl) {
                declare(s->text, s->line);
            } else if (s->kind == NK::FunctionDecl) {
                if (functionTop) {
                    vars.insert(s->text);
                } else if (!inStrictCode() && !s->a->isGenerator && !s->a->isAsync) {
                    if (!plainFunctions.insert(s->text).second)
                        continue;
                    if (lexical.count(s->text))
                        fail(s->line, "a name is declared twice: " + toUtf8(s->text));
                    lexical.insert(s->text);
                } else {
                    declare(s->text, s->line);
                }
            } else {
                varNames(*s, names);
                vars.insert(names.begin(), names.end());
            }
        }
        for (const String& name : vars) {
            if (lexical.count(name))
                fail(statements.front()->line, "a name is declared both with 'var' and lexically: " + toUtf8(name));
        }
        if (params) {
            for (const String& name : *params) {
                if (lexical.count(name))
                    fail(statements.front()->line, "a parameter is declared again: " + toUtf8(name));
            }
        }
    }

    void checkClassMembers(const Node& n)
    {
        bool hasConstructor = false;
        for (const NodePtr& m : n.list) {
            if ((m->kind != NK::MethodDef && m->kind != NK::FieldDef) || m->computed || m->isPrivate || !m->a)
                continue;
            const bool isCtorName = m->a->text == ascii("constructor");
            if (m->kind == NK::FieldDef) {
                if (isCtorName || (m->isStatic && m->a->text == ascii("prototype")))
                    fail(m->line, "a field cannot be called " + toUtf8(m->a->text));
                continue;
            }
            if (m->isStatic && m->a->text == ascii("prototype"))
                fail(m->line, "a static method cannot be called prototype");
            if (m->propKind == PropKind::Constructor) {
                if (hasConstructor)
                    fail(m->line, "a class has one constructor");
                hasConstructor = true;
                if (m->b->isGenerator || m->b->isAsync)
                    fail(m->line, "a constructor cannot be a generator or async");
            } else if (isCtorName && !m->isStatic) {
                fail(m->line, "a constructor cannot be an accessor, a generator or async");
            }
        }
    }

    void classExpr(const Node& n)
    {
        // A name given by what it is assigned to, for this class and no class inside it.
        const String className = classNameHint_;
        classNameHint_.clear();
        const StrictScope strictBody(*this, true);
        const bool derived = n.a != nullptr;
        if (earlyErrors())
            checkClassMembers(n);
        beginScope();

        // The private names, each a variable of the class's scope that its methods capture as they do any other.
        std::map<String, int> privateKinds;  // 1 getter, 2 setter, 4 anything else, 8 static
        for (const NodePtr& m : n.list) {
            if ((m->kind != NK::MethodDef && m->kind != NK::FieldDef) || !m->isPrivate)
                continue;
            const String& name = m->a->text;
            if (name == ascii("#constructor"))
                fail(m->line, "a private name cannot be #constructor");
            const int kind = m->kind == NK::MethodDef && m->propKind == PropKind::Getter ? 1
                             : m->kind == NK::MethodDef && m->propKind == PropKind::Setter ? 2 : 4;
            const auto seen = privateKinds.find(name);
            if (seen != privateKinds.end() && (kind == 4 || (seen->second & 7 & kind) || (seen->second & 4) || bool(seen->second & 8) != m->isStatic))
                fail(m->line, "a private name is declared twice: " + toUtf8(name));
            privateKinds[name] |= kind | (m->isStatic ? 8 : 0);
        }
        privateScopes_.push_back({});
        for (const auto& entry : privateKinds) {
            privateScopes_.back().insert(entry.first);
            fn_->captured.insert(entry.first);
            const std::uint32_t slot = declareLocal(entry.first, true);
            emitU(Op::NewPrivateName, atomFor(entry.first));
            emitU(fn_->locals.back().boxed ? Op::SetCell : Op::SetLocal, slot);
        }

        std::int64_t innerName = -1;
        bool innerBoxed = false;
        if (!n.text.empty()) {
            innerName = declareLocal(n.text, true);
            innerBoxed = fn_->locals.back().boxed;
        }

        if (derived)
            expr(*n.a);

        const Node* ctorNode = nullptr;
        for (const NodePtr& m : n.list) {
            if (m->kind == NK::MethodDef && m->propKind == PropKind::Constructor)
                ctorNode = m->b.get();
        }
        NodePtr synthesized;
        Node* ctor = const_cast<Node*>(ctorNode);
        if (!ctor) {
            synthesized = makeNode(NK::FunctionExpr, n.line);
            synthesized->d = makeNode(NK::Block, n.line);
            if (derived) {
                NodePtr rest = makeNode(NK::RestElement, n.line);
                NodePtr id = makeNode(NK::Identifier, n.line);
                id->text = ascii(" args");
                rest->a = std::move(id);
                synthesized->list.push_back(std::move(rest));

                NodePtr call = makeNode(NK::Call, n.line);
                call->a = makeNode(NK::SuperExpr, n.line);
                NodePtr sp = makeNode(NK::Spread, n.line);
                NodePtr ref = makeNode(NK::Identifier, n.line);
                ref->text = ascii(" args");
                sp->a = std::move(ref);
                call->list.push_back(std::move(sp));
                NodePtr es = makeNode(NK::ExprStatement, n.line);
                es->a = std::move(call);
                synthesized->d->list.push_back(std::move(es));
            }
            ctor = synthesized.get();
        }
        ctor->propKind = PropKind::Constructor;
        ctor->isStatic = derived && !n.allocatesThis;  // read back as isDerivedConstructor
        function(*ctor, n.text.empty() ? className : n.text);
        ctor->isStatic = false;
        ctor->propKind = PropKind::Init;

        emitU(Op::ClassSetup, derived ? (n.allocatesThis ? 3u : 1u) : 0u);

        // Methods go on as they are met, and a computed field name is worked out then, once, for every instance to come.
        NodePtr instanceInit = initFunction(n.line);
        std::vector<NodePtr> statics;
        std::uint32_t hidden = 0;
        const auto hiddenVar = [&](const String& name) {
            fn_->captured.insert(name);
            declareLocal(name, false);
            return &fn_->locals.back();
        };
        for (const NodePtr& m : n.list) {
            if (m->kind == NK::StaticBlock) {
                NodePtr fn = initFunction(m->line);
                for (const NodePtr& s : m->d->list)
                    fn->d->list.push_back(cloneExpression(*s));
                statics.push_back(std::move(fn));
                continue;
            }
            if (m->kind == NK::FieldDef) {
                if (m->declared)
                    continue;
                if (earlyErrors() && m->b && mentionsArguments(*m->b))
                    fail(m->line, "a field's initialiser cannot use 'arguments'");
                NodePtr init = makeNode(NK::FieldInit, m->line);
                init->isPrivate = m->isPrivate;
                init->computed = m->computed;
                if (m->b)
                    init->b = cloneExpression(*m->b);
                if (m->computed) {
                    const String name = ascii(" fk") + numberToString(double(hidden++));
                    expr(*m->a);
                    emit(Op::ToPropertyKey);
                    const Local* local = hiddenVar(name);
                    emitU(local->boxed ? Op::SetCell : Op::SetLocal, local->slot);
                    init->a = makeNode(NK::Identifier, m->line);
                    init->a->text = name;
                } else {
                    init->text = m->a->text;
                }
                if (m->isStatic) {
                    NodePtr fn = initFunction(m->line);
                    fn->d->list.push_back(std::move(init));
                    statics.push_back(std::move(fn));
                } else {
                    instanceInit->d->list.push_back(std::move(init));
                }
                continue;
            }
            if (m->kind != NK::MethodDef || m->propKind == PropKind::Constructor || m->declared)
                continue;

            emit(Op::Dup);
            if (!m->isStatic)
                emitU(Op::GetProp, siteOperand(ascii("prototype")));
            const bool getter = m->propKind == PropKind::Getter;
            const bool setter = m->propKind == PropKind::Setter;
            if (m->isPrivate) {
                // A private method belongs to the instance, and a static one to the class, from the start of each.
                const String& name = m->a->text;
                function(*m->b, getter ? ascii("get ") + name : setter ? ascii("set ") + name : name);
                emit(Op::SetHome);
                const String holder = name + (getter ? ascii(" get") : setter ? ascii(" set") : ascii(" method"));
                const Local* local = hiddenVar(holder);
                emitU(local->boxed ? Op::SetCell : Op::SetLocal, local->slot);
                NodePtr init = makeNode(NK::FieldInit, m->line);
                init->propKind = getter ? PropKind::Getter : setter ? PropKind::Setter : PropKind::Method;
                init->text = name;
                init->a = makeNode(NK::Identifier, m->line);
                init->a->text = holder;
                if (m->isStatic) {
                    // Ahead of the static fields: run where the class is finished.
                    NodePtr fn = initFunction(m->line);
                    fn->d->list.push_back(std::move(init));
                    statics.insert(statics.begin(), std::move(fn));
                } else {
                    instanceInit->d->list.insert(instanceInit->d->list.begin(), std::move(init));
                }
                continue;
            }
            if (m->computed) {
                expr(*m->a);
                emit(Op::ToPropertyKey);
                function(*m->b, ascii(""));
                if (getter || setter)
                    emitU(Op::DefAccessorElem, (setter ? 1u : 0u) | 2u);
                else
                    emit(Op::DefMethodElem);
            } else if (getter || setter) {
                expr(*m->a);
                function(*m->b, (getter ? ascii("get ") : ascii("set ")) + m->a->text);
                emitU(Op::DefAccessorElem, (setter ? 1u : 0u) | 2u);
            } else {
                function(*m->b, m->a->text);
                emitU(Op::DefMethod, atomFor(m->a->text));
            }
            emit(Op::Pop);
        }

        if (!instanceInit->d->list.empty()) {
            function(*instanceInit, ascii(""));
            emit(Op::SetFieldInit);
        }
        if (innerName >= 0) {
            emit(Op::Dup);
            emitU(innerBoxed ? Op::SetCell : Op::SetLocal, std::uint32_t(innerName));
        }
        for (const NodePtr& fn : statics) {
            function(*fn, ascii(""));
            emit(Op::CallStaticInit);
        }
        privateScopes_.pop_back();
        endScope();
    }

    /// Field initialisers are compiled into the constructor, so the nodes have
    /// to be copied out of the class body.
    NodePtr cloneExpression(const Node& n)
    {
        checkNativeStack();
        NodePtr copy = makeNode(n.kind, n.line);
        copy->op = n.op;
        copy->varKind = n.varKind;
        copy->propKind = n.propKind;
        copy->computed = n.computed;
        copy->isStatic = n.isStatic;
        copy->prefix = n.prefix;
        copy->optional = n.optional;
        copy->namedExpr = n.namedExpr;
        copy->isGenerator = n.isGenerator;
        copy->isAsync = n.isAsync;
        copy->isPrivate = n.isPrivate;
        copy->isMethod = n.isMethod;
        copy->raws = n.raws;
        copy->declared = n.declared;
        copy->text = n.text;
        copy->number = n.number;
        copy->strs = n.strs;
        if (n.a)
            copy->a = cloneExpression(*n.a);
        if (n.b)
            copy->b = cloneExpression(*n.b);
        if (n.c)
            copy->c = cloneExpression(*n.c);
        if (n.d)
            copy->d = cloneExpression(*n.d);
        for (const NodePtr& kid : n.list)
            copy->list.push_back(kid ? cloneExpression(*kid) : nullptr);
        return copy;
    }

    Runtime& rt_;
    std::shared_ptr<const std::string> sourceName_;
    String classNameHint_;
    FnState* fn_ = nullptr;
    std::vector<std::set<String>> privateScopes_;
    bool sloppyThis_ = false;
    int strictDepth_ = 0;  ///< how many enclosing scopes are strict: a directive, or a class body
};

}  // namespace

std::shared_ptr<FunctionCode> compileProgram(Runtime& rt, const Node& program,
                                             const std::string& sourceName, bool sloppyThis)
{
    Compiler compiler(rt, sourceName);
    compiler.setSloppyThis(sloppyThis);
    return compiler.program(program);
}

std::shared_ptr<FunctionCode> compileEval(Runtime& rt, const Node& program, const std::string& sourceName,
                                          const EvalScope& scope)
{
    Compiler compiler(rt, sourceName);
    compiler.setSloppyThis(true);
    return compiler.evalCode(program, scope);
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
