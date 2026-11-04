// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The syntax tree, and the arena it lives in.
//
// Nodes are bump-allocated and released in one go when the compilation ends,
// so compiling a million programs leaves nothing behind and no individual node
// is ever freed. The checker writes its conclusions back into the same nodes --
// resolved type, resolved symbol, folded constant -- and the emitter reads
// them; there is no second tree.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

#include "mem/object_arena.hpp"
#include "script/internal/common.hpp"
#include "script/internal/typed/lexer.hpp"
#include "script/internal/typed/metadata.hpp"

namespace script {
namespace internal {
namespace typed {

// ---------------------------------------------------------------- arena ---

/// A syntax tree goes away together, with the compilation.
using Arena = mem::ObjectArena;

// ----------------------------------------------------------------- kinds ---

enum class ExprKind : std::uint8_t
{
    Literal,
    Interpolated,
    Name,
    MemberAccess,
    Invocation,
    ElementAccess,
    ObjectCreation,
    ArrayCreation,
    Unary,
    Binary,
    Assignment,
    Conditional,
    Cast,
    TypeTest,   ///< `x is T`, `x is <pattern>` and `x as T`
    Lambda,
    This,
    Base,
    Default,
    TypeOf,
    NameOf,
    /// Inserted by the checker: an implicit conversion the emitter must apply.
    Convert,
    Switch,      ///< `x switch { ... }`
    Tuple,       ///< `(a, b)`
    Throw,       ///< `throw` in expression position
    Discard,     ///< `_`
    Range,       ///< `a..b`
    Index,       ///< `^a`
    Collection,  ///< `[1, 2, ..rest]`
    With,        ///< `record with { ... }`
    /// Inserted by the checker: run each of `before` for its effect, then
    /// yield `value`. Everything that needs a temporary lowers through this.
    Sequence,
};

enum class StmtKind : std::uint8_t
{
    Block,
    LocalDeclaration,
    Expression,
    If,
    While,
    DoWhile,
    For,
    ForEach,
    Switch,
    Break,
    Continue,
    Return,
    Throw,
    Try,
    Empty,
    Yield,          ///< `yield return` / `yield break`
    Using,          ///< `using (r) { }`
    LocalFunction,  ///< a function declared inside a method body
};

enum class UnaryOp : std::uint8_t
{
    Plus,
    Minus,
    Not,
    Complement,
    PreIncrement,
    PreDecrement,
    PostIncrement,
    PostDecrement,
};

enum class BinaryOp : std::uint8_t
{
    Add,
    Subtract,
    Multiply,
    Divide,
    Modulo,
    And,
    Or,
    Xor,
    ShiftLeft,
    ShiftRight,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    LogicalAnd,
    LogicalOr,
    Coalesce,
};

const char* operatorText(BinaryOp op) noexcept;
const char* operatorText(UnaryOp op) noexcept;

// ------------------------------------------------------------ type syntax ---

/// A type as it was written. Resolution to a `TypeInfo` happens in the checker
/// once every declaration in the unit is known, so forward references work.
struct TypeRef
{
    Tok predefined = Tok::Eof;          ///< `int`, `string`, ... or Eof
    std::vector<String> path;           ///< `System.Collections.Generic.List`
    std::vector<TypeRef*> typeArgs;
    std::uint32_t arrayRank = 0;        ///< number of `[]` suffixes
    bool nullable = false;              ///< a trailing `?`
    bool isVar = false;
    bool isTuple = false;               ///< `(int, string)`
    std::vector<String> tupleNames;     ///< one per element, possibly empty
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    /// Filled in by the checker.
    const TypeInfo* resolved = nullptr;
};

/// What a `where T : ...` clause demanded.
struct TypeConstraint
{
    String name;                      ///< the type parameter it constrains
    bool hasClass = false;            ///< `where T : class`
    bool hasStruct = false;           ///< `where T : struct`
    bool hasNew = false;              ///< `where T : new()`
    std::vector<TypeRef*> types;      ///< base class and interfaces
    std::uint32_t line = 0;
    std::uint32_t column = 0;
};

// ----------------------------------------------------------- expressions ---

struct Expr;
struct Stmt;
struct Pattern;

// --------------------------------------------------------------- patterns ---

enum class PatternKind : std::uint8_t
{
    Discard,      ///< `_`
    Constant,     ///< `42`, `"a"`, `null`, an enum member
    Declaration,  ///< `Foo f`, or a bare type test
    Var,          ///< `var x`
    Relational,   ///< `> 10`
    And,
    Or,
    Not,
    Property,     ///< `{ Name: "a", Age: > 18 }`
    Positional,   ///< `(1, var y)`
    List,         ///< `[1, .., var last]`
    Slice,        ///< `..` inside a list pattern
};

struct PropertySubpattern
{
    String name;
    Pattern* pattern = nullptr;
    /// Filled in by the checker.
    const FieldInfo* field = nullptr;
    const PropertyInfo* prop = nullptr;
    MethodInfo* getter = nullptr;
};

/// One node of a pattern. Every C# pattern form is a tree of these, and the
/// checker lowers the tree into tests the emitter already knows how to make.
struct Pattern
{
    PatternKind kind = PatternKind::Discard;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    TypeRef* typeRef = nullptr;        ///< `Foo f`, or a type before `{ }`
    String bindingName;                ///< the `f` in `Foo f`
    Expr* value = nullptr;             ///< a constant or relational operand
    BinaryOp op = BinaryOp::Less;      ///< which relational test
    bool negated = false;
    std::vector<Pattern*> children;    ///< and/or/not operands, list elements
    std::vector<PropertySubpattern> properties;
    /// Filled in by the checker.
    std::uint32_t bindingSlot = 0;
    bool hasSlot = false;
    const TypeInfo* boundType = nullptr;
    const TypeInfo* testType = nullptr;
    MethodInfo* deconstructor = nullptr;
};

struct MethodDecl;
struct LambdaExpr;

/// What a name turned out to mean.
enum class SymbolKind : std::uint8_t
{
    None,
    Local,        ///< a frame slot
    Parameter,
    Field,
    StaticField,
    Property,
    StaticProperty,
    Method,       ///< a method group, resolved by the call that uses it
    Type,
    EnumMember,
    Constant,
};

struct Expr
{
    ExprKind kind;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    /// The static type, decided by the checker.
    const TypeInfo* type = nullptr;
    /// True when the checker folded this whole expression to `constant`.
    bool isConstant = false;
    Value constant;

