// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The checker: name resolution, type checking, layout and constant folding.
//
// One pass over the declarations decides what exists and where it lives -- the
// slot of every field, the vtable index of every virtual method, the size of
// every instance -- and a second pass over the bodies types every expression
// and allocates every local. Nothing is looked up by name after this, which is
// why the emitter and the VM never see a string.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "script/internal/typed/ast.hpp"
#include "script/internal/typed/runtime.hpp"
#include "script/internal/typed/types.hpp"

namespace script {
namespace internal {
namespace typed {

class Checker
{
public:
    Checker(Runtime& runtime, Arena& arena, const CompileOptions& options,
            AssemblyImpl& assembly);

    /// Returns false when `diagnostics` holds at least one error.
    bool check(CompilationUnit* unit, std::vector<Diagnostic>& diagnostics);

private:
    struct Local
    {
        std::uint32_t slot = 0;
        const TypeInfo* type = nullptr;
        bool isConst = false;
        bool boxed = false;
        Value constant;
    };

    /// One method being checked. Lambdas push another of these, linked to the
    /// enclosing one, which is how a capture is discovered.
    struct Function
    {
        MethodInfo* method = nullptr;
        MethodDecl* decl = nullptr;
        Function* parent = nullptr;
        LambdaExpr* lambda = nullptr;
        std::vector<std::unordered_map<String, Local>> scopes;
        std::vector<const TypeInfo*> localTypes;
        std::vector<std::uint8_t> boxed;
        /// For a lambda: the enclosing slot each captured box comes from.
        std::vector<std::uint32_t> captures;
        const TypeInfo* owner = nullptr;
        bool isStatic = true;
        bool capturesThis = false;
        int loopDepth = 0;
    };

    // ---- declaration passes ----
    /// The name of the copy method a record gets, which `with` calls.
    static constexpr const char* kRecordClone = "__clone";
    /// Expand a record or primary constructor into the members it means.
    void synthesizeRecord(TypeDecl* decl);
    void declareTypes(CompilationUnit* unit);
    void resolveBases(CompilationUnit* unit);
    void resolveBaseTypes(TypeDecl* decl);
    void layoutType(TypeDecl* decl);
    void declareMembers(TypeDecl* decl);
    void declareMethod(TypeDecl* decl, MethodDecl* method);
    void buildVTable(TypeInfo* type);
    /// Every interface member has to be there, with the same signature.
    void verifyInterfaces(TypeDecl* decl);
    void checkBodies(CompilationUnit* unit);
    void checkMethodBody(TypeDecl* decl, MethodDecl* method);
    MethodDecl* synthesizeEntryPoint(CompilationUnit* unit);

    // ---- types ----
    const TypeInfo* resolve(TypeRef* ref, bool reportErrors = true);
    const TypeInfo* lookupType(StringView name) const;

    // ---- generics ----
    /// A generic declaration, kept as a template and cloned per instantiation.
    struct GenericType
    {
        TypeDecl* decl = nullptr;
        std::size_t arity = 0;
    };
    /// `Box` with two arguments is a different template from `Box` with one.
    static String templateKey(StringView name, std::size_t arity);
    /// The name an instantiation is registered and diagnosed under.
    String instantiatedName(StringView base, const std::vector<const TypeInfo*>& args) const;
    /// Build (or find) the closed type for `Box<int>`, running every
    /// declaration pass on a fresh copy of the template's tree.
    const TypeInfo* instantiate(const GenericType& generic,
                                const std::vector<const TypeInfo*>& args, std::uint32_t line,
                                std::uint32_t column, bool reportErrors);
    /// Build (or find) the closed method for `Max<int>`.
    MethodInfo* instantiateMethod(MethodDecl* templateDecl,
                                  const std::vector<const TypeInfo*>& args, std::uint32_t line,
                                  std::uint32_t column, bool reportErrors);
    /// Work out the type arguments of a generic method from its arguments.
    bool inferTypeArguments(const MethodDecl* templateDecl, const std::vector<Argument>& args,
                            std::vector<const TypeInfo*>& out);
    /// Match a parameter's written type against an actual type, binding any
    /// type parameter it mentions.
    bool unify(const TypeRef* param, const TypeInfo* actual,
               const std::vector<String>& typeParams,
               std::vector<const TypeInfo*>& bindings) const;
    /// A type parameter currently in scope, or null.
    const TypeInfo* lookupTypeParam(StringView name) const;
    bool checkConstraints(const std::vector<TypeConstraint>& constraints,
                          const std::vector<String>& params,
                          const std::vector<const TypeInfo*>& args, std::uint32_t line,
                          std::uint32_t column, bool reportErrors);
    /// Run the declaration passes over one freshly instantiated declaration.
    void declareInstantiated(TypeDecl* decl);

