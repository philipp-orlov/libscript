// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// C and C++ parser. Recursive descent over the token stream, producing the
// typed engine's syntax tree -- the one the C# checker consumes -- so that
// name resolution, overloads, layout and code generation are the C# front
// end's. The C-specific work happens here: C's types are tracked well enough
// to insert the conversions C makes silently (int <- double, char <- int, a
// number or pointer where a condition is wanted, a comparison where a number
// is wanted), and C's spellings are mapped onto the engine's:
//
//   struct T            a class; `.` and `->` both read as member access
//   T a[N], T* (local)  an array
//   T* (parameter)      `ref T` when the body dereferences it, `T[]` when it indexes it
//   char*               string
//   malloc(sizeof T)    new T(); free() is dropped
//   functions, globals  static members of one synthesized class
//   the C library       static native methods on `libc` (see libc.cpp)
//
// With the C++ dialect on (`Dialect::Cpp`, the members in cxx.cpp), the same
// parser also maps:
//
//   class / struct with members   a class with fields, methods, constructors,
//                                 a base class and virtual dispatch
//   T& (parameter)                `ref T` for scalars and strings; classes and
//                                 containers are references already
//   new T(...), delete p          new T(...); delete is dropped
//   bool                          the engine's bool
//   std::string                   string
//   std::vector<T>                List<T>
//   std::map<K, V>                Dictionary<K, V>
//   std::cout << x << std::endl   one `libc.__cout` call per statement
//   throw / try / catch           the engine's exceptions
//   namespaces, using, auto, enum class, default arguments, overloading
//
// Whatever has no such mapping -- pointer arithmetic, function pointers,
// unions, goto, `void*`, templates, lambdas, operator overloading, iterators
// -- is refused with a SyntaxError that says so.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "script/internal/c/lexer.hpp"
#include "script/internal/typed/ast.hpp"

namespace script {
namespace internal {
namespace c {

/// The synthesized class that holds every function and global. The `$` keeps
/// it clear of any name a C program can spell.
inline constexpr const char* kGlobalsClass = "C$";

enum class Dialect : std::uint8_t
{
    C,
    Cpp,
};

/// A C or C++ type, as far as the front end tracks it.
struct CType
{
    enum class Kind : std::uint8_t
    {
        Void,
        Bool,    ///< the result of a comparison; in C++ also the `bool` type
        Char,
        UChar,
        Short,
        UShort,
        Int,
        UInt,
        Long,
        ULong,
        Float,
        Double,
        Str,     ///< `char*` and `std::string`: an immutable string
        Struct,  ///< a class
        Array,   ///< `T[]`
        Null,    ///< the `NULL` / `nullptr` literal
        Vector,  ///< `std::vector<T>`: `List<T>`
        Map,     ///< `std::map<K, V>`: `Dictionary<K, V>`
        Pair,    ///< `std::pair<A, B>`, only as a range-for variable over a map
    };

    Kind kind = Kind::Int;
    String structName;             ///< Kind::Struct
    std::shared_ptr<CType> elem;   ///< Kind::Array, Vector; the key of a Map; first of a Pair
    std::shared_ptr<CType> elem2;  ///< the value of a Map; second of a Pair

    static CType of(Kind k) { return CType{k, {}, nullptr, nullptr}; }
    static CType structOf(String name) { return CType{Kind::Struct, std::move(name), nullptr, nullptr}; }
    static CType arrayOf(const CType& element)
    {
        return CType{Kind::Array, {}, std::make_shared<CType>(element), nullptr};
    }
    static CType vectorOf(const CType& element)
    {
        return CType{Kind::Vector, {}, std::make_shared<CType>(element), nullptr};
    }
    static CType mapOf(const CType& key, const CType& value)
    {
        return CType{Kind::Map, {}, std::make_shared<CType>(key), std::make_shared<CType>(value)};
    }
    static CType pairOf(const CType& first, const CType& second)
    {
        return CType{Kind::Pair, {}, std::make_shared<CType>(first), std::make_shared<CType>(second)};
    }

