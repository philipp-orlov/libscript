// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The emitter: typed AST in, bytecode out.
//
// Everything the emitter needs was decided by the checker -- slots, overloads,
// conversions, layout -- so this pass never asks a question it could get wrong.
// It walks the tree once, tracks the evaluation stack depth to size the frame,
// and patches jumps on the way out.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "script/internal/typed/ast.hpp"
#include "script/internal/typed/bytecode.hpp"
#include "script/internal/typed/runtime.hpp"

namespace script {
namespace internal {
namespace typed {

class Emitter
{
public:
    Emitter(Runtime& runtime, AssemblyImpl& assembly, const CompileOptions& options);

    void emitUnit(CompilationUnit* unit);

private:
    struct Loop
    {
        std::vector<std::size_t> breaks;
        std::vector<std::size_t> continues;
        std::uint32_t continueTarget = 0;
        bool hasContinueTarget = false;
    };

    void emitMethod(TypeDecl* decl, MethodDecl* method);
    void emitConstructorPrologue(TypeDecl* decl, MethodDecl* method);
    void emitStaticInitializers(TypeDecl* decl);
    void emitAccessor(MethodInfo* method);

    // ---- statements ----
    void emitStatement(Stmt* stmt);
    void emitBlock(BlockStmt* block);
    void emitIf(IfStmt* stmt);
    void emitWhile(WhileStmt* stmt);
    void emitDoWhile(DoWhileStmt* stmt);
    void emitFor(ForStmt* stmt);
    void emitForEach(ForEachStmt* stmt);
    void emitSwitch(SwitchStmt* stmt);
    void emitUsing(UsingStmt* stmt);
    void emitDeconstruction(Deconstruction* node, Expr* value);
    void emitTry(TryStmt* stmt);
    void emitReturn(ReturnStmt* stmt);

    // ---- expressions ----
    void emitExpr(const Expr* expr);
    /// Evaluate for its side effects, dropping any result.
    void emitDiscard(const Expr* expr);
    void emitConstant(const Value& value, const TypeInfo* type);
    void emitConvert(const ConvertExpr* expr);
    void emitBinary(const BinaryExpr* expr);
    void emitUnary(const UnaryExpr* expr);
    void emitAssignment(const AssignmentExpr* expr, bool keepValue);
    void emitIncDec(const UnaryExpr* expr, bool keepValue);
    void emitInvocation(const InvocationExpr* expr, bool keepValue);
    void emitObjectCreation(const ObjectCreationExpr* expr);
    void emitArrayCreation(const ArrayCreationExpr* expr);
    void emitInterpolated(const InterpolatedExpr* expr);
    void emitLambda(const LambdaExpr* expr);
    void emitLoad(const Expr* expr);
    /// Push the receiver of a member access, if it has one.
    void emitReceiver(const Expr* expr);
    void emitToString(const Expr* expr);
    void emitCall(const MethodInfo* method, std::uint32_t argc, bool isVirtual, bool isInterface);
    void emitStatic(Op code, const FieldInfo* field);
    void emitNullCheckedTail(const Expr* expr, std::size_t& patch);

    // ---- instruction stream ----
    void op(Op code);
    void op(Op code, std::uint32_t a);
    void op(Op code, std::uint32_t a, std::uint32_t b);
    void op(Op code, std::uint32_t a, std::uint32_t b, std::uint32_t c);
    std::size_t jump(Op code);
    void patchTo(std::size_t at, std::uint32_t target);
    void patchHere(std::size_t at);
    std::uint32_t here() const noexcept { return std::uint32_t(code().size()); }
    std::vector<std::uint8_t>& code() noexcept { return method_->body.code; }
    const std::vector<std::uint8_t>& code() const noexcept { return method_->body.code; }
    void adjust(int delta);
    void line(std::uint32_t line);

    std::uint32_t typeRef(const TypeInfo* type);
    std::uint32_t methodRef(const MethodInfo* method);
    std::uint32_t stringRef(StringView text);
    /// A slot the emitter needs and the checker did not know about.
    std::uint32_t scratch(const TypeInfo* type);
    Op arithmeticOp(BinaryOp binary, const TypeInfo* type);
    Op compareOp(BinaryOp binary, const TypeInfo* type);

    Runtime& rt_;
    AssemblyImpl& assembly_;
    CompileOptions options_;
    MethodInfo* method_ = nullptr;
    TypeDecl* typeDecl_ = nullptr;
    MethodDecl* decl_ = nullptr;
    int stack_ = 0;
    int maxStack_ = 0;
    std::uint32_t lastLine_ = 0;
    std::vector<Loop> loops_;
    /// A `return` inside a `try` that has a `finally` cannot just return: the
    /// finally has to run first. Those returns stash the value and leave to a
    /// single epilogue at the end of the method.
    int protectedDepth_ = 0;
    std::uint32_t returnSlot_ = 0;
    bool hasReturnSlot_ = false;
    std::vector<std::size_t> returnJumps_;
};

}  // namespace typed
}  // namespace internal
}  // namespace script
