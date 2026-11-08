// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The syntax tree.
//
// One node type carries every construct. The alternative -- sixty small
// classes -- buys little here because the compiler visits nodes through a
// single switch anyway, and the flat layout keeps the parser and the compiler
// half their size. Each kind documents which of `a`..`d` and `list` it uses.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "script/internal/common.hpp"
#include "script/internal/dyn/lexer.hpp"

namespace script {
namespace internal {
namespace dyn {

enum class NK : std::uint8_t
{
    // ---- expressions -------------------------------------- a b c d list --
    NumberLit,     //                                        text/number
    StringLit,     //                                        text
    BoolLit,       //                                        number != 0
    NullLit,       //
    Identifier,    //                                        text
    ThisExpr,      //
    SuperExpr,     //
    TemplateLit,   //                                        list = expressions, strs
    ArrayLit,      //                                        list = elements (may hold Spread/Hole)
    ObjectLit,     //                                        list = Property nodes
    Property,      // a=key  b=value                         text, computed, propKind
    FunctionExpr,  // d=body                                 list = params, text = name
    ArrowExpr,     // d=body                                 list = params
    ClassExpr,     // a=superclass                           list = MethodDef, text = name
    MethodDef,     // a=key  b=FunctionExpr                  isStatic, computed, propKind
    FieldDef,      // a=key  b=initialiser                   isStatic, computed
    Unary,         // a=operand                              op
    Update,        // a=operand                              op, prefix
    Binary,        // a=left b=right                         op
    Logical,       // a=left b=right                         op (&& || ??)
    Assign,        // a=target b=value                       op
    Conditional,   // a=test b=then c=else
    Call,          // a=callee                               list = args, optional
    NewExpr,       // a=callee                               list = args
    Member,        // a=object b=computed key                text, computed, optional
    Sequence,      //                                        list
    Spread,        // a=argument
    Yield,         // a=argument (may be null)               prefix = `yield*`
    Await,         // a=argument
    NewTarget,     //                                        `new.target`
    BigIntLit,     //                                        text = the digits, with any 0x, 0o or 0b
    RegExpLit,     //                                        text = pattern, strs[0] = flags
    TemplateObject,//                                        strs = cooked, raws: the first argument of a tagged template
    StaticBlock,   // d=block
    FieldInit,     // a=computed key or private name  b=value  text, computed, propKind: what a field or private method does to `this`
    Hole,          // elision in an array literal

    // ---- patterns ----------------------------------------------------------
    ArrayPattern,   //                                        list
    ObjectPattern,  //                                        list = Property
    AssignPattern,  // a=target b=default
    RestElement,    // a=target

    // ---- statements --------------------------------------------------------
    Program,        //                                        list
    ExprStatement,  // a=expression
    VarDecl,        //                                        list = Declarator, varKind
    Declarator,     // a=target b=init
    Block,          //                                        list
    If,             // a=test b=then c=else
    For,            // a=init b=test c=update d=body
    ForIn,          // a=left b=right d=body                  varKind (Invalid = plain target)
    ForOf,          // a=left b=right d=body                  varKind
    While,          // a=test d=body
    DoWhile,        // a=test d=body
    ReturnStmt,     // a=argument
    BreakStmt,      //                                        text = label
    ContinueStmt,   //                                        text = label
    ThrowStmt,      // a=argument
    Try,            // a=block b=catchParam c=catchBody d=finally
    Switch,         // a=discriminant                         list = SwitchCase
    SwitchCase,     // a=test (null for default)              list = body
    Labeled,        // d=body                                 text
    FunctionDecl,   // a=FunctionExpr                         text
    ClassDecl,      // a=ClassExpr                            text
    Empty,

    // ---- TypeScript expressions and declarations ---------------------------
    AsExpr,          // a=expression  type=target              op = As|Satisfies
    NonNull,         // a=expression                           `e!`
    TypeAlias,       // type=aliased type                      text, typeParams
    InterfaceDecl,   // type=TypeLit  list=extends TypeRefs     text, typeParams
    EnumDecl,        //                                        list = EnumMember, text
    EnumMember,      // a=initialiser                          text
    ModuleDecl,      //                                        list = body, text
    TypeParam,       // a=constraint b=default                 text
    HeritageClause,  //                                        list = TypeRef

