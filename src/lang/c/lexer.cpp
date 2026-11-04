// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// C tokenizer, with the little preprocessing the front end does.
#include "base/ascii.hpp"
#include "base/number.hpp"
#include "base/utf16.hpp"
#include "script/internal/c/lexer.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "script/error.hpp"

namespace script {
namespace internal {
namespace c {

namespace {

bool isIdentStart(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool isIdentChar(char c) noexcept
{
    return isIdentStart(c) || (c >= '0' && c <= '9');
}

bool isDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

struct Keyword
{
    const char* word;
    Tok token;
};

constexpr Keyword kKeywords[] = {
    {"auto", Tok::Auto},         {"_Bool", Tok::Bool},       {"bool", Tok::Bool},
    {"break", Tok::Break},       {"case", Tok::Case},        {"char", Tok::Char},
    {"const", Tok::Const},       {"continue", Tok::Continue}, {"default", Tok::Default},
    {"do", Tok::Do},             {"double", Tok::Double},    {"else", Tok::Else},
    {"enum", Tok::Enum},         {"extern", Tok::Extern},    {"false", Tok::False},
    {"float", Tok::Float},       {"for", Tok::For},          {"goto", Tok::Goto},
    {"if", Tok::If},             {"inline", Tok::Inline},    {"int", Tok::Int},
    {"long", Tok::Long},         {"register", Tok::Register}, {"restrict", Tok::Restrict},
    {"return", Tok::Return},     {"short", Tok::Short},      {"signed", Tok::Signed},
    {"sizeof", Tok::Sizeof},     {"static", Tok::Static},    {"struct", Tok::Struct},
    {"switch", Tok::Switch},     {"true", Tok::True},        {"typedef", Tok::Typedef},
    {"union", Tok::Union},       {"unsigned", Tok::Unsigned}, {"void", Tok::Void},
    {"volatile", Tok::Volatile}, {"while", Tok::While},
};

constexpr Keyword kCppKeywords[] = {
    {"class", Tok::Class},           {"public", Tok::Public},       {"private", Tok::Private},
    {"protected", Tok::Protected},   {"this", Tok::This},           {"new", Tok::New},
    {"delete", Tok::Delete},         {"namespace", Tok::Namespace}, {"using", Tok::Using},
    {"virtual", Tok::Virtual},       {"override", Tok::Override},   {"nullptr", Tok::Nullptr},
    {"try", Tok::Try},               {"catch", Tok::Catch},         {"throw", Tok::Throw},
    {"template", Tok::Template},     {"typename", Tok::Typename},   {"operator", Tok::Operator},
    {"friend", Tok::Friend},         {"explicit", Tok::Explicit},   {"constexpr", Tok::Constexpr},
    {"mutable", Tok::Mutable},       {"noexcept", Tok::Noexcept},   {"static_cast", Tok::StaticCast},
    {"dynamic_cast", Tok::DynamicCast}, {"const_cast", Tok::ConstCast},
    {"reinterpret_cast", Tok::ReinterpretCast},
};

bool sameWord(StringView word, const char* text) noexcept
{
    const std::size_t n = std::strlen(text);
    if (word.size() != n)
        return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (word[i] != char16_t(text[i]))
            return false;
    }
    return true;
}

}  // namespace

Tok keywordOf(StringView word, bool cpp) noexcept
{
    for (const Keyword& k : kKeywords) {
        if (sameWord(word, k.word))
            return k.token;
    }
    if (cpp) {
        for (const Keyword& k : kCppKeywords) {
            if (sameWord(word, k.word))
                return k.token;
        }
    }
    return Tok::Identifier;
}

const char* tokenName(Tok t) noexcept
{
    switch (t) {
        case Tok::Eof: return "end of input";
        case Tok::Identifier: return "identifier";
        case Tok::IntLit:
        case Tok::FloatLit: return "number";
        case Tok::CharLit: return "character literal";
        case Tok::StringLit: return "string literal";
        case Tok::LBrace: return "'{'";
        case Tok::RBrace: return "'}'";
        case Tok::LParen: return "'('";
        case Tok::RParen: return "')'";
        case Tok::LBracket: return "'['";
        case Tok::RBracket: return "']'";
        case Tok::Semicolon: return "';'";
        case Tok::Comma: return "','";
        case Tok::Dot: return "'.'";
        case Tok::Arrow: return "'->'";
        case Tok::Colon: return "':'";
        case Tok::ColonColon: return "'::'";
        case Tok::Question: return "'?'";
        case Tok::Assign: return "'='";
        case Tok::Star: return "'*'";
        default: break;
    }
    for (const Keyword& k : kKeywords) {
        if (k.token == t)
            return k.word;
    }
    for (const Keyword& k : kCppKeywords) {
        if (k.token == t)
            return k.word;
    }
    return "operator";
}

Lexer::Lexer(std::string_view source, std::string name, bool cpp)
    : src_(source), name_(std::move(name)), cpp_(cpp)
{}

void Lexer::fail(const std::string& sourceName, std::uint32_t line, const std::string& message)
{
    throw Error(ErrorCode::SyntaxError, sourceName + ":" + std::to_string(line) + ": " + message);
}

std::vector<Token> Lexer::tokenize()
{
    std::vector<Token> out;
    if (src_.size() >= 3 && src_[0] == '\xEF' && src_[1] == '\xBB' && src_[2] == '\xBF')
        pos_ = 3;
    while (readToken(out)) {
    }
    Token eof;
    eof.type = Tok::Eof;
    eof.line = line_;
    eof.column = std::uint32_t(pos_ - lineStart_ + 1);
    out.push_back(std::move(eof));
    return out;
}

void Lexer::skipSpace()
{
    for (;;) {
        const char c = peek();
        if (c == '\n') {
            ++pos_;
            ++line_;
            lineStart_ = pos_;
            atLineStart_ = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            ++pos_;
            continue;
        }
        if (c == '\\' && (peek(1) == '\n' || (peek(1) == '\r' && peek(2) == '\n'))) {
            pos_ += peek(1) == '\r' ? 3 : 2;
            ++line_;
            lineStart_ = pos_;
            continue;
        }
        if (c == '/' && peek(1) == '/') {
            while (pos_ < src_.size() && src_[pos_] != '\n')
                ++pos_;
            continue;
        }
        if (c == '/' && peek(1) == '*') {
            pos_ += 2;
            while (pos_ < src_.size() && !(src_[pos_] == '*' && peek(1) == '/')) {
                if (src_[pos_] == '\n') {
                    ++line_;
                    lineStart_ = pos_ + 1;
                }
                ++pos_;
            }
            if (pos_ >= src_.size())
                fail("unterminated comment");
            pos_ += 2;
            continue;
        }
        return;
    }
}

/// A `#` line. `#define NAME replacement` records an object-like macro whose
/// replacement is tokenized (and itself macro-expanded) here; everything else
/// is skipped to the end of the line.
void Lexer::directive(std::vector<Token>&)
{
    ++pos_;  // '#'
    while (peek() == ' ' || peek() == '\t')
        ++pos_;
    std::size_t start = pos_;
    while (isIdentChar(peek()))
        ++pos_;
    const std::string_view word = src_.substr(start, pos_ - start);
    auto restOfLine = [&] {
        // A backslash-newline continues the directive.
        for (;;) {
            while (pos_ < src_.size() && src_[pos_] != '\n')
                ++pos_;
            if (pos_ >= 1 && src_[pos_ - 1] == '\\' && pos_ < src_.size()) {
                ++pos_;
                ++line_;
                lineStart_ = pos_;
                continue;
            }
            break;
        }
    };
    if (word == "define") {
        while (peek() == ' ' || peek() == '\t')
            ++pos_;
        start = pos_;
        while (isIdentChar(peek()))
            ++pos_;
        if (start == pos_)
            fail("expected a macro name after #define");
        const String name = fromUtf8(src_.substr(start, pos_ - start));
        if (peek() == '(')
            fail("function-like macros are not supported: #define " + toUtf8(name) + "(...)");
        // Tokenize the replacement with a nested lexer that knows the macros so
        // far, so a macro defined in terms of another expands once, here.
        const std::size_t bodyStart = pos_;
        restOfLine();
        std::string body(src_.substr(bodyStart, pos_ - bodyStart));
        for (std::size_t i = 0; i + 1 < body.size(); ++i) {
            if (body[i] == '\\' && body[i + 1] == '\n')
                body[i] = ' ';
        }
        Lexer inner(body, name_, cpp_);
        inner.macros_ = macros_;
        inner.line_ = line_;
        std::vector<Token> replacement = inner.tokenize();
        replacement.pop_back();  // Eof
        macros_[name] = std::move(replacement);
        return;
    }
    if (word == "undef") {
        while (peek() == ' ' || peek() == '\t')
            ++pos_;
        start = pos_;
        while (isIdentChar(peek()))
            ++pos_;
        macros_.erase(fromUtf8(src_.substr(start, pos_ - start)));
        restOfLine();
        return;
    }
    // #include, #ifdef, #ifndef, #if, #else, #endif, #pragma, #error, #line:
    // the library is built in and the guards guard nothing here.
    restOfLine();
}

void Lexer::emit(std::vector<Token>& out, Token token)
{
    if (token.type == Tok::Identifier) {
        auto it = macros_.find(token.text);
        if (it != macros_.end()) {
            if (++expansionDepth_ > 64)
                fail("macro expansion too deep: " + toUtf8(token.text));
            for (Token t : it->second) {
                t.line = token.line;
                t.column = token.column;
                emit(out, std::move(t));
            }
            --expansionDepth_;
            return;
        }
    }
    out.push_back(std::move(token));
}

bool Lexer::readToken(std::vector<Token>& out)
{
    skipSpace();
    if (pos_ >= src_.size())
        return false;
    if (peek() == '#' && atLineStart_) {
        directive(out);
        return true;
    }
    atLineStart_ = false;

    const char c = peek();
    const std::uint32_t column = std::uint32_t(pos_ - lineStart_ + 1);
    Token t;
    if (isDigit(c) || (c == '.' && isDigit(peek(1)))) {
        t = readNumber();
    } else if (isIdentStart(c)) {
        t = readIdentifier();
    } else if (c == '\'') {
        t = readChar();
    } else if (c == '"') {
        t = readString();
    } else {
        struct Op
        {
            const char* text;
            Tok token;
        };
        static constexpr Op ops[] = {
            {"...", Tok::Ellipsis},   {"<<=", Tok::ShlAssign},  {">>=", Tok::ShrAssign},
            {"->", Tok::Arrow},       {"++", Tok::PlusPlus},    {"--", Tok::MinusMinus},
            {"<<", Tok::Shl},         {">>", Tok::Shr},         {"<=", Tok::Le},
            {">=", Tok::Ge},          {"==", Tok::EqEq},        {"!=", Tok::NotEq},
            {"&&", Tok::AndAnd},      {"||", Tok::OrOr},        {"+=", Tok::PlusAssign},
            {"-=", Tok::MinusAssign}, {"*=", Tok::StarAssign},  {"/=", Tok::SlashAssign},
            {"%=", Tok::PercentAssign}, {"&=", Tok::AmpAssign}, {"|=", Tok::PipeAssign},
            {"^=", Tok::CaretAssign}, {"{", Tok::LBrace},       {"}", Tok::RBrace},
            {"(", Tok::LParen},       {")", Tok::RParen},       {"[", Tok::LBracket},
            {"]", Tok::RBracket},     {";", Tok::Semicolon},    {",", Tok::Comma},
            {".", Tok::Dot},          {"::", Tok::ColonColon},  {":", Tok::Colon},
            {"?", Tok::Question},
            {"=", Tok::Assign},       {"+", Tok::Plus},         {"-", Tok::Minus},
            {"*", Tok::Star},         {"/", Tok::Slash},        {"%", Tok::Percent},
            {"&", Tok::Amp},          {"|", Tok::Pipe},         {"^", Tok::Caret},
            {"~", Tok::Tilde},        {"!", Tok::Bang},         {"<", Tok::Lt},
            {">", Tok::Gt},
        };
        bool found = false;
        for (const Op& op : ops) {
            const std::size_t n = std::strlen(op.text);
            if (src_.compare(pos_, n, op.text) == 0) {
                t.type = op.token;
                pos_ += n;
                found = true;
                break;
            }
        }
        if (!found)
            fail(std::string("unexpected character '") + c + "'");
    }
    t.line = line_;
    t.column = column;
    emit(out, std::move(t));
    return true;
}

Token Lexer::readNumber()
{
    Token t;
    const std::size_t start = pos_;
    bool isFloat = false;
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        pos_ += 2;
        std::uint64_t value = 0;
        while (base::hexValue(peek()) >= 0) {
            value = value * 16 + std::uint64_t(base::hexValue(peek()));
            ++pos_;
        }
        t.type = Tok::IntLit;
        t.intValue = std::int64_t(value);
    } else if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
        pos_ += 2;
        std::uint64_t value = 0;
        while (peek() == '0' || peek() == '1') {
            value = value * 2 + std::uint64_t(peek() - '0');
            ++pos_;
        }
        t.type = Tok::IntLit;
        t.intValue = std::int64_t(value);
    } else {
        while (isDigit(peek()))
            ++pos_;
        if (peek() == '.' && peek(1) != '.') {
            isFloat = true;
            ++pos_;
            while (isDigit(peek()))
                ++pos_;
        }
        if ((peek() == 'e' || peek() == 'E') &&
            (isDigit(peek(1)) || ((peek(1) == '+' || peek(1) == '-') && isDigit(peek(2))))) {
            isFloat = true;
            pos_ += 2;
            while (isDigit(peek()))
                ++pos_;
        }
        const std::string text(src_.substr(start, pos_ - start));
        if (isFloat) {
            t.type = Tok::FloatLit;
            t.realValue = base::parseDouble(text).value;
        } else {
            t.type = Tok::IntLit;
            // A leading zero means octal in C.
            const int base = text.size() > 1 && text[0] == '0' ? 8 : 10;
            t.intValue = std::int64_t(std::strtoull(text.c_str(), nullptr, base));
        }
    }
    // Suffixes: u, l, ll, f, in any case and (for integers) either order.
    for (;;) {
        const char s = peek();
        if (s == 'u' || s == 'U') {
            t.isUnsigned = true;
        } else if (s == 'l' || s == 'L') {
            t.isLong = true;
            if (peek(1) == 'l' || peek(1) == 'L')
                ++pos_;
        } else if ((s == 'f' || s == 'F') && t.type == Tok::FloatLit) {
            t.isFloat = true;
        } else {
            break;
        }
        ++pos_;
    }
    if (isIdentChar(peek()))
        fail("invalid suffix on number literal");
    if (t.type == Tok::IntLit && !t.isLong && !t.isUnsigned &&
        (t.intValue > 2147483647LL || t.intValue < -2147483648LL))
        t.isLong = true;  // too big for int: C gives it type long
    if (t.type == Tok::IntLit && t.isUnsigned && !t.isLong && t.intValue > 4294967295LL)
        t.isLong = true;
    return t;
}

std::uint32_t Lexer::readEscape()
{
    ++pos_;  // backslash
    const char e = peek();
    ++pos_;
    switch (e) {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7': {
            std::uint32_t v = std::uint32_t(e - '0');
            for (int k = 0; k < 2 && peek() >= '0' && peek() <= '7'; ++k)
                v = v * 8 + std::uint32_t(src_[pos_++] - '0');
            return v;
        }
        case 'a': return '\a';
        case 'b': return '\b';
        case 'f': return '\f';
        case 'v': return '\v';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"': return '"';
        case '?': return '?';
        case 'x': {
            std::uint32_t v = 0;
            int count = 0;
            while (base::hexValue(peek()) >= 0) {
                v = v * 16 + std::uint32_t(base::hexValue(peek()));
                ++pos_;
                ++count;
            }
            if (count == 0)
                fail("\\x used with no following hex digits");
            return v;
        }
        case 'u':
        case 'U': {
            const int count = e == 'u' ? 4 : 8;
            std::uint32_t v = 0;
            for (int k = 0; k < count; ++k) {
                if (base::hexValue(peek()) < 0)
                    fail("incomplete universal character name");
                v = v * 16 + std::uint32_t(base::hexValue(peek()));
                ++pos_;
            }
            return v;
        }
        default: fail(std::string("unknown escape sequence '\\") + e + "'");
    }
}