    explicit Expr(ExprKind k) : kind(k) {}
};

enum class LiteralKind : std::uint8_t
{
    Null,
    Bool,
    Char,
    Int,
    UInt,
    Long,
    ULong,
    Float,
    Double,
    String,
};

struct LiteralExpr : Expr
{
    LiteralKind literal = LiteralKind::Null;
    std::int64_t intValue = 0;
    double realValue = 0;
    String text;

    LiteralExpr() : Expr(ExprKind::Literal) {}
};

struct InterpolatedExpr : Expr
{
    std::vector<String> parts;   ///< one more than `holes`
    std::vector<Expr*> holes;
    std::vector<String> formats;

    InterpolatedExpr() : Expr(ExprKind::Interpolated) {}
};

struct NameExpr : Expr
{
    String name;
    std::vector<TypeRef*> typeArgs;      ///< `Max<int>(...)`
    /// Resolution results.
    SymbolKind symbol = SymbolKind::None;
    std::uint32_t slot = 0;              ///< local, parameter or field slot
    const FieldInfo* field = nullptr;
    const PropertyInfo* prop = nullptr;
    const TypeInfo* namedType = nullptr;
    std::vector<MethodInfo*> group;      ///< candidate methods
    /// Generic methods of the same name, instantiated by the call that uses
    /// them. They have no `MethodInfo` until then.
    std::vector<MethodDecl*> genericGroup;
    /// A local function's synthesized method, when the name is one.
    MethodDecl* localFunction = nullptr;

