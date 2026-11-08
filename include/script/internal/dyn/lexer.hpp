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
namespace dyn {

enum class Tok : std::uint8_t
{
    Eof,
    Identifier,
    PrivateName,  // #field
    Number,
    StringLit,
    Template,  // a whole `...${...}...` literal, pre-split
    BigIntLit, // text = the digits, with any 0x, 0o or 0b
    Regex,     // recognised so the error can say so

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
    Ellipsis,
    Colon,
    Question,
    QuestionDot,
    QuestionQuestion,
    Arrow,
    Assign,
    PlusAssign,
    MinusAssign,
    StarAssign,
    SlashAssign,
    PercentAssign,
    StarStarAssign,
    ShlAssign,
    ShrAssign,
    UShrAssign,
    AmpAssign,
    PipeAssign,
    CaretAssign,
    AndAssign,
    OrAssign,
    NullishAssign,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    StarStar,
    PlusPlus,
    MinusMinus,
    Shl,
    Shr,
    UShr,
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
    EqEqEq,
    NotEqEq,
    AndAnd,
    OrOr,

    // keywords
    Break,
    Case,
    Catch,
    Class,
    Const,
    Continue,
    Debugger,
    Default,
    Delete,
    Do,
    Else,
    Extends,
    Finally,
    For,
    Function,
    If,
    In,
    InstanceOf,
    Let,
    New,
    Return,
    Static,
    Super,
    Switch,
    This,
    Throw,
    Try,
    TypeOf,
    Var,
    Void,
    While,
    With,
    Yield,
    Async,
    Await,
    Of,
    Get,
    Set,
    True,
    False,
    Null,

    // TypeScript. All of these except `enum` are contextual: `identifierLike`
    // lets every one of them still be spelled as an ordinary name.
    Interface,
    Type,
    Enum,
    Implements,
    Declare,
    Namespace,
    Abstract,
    Readonly,
    KeyOf,
    As,
    Satisfies,
    Is,
    Public,
    Private,
    Protected,
    Override,
    Export,
};

const char* tokenName(Tok t) noexcept;

struct Token
{
    Tok type = Tok::Eof;
    std::uint32_t line = 1;
    bool newlineBefore = false;  ///< drives automatic semicolon insertion
    String text;                 ///< identifier name or string value
    double number = 0;
    /// Template literals arrive pre-split: `parts` are the cooked chunks and
    /// `exprs` the raw source of each `${...}`, re-parsed by the parser.
    std::vector<String> parts;
    std::vector<std::string> exprs;
    /// Each chunk as written, for a tag to see.
    std::vector<String> raws;
    /// An escape spelled this word, which was then no keyword.
    bool escapedKeyword = false;
    /// A number written with a leading zero, `017` or `09`.
    bool legacyOctal = false;
};

/// A single-pass tokenizer. Division and regular-expression literals are told
/// apart by the parser, which asks for a token knowing whether an operand or
/// an operator may come next.
class Lexer
{
public:
    Lexer(std::string_view source, std::string name);

    /// Read the next token. `regexAllowed` selects the `/` interpretation.
    Token next(bool regexAllowed = true);
    /// `slash` was the last token read: back up and read it again as a regular expression literal.
    Token rescanAsRegex(const Token& slash);

    [[noreturn]] void fail(const std::string& message) const;
    [[noreturn]] static void fail(const std::string& sourceName, std::uint32_t line,
                                  const std::string& message);

private:
    char peek(std::size_t ahead = 0) const noexcept
    {
        return pos_ + ahead < src_.size() ? src_[pos_ + ahead] : '\0';
    }
    bool skipSpace();  ///< returns true when a line terminator was crossed
    Token readToken(bool regexAllowed);
    Token readNumber();
    Token readString(char quote);
    Token readTemplate();
    Token readIdentifier();
    void readEscape(String& out);
    /// The last escape was a legacy octal one, or `\8` or `\9`: fine in sloppy code, an error in strict code or a template.
    bool sawLegacyEscape_ = false;

    bool unicodeSpace(bool& sawNewline);
    std::string_view src_;
    std::string name_;
    std::size_t pos_ = 0;
    std::uint32_t line_ = 1;
};

/// Maps an identifier to its keyword token, or `Tok::Identifier`.
Tok keywordOf(StringView word) noexcept;

}  // namespace dyn
}  // namespace internal
}  // namespace script