    // ---- type syntax -------------------------------------------------------
    // These never reach the compiler: `erase` removes them once the checker
    // has run. They hang off `Node::type`, never off `a`..`d`.
    TypeRef,           //                                        text, list = arguments
    TypeLit,           //                                        list = TypeMember
    TypeMember,        // a=key (computed index signature)        text, type, optional, readOnly
    ArrayType,         // type=element
    TupleType,         //                                        list = element types
    UnionType,         //                                        list
    IntersectionType,  //                                        list
    FunctionType,      // type=return                            list = params
    ConstructorType,   // type=return                            list = params
    LiteralType,       //                                        text/number, op = True|False|Null
    TypeQuery,         //                                        text = `typeof name`
    KeyOfType,         // type=operand
    IndexedAccess,     // type=object  b=index type
    TypePredicate,     // type=asserted type                     text = parameter name
};

enum class VarKind : std::uint8_t
{
    Var,
    Let,
    Const,
    None,  ///< `for (x of y)` without a declaration
};

enum class PropKind : std::uint8_t
{
    Init,
    Getter,
    Setter,
    Method,
    Constructor,
    Spread,
};

/// `public`/`private`/`protected` on a class member. Only `Public` has runtime
/// meaning; the others are checked and then erased.
enum class Visibility : std::uint8_t
{
    Public,
    Protected,
    Private,
};

struct Node;
using NodePtr = std::unique_ptr<Node>;

struct Node
{
    NK kind = NK::Empty;
    std::uint32_t line = 0;
    Tok op = Tok::Eof;
    VarKind varKind = VarKind::Var;
    PropKind propKind = PropKind::Init;
    Visibility visibility = Visibility::Public;
    /// A function's or class's text in the source, in bytes, for `Function.prototype.toString`; both 0 when unknown.
    std::uint32_t srcStart = 0;
    std::uint32_t srcEnd = 0;
    bool computed = false;
    /// `__proto__: value` in an object literal, which sets the prototype rather than making a property.
    bool setsProto = false;
    bool isStatic = false;
    bool prefix = false;
    bool bindMethod = false;  ///< a Member: reading a method makes a bound method, as Python says
    bool optional = false;  ///< `?.` member access or call, or `x?:` on a binding
    bool namedExpr = false; ///< FunctionExpr written `function name() {}`: the name binds inside
    bool readOnly = false;  ///< `readonly` property or `readonly T[]`
    bool declared = false;  ///< ambient: `declare`, or a signature with no body
    bool exported = false;  ///< `export`, which only means anything in a namespace
    bool isAbstract = false;
    bool isParamProp = false;  ///< a constructor parameter that declares a field
    bool allocatesThis = false;  ///< ClassExpr: Python's way, where `self` exists before the base's `__init__` runs
    bool isMethod = false;     ///< FunctionExpr: a method or accessor, or what runs a field: `super.x` means something
    bool legacyOctal = false;  ///< NumberLit written as `017`: an error in strict code
    bool isPrivate = false;    ///< a member named `#x`; an Identifier that is such a name
    bool isGenerator = false;  ///< FunctionExpr: `function*`
    bool isAsync = false;      ///< FunctionExpr, ArrowExpr: `async`; ForOf: `for await`

    String text;
    double number = 0;
    std::vector<String> strs;  ///< cooked chunks of a template literal
    std::vector<String> raws;  ///< the same as written, for a tag

    NodePtr a, b, c, d;
    std::vector<NodePtr> list;

    /// The type annotation written on this node, if any: the declared type of a
    /// binding, the return type of a function, the operand of a type operator.
    NodePtr type;
    std::vector<NodePtr> typeParams;

    explicit Node(NK k, std::uint32_t ln = 0) : kind(k), line(ln) {}
};

inline NodePtr makeNode(NK k, std::uint32_t line = 0)
{
    return std::make_unique<Node>(k, line);
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