    NameExpr() : Expr(ExprKind::Name) {}
};

struct MemberAccessExpr : Expr
{
    Expr* target = nullptr;         ///< null when the left side named a type
    const TypeInfo* staticTarget = nullptr;
    String name;
    std::vector<TypeRef*> typeArgs; ///< `x.Map<int>(...)`
    bool nullConditional = false;

    SymbolKind symbol = SymbolKind::None;
    const FieldInfo* field = nullptr;
    const PropertyInfo* prop = nullptr;
    std::vector<MethodInfo*> group;
    std::vector<MethodDecl*> genericGroup;
    /// `array.Length` is the one member with no metadata behind it: the
    /// length lives in the cell header, so it gets its own instruction.
    bool arrayLength = false;

    MemberAccessExpr() : Expr(ExprKind::MemberAccess) {}
};

struct Argument
{
    Expr* value = nullptr;
    String name;                          ///< a named argument, or empty
    ParamMode mode = ParamMode::Value;    ///< `ref` / `out` at the call site
    /// `out var x` and `out int x` declare the variable at the call site.
    TypeRef* outTypeRef = nullptr;
    String outName;
    std::uint32_t outLine = 0;
};

struct InvocationExpr : Expr
{
    Expr* callee = nullptr;
    std::vector<Argument> args;
    /// Resolution results: exactly one of these is set.
    MethodInfo* method = nullptr;
    bool virtualCall = false;
    bool interfaceCall = false;
    bool delegateCall = false;
    Expr* receiver = nullptr;
    bool nullConditional = false;

    InvocationExpr() : Expr(ExprKind::Invocation) {}
};

struct ElementAccessExpr : Expr
{
    Expr* target = nullptr;
    Expr* index = nullptr;
    bool nullConditional = false;
    /// Set when the target is not an array: `List<T>`, `Dictionary<K,V>` and
    /// `string` all index through a method.
    MethodInfo* getter = nullptr;
    MethodInfo* setter = nullptr;
    /// `a[^1]` and `a[1..3]`, lowered by the checker to arithmetic.
    bool fromEnd = false;
    bool isSlice = false;
    Expr* slice = nullptr;

    ElementAccessExpr() : Expr(ExprKind::ElementAccess) {}
};

struct InitializerEntry
{
    String name;     ///< object initializer: `Name = value`
    Expr* key = nullptr;   ///< dictionary initializer: `[key] = value`
    Expr* value = nullptr;
    const FieldInfo* field = nullptr;
    const PropertyInfo* prop = nullptr;
    MethodInfo* setter = nullptr;
};

struct ObjectCreationExpr : Expr
{
    TypeRef* typeRef = nullptr;      ///< null for a target-typed `new(...)`
    std::vector<Argument> args;
    std::vector<InitializerEntry> initializers;
    /// Collection initializer elements: `new List<int> { 1, 2, 3 }`.
    std::vector<Expr*> elements;
    MethodInfo* ctor = nullptr;

    ObjectCreationExpr() : Expr(ExprKind::ObjectCreation) {}
};

struct ArrayCreationExpr : Expr
{
    TypeRef* elementTypeRef = nullptr;
    Expr* size = nullptr;                ///< `new int[n]`
    std::vector<Expr*> elements;         ///< `new int[] { ... }`
    bool hasInitializer = false;

    ArrayCreationExpr() : Expr(ExprKind::ArrayCreation) {}
};

struct UnaryExpr : Expr
{
    UnaryOp op = UnaryOp::Plus;
    Expr* operand = nullptr;

    UnaryExpr() : Expr(ExprKind::Unary) {}
};

struct BinaryExpr : Expr
{
    BinaryOp op = BinaryOp::Add;
    Expr* left = nullptr;
    Expr* right = nullptr;
    /// The type both sides were converted to, which selects the opcode.
    const TypeInfo* operandType = nullptr;