    bool isIntegral() const noexcept { return kind >= Kind::Char && kind <= Kind::ULong; }
    bool isFloating() const noexcept { return kind == Kind::Float || kind == Kind::Double; }
    bool isNumeric() const noexcept { return isIntegral() || isFloating(); }
    bool isContainer() const noexcept { return kind == Kind::Vector || kind == Kind::Map; }
    bool isReference() const noexcept
    {
        return kind == Kind::Str || kind == Kind::Struct || kind == Kind::Array || kind == Kind::Null ||
               kind == Kind::Vector || kind == Kind::Map || kind == Kind::Pair;
    }
    bool same(const CType& o) const noexcept;
};

/// A host class as the binding layer declared it, spelled for this front
/// end: `Env.Greet("x")` calls a static member, `Vec2 v = Vec2(1, 2)`
/// constructs an instance, `v.Length()` and `v.X` reach its members.
struct HostClassSpec
{
    struct Member
    {
        String name;
        bool isMethod = false;
        bool isConstructor = false;
        bool isStatic = true;
        CType result;
        std::vector<CType> params;
    };
    String name;
    bool hasConstructor = false;
    std::vector<Member> members;
};

class Parser
{
public:
    Parser(std::string_view source, std::string name, typed::Arena& arena,
           std::vector<HostClassSpec> hostClasses = {}, Dialect dialect = Dialect::C);

    /// The whole translation unit. Throws `Error(SyntaxError)`.
    typed::CompilationUnit* parseUnit();

private:
    using Expr = typed::Expr;
    using Stmt = typed::Stmt;
    using TypeRef = typed::TypeRef;

    struct ClassInfo;
    struct FuncInfo;

    /// An expression with the C type the front end worked out for it.
    struct Typed
    {
        Expr* expr = nullptr;
        CType type;
        /// `&x`: the operand, to be passed by reference or as an array.
        Expr* addressOf = nullptr;
        CType addressType;
        /// A reference parameter named bare, passable on by reference.
        bool isRef = false;
        /// A function name, valid only as a callee.
        String funcName;
        /// The class whose method `funcName` names, when a method was named
        /// bare inside one of its own methods.
        const ClassInfo* methodOwner = nullptr;
        /// A host class named bare: `Env` in `Env.Greet(...)`, or a
        /// constructor call waiting for its arguments.
        const HostClassSpec* hostClass = nullptr;
        /// A C++ class named bare: `Point(1, 2)` or `Point::count`.
        const ClassInfo* classType = nullptr;
        /// `sizeof(T)`, and `n * sizeof(T)`: what `malloc` needs to know.
        std::shared_ptr<CType> sizeofType;
        Expr* countExpr = nullptr;
        /// C++: `m[k]` reads through a helper that inserts a default the way
        /// `operator[]` does; this is the indexer form to assign through, and
        /// the map's type, for a compound assignment's helper.
        Expr* store = nullptr;
        std::shared_ptr<CType> mapType;
        /// C++: `std::cout` (1) or `std::cerr` (2), or the `<<` chain so far.
        int stream = 0;
        /// C++: the range-for variable over a map. `first` is the key
        /// variable, `second` reads `pairMap[first]`.
        String pairKey;
        Expr* pairMap = nullptr;
        /// C++: a `{...}` argument, typed by whatever it is given to.
        std::shared_ptr<std::vector<Typed>> braceList;
    };

    struct Symbol
    {
        CType type;
        bool isRef = false;     ///< a `ref` parameter: `*p` reads it, `p` alone passes it on
        bool isGlobal = false;  ///< a static field of the synthesized class
        String globalName;      ///< what the field is called (static locals are renamed)
        String pairKey;         ///< C++: a map range-for variable; see Typed
        Expr* pairMap = nullptr;
    };

