// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// C and C++ tokenizer. Reads UTF-8 source and produces the whole token
// stream, with the preprocessor already applied to the extent the front end
// supports it: `#include` lines are dropped (the library is built in),
// object-like `#define` macros are expanded in place, and every other
// directive is skipped. Function-like macros are refused. With `cpp` on, the
// C++ keywords are keywords; otherwise they are identifiers.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "script/internal/common.hpp"

namespace script {
namespace internal {
namespace c {

enum class Tok : std::uint8_t
{
    Eof,
    Identifier,
    IntLit,     ///< value in `intValue`; `isUnsigned`, `isLong` from the suffix
    FloatLit,   ///< value in `realValue`; `isFloat` for an `f` suffix
    CharLit,    ///< value in `intValue`
    StringLit,  ///< cooked text in `text`

    // punctuation
    LBrace,
    RBrace,
    LParen,
    RParen,
    LBracket,
    RBracket,
    Semicolon,
    Comma,
    Dot,
    Arrow,      // ->
    Colon,
    ColonColon, // ::
    Question,
    Ellipsis,
    Assign,
    PlusAssign,
    MinusAssign,
    StarAssign,
    SlashAssign,
    PercentAssign,
    ShlAssign,
    ShrAssign,
    AmpAssign,
    PipeAssign,
    CaretAssign,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    PlusPlus,
    MinusMinus,
    Shl,
    Shr,
    Amp,
    Pipe,
    Caret,
    Tilde,
    Bang,
    Lt,
    Gt,
    Le,
    Ge,
    EqEq,
    NotEq,
    AndAnd,
    OrOr,

    // keywords
    Auto,
    Bool,       // _Bool and bool
    Break,
    Case,
    Char,
    Const,
    Continue,
    Default,
    Do,
    Double,
    Else,
    Enum,
    Extern,
    False,
    Float,
    For,
    Goto,
    If,
    Inline,
    Int,
    Long,
    Register,
    Restrict,
    Return,
    Short,
    Signed,
    Sizeof,
    Static,
    Struct,
    Switch,
    True,
    Typedef,
    Union,
    Unsigned,
    Void,
    Volatile,
    While,

    // C++ only
    Class,
    Public,
    Private,
    Protected,
    This,
    New,
    Delete,
    Namespace,
    Using,
    Virtual,
    Override,
    Nullptr,
    Try,
    Catch,
    Throw,
    Template,
    Typename,
    Operator,
    Friend,
    Explicit,
    Constexpr,
    Mutable,
    Noexcept,
    StaticCast,
    DynamicCast,
    ConstCast,
    ReinterpretCast,
};

const char* tokenName(Tok t) noexcept;

struct Token
{
    Tok type = Tok::Eof;
    std::uint32_t line = 1;
    std::uint32_t column = 1;
    String text;  ///< identifier name or string value
    std::int64_t intValue = 0;
    double realValue = 0;
    bool isUnsigned = false;
    bool isLong = false;
    bool isFloat = false;
};

class Lexer
{
public:
    Lexer(std::string_view source, std::string name, bool cpp = false);

    /// The whole stream, ending in `Eof`. Throws `Error(SyntaxError)`.
    std::vector<Token> tokenize();

    [[noreturn]] static void fail(const std::string& sourceName, std::uint32_t line,
                                  const std::string& message);

private:
    char peek(std::size_t ahead = 0) const noexcept
    {
        return pos_ + ahead < src_.size() ? src_[pos_ + ahead] : '\0';
    }
    [[noreturn]] void fail(const std::string& message) const { fail(name_, line_, message); }

    /// Skip whitespace and comments; returns true at the start of a line.
    void skipSpace();
    void directive(std::vector<Token>& out);
    bool readToken(std::vector<Token>& out);
    Token readNumber();
    Token readChar();
    Token readString();
    Token readIdentifier();
    std::uint32_t readEscape();
    void emit(std::vector<Token>& out, Token token);

    std::string_view src_;
    std::string name_;
    bool cpp_ = false;
    std::size_t pos_ = 0;
    std::size_t lineStart_ = 0;
    std::uint32_t line_ = 1;
    bool atLineStart_ = true;
    /// Object-like macros, by name, as the tokens that replace them.
    std::unordered_map<String, std::vector<Token>> macros_;
    int expansionDepth_ = 0;
};

Tok keywordOf(StringView word, bool cpp) noexcept;

}  // namespace c
}  // namespace internal
}  // namespace script