    BinaryExpr() : Expr(ExprKind::Binary) {}
};

struct AssignmentExpr : Expr
{
    /// `Add` for `+=`, and so on; `Coalesce` for `??=`. Plain `=` uses the
    /// sentinel below.
    BinaryOp op = BinaryOp::Add;
    bool compound = false;
    Expr* target = nullptr;
    Expr* value = nullptr;
    const TypeInfo* operandType = nullptr;

    AssignmentExpr() : Expr(ExprKind::Assignment) {}
};

struct ConditionalExpr : Expr
{
    Expr* condition = nullptr;
    Expr* whenTrue = nullptr;
    Expr* whenFalse = nullptr;

    ConditionalExpr() : Expr(ExprKind::Conditional) {}
};

struct CastExpr : Expr
{
    TypeRef* typeRef = nullptr;
    Expr* operand = nullptr;

    CastExpr() : Expr(ExprKind::Cast) {}
};

struct TypeTestExpr : Expr
{
    Expr* operand = nullptr;
    TypeRef* typeRef = nullptr;
    bool isAs = false;         ///< `as` rather than `is`
    String bindingName;        ///< `x is Foo f`
    std::uint32_t bindingSlot = 0;
    /// `x is <pattern>` for everything richer than a type test.
    Pattern* pattern = nullptr;
    /// What the checker turned that pattern into; the emitter reads this
    /// instead of the node when it is set.
    Expr* loweredPattern = nullptr;

    TypeTestExpr() : Expr(ExprKind::TypeTest) {}
};

struct LambdaParam
{
    String name;
    TypeRef* typeRef = nullptr;   ///< null when inferred
    const TypeInfo* type = nullptr;
    Expr* defaultValue = nullptr; ///< `(int x = 1) => ...`
    ParamMode mode = ParamMode::Value;
    std::uint32_t line = 0;
};

struct LambdaExpr : Expr
{
    std::vector<LambdaParam> params;
    Expr* bodyExpr = nullptr;
    Stmt* bodyBlock = nullptr;
    TypeRef* returnTypeRef = nullptr;
    bool isStatic = false;
    /// Filled in by the checker: the synthesized method and what it captures.
    MethodInfo* method = nullptr;
    std::vector<std::uint32_t> capturedSlots;
    bool capturesThis = false;

    LambdaExpr() : Expr(ExprKind::Lambda) {}
};

struct ThisExpr : Expr
{
    ThisExpr() : Expr(ExprKind::This) {}
};

struct BaseExpr : Expr
{
    BaseExpr() : Expr(ExprKind::Base) {}
};

struct DefaultExpr : Expr
{
    TypeRef* typeRef = nullptr;  ///< null for a bare `default`

    DefaultExpr() : Expr(ExprKind::Default) {}
};

struct TypeOfExpr : Expr
{
    TypeRef* typeRef = nullptr;

    TypeOfExpr() : Expr(ExprKind::TypeOf) {}
};

struct NameOfExpr : Expr
{
    String name;

    NameOfExpr() : Expr(ExprKind::NameOf) {}
};

/// An implicit conversion the checker inserted: widening, boxing, a user
/// reference conversion, or a method group turning into a delegate. The kinds
/// themselves live in metadata.hpp, next to the types they relate.
struct ConvertExpr : Expr
{
    Expr* operand = nullptr;
    ConvKind conv = ConvKind::Identity;

    ConvertExpr() : Expr(ExprKind::Convert) {}
};

struct SwitchArm
{
    Pattern* pattern = nullptr;
    Expr* guard = nullptr;      ///< `when ...`
    Expr* value = nullptr;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
};

/// `subject switch { pattern => value, ... }`.
struct SwitchExpr : Expr
{
    Expr* subject = nullptr;
    std::vector<SwitchArm> arms;
    std::uint32_t subjectSlot = 0;
    /// The conditional chain the checker turned the arms into.
    Expr* lowered = nullptr;