    struct FuncInfo
    {
        CType returnType;
        std::vector<CType> params;
        std::vector<bool> byRef;
        /// C: a `T*` parameter made `ref` by its body's `*p`; the name alone is
        /// the pointer. False for a C++ `T&`, whose name is the value.
        std::vector<bool> refByPointer;
        std::vector<Expr*> defaults;  ///< per parameter; null when it has none
        std::size_t required = 0;     ///< parameters without a default
        bool variadic = false;
        bool isLibc = false;
        bool isStatic = false;     ///< C++: a static member function
        bool isVirtual = false;    ///< C++: declared or inherited as virtual
        std::uint16_t access = 0;  ///< C++: the modifier the class body gave it
    };

    struct Field
    {
        String name;
        CType type;
        bool isStatic = false;
        std::uint16_t access = 0;  ///< C++: the modifier the class body gave it; 0 is public
    };

    /// A C struct or a C++ class.
    struct ClassInfo
    {
        String name;
        std::vector<Field> fields;
        typed::TypeDecl* decl = nullptr;
        // C++
        String base;
        std::unordered_map<String, std::vector<FuncInfo>> methods;
        std::vector<FuncInfo> ctors;
        bool isClass = false;      ///< `class`: members are private by default
        bool isAbstract = false;
    };

    // ---- token plumbing ----
    const Token& peek(std::size_t ahead = 0) const noexcept
    {
        const std::size_t i = pos_ + ahead;
        return i < toks_.size() ? toks_[i] : toks_.back();
    }
    Tok type(std::size_t ahead = 0) const noexcept { return peek(ahead).type; }
    const Token& advance() { return toks_[pos_ < toks_.size() - 1 ? pos_++ : pos_]; }
    bool is(Tok t) const noexcept { return type() == t; }
    bool eat(Tok t);
    void expect(Tok t, const char* what);
    String expectName(const char* what);
    [[noreturn]] void fail(const std::string& message) const;
    [[noreturn]] void fail(std::uint32_t line, const std::string& message) const;
    std::uint32_t line() const noexcept { return peek().line; }
    std::uint32_t column() const noexcept { return peek().column; }
    void skipBalanced(Tok open, Tok close);
    std::string describe(const CType& t) const;

    // ---- declarations ----
    void external();
    /// The specifiers of a declaration: storage class and the base type.
    struct Specifiers
    {
        CType base;
        bool isTypedef = false;
        bool isStatic = false;
        bool isExtern = false;
        bool isConst = false;
        bool isVoid = false;
        bool isAuto = false;     ///< C++: the type comes from the initializer
        bool isVirtual = false;  ///< C++
        bool any = false;
    };
    Specifiers specifiers();
    bool startsType() const;
    /// The `*`s, name and `[N]`s after the specifiers. Returns the full type;
    /// `arrayLengths` holds each `[N]` (null for `[]`).
    struct Declarator
    {
        String name;
        String owner;   ///< C++: `Point::length` names a member out of line
        CType type;
        std::vector<Expr*> arrayLengths;
        int pointers = 0;
        bool isReference = false;  ///< C++: `T&`
        std::uint32_t line = 0;
        std::uint32_t column = 0;
        bool isFunction = false;
    };
    Declarator declarator(const CType& base, bool allowAbstract = false);
    CType pointerTo(const CType& base, std::uint32_t at) const;
    void structDefinition(const String& tag, std::uint32_t at);
    void enumDefinition();
    CType structType(std::uint32_t at);
    void functionDefinition(const Specifiers& spec, Declarator decl);
    std::vector<typed::ParamDecl> parameters(FuncInfo& info, std::vector<CType>& types,
                                             std::vector<String>& names,
                                             std::vector<bool>& scalarPointer);
    /// Decide `ref` or array for each scalar pointer parameter from the body
    /// that follows, and record the signature in `info`.
    void resolvePointerParams(std::vector<typed::ParamDecl>& params, std::vector<CType>& types,
                              const std::vector<String>& names,
                              const std::vector<bool>& scalarPointer, FuncInfo& info,
                              bool isDefinition);
    /// Parse a body with its parameters in scope.
    Stmt* functionBody(const std::vector<typed::ParamDecl>& params, const std::vector<CType>& types,
                       const std::vector<String>& names, const FuncInfo& info,
                       const CType& returnType, const String& qualifiedName);
    /// Record a function or method signature; a redeclaration replaces its own
    /// entry (keeping the defaults a prototype declared), a new C++ overload is
    /// appended. Fills `params` with the defaults the entry ends up with.
    void registerFunction(std::vector<FuncInfo>& overloads, FuncInfo& info, const String& name,
                          std::uint32_t at, std::vector<typed::ParamDecl>* params = nullptr);
    void globalDeclaration(const Specifiers& spec, Declarator decl);
    Expr* initializer(const CType& type, Declarator* decl, std::uint32_t at);
    Expr* structInitializer(const CType& type, std::uint32_t at);
    Expr* arrayInitializer(const CType& type, Expr* declaredLength, std::uint32_t at);
    /// What an uninitialised variable of this type holds: a fresh struct, a
    /// fresh array of the declared length, nothing for a scalar.
    Expr* defaultValue(const CType& type, const Declarator* decl, std::uint32_t at);
    static bool evalConst(const Expr* e, std::int64_t& out);
    static bool isZero(const Typed& v);
    /// `new T[n]` with every element constructed, for an array of structs.
    Expr* newStructArray(const CType& element, Expr* length, std::uint32_t at);
    Expr* newJaggedArray(const CType& element, Expr* outer, Expr* inner, std::uint32_t at);
    int pointerUsage(const String& name, std::size_t from, std::size_t to);
    typed::MethodDecl* newHelper(const String& name, const CType& returnType, std::uint32_t at);

