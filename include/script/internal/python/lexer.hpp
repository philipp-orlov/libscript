// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Python tokenizer. Reads UTF-8 source and produces the whole token stream at
// once, with the indentation structure already turned into `Indent` and
// `Dedent` tokens and every f-string pre-split into its literal chunks and
// the source of its replacement fields.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "script/internal/common.hpp"

namespace script {
namespace internal {
namespace python {

enum class Tok : std::uint8_t
{
    Eof,
    Newline,  ///< end of a logical line
    Indent,
    Dedent,
    Name,
    Number,
    String,
    FString,  ///< `f"..."`, pre-split

    // punctuation
    LParen,
    RParen,
    LBracket,
    RBracket,
    LBrace,
    RBrace,
    Comma,
    Colon,
    Dot,
    Semicolon,
    Arrow,     // ->
    At,
    Ellipsis,

    // operators
    Plus,
    Minus,
    Star,
    Slash,
    DoubleSlash,
    Percent,
    StarStar,
    Amp,
    Pipe,
    Caret,
    Tilde,
    Shl,
    Shr,
    Lt,
    Gt,
    Le,
    Ge,
    EqEq,
    NotEq,
    Assign,
    Walrus,    // :=
    PlusAssign,
    MinusAssign,
    StarAssign,
    SlashAssign,
    DoubleSlashAssign,
    PercentAssign,
    StarStarAssign,
    AmpAssign,
    PipeAssign,
    CaretAssign,
    ShlAssign,
    ShrAssign,
    AtAssign,

    // keywords
    False,
    None,
    True,
    And,
    As,
    Assert,
    Async,
    Await,
    Break,
    Class,
    Continue,
    Def,
    Del,
    Elif,
    Else,
    Except,
    Finally,
    For,
    From,
    Global,
    If,
    Import,
    In,
    Is,
    Lambda,
    Nonlocal,
    Not,
    Or,
    Pass,
    Raise,
    Return,
    Try,
    While,
    With,
    Yield,
};

const char* tokenName(Tok t) noexcept;

struct Token
{
    Tok type = Tok::Eof;
    std::uint32_t line = 1;
    String text;        ///< a name, or the cooked value of a string
    double number = 0;
    /// An f-string arrives pre-split: `parts` are the cooked literal chunks
    /// (one more than the fields), and for each field `exprs` holds the raw
    /// source of the expression, `specs` the raw format spec after `:` (or
    /// empty) and `convs` the `!r`/`!s` conversion character (or 0).
    std::vector<String> parts;
    std::vector<std::string> exprs;
    std::vector<std::string> specs;
    std::vector<char> convs;
};

class Lexer
{
public:
    Lexer(std::string_view source, std::string name);

    /// The whole stream, ending in `Eof`. Throws `Error(SyntaxError)`.
    std::vector<Token> tokenize();

    [[noreturn]] static void fail(const std::string& sourceName, std::uint32_t line,
                                  const std::string& message);

    /// Split the body of an f-string (the text between the quotes) into
    /// `token.parts`, `exprs`, `specs` and `convs`. A format spec may itself
    /// hold replacement fields; the parser splits it again with this.
    void splitFString(std::string_view body, bool isRaw, Token& token) const;

private:
    char peek(std::size_t ahead = 0) const noexcept
    {
        return pos_ + ahead < src_.size() ? src_[pos_ + ahead] : '\0';
    }
    [[noreturn]] void fail(const std::string& message) const { fail(name_, line_, message); }

    void lineStart(std::vector<Token>& out);
    void readToken(std::vector<Token>& out);
    Token readNumber();
    Token readString(std::string_view prefix);
    Token readName();
    void readEscape(String& out, std::size_t& i, std::string_view body) const;
    String cook(std::string_view raw, bool isRaw) const;
    void push(std::vector<Token>& out, Tok type, std::size_t width);

    std::string_view src_;
    std::string name_;
    std::size_t pos_ = 0;
    std::uint32_t line_ = 1;
    int depth_ = 0;  ///< open brackets: newlines inside them are not logical
    std::vector<int> indents_{0};
};

/// Maps a name to its keyword token, or `Tok::Name`.
Tok keywordOf(StringView word) noexcept;

}  // namespace python
}  // namespace internal
}  // namespace script