    SwitchExpr() : Expr(ExprKind::Switch) {}
};

struct TupleElement
{
    String name;       ///< `(x: 1, y: 2)`, or empty
    Expr* value = nullptr;
};

struct TupleExpr : Expr
{
    std::vector<TupleElement> elements;
    MethodInfo* ctor = nullptr;

    TupleExpr() : Expr(ExprKind::Tuple) {}
};

/// `throw` where a value is expected: `x ?? throw new ...`.
struct ThrowExpr : Expr
{
    Expr* value = nullptr;

    ThrowExpr() : Expr(ExprKind::Throw) {}
};

/// `_` as an assignment target or an argument.
struct DiscardExpr : Expr
{
    DiscardExpr() : Expr(ExprKind::Discard) {}
};

/// `start..end`, with either side optional.
struct RangeExpr : Expr
{
    Expr* start = nullptr;
    Expr* end = nullptr;

    RangeExpr() : Expr(ExprKind::Range) {}
};

/// `^n`, an index counted from the end.
struct IndexExpr : Expr
{
    Expr* operand = nullptr;

    IndexExpr() : Expr(ExprKind::Index) {}
};

struct CollectionElement
{
    Expr* value = nullptr;
    bool spread = false;   ///< `..other`
};

/// `[1, 2, ..rest]`, typed by what it is assigned to.
struct CollectionExpr : Expr
{
    std::vector<CollectionElement> elements;
    /// Filled in by the checker: how to build the target type.
    MethodInfo* ctor = nullptr;
    MethodInfo* adder = nullptr;
    /// What the checker turned this into once the target type was known.
    Expr* lowered = nullptr;

    CollectionExpr() : Expr(ExprKind::Collection) {}
};

/// `source with { Member = value }`, a copy with changes.
struct WithExpr : Expr
{
    Expr* target = nullptr;
    std::vector<InitializerEntry> initializers;
    MethodInfo* cloneMethod = nullptr;

    WithExpr() : Expr(ExprKind::With) {}
};

/// Evaluate each of `before` and discard it, then yield `value`. The checker
/// builds these when a construct needs a temporary: a switch expression has
/// to evaluate its subject once, a deconstruction has to hold the source
/// while it pulls the pieces out.
struct SequenceExpr : Expr
{
    std::vector<Expr*> before;
    Expr* value = nullptr;

    SequenceExpr() : Expr(ExprKind::Sequence) {}
};

// ------------------------------------------------------------ statements ---

struct Stmt
{
    StmtKind kind;
    std::uint32_t line = 0;

    explicit Stmt(StmtKind k) : kind(k) {}
};

struct BlockStmt : Stmt
{
    std::vector<Stmt*> statements;

    BlockStmt() : Stmt(StmtKind::Block) {}
};

struct VarDeclarator
{
    String name;
    Expr* init = nullptr;
    std::uint32_t slot = 0;
    std::uint32_t line = 0;
    const TypeInfo* type = nullptr;
};

struct Deconstruction;

/// One target of `var (a, _, (c, d)) = value`.
struct DeconstructTarget
{
    String name;
    TypeRef* typeRef = nullptr;
    bool discard = false;
    Deconstruction* nested = nullptr;
    std::uint32_t line = 0;
    /// Filled in by the checker.
    std::uint32_t slot = 0;
    const TypeInfo* type = nullptr;
    Expr* nestedValue = nullptr;
};

struct Deconstruction
{
    TypeRef* typeRef = nullptr;   ///< the `var` or explicit type in front
    std::vector<DeconstructTarget> targets;
    /// Filled in by the checker: `Deconstruct` when the source is not a tuple.
    MethodInfo* deconstructor = nullptr;
    std::uint32_t sourceSlot = 0;
    const TypeInfo* sourceType = nullptr;
    /// The Deconstruct call, built by the checker with `out` arguments.
    Expr* call = nullptr;
};

struct LocalDeclStmt : Stmt
{
    TypeRef* typeRef = nullptr;
    std::vector<VarDeclarator> vars;
    bool isConst = false;
    bool isUsing = false;          ///< `using var f = ...;`
    /// `var (a, b) = pair;` instead of the declarators above.
    Deconstruction* deconstruction = nullptr;
    Expr* deconstructionValue = nullptr;