Token Lexer::readChar()
{
    Token t;
    t.type = Tok::CharLit;
    ++pos_;  // opening quote
    if (peek() == '\'' || peek() == '\n' || peek() == '\0')
        fail("empty character constant");
    std::uint32_t value = 0;
    if (peek() == '\\') {
        value = readEscape();
    } else {
        // One UTF-8 sequence.
        const unsigned char lead = static_cast<unsigned char>(peek());
        std::size_t n = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        const String decoded = fromUtf8(src_.substr(pos_, n));
        value = decoded.empty() ? 0 : decoded[0];
        pos_ += n;
    }
    if (peek() != '\'')
        fail("multi-character constants are not supported");
    ++pos_;
    t.intValue = std::int64_t(value);
    return t;
}

Token Lexer::readString()
{
    Token t;
    t.type = Tok::StringLit;
    // Adjacent literals concatenate: "a" "b".
    for (;;) {
        ++pos_;  // opening quote
        std::string raw;
        for (;;) {
            if (pos_ >= src_.size() || peek() == '\n')
                fail("unterminated string literal");
            const char c = peek();
            if (c == '"')
                break;
            if (c == '\\') {
                const std::size_t before = pos_;
                const std::uint32_t v = readEscape();
                (void)before;
                String one;
                base::appendCodePointUtf16(one, v);
                raw += toUtf8(one);
                continue;
            }
            raw.push_back(c);
            ++pos_;
        }
        ++pos_;  // closing quote
        t.text += fromUtf8(raw);
        // Look past whitespace and comments for another literal.
        const std::size_t save = pos_;
        const std::uint32_t saveLine = line_;
        const std::size_t saveStart = lineStart_;
        const bool saveAtLineStart = atLineStart_;
        skipSpace();
        if (peek() == '"')
            continue;
        pos_ = save;
        line_ = saveLine;
        lineStart_ = saveStart;
        atLineStart_ = saveAtLineStart;
        break;
    }
    return t;
}

Token Lexer::readIdentifier()
{
    Token t;
    const std::size_t start = pos_;
    while (isIdentChar(peek()))
        ++pos_;
    t.text = fromUtf8(src_.substr(start, pos_ - start));
    t.type = keywordOf(t.text, cpp_);
    return t;
}

}  // namespace c
}  // namespace internal
}  // namespace script
