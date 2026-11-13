// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// AST to register bytecode (ls-jsc.md, C-3). Expressions are compiled towards a destination register the caller chose,
// so `x = a + b` for a local `x` is one `Add`. A variable a nested function reads or writes lives in a `Box`, held in its
// register; every other local is just its register. Top-level declarations are properties of the global object.
//
// Not compiled yet, and refused with `NotSupported`: classes, generators, async functions, `for await`, spread, rest
// elements, tagged templates, regular expression and BigInt literals, `with`, `arguments`, `super`, `new.target`, private
// names, and getters and setters other than in object literals.
#include "script/internal/vm/compiler.hpp"

#include <algorithm>
#include <map>
#include <set>

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

struct Binding
{
    String name;
    std::uint16_t reg = 0;
    bool boxed = false;
    BKind kind = BKind::Var;
    bool tdz = false;          ///< `LoadUninit` at scope entry: a read before the declaration needs a check
    bool initialized = false;  ///< the declaration has run, on the path being compiled
};

struct Scope
{
    std::vector<Binding> bindings;
    std::uint16_t baseTop = 0;
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
    const Node* finallyBody = nullptr;  ///< null for the catch part: nothing to run on the way out
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
    bool strict = false;
    std::uint16_t resultReg = 0;
    std::vector<JumpCtx> jumps;
    std::vector<TryCtx> tries;
    std::map<String, std::uint16_t> upvalIndex;
    std::map<Key, std::uint16_t> atomIndex;
    std::map<std::uint64_t, std::uint16_t> constIndex;
};

bool isFunctionNode(const Node& n) noexcept
{
    return n.kind == NK::FunctionExpr || n.kind == NK::ArrowExpr;
}

/// The names a function's nested functions mention, and whether an arrow inside it reads `this`: those variables
/// live in boxes. A name used by any nested function counts, whichever scope declares it, which over-approximates.
void collectCaptured(const Node& n, std::set<String>& out, bool nested, bool arrowChain)
{
    if (n.kind == NK::Identifier && nested)
        out.insert(n.text);
    if (n.kind == NK::ThisExpr && nested && arrowChain)
        out.insert(String(u"this"));
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
            collectCaptured(**child, out, childNested, childArrow);
    for (const dyn::NodePtr& child : n.list)
        collectCaptured(*child, out, childNested, childArrow);
}

/// The names `var` declares in a statement list, looking through blocks but not into functions.
void collectVars(const Node& n, std::vector<String>& out);

