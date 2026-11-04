// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Tokenizer. Reads UTF-8 source, produces tokens whose text payloads are
// already decoded into UTF-16.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "script/internal/common.hpp"

namespace script {
namespace internal {
namespace typed {

enum class Tok : std::uint8_t
{
    Eof,
    Identifier,
    IntLit,
    LongLit,
    UIntLit,
    ULongLit,
    FloatLit,
    DoubleLit,
    DecimalLit,  ///< recognised so the error can say `decimal` is unsupported
    CharLit,
    StringLit,
    /// `$"..."`, pre-split: `parts` are the literal chunks and `exprs` the
    /// source of each hole, re-parsed by the parser.
    InterpolatedLit,

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
    DotDot,       // ..
    Colon,
    ColonColon,
    Question,
    QuestionDot,
    QuestionQuestion,
    QuestionQuestionAssign,
    Arrow,        // =>
    MinusArrow,   // ->
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
    Abstract,
    As,
    Base,
    Bool,
    Break,
    Byte,
    Case,
    Catch,
    Char,
    Checked,
    Class,
    Const,
    Continue,
    Decimal,
    Default,
    Delegate,
    Do,
    Double,
    Else,
    Enum,
    Explicit,
    Extern,
    False,
    Finally,
    Float,
    For,
    Foreach,
    Goto,
    If,
    Implicit,
    In,
    Int,
    Interface,
    Internal,
    Is,
    Lock,
    Long,
    Namespace,
    New,
    Null,
    Object,
    Operator,
    Out,
    Override,
    Params,
    Private,
    Protected,
    Public,
    Readonly,
    Ref,
    Return,
    SByte,
    Sealed,
    Short,
    Sizeof,
    Static,
    StringKw,
    Struct,
    Switch,
    This,
    Throw,
    True,
    Try,
    Typeof,
    UInt,
    ULong,
    Unchecked,
    Unsafe,
    UShort,
    Using,
    Virtual,
    Void,
    Volatile,
    While,

    /// Contextual keywords. `identifierLike` lets every one of them still be
    /// spelled as an ordinary name, which the language requires.
    Var,
    Value,
    Get,
    Set,
    Partial,
    Where,
    Yield,
    Async,
    Await,
    Global,
    Nameof,
    When,
    Record,
    Init,
    Required,
    With,
    Field,
    And,
    Or,
    Not,
};

const char* tokenName(Tok t) noexcept;
/// True for tokens that may appear where an identifier is expected.
bool identifierLike(Tok t) noexcept;
/// True for the built-in type keywords: `int`, `string`, `object` ...
bool typeKeyword(Tok t) noexcept;

struct Token
{
    Tok type = Tok::Eof;
    std::uint32_t line = 1;
    std::uint32_t column = 1;
    String text;              ///< identifier name or string value
    std::int64_t intValue = 0;
    double realValue = 0;
    std::vector<String> parts;        ///< interpolation: literal chunks
    std::vector<std::string> exprs;   ///< interpolation: hole sources
    std::vector<std::string> formats; ///< interpolation: the `:fmt` of each hole
};

/// A single-pass tokenizer. `>>` is deliberately not produced as one token
/// where a type argument list could close, so the parser splits it itself; see
/// `Parser::splitShift`.
class Lexer
{
public:
    Lexer(std::string_view source, std::string name);

    Token next();
    /// Tokenize the whole input. The parser works off the vector because C#
    /// needs unbounded lookahead in a couple of places -- telling a cast from
    /// a parenthesized expression, and a generic call from a comparison.
    std::vector<Token> tokenize();

    [[noreturn]] void fail(const std::string& message) const;
    [[noreturn]] static void fail(const std::string& sourceName, std::uint32_t line,
                                  std::uint32_t column, const std::string& message);

private:
    char peek(std::size_t ahead = 0) const noexcept
    {
        return pos_ + ahead < src_.size() ? src_[pos_ + ahead] : '\0';
    }
    void skipSpace();
    Token readToken();
    Token readNumber();
    Token readString();
    Token readVerbatimString();
    Token readRawString();    Token readInterpolated(bool verbatim);
    Token readChar();
    Token readIdentifier();
    Char readEscape();
    Token make(Tok type, std::size_t length) noexcept;

    std::string_view src_;
    std::string name_;
    std::size_t pos_ = 0;
    std::uint32_t line_ = 1;
    std::size_t lineStart_ = 0;
};

/// Maps an identifier to its keyword token, or `Tok::Identifier`.
Tok keywordOf(StringView word) noexcept;

}  // namespace typed
}  // namespace internal
}  // namespace script