    // ---- statements ----
    void checkStatement(Stmt* stmt);
    void checkBlock(BlockStmt* block);

    // ---- expressions ----
    const TypeInfo* checkExpr(Expr*& expr);
    const TypeInfo* checkName(NameExpr* expr);
    const TypeInfo* checkMember(MemberAccessExpr* expr);
    const TypeInfo* checkInvocation(InvocationExpr* expr);
    const TypeInfo* checkElementAccess(ElementAccessExpr* expr);
    const TypeInfo* checkObjectCreation(ObjectCreationExpr* expr);
    /// `new(...)` with the type supplied by the context.
    const TypeInfo* checkObjectCreationOf(ObjectCreationExpr* expr, const TypeInfo* type);
    const TypeInfo* checkArrayCreation(ArrayCreationExpr* expr);
    const TypeInfo* checkUnary(UnaryExpr* expr);
    const TypeInfo* checkBinary(BinaryExpr* expr);
    const TypeInfo* checkAssignment(AssignmentExpr* expr);
    const TypeInfo* checkConditional(ConditionalExpr* expr);
    const TypeInfo* checkCast(CastExpr* expr);
    const TypeInfo* checkTypeTest(TypeTestExpr* expr);
    const TypeInfo* checkLambda(LambdaExpr* expr, const TypeInfo* expected);
    const TypeInfo* checkInterpolated(InterpolatedExpr* expr);
    const TypeInfo* checkLiteral(LiteralExpr* expr);

    // ---- patterns ----
    /// Turn a pattern into the boolean expression that tests it, declaring a
    /// local for every binding on the way. Nothing downstream knows patterns
    /// exist: the emitter sees the tests the language could already express.
    Expr* lowerPattern(Pattern* pattern, Expr* subject, const TypeInfo* subjectType);
    Expr* lowerPositionalPattern(Pattern* pattern, std::uint32_t slot, const TypeInfo* type,
                                 const Expr* at);
    Expr* lowerListPattern(Pattern* pattern, std::uint32_t slot, const TypeInfo* type,
                           const Expr* at);
    Expr* makeBool(bool value, const Expr* at);
    Expr* makeInt(std::int32_t value, const Expr* at);
    Expr* makeLocalRead(std::uint32_t slot, const TypeInfo* type, const Expr* at);
    Expr* makeMemberRead(std::uint32_t slot, const TypeInfo* type, const String& name,
                         const Expr* at);
    Expr* makeLengthRead(std::uint32_t slot, const TypeInfo* type, const Expr* at);
    Expr* makeElementRead(std::uint32_t slot, const TypeInfo* type, Expr* index, const Expr* at);
    Expr* makeAnd(Expr* left, Expr* right);
    /// True when a type reference names something that exists as a type.
    bool resolvesToType(TypeRef* ref) const;
    /// Rebuild a dotted type reference as the expression it also reads as.
    Expr* typeRefAsExpr(const TypeRef* ref);
    const TypeInfo* checkSwitchExpr(SwitchExpr* expr);
    const TypeInfo* checkTuple(TupleExpr* expr);
    const TypeInfo* checkWith(WithExpr* expr);
    void checkThrowValue(Expr*& value, const Expr* at);
    void checkDeconstruction(Deconstruction* node, Expr*& value, std::uint32_t line);
    void checkUsing(UsingStmt* stmt);
    /// Turn catch clauses with `when` into one handler holding the chain they
    /// describe, so a failing filter falls through to the next clause.
    void mergeFilteredCatches(TryStmt* stmt);
    /// The length of an indexable target, however it is spelled.
    Expr* lengthOf(Expr* target, const TypeInfo* type, const Expr* at);
    const TypeInfo* checkSlice(ElementAccessExpr* expr, const TypeInfo* target);
    const TypeInfo* checkCollection(CollectionExpr* expr, const TypeInfo* target);
    Expr* makeElementOf(Deconstruction* node, std::size_t index, std::uint32_t line);
    Expr* makeMatchFailure(const Expr* at, const TypeInfo* type);
    Expr* makeDefault(const TypeInfo* type, const Expr* at);

    /// Check `expr` and convert it to `target`, reporting when it will not go.
    Expr* coerce(Expr* expr, const TypeInfo* target, const char* context);
    /// Insert the conversion without checking the expression again.
    Expr* convert(Expr* expr, const TypeInfo* target);
    bool assignable(Expr* expr) const;
    /// Constant arithmetic, so `const` fields, enum members, case labels and
    /// array sizes are known before anything runs.
    static Value foldConversion(const Value& value, const TypeInfo* target);
    static bool constantFits(const Value& value, const TypeInfo* target);
    bool foldBinary(BinaryExpr* expr, const TypeInfo* operandType);
    /// The type a shift produces, which is the left operand's, not a
    /// promotion of both.
    const TypeInfo* shiftResult(const TypeInfo* left) const;
    /// The overload of a method group whose signature a delegate type wants.
    MethodInfo* matchDelegate(const std::vector<MethodInfo*>& group, const TypeInfo* delegateType);