    // ---- statements ----
    /// One C statement may become several engine statements (a declaration
    /// list of mixed types, a comma expression), so they are appended.
    void statement(std::vector<Stmt*>& out);
    /// A statement in a position that holds exactly one: a loop body, a branch.
    Stmt* embedded();
    Stmt* block();
    void localDeclaration(const Specifiers& spec, std::vector<Stmt*>& out);
    Stmt* ifStatement();
    Stmt* whileStatement();
    Stmt* doStatement();
    Stmt* forStatement();
    Stmt* switchStatement();
    Stmt* returnStatement();
    Stmt* exprStatement(Expr* e, std::uint32_t at);
    bool endsSection(const Stmt* s) const;

    // ---- expressions ----
    Typed expression();
    Typed assignment();
    Typed conditional();
    Typed binary(int precedence);
    Typed unary();
    Typed postfix(Typed base);
    Typed primary();
    Typed call(Typed callee, const String& name, std::uint32_t at, std::uint32_t col);
    std::vector<Typed> arguments();
    Typed libcCall(const String& name, std::vector<Typed>& args, std::uint32_t at,
                   std::uint32_t col);
    Typed sizeofExpr();
    Typed member(Typed base, std::uint32_t at, std::uint32_t col);
    Typed hostMember(Typed base, const HostClassSpec& spec, std::uint32_t at, std::uint32_t col);
    Typed hostConstruct(const HostClassSpec& spec, std::uint32_t at, std::uint32_t col);
    const HostClassSpec* hostClassNamed(const String& name) const;
    Typed index(Typed base, std::uint32_t at, std::uint32_t col);
    Typed compound(Typed target, typed::BinaryOp op, Typed value, std::uint32_t at,
                   std::uint32_t col);
    Typed increment(Typed target, bool positive, bool prefix, std::uint32_t at, std::uint32_t col);
    std::int64_t constantInt(const char* what);

    // ---- typing ----
    TypeRef* typeRef(const CType& type, std::uint32_t at) const;
    CType promote(const CType& a, const CType& b) const;
    static bool widens(const CType& from, const CType& to);
    Expr* coerce(Typed value, const CType& to);
    Expr* condition(Typed value);
    Expr* logicalNot(Typed value, std::uint32_t at, std::uint32_t col);
    Typed makeBinary(typed::BinaryOp op, Typed left, Typed right, std::uint32_t at,
                     std::uint32_t col);
    Expr* cloneSimple(const Expr* e) const;
    int sizeOf(const CType& type) const;