    LocalDeclStmt() : Stmt(StmtKind::LocalDeclaration) {}
};

struct ExprStmt : Stmt
{
    Expr* expr = nullptr;

    ExprStmt() : Stmt(StmtKind::Expression) {}
};

struct IfStmt : Stmt
{
    Expr* condition = nullptr;
    Stmt* thenBranch = nullptr;
    Stmt* elseBranch = nullptr;

    IfStmt() : Stmt(StmtKind::If) {}
};

struct WhileStmt : Stmt
{
    Expr* condition = nullptr;
    Stmt* body = nullptr;

    WhileStmt() : Stmt(StmtKind::While) {}
};

struct DoWhileStmt : Stmt
{
    Expr* condition = nullptr;
    Stmt* body = nullptr;

    DoWhileStmt() : Stmt(StmtKind::DoWhile) {}
};

struct ForStmt : Stmt
{
    std::vector<Stmt*> init;
    Expr* condition = nullptr;
    std::vector<Expr*> update;
    Stmt* body = nullptr;

    ForStmt() : Stmt(StmtKind::For) {}
};

struct ForEachStmt : Stmt
{
    TypeRef* typeRef = nullptr;
    String name;
    std::uint32_t slot = 0;
    Expr* sequence = nullptr;
    Stmt* body = nullptr;
    /// `foreach (var (k, v) in pairs)`.
    Deconstruction* deconstruction = nullptr;
    /// How the checker decided to walk it.
    enum class Over : std::uint8_t
    {
        Array,
        String,
        List,
        Dictionary,
        Enumerable,
    } over = Over::Array;
    std::uint32_t cursorSlot = 0;
    std::uint32_t sequenceSlot = 0;
    MethodInfo* getEnumerator = nullptr;
    MethodInfo* moveNext = nullptr;
    MethodInfo* current = nullptr;
    MethodInfo* countGetter = nullptr;
    MethodInfo* itemGetter = nullptr;

    ForEachStmt() : Stmt(StmtKind::ForEach) {}
};

/// `case <pattern> when <guard>:`.
struct SwitchCaseLabel
{
    Pattern* pattern = nullptr;
    Expr* guard = nullptr;
};

struct SwitchSection
{
    /// Empty `labels` with `isDefault` marks `default:`.
    std::vector<Expr*> labels;
    std::vector<SwitchCaseLabel> patterns;
    bool isDefault = false;
    std::vector<Stmt*> statements;
    std::uint32_t line = 0;
    /// The pattern labels, lowered and joined; null when there are none.
    Expr* test = nullptr;
};

struct SwitchStmt : Stmt
{
    Expr* subject = nullptr;
    std::vector<SwitchSection> sections;
    /// Set by the checker when any case is a pattern: the subject lives in
    /// this slot and each section's `test` reads it.
    bool usesPatterns = false;
    std::uint32_t subjectSlot = 0;

    SwitchStmt() : Stmt(StmtKind::Switch) {}
};

struct BreakStmt : Stmt
{
    BreakStmt() : Stmt(StmtKind::Break) {}
};

struct ContinueStmt : Stmt
{
    ContinueStmt() : Stmt(StmtKind::Continue) {}
};

struct ReturnStmt : Stmt
{
    Expr* value = nullptr;

    ReturnStmt() : Stmt(StmtKind::Return) {}
};

struct ThrowStmt : Stmt
{
    Expr* value = nullptr;  ///< null for a bare `throw;`