    /// Pick the overload the arguments fit. Returns null and reports when
    /// nothing fits or the choice is ambiguous.
    MethodInfo* pickOverload(const std::vector<MethodInfo*>& candidates,
                             std::vector<Argument>& args, const String& name, std::uint32_t line,
                             std::uint32_t column, bool report = true);
    bool bindArguments(MethodInfo* method, std::vector<Argument>& args, bool apply);
    /// How well an overload fits, for the rare call that two of them accept.
    int overloadScore(MethodInfo* method, const std::vector<Argument>& args);

    // ---- scopes ----
    void pushScope();
    void popScope();
    Local* findLocal(StringView name, Function** owner = nullptr);
    std::uint32_t declareLocal(StringView name, const TypeInfo* type, std::uint32_t line,
                               bool isConst = false);
    std::uint32_t addTemporary(const TypeInfo* type);
    static std::uint32_t addLocal(Function& fn, const TypeInfo* type);
    /// Mark a local of an enclosing function as captured, threading a box
    /// through every lambda in between.
    std::uint32_t capture(Function* from, Function* owner, std::uint32_t slot);

    // ---- diagnostics ----
    void error(std::uint32_t line, std::uint32_t column, std::uint32_t code,
               const std::string& message);
    void errorAt(const Expr* expr, std::uint32_t code, const std::string& message);
    std::string describe(const TypeInfo* type) const { return types_.describe(type); }

    MethodInfo* newMethod(TypeInfo* owner, const String& name, const TypeInfo* returnType,
                          std::uint16_t flags);
    TypeInfo* newType(TypeKind kind, const String& fullName);

    Runtime& rt_;
    Arena& arena_;
    TypeTable& types_;
    CompileOptions options_;
    AssemblyImpl& assembly_;
    std::vector<Diagnostic> diagnostics_;
    std::string sourceName_;
    std::vector<String> usings_;
    std::vector<TypeDecl*> decls_;
    Function* fn_ = nullptr;
    /// Lambda bodies are checked after the method that contains them, because
    /// the delegate type they convert to is only known at the use site.
    std::vector<std::pair<LambdaExpr*, Function*>> pendingLambdas_;
    int lambdaCounter_ = 0;

    /// The generic declarations of this unit, by name and arity.
    std::unordered_map<String, GenericType> generics_;
    /// Generic methods, by the type that declares them.
    std::unordered_map<const TypeInfo*, std::vector<MethodDecl*>> genericMethods_;
    /// Collect the generic methods called `name` visible on `type`.
    void collectGenericMethods(const TypeInfo* type, StringView name,
                               std::vector<MethodDecl*>& out) const;
    /// Resolve a call to a generic method: bind the type arguments, whether
    /// they were written or have to be inferred, and instantiate.
    MethodInfo* resolveGenericCall(const std::vector<MethodDecl*>& candidates,
                                   const std::vector<TypeRef*>& explicitArgs,
                                   std::vector<Argument>& args, const String& name,
                                   std::uint32_t line, std::uint32_t column);
    /// Closed types already built, by instantiated name. An entry is made
    /// before the members are declared, so a type that mentions itself --
    /// `class Node<T> { Node<T> next; }` -- terminates.
    std::unordered_map<String, const TypeInfo*> instantiations_;
    std::unordered_map<String, MethodInfo*> methodInstantiations_;
    /// Type parameters bound by the instantiation being checked. Nested
    /// instantiations push another frame.
    std::vector<std::unordered_map<String, const TypeInfo*>> typeParamScopes_;
    /// Instantiated declarations whose bodies still need checking. Checking
    /// one may instantiate more, so this is drained rather than iterated.
    std::vector<TypeDecl*> pendingInstantiations_;
    /// Every instantiation built, in the order they were built, to be handed
    /// to the emitter once checking is done.
    std::vector<TypeDecl*> instantiatedDecls_;
    /// Local functions in scope, innermost last. A local function is renamed
    /// when it is hoisted onto its type, so the name it was written with only
    /// exists here -- and it has to outlive the enclosing `Function`, because
    /// the function's own body is checked while the block is still open.
    std::vector<std::unordered_map<String, MethodDecl*>> localFunctionScopes_;
    /// Depth guard: `class Rec<T> { Rec<Rec<T>> f; }` has no fixed point.
    int instantiationDepth_ = 0;
};

}  // namespace typed
}  // namespace internal
}  // namespace script