void collectPatternNames(const Node& n, std::vector<String>& out)
{
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
            if (n.a && n.a->kind == NK::VarDecl)
                collectVars(*n.a, out);
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

bool mentions(const Node& n, const String& name)
{
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

/// A `let`, `const` or `class` binding needs a hole at scope entry when something can read it before it is initialised:
/// text before its declaration, its own initialiser, or a function declaration, which is made at scope entry.
bool needsTdz(const std::vector<dyn::NodePtr>& statements, std::size_t declIndex, const String& name)
{
    for (std::size_t i = 0; i <= declIndex; ++i)
        if (mentions(*statements[i], name))
            return true;
    for (std::size_t i = declIndex + 1; i < statements.size(); ++i)
        if (statements[i]->kind == NK::FunctionDecl && mentions(*statements[i], name))
            return true;
    return false;
}

/// Whether evaluating a node can assign to a variable of this function.
bool hasAssignment(const Node& n)
{
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

/// Whether `arguments` is read in a function's body or in an arrow inside it (not in another function, which has its own).
bool usesArguments(const Node& n)
{
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
    Compiler(VM& vm, std::string_view file, std::shared_ptr<const std::string> source)
        : vm_(vm), ctx_(vm.ctx), file_(fromUtf8(file)), source_(std::move(source))
    {
    }

    CodeBlock* program(const Node& program);

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
    bool discard_ = false;  ///< the expression about to be compiled has its value thrown away

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

    std::uint16_t atom(const String& name)
    {
        Key key = vm_.internKey(name);  // a count the block keeps
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
    std::uint16_t constant(JSValue owned)
    {
        auto it = fn_->constIndex.find(owned.bits);
        if (it != fn_->constIndex.end()) {
            release(owned);
            return it->second;
        }
        const std::size_t index = fn_->block->consts.size();
        if (index > 0xFFFF)
            throw Error(ErrorCode::NotSupported, "vm core: too many constants in one function");
        fn_->block->consts.push_back(owned);
        fn_->constIndex[owned.bits] = std::uint16_t(index);
        return std::uint16_t(index);
    }

    void loadNumber(std::uint16_t dst, double d)
    {
        if (d == double(std::int16_t(d)) && !(d == 0 && std::signbit(d)))
            emit(Op::LoadInt, dst, std::uint16_t(std::int16_t(d)));
        else
            emit(Op::LoadK, dst, constant(JSValue::boxNumber(d)));
    }
    void loadString(std::uint16_t dst, const String& text)
    {
        emit(Op::LoadK, dst, constant(fromValue(vm_, ctx_.str(text))));
    }

    // ---------------------------------------------------------------- scopes ---
    Binding* findInScopes(Fn& fn, const String& name)
    {
        for (std::size_t i = fn.scopes.size(); i-- > 0;)
            for (Binding& b : fn.scopes[i].bindings)
                if (b.name == name)
                    return &b;
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

    Binding& declare(const String& name, BKind kind)
    {
        Scope& scope = fn_->scopes.back();
        for (Binding& b : scope.bindings)
            if (b.name == name) {
                // A second `var` or function of the same name is the first one.
                return b;
            }
        Binding b;
        b.name = name;
        b.kind = kind;
        b.reg = temp();
        b.boxed = fn_->captured.count(name) != 0;
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
        r.checked = outer.checked;
        return r;
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
                emitCached(forTypeof ? Op::GetGlobalOrUndef : Op::GetGlobal, d, atom(name), 0);
            }
        }
        (void)at;
        return d;
    }

    /// Store `src` in the variable `name`; `init` for the statement that declares it.
    void storeVar(const String& name, std::uint16_t src, bool init, const Node& at)
    {
        Ref r = resolve(name);
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
            if (init)
                emit(Op::DefGlobal, atom(name), src);
            else
                emitCached(Op::PutGlobal, atom(name), src, 0);
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
    void exprVia(const Node& n, int dst, std::uint16_t result);

    std::uint16_t member(const Node& n, int dst);
    std::uint16_t call(const Node& n, int dst, bool isNew);
    std::uint16_t chainRoot(const Node& n, int dst, bool isNew);
    std::uint16_t unary(const Node& n, int dst);
    std::uint16_t update(const Node& n, int dst, bool discard);
    std::uint16_t binary(const Node& n, int dst);
    std::uint16_t logical(const Node& n, int dst);
    std::uint16_t assign(const Node& n, int dst);
    std::uint16_t conditional(const Node& n, int dst);
    std::uint16_t objectLit(const Node& n, int dst);
    std::uint16_t arrayLit(const Node& n, int dst);
    std::uint16_t templateLit(const Node& n, int dst);
    std::uint16_t function(const Node& n, const String& inferredName, int dst);
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
    void closeTry(TryCtx& ctx, std::uint32_t end, std::uint32_t target, std::uint16_t reg);

    std::vector<String> pendingLabels_;

    CodeBlock* compileFunction(const Node& n, const String& name);
    CodeBlock* compileFunctionOnce(const Node& n, const String& name, std::uint32_t extraFlags);
    std::uint32_t pendingFlags_ = 0;  ///< flags for the next function compiled: a class constructor's
    std::uint16_t classExpr(const Node& cls, int dst, const String& inferredName, bool innerBinding = true);
    CodeBlock* compileInitFunction(const std::vector<const Node*>& items, bool isStatic);
    CodeBlock* compileInitOnce(const std::vector<const Node*>& items, bool isStatic);
    void storeThis(std::uint16_t src);
    CodeBlock* programOnce(const Node& program);
    void functionBody(const Node& n, CodeBlock* block);
};


// ======================================================================= expressions ===

void Compiler::exprVia(const Node& n, int dst, std::uint16_t result)
{
    (void)n;
    if (dst >= 0 && dst != result)
        emit(Op::Mov, std::uint16_t(dst), result);
}

std::uint16_t Compiler::expr(const Node& n, int dst)
{
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
            const std::uint16_t d = dest(dst);
            loadNumber(d, n.number);
            return d;
        }
        case NK::StringLit: {
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
        case NK::Identifier: return loadVar(n.text, dst, n);
        case NK::ThisExpr: {
            if (fn_->isArrow || findInScopes(*fn_, String(u"this")))
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
        case NK::Assign: return assign(n, dst);
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
        case NK::SuperExpr: unsupported(n, "super");
        case NK::Spread: unsupported(n, "spread");
        case NK::Yield: {
            if (!(fn_->block->flags & CodeBlock::kGenerator))
                unsupported(n, "yield here");
            const std::uint16_t mark = fn_->top;
            const std::uint16_t d = dest(dst);
            const std::uint16_t inner = fn_->top;
            if (n.prefix) {
                // `yield*` hands out what the inner iterator does; what it sends back and its return value are not passed on.
                const std::uint16_t src = exprAny(*n.a);
                const std::uint16_t it = temps(2);
                const std::uint16_t v = temp();
                const std::uint16_t sent = temp();
                Label top, done;
                emit(Op::IterInit, it, src);
                bind(top);
                emit(Op::IterNext, v, it, 0);
                emitJump(Op::JHole, v, done);
                emit(Op::Yield, sent, v);
                jump(top);
                bind(done);
                emit(Op::LoadUndef, d);
            } else {
                std::uint16_t v;
                if (n.a) {
                    v = exprAny(*n.a);
                } else {
                    v = temp();
                    emit(Op::LoadUndef, v);
                }
                emit(Op::Yield, d, v);
            }
            fn_->top = inner;
            (void)mark;
            return d;
        }
        case NK::Await: unsupported(n, "async functions");
        case NK::ClassExpr: return classExpr(n, dst, String());
        case NK::BigIntLit: unsupported(n, "BigInt");
        case NK::RegExpLit: {
            const std::uint16_t d = dest(dst);
            const std::uint16_t pattern = constant(fromValue(vm_, ctx_.str(n.text)));
            const std::uint16_t flags = constant(fromValue(vm_, ctx_.str(n.extra().strs[0])));
            emit(Op::NewRegExp, d, pattern, flags);
            return d;
        }
        case NK::NewTarget: {
            if (fn_->isArrow || fn_->isProgram)
                unsupported(n, "new.target here");
            const std::uint16_t d = dest(dst);
            emit(Op::NewTarget, d);
            return d;
        }
        case NK::TemplateObject: unsupported(n, "tagged templates");
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
            if (p->propKind == PropKind::Getter || p->propKind == PropKind::Setter) {
                const std::uint16_t f = temp();
                function(*p->b, String(), f);
                emit(p->propKind == PropKind::Getter ? Op::InitGetterElem : Op::InitSetterElem, t, k, f);
            } else {
                const std::uint16_t v = temp();
                if (isAnonymousFunction(*p->b))
                    function(*p->b, String(), v);
                else
                    expr(*p->b, v);
                emit(Op::InitElem, t, k, v);
            }
        } else {
            const String name = p->text;
            if (p->propKind == PropKind::Getter || p->propKind == PropKind::Setter) {
                const std::uint16_t f = temp();
                function(*p->b, name, f);
                emit(p->propKind == PropKind::Getter ? Op::InitGetter : Op::InitSetter, t, atom(name), f);
            } else {
                const std::uint16_t v = temp();
                exprNamed(*p->b, v, name);
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

std::uint16_t Compiler::function(const Node& n, const String& inferredName, int dst)
{
    if (n.isAsync)
        unsupported(n, "async functions");
    CodeBlock* child = compileFunction(n, n.text.empty() ? inferredName : n.text);
    const std::size_t index = fn_->block->children.size();
    fn_->block->children.push_back(child);
    const std::uint16_t d = dest(dst);
    emit(Op::Closure, d, std::uint32_t(index));
    return d;
}

void Compiler::storeThis(std::uint16_t src)
{
    if (src != 0)
        emit(Op::Mov, 0, src);
    if (Binding* b = findInScopes(*fn_, String(u"this"))) {
        if (b->boxed)
            emit(Op::PutBox, b->reg, 0);
        else
            emit(Op::Mov, b->reg, 0);
    }
}

std::uint16_t Compiler::classExpr(const Node& cls, int dst, const String& inferredName, bool innerBinding)
{
    const String name = cls.text.empty() ? inferredName : cls.text;
    const bool derived = cls.a != nullptr;
    const Node* ctor = nullptr;
    for (const dyn::NodePtr& m : cls.list)
        if (m->kind == NK::MethodDef && m->propKind == PropKind::Constructor && !m->declared)
            ctor = m.get();
    for (const dyn::NodePtr& m : cls.list) {
        if (m->isPrivate)
            unsupported(*m, "private class members");
        if (m->computed && m->kind == NK::FieldDef && !m->declared)
            unsupported(*m, "computed field names");
        if ((m->kind == NK::MethodDef || m->kind == NK::FieldDef) && !m->computed && m->a && m->a->kind != NK::Identifier &&
            m->a->kind != NK::StringLit && m->a->kind != NK::NumberLit)
            unsupported(*m, "this class member name");
    }

    const std::uint16_t mark = fn_->top;
    const std::uint16_t d = dest(dst);
    const std::uint16_t inner = fn_->top;
    const bool scoped = innerBinding && !cls.text.empty();
    if (scoped) {
        pushScope();
        Binding& b = declare(cls.text, BKind::Const);
        b.initialized = true;
        emit(Op::LoadUndef, b.reg);
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
            call->list.push_back(std::move(spread));
            dyn::NodePtr stmt = std::make_unique<Node>(NK::ExprStatement, cls.line);
            stmt->a = std::move(call);
            made->d->list.push_back(std::move(stmt));
        }
        ctorFn = made.get();
    }
    if (ctorFn->isAsync || ctorFn->isGenerator)
        unsupported(*ctorFn, "this constructor");
    pendingFlags_ = CodeBlock::kClassConstructor | (derived ? CodeBlock::kDerived : 0);
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
            function(*m->b, m->a->text, t);
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
        emit(Op::Mov, base + 1, f);
        emit(Op::Call, base, base, 0);
        fn_->top = at;
    }
    emit(Op::Mov, d, f);
    if (scoped)
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
    child.parent = fn_;
    child.block = block;
    child.strict = true;
    for (const Node* item : items) {
        if (item->kind == NK::FieldDef) {
            if (item->b)
                collectCaptured(*item->b, child.captured, false, true);
        } else {
            collectCaptured(*item->d, child.captured, false, true);
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
        if (fn_->captured.count(String(u"this"))) {
            Binding& b = declare(String(u"this"), BKind::Var);
            emit(Op::Mov, b.reg, 0);
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
        for (const Node* item : items) {
            line_ = item->line;
            const std::uint16_t mark = fn_->top;
            if (item->kind == NK::StaticBlock) {
                statement(*item->d);
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

bool smallInt(const Node& n) noexcept
{
    return n.kind == NK::NumberLit && n.number == double(std::int16_t(n.number)) && !(n.number == 0 && std::signbit(n.number));
}

}  // namespace

std::uint16_t Compiler::binary(const Node& n, int dst)
{
    if (n.op == Tok::In && n.a->isPrivate)
        unsupported(n, "private names");
    bool ok;
    const Op op = binaryOp(n.op, &ok);
    if (!ok)
        unsupported(n, "this binary operator");
    const std::uint16_t mark = fn_->top;
    if ((op == Op::Add || op == Op::Sub) && smallInt(*n.b)) {
        const std::uint16_t a = exprAny(*n.a);
        fn_->top = mark;
        const std::uint16_t d = dest(dst);
        const int imm = int(n.b->number);
        emit(op == Op::Add ? Op::AddI : Op::SubI, d, a, std::uint16_t(std::int16_t(imm)));
        return d;
    }
    const std::uint16_t a = isPure(*n.b) ? exprAny(*n.a) : exprTemp(*n.a);
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
    if (cond.kind == NK::Binary && !noFuse_) {
        Op fused = Op::Nop;
        switch (cond.op) {
            case Tok::Lt: fused = Op::JLt; break;
            case Tok::Le: fused = Op::JLe; break;
            case Tok::Gt: fused = Op::JGt; break;
            case Tok::Ge: fused = Op::JGe; break;
            case Tok::EqEqEq: fused = Op::JStrictEq; break;
            case Tok::NotEqEq: fused = Op::JStrictNe; break;
            default: break;
        }
        if (fused != Op::Nop) {
            const std::uint16_t mark = fn_->top;
            const std::uint16_t a = isPure(*cond.b) ? exprAny(*cond.a) : exprTemp(*cond.a);
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
    if (cond.kind == NK::Logical && cond.op == Tok::AndAnd) {
        condJump(*cond.a, whenFalse);
        condJump(*cond.b, whenFalse);
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
            default: break;
        }
        if (fused != Op::Nop) {
            const std::uint16_t mark = fn_->top;
            const std::uint16_t a = isPure(*cond.b) ? exprAny(*cond.a) : exprTemp(*cond.a);
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
        if (r.kind == Ref::Local && !r.binding->boxed && !r.checked && !r.isConst) {
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
        const std::uint16_t o = exprTemp(*target.a);
        std::uint16_t k = 0;
        if (target.computed)
            k = exprTemp(*target.b);
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

std::uint16_t Compiler::assign(const Node& n, int dst)
{
    const std::uint16_t mark = fn_->top;
    const Node& target = *n.a;
    bool isCompound;
    const Op op = compoundOp(n.op, &isCompound);
    const bool isLogical = n.op == Tok::AndAssign || n.op == Tok::OrAssign || n.op == Tok::NullishAssign;

    auto finish = [&](std::uint16_t value) -> std::uint16_t {
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
            if (r.kind == Ref::Local && !r.binding->boxed && !r.checked && !r.isConst &&
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
            if (r.kind == Ref::Local && !r.binding->boxed && !r.checked && !r.isConst && !hasAssignment(*n.b)) {
                const std::uint16_t reg = r.binding->reg;
                if ((op == Op::Add || op == Op::Sub) && smallInt(*n.b))
                    emit(op == Op::Add ? Op::AddI : Op::SubI, reg, reg, std::uint16_t(std::int16_t(n.b->number)));
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
        const std::uint16_t o = exprTemp(*target.a);
        std::uint16_t k = 0;
        const bool computed = target.computed;
        if (computed)
            k = exprTemp(*target.b);
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
            const std::uint16_t v = temp();
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
    const std::uint16_t t = temp();
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
        if (fn_->isArrow || n.computed)
            unsupported(n, "this use of super");
        const std::uint16_t d = dest(dst);
        emit(Op::GetSuperProp, d, atom(n.text));
        return d;
    }
    const std::uint16_t mark = fn_->top;
    const bool constantKey = n.computed && n.b->kind == NK::StringLit;
    const std::uint16_t o = (n.computed && !constantKey && !isPure(*n.b)) ? exprTemp(*n.a) : exprAny(*n.a);
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
    emitCached(Op::GetProp, d, o, atom(constantKey ? n.b->text : n.text));
    return d;
}

std::uint16_t Compiler::call(const Node& n, int dst, bool isNew)
{
    const std::uint16_t mark = fn_->top;
    const std::uint16_t base = temps(2);
    const Node& callee = *n.a;
    const bool superCall = callee.kind == NK::SuperExpr;
    if (superCall && (isNew || fn_->isArrow || !(fn_->block->flags & CodeBlock::kClassConstructor)))
        unsupported(n, "super() here");
    if (!isNew && callee.kind == NK::Member && callee.a->kind == NK::SuperExpr) {
        if (fn_->isArrow || callee.computed)
            unsupported(n, "this use of super");
        emit(Op::GetSuperProp, base, atom(callee.text));
        emit(Op::Mov, base + 1, 0);
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
        emit(Op::NewArray, arr, 0);
        for (const dyn::NodePtr& arg : n.list) {
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
    emit(spread ? (superCall ? Op::SuperSpread : isNew ? Op::NewSpread : Op::CallSpread)
                : (superCall ? Op::SuperCall : isNew ? Op::New : Op::Call),
         d, base, argc);
    if (superCall) {
        // The constructor's `this` is what the parent made; the fields of this class are initialised now.
        storeThis(d);
        emit(Op::InitFields);
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
            b.tdz = needsTdz(list, i, name);
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
    const bool global = fn_->isProgram && fn_->scopes.size() == 1;
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

void Compiler::blockScoped(const std::vector<dyn::NodePtr>& list, bool newScope)
{
    if (newScope)
        pushScope();
    declareLexical(list);
    hoistFunctions(list);
    statements(list);
    if (newScope)
        popScope();
}

void Compiler::statement(const Node& n)
{
    if (n.line)
        line_ = n.line;
    const std::uint16_t mark = fn_->top;
    switch (n.kind) {
        case NK::Empty:
        case NK::FunctionDecl: break;
        case NK::ExprStatement:
            if (fn_->isProgram) {
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
    }
    fn_->top = mark;
}

void Compiler::ifStatement(const Node& n)
{
    Label elseL, end;
    condJump(*n.a, elseL);
    statement(*n.b);
    if (n.c) {
        jump(end);
        bind(elseL);
        statement(*n.c);
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
            b.tdz = mentions(*n.a, name) && false;
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
    if (n.isAsync)
        unsupported(n, "for await");
    std::vector<String> labels;
    labels.swap(pendingLabels_);
    const std::uint16_t mark = fn_->top;
    const std::uint16_t src = exprTemp(*n.b);
    const std::uint16_t it = temps(2);
    if (isOf) {
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
    emit(Op::IterNext, v, it, 0);
    emitJump(Op::JHole, v, end);
    pushScope();
    const Node& left = *n.a;
    if (left.kind == NK::VarDecl) {
        const Node& target = *left.list[0]->a;
        const bool lexical = left.varKind != VarKind::Var;
        if (lexical) {
            std::vector<String> names;
            collectPatternNames(target, names);
            for (const String& name : names) {
                Binding& b = declare(name, left.varKind == VarKind::Const ? BKind::Const : BKind::Let);
                if (b.boxed)
                    emit(Op::NewBox, b.reg);
            }
        }
        bindTarget(target, v, lexical, left.varKind);
    } else {
        bindTarget(left, v, false, VarKind::None);
    }
    loopBody(*n.d, end, top, std::move(labels));
    popScope();
    jump(top);
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
        if (body) {
            const std::uint32_t start = pc();
            statement(*body);
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
    if (fn_->isProgram)
        syntaxError(n, "Illegal return statement");
    const std::uint16_t mark = fn_->top;
    std::uint16_t r;
    if (n.a) {
        r = exprTemp(*n.a);
    } else {
        r = temp();
        emit(Op::LoadUndef, r);
    }
    emitFinallyFor(0);
    emit(Op::Ret, r);
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
    emitFinallyFor(target->tryDepth);
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
    fn_->jumps.push_back(std::move(ctx));
    for (std::size_t i = 0; i < n.list.size(); ++i) {
        bind(caseLabels[i]);
        statements(n.list[i]->list);
    }
    fn_->jumps.pop_back();
    bind(end);
    popScope();
    fn_->top = mark;
}

void Compiler::closeTry(TryCtx& ctx, std::uint32_t end, std::uint32_t target, std::uint16_t reg)
{
    // The protected range with the finally copies cut out of it.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> holes = ctx.holes;
    std::sort(holes.begin(), holes.end());
    std::uint32_t from = ctx.start;
    for (const auto& h : holes) {
        if (h.first > from)
            fn_->block->handlers.push_back(Handler{from, h.first, target, reg});
        from = std::max(from, h.second);
    }
    if (end > from)
        fn_->block->handlers.push_back(Handler{from, end, target, reg});
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
        closeTry(done, tryEnd, catchStart, catchReg);
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

CodeBlock* Compiler::compileFunctionOnce(const Node& n, const String& name, std::uint32_t extraFlags)
{
    const bool arrow = n.kind == NK::ArrowExpr;
    Key nameKey = name.empty() ? nullptr : vm_.internKey(StringView(name));
    CodeBlock* block = vm_.newCodeBlock(nameKey, 0, !arrow && !n.isGenerator && (!n.isMethod || (extraFlags & CodeBlock::kClassConstructor)));
    if (n.isGenerator)
        block->flags |= CodeBlock::kGenerator;
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
    Fn child;
    child.parent = fn_;
    child.block = block;
    child.isArrow = arrow;
    child.strict = fn_->strict || (n.d && hasUseStrict(*n.d)) || (extraFlags & CodeBlock::kClassConstructor) != 0 || n.isMethod;
    if (child.strict)
        block->flags |= CodeBlock::kStrict;
    if (arrow)
        block->flags |= CodeBlock::kArrow;
    collectCaptured(*n.d, child.captured, false, true);
    for (const dyn::NodePtr& p : n.list)
        collectCaptured(*p, child.captured, false, true);

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
        vm_.releaseCode(block);
        throw;
    }
    fn_ = saved;
    line_ = savedLine;
    chain_ = savedChain;
    pendingLabels_.swap(savedLabels);
    return block;
}

void Compiler::functionBody(const Node& n, CodeBlock* block)
{
    const bool arrow = fn_->isArrow;
    const std::size_t nparams = n.list.size();
    const std::size_t fixed = block->nparams;
    const bool hasRest = fixed < nparams;
    const bool needsArgs = hasRest || (!arrow && usesArguments(*n.d));
    // `this`, the fixed parameters, then (if any) the arguments array.
    fn_->top = std::uint16_t(1 + fixed + (needsArgs ? 1 : 0));
    block->nregs = fn_->top;
    if (needsArgs) {
        block->flags |= CodeBlock::kNeedsArgs;
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
        b.boxed = fn_->captured.count(b.name) != 0;
        scope.bindings.push_back(b);
        if (b.boxed)
            emit(Op::NewBox, b.reg);
    }
    // `this` for the arrows inside.
    if (!arrow && fn_->captured.count(String(u"this"))) {
        Binding& b = declare(String(u"this"), BKind::Var);
        emit(Op::Mov, b.reg, 0);
        if (b.boxed)
            emit(Op::NewBox, b.reg);
    }
    if ((block->flags & CodeBlock::kClassConstructor) && !(block->flags & CodeBlock::kDerived))
        emit(Op::InitFields);
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
            b.boxed = fn_->captured.count(b.name) != 0;
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
    for (std::size_t i = 0; i < fixed; ++i) {
        const Node& p = *n.list[i];
        const std::uint16_t reg = std::uint16_t(i + 1);
        const Node* target = &p;
        if (p.kind == NK::AssignPattern) {
            Label has;
            emitJump(Op::JNotUndefined, reg, has);
            exprNamed(*p.b, reg, p.a->kind == NK::Identifier ? p.a->text : String());
            bind(has);
            target = p.a.get();
        }
        if (target->kind == NK::Identifier) {
            Binding* b = findInScopes(*fn_, target->text);
            if (b && b->reg == reg && b->boxed)
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
        emit(Op::RestSlice, v, std::uint16_t(1 + fixed), std::uint32_t(fixed));
        bindTarget(target, v, true, VarKind::Let);
        fn_->top = mark;
    }

    // `var` names and function declarations of the whole body, then the block's own `let`s.
    std::vector<String> vars;
    collectVars(*n.d, vars);
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
    statements(n.d->list);
    const std::uint16_t r = temp();
    emit(Op::LoadUndef, r);
    emit(Op::Ret, r);
    if (n.kind == NK::FunctionExpr && n.namedExpr && !n.text.empty())
        fn_->scopes.pop_back();
    fn_->scopes.pop_back();
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
    collectCaptured(program, top.captured, false, true);
    fn_ = &top;
    try {
        top.top = 1;
        block->nregs = 1;
        pushScope();
        top.resultReg = temp();
        emit(Op::LoadUndef, top.resultReg);
        // `this` for arrows at the top of a program.
        if (top.captured.count(String(u"this"))) {
            Binding& b = declare(String(u"this"), BKind::Var);
            emit(Op::Mov, b.reg, 0);
            if (b.boxed)
                emit(Op::NewBox, b.reg);
        }
        // Declarations of the top level are properties of the global object.
        std::vector<String> vars;
        collectVars(program, vars);
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

CodeBlock* compileProgram(VM& vm, const dyn::Node& program, std::string_view file, std::shared_ptr<const std::string> source)
{
    Compiler compiler(vm, file, std::move(source));
    return compiler.program(program);
}

}  // namespace vm
}  // namespace internal
}  // namespace script