    ThrowStmt() : Stmt(StmtKind::Throw) {}
};

struct CatchClause
{
    TypeRef* typeRef = nullptr;  ///< null for `catch { }`
    String name;
    std::uint32_t slot = 0;
    Stmt* body = nullptr;
    Expr* filter = nullptr;      ///< `when (...)`
    const TypeInfo* type = nullptr;
    std::uint32_t line = 0;
};

struct TryStmt : Stmt
{
    Stmt* body = nullptr;
    std::vector<CatchClause> catches;
    Stmt* finallyBlock = nullptr;

    TryStmt() : Stmt(StmtKind::Try) {}
};

struct EmptyStmt : Stmt
{
    EmptyStmt() : Stmt(StmtKind::Empty) {}
};

/// `yield return x;` and `yield break;`. A method containing one of these is
/// rewritten into a state machine before it is checked.
struct YieldStmt : Stmt
{
    Expr* value = nullptr;
    bool isBreak = false;

    YieldStmt() : Stmt(StmtKind::Yield) {}
};

/// `using (resource) body`. The declaration form becomes one of these too,
/// scoped to the end of the enclosing block.
struct UsingStmt : Stmt
{
    Stmt* declaration = nullptr;   ///< `using (var f = ...)`
    Expr* resource = nullptr;      ///< `using (expr)`
    Stmt* body = nullptr;
    std::uint32_t resourceSlot = 0;
    MethodInfo* dispose = nullptr;
    /// The try/finally the checker turned this into.
    Stmt* lowered = nullptr;

    UsingStmt() : Stmt(StmtKind::Using) {}
};

struct MethodDecl;

/// A function declared inside a method body. It becomes a static method of the
/// enclosing type, with any captures passed as a closure box.
struct LocalFunctionStmt : Stmt
{
    MethodDecl* method = nullptr;