    // ---- node factories ----
    template <class T>
    T* node(std::uint32_t at, std::uint32_t col = 0)
    {
        T* n = arena_.make<T>();
        n->line = at;
        if constexpr (std::is_base_of_v<Expr, T>)
            n->column = col;
        return n;
    }
    Expr* intLiteral(std::int64_t value, std::uint32_t at);
    Expr* boolLiteral(bool value, std::uint32_t at);
    Expr* nullLiteral(std::uint32_t at);
    Expr* stringLiteral(const String& text, std::uint32_t at);
    Expr* nameRef(const String& name, std::uint32_t at, std::uint32_t col = 0);
    /// A function or global of the program: bare inside the synthesized
    /// class, `C$.name` from inside a C++ class.
    Expr* globalRef(const String& name, std::uint32_t at, std::uint32_t col = 0);
    Expr* libcRef(const char* member, std::uint32_t at);
    Expr* memberRef(Expr* target, const char* member, std::uint32_t at);
    Expr* castTo(Expr* e, const CType& to, std::uint32_t at);
    Expr* invoke(Expr* callee, std::vector<Expr*> args, std::uint32_t at, std::uint32_t col = 0);

    // ---- scopes ----
    void pushScope() { scopes_.emplace_back(); }
    void popScope() { scopes_.pop_back(); }
    const Symbol* lookup(const String& name) const;
    void declare(const String& name, Symbol symbol, std::uint32_t at);

    // ---- C++ (cxx.cpp) ----
    /// `class X : public B { ... };` in either pass.
    void classDefinition(const String& name, bool isClass, std::uint32_t at);
    /// One member declaration inside a class body.
    void memberDeclaration(ClassInfo& info, std::uint16_t access);
    /// `T X::m(...) { }` and `X::X(...) : ... { }` outside the class.
    void outOfLineDefinition(const Specifiers& spec, Declarator decl);
    void constructorDefinition(ClassInfo& info, std::uint16_t access, std::uint32_t at);
    /// The parts of a constructor after its parameters: `: x(a), Base(b) { }`.
    void constructorBody(ClassInfo& info, typed::MethodDecl* method, const std::vector<CType>& types,
                         const std::vector<String>& names, const FuncInfo& info2);
    void methodDefinition(ClassInfo& info, const Specifiers& spec, Declarator decl,
                          std::uint16_t access, bool inClass);
    const ClassInfo* classNamed(const String& name) const;
    const Field* findField(const ClassInfo& info, const String& name, const ClassInfo** owner) const;
    const std::vector<FuncInfo>* findMethods(const ClassInfo& info, const String& name) const;
    const ClassInfo* methodOwner(const ClassInfo& info, const String& name) const;
    /// Refuse a private or protected member reached from outside its class.
    void checkAccess(const ClassInfo& owner, std::uint16_t access, const String& member,
                     std::uint32_t at) const;
    bool isSubclass(const String& derived, const String& base) const;
    const FuncInfo& pickOverload(const std::vector<FuncInfo>& overloads, const std::vector<Typed>& args,
                                 const std::string& what, std::uint32_t at) const;
    Expr* bindArguments(const FuncInfo& info, std::vector<Typed>& args, std::vector<typed::Argument>& out,
                        const std::string& what, std::uint32_t at);
    Typed methodCall(Expr* target, const ClassInfo& owner, const String& name, std::uint32_t at,
                     std::uint32_t col);
    Typed classMember(Typed base, const ClassInfo& info, std::uint32_t at, std::uint32_t col);
    Typed staticMember(const ClassInfo& info, std::uint32_t at, std::uint32_t col);
    Typed construct(const CType& type, std::vector<Typed>& args, std::uint32_t at, std::uint32_t col);
    Typed newExpression();
    Stmt* deleteStatement();
    Stmt* tryStatement();
    Stmt* throwStatement();
    Stmt* rangeFor(const Specifiers& spec, Declarator decl, std::uint32_t at);
    void namespaceBlock();
    void usingDirective();
    /// Skip `std::` or a known namespace qualifier; true if one was skipped.
    bool skipQualifier();
    /// `<T>` / `<K, V>` after `vector`, `map`, `pair`.
    std::vector<CType> templateArguments(std::size_t count, const char* what);
    void expectCloseAngle();
    CType typeName(std::uint32_t at);
    /// A `std::` type keyword, or a user class, spelled as a type in an
    /// expression: `std::string(...)`, `int(x)`, `Point(1, 2)`.
    Typed functionalCast(const CType& type, std::uint32_t at, std::uint32_t col);
    Typed stringMember(Typed base, const String& name, std::uint32_t at, std::uint32_t col);
    Typed vectorMember(Typed base, const String& name, std::uint32_t at, std::uint32_t col);
    Typed mapMember(Typed base, const String& name, std::uint32_t at, std::uint32_t col);
    Typed pairMember(Typed base, const String& name, std::uint32_t at, std::uint32_t col);
    Typed stdFunction(const String& name, std::uint32_t at, std::uint32_t col, bool& handled);
    Typed streamOutput(Typed left, Typed right, std::uint32_t at, std::uint32_t col);
    Expr* mapGet(Typed map, Typed key, std::uint32_t at, std::uint32_t col);
    /// `m[k] op= v` as one helper call: `$map<op>(m, k, v)`.
    Typed mapUpdate(Expr* map, Expr* key, const CType& mapType, typed::BinaryOp op, Typed value,
                    std::uint32_t at, std::uint32_t col);
    Expr* vectorFill(const CType& type, Expr* count, Typed* value, std::uint32_t at);
    Expr* containerCreation(const CType& type, std::uint32_t at);
    Expr* zeroValue(const CType& type, std::uint32_t at);
    String mangle(const CType& type) const;
    const char* exceptionTypeFor(const String& name) const;
    Typed castExpression(Tok kind, std::uint32_t at, std::uint32_t col);
    Typed stringCompare(typed::BinaryOp op, Typed left, Typed right, std::uint32_t at, std::uint32_t col);
    Expr* countOf(Typed& container, std::uint32_t at, std::uint32_t col);
    bool cpp() const noexcept { return dialect_ == Dialect::Cpp; }

    std::vector<Token> toks_;
    std::size_t pos_ = 0;
    std::string name_;
    typed::Arena& arena_;
    Dialect dialect_ = Dialect::C;
    typed::CompilationUnit* unit_ = nullptr;
    typed::TypeDecl* globals_ = nullptr;
    bool prepass_ = false;

    std::vector<std::unordered_map<String, Symbol>> scopes_;
    std::unordered_map<String, ClassInfo> structs_;
    std::unordered_map<String, CType> typedefs_;
    std::unordered_map<String, std::int64_t> constants_;
    std::unordered_map<String, std::vector<FuncInfo>> functions_;
    std::unordered_map<String, bool> helpers_;  ///< synthesized methods already made
    std::unordered_set<String> namespaces_;
    std::vector<HostClassSpec> hostClasses_;
    /// The function being parsed: its return type, for `return`.
    CType returnType_;
    String currentFunction_;
    /// C++: the class whose method body is being parsed, and whether it is static.
    const ClassInfo* currentClass_ = nullptr;
    bool currentStatic_ = false;
    /// Whether the last `arguments()` were `{...}`: `vector<int> v{3}` is one
    /// element, `vector<int> v(3)` is three zeros.
    bool lastArgsBraced_ = false;
    int staticCounter_ = 0;
    std::size_t depth_ = 0;
    std::size_t nest_ = 0;  ///< levels of nested statements and expressions being parsed
};

}  // namespace c
}  // namespace internal
}  // namespace script