    LocalFunctionStmt() : Stmt(StmtKind::LocalFunction) {}
};

// ---------------------------------------------------------- declarations ---

enum Modifier : std::uint16_t
{
    ModPublic = 1u << 0,
    ModPrivate = 1u << 1,
    ModProtected = 1u << 2,
    ModInternal = 1u << 3,
    ModStatic = 1u << 4,
    ModVirtual = 1u << 5,
    ModOverride = 1u << 6,
    ModAbstract = 1u << 7,
    ModSealed = 1u << 8,
    ModReadonly = 1u << 9,
    ModConst = 1u << 10,
    ModPartial = 1u << 11,
    ModExtern = 1u << 12,
    ModRequired = 1u << 13,
};

struct ParamDecl
{
    String name;
    TypeRef* typeRef = nullptr;
    ParamMode mode = ParamMode::Value;
    Expr* defaultValue = nullptr;
    std::uint32_t line = 0;
};

struct TypeDecl;

struct MethodDecl
{
    String name;
    std::uint16_t modifiers = 0;
    TypeRef* returnTypeRef = nullptr;
    std::vector<ParamDecl> params;
    Stmt* body = nullptr;
    Expr* expressionBody = nullptr;   ///< `=> expr;`
    bool isConstructor = false;
    /// `: base(...)` or `: this(...)`.
    bool chainsToBase = false;
    bool chainsToThis = false;
    std::vector<Argument> chainArgs;
    std::uint32_t line = 0;
    TypeDecl* owner = nullptr;
    MethodInfo* info = nullptr;      ///< filled in by the checker's declare pass
    MethodInfo* chainCtor = nullptr; ///< the `base(...)` or `this(...)` target
    /// `T Max<T>(...)`: the parameters, and what each was bound to in this
    /// instantiation. A declaration with `typeParams` and no `typeArgs` is a
    /// template; the checker never emits it directly.
    std::vector<String> typeParams;
    std::vector<TypeConstraint> constraints;
    std::vector<const TypeInfo*> typeArgs;
    /// The template this was instantiated from, for diagnostics.
    MethodDecl* genericFrom = nullptr;
    /// Declared inside a method body rather than on a type.
    bool isLocalFunction = false;
    bool checked = false;
};

struct FieldDecl
{
    String name;
    std::uint16_t modifiers = 0;
    TypeRef* typeRef = nullptr;
    Expr* init = nullptr;
    std::uint32_t line = 0;
};

struct PropertyDecl
{
    String name;
    std::uint16_t modifiers = 0;
    TypeRef* typeRef = nullptr;
    Stmt* getterBody = nullptr;
    Expr* getterExpr = nullptr;
    Stmt* setterBody = nullptr;
    Expr* setterExpr = nullptr;
    bool hasGetter = false;
    bool hasSetter = false;
    bool autoProperty = false;
    bool isInitOnly = false;      ///< `init;` rather than `set;`
    bool isRequired = false;      ///< `required` on the declaration
    bool usesFieldKeyword = false;///< the body mentioned `field`
    Expr* init = nullptr;         ///< `{ get; set; } = value;`
    std::uint32_t line = 0;
    /// Filled in by the checker.
    MethodDecl* getterMethod = nullptr;
    MethodDecl* setterMethod = nullptr;
    String backingField;
};

struct EnumMemberDecl
{
    String name;
    Expr* value = nullptr;
    std::uint32_t line = 0;
};

enum class DeclKind : std::uint8_t
{
    Class,
    Struct,
    Interface,
    Enum,
    Delegate,
};

struct TypeDecl
{
    DeclKind kind = DeclKind::Class;
    String name;
    String fullName;
    std::uint16_t modifiers = 0;
    std::vector<TypeRef*> baseTypes;
    std::vector<FieldDecl> fields;
    std::vector<MethodDecl*> methods;
    std::vector<PropertyDecl> props;
    std::vector<EnumMemberDecl> enumMembers;
    /// A delegate declaration's signature.
    TypeRef* returnTypeRef = nullptr;
    std::vector<ParamDecl> params;
    std::uint32_t line = 0;
    TypeInfo* info = nullptr;  ///< filled in by the checker's declare pass
    /// Field initializers are checked once, in the first constructor that
    /// runs them, not once per constructor.
    bool instanceInitializersChecked = false;
    bool staticInitializersChecked = false;
    /// `class Box<T>`: the parameters, and what each was bound to in this
    /// instantiation. A declaration with `typeParams` and no `typeArgs` is a
    /// template: it is cloned per instantiation and never checked directly.
    std::vector<String> typeParams;
    std::vector<TypeConstraint> constraints;
    std::vector<const TypeInfo*> typeArgs;
    /// The template this was instantiated from, for diagnostics.
    TypeDecl* genericFrom = nullptr;
    /// A `record` gets its members synthesized before the declare pass.
    bool isRecord = false;
    bool isPrimaryConstructed = false;
};

struct CompilationUnit
{
    std::vector<String> usings;
    /// `using static Math;` -- the members of these types are in scope
    /// unqualified.
    std::vector<String> staticUsings;
    std::vector<TypeDecl*> types;
    /// Top-level statements, wrapped into a synthesized entry point.
    std::vector<Stmt*> topLevel;
    /// Functions declared beside the top-level statements. They become static
    /// methods of the same synthesized class, which is what makes them visible
    /// before the line that declares them.
    std::vector<MethodDecl*> topLevelMethods;
    std::string sourceName;
};

// ---------------------------------------------------------------- clone ---

/// A fresh copy of a declaration, with none of the checker's annotations.
///
/// Generics are instantiated by reification: `Box<int>` and `Box<string>` are
/// separate types built from separate copies of the same tree. That is only
/// possible because a copy exists -- the checker writes its conclusions into
/// the nodes, so two instantiations cannot share one.
TypeDecl* cloneTypeDecl(Arena& arena, const TypeDecl* decl);
MethodDecl* cloneMethodDecl(Arena& arena, const MethodDecl* decl, TypeDecl* owner);

}  // namespace typed
}  // namespace internal
}  // namespace script
