// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Python tokenizer: indentation, logical lines, numbers, strings and
// f-strings. The whole source is tokenized up front, which is what makes the
// indentation bookkeeping simple: every physical line is looked at once, at
// its start, and the bracket depth decides whether its end is a logical one.
#include "base/ascii.hpp"
#include "base/number.hpp"
#include "base/utf16.hpp"
#include "script/internal/python/lexer.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "script/error.hpp"

namespace script {
namespace internal {
namespace python {

namespace {

bool isNameStart(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           static_cast<unsigned char>(c) >= 0x80;
}

bool isNameChar(char c) noexcept
{
    return isNameStart(c) || (c >= '0' && c <= '9');
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
    {"False", Tok::False},   {"None", Tok::None},         {"True", Tok::True},
    {"and", Tok::And},       {"as", Tok::As},             {"assert", Tok::Assert},
    {"async", Tok::Async},   {"await", Tok::Await},       {"break", Tok::Break},
    {"class", Tok::Class},   {"continue", Tok::Continue}, {"def", Tok::Def},
    {"del", Tok::Del},       {"elif", Tok::Elif},         {"else", Tok::Else},
    {"except", Tok::Except}, {"finally", Tok::Finally},   {"for", Tok::For},
    {"from", Tok::From},     {"global", Tok::Global},     {"if", Tok::If},
    {"import", Tok::Import}, {"in", Tok::In},             {"is", Tok::Is},
    {"lambda", Tok::Lambda}, {"nonlocal", Tok::Nonlocal}, {"not", Tok::Not},
    {"or", Tok::Or},         {"pass", Tok::Pass},         {"raise", Tok::Raise},
    {"return", Tok::Return}, {"try", Tok::Try},           {"while", Tok::While},
    {"with", Tok::With},     {"yield", Tok::Yield},
};

}  // namespace

Tok keywordOf(StringView word) noexcept
{
    for (const Keyword& k : kKeywords) {
        const std::size_t n = std::strlen(k.word);
        if (word.size() != n)
            continue;
        bool same = true;
        for (std::size_t i = 0; i < n && same; ++i)
            same = word[i] == char16_t(k.word[i]);
        if (same)
            return k.token;
    }
    return Tok::Name;
}

const char* tokenName(Tok t) noexcept
{
    switch (t) {
        case Tok::Eof: return "end of input";
        case Tok::Newline: return "end of line";
        case Tok::Indent: return "indent";
        case Tok::Dedent: return "dedent";
        case Tok::Name: return "name";
        case Tok::Number: return "number";
        case Tok::String:
        case Tok::FString: return "string";
        case Tok::LParen: return "'('";
        case Tok::RParen: return "')'";
        case Tok::LBracket: return "'['";
        case Tok::RBracket: return "']'";
        case Tok::LBrace: return "'{'";
        case Tok::RBrace: return "'}'";
        case Tok::Comma: return "','";
        case Tok::Colon: return "':'";
        case Tok::Dot: return "'.'";
        case Tok::Semicolon: return "';'";
        case Tok::Arrow: return "'->'";
        case Tok::At: return "'@'";
        case Tok::Ellipsis: return "'...'";
        case Tok::Assign: return "'='";
        case Tok::In: return "'in'";
        case Tok::Else: return "'else'";
        case Tok::Import: return "'import'";
        default: break;
    }
    for (const Keyword& k : kKeywords) {
        if (k.token == t)
            return k.word;
    }
    return "operator";
}

Lexer::Lexer(std::string_view source, std::string name) : src_(source), name_(std::move(name)) {}

void Lexer::fail(const std::string& sourceName, std::uint32_t line, const std::string& message)
{
    throw Error(ErrorCode::SyntaxError, sourceName + ":" + std::to_string(line) + ": " + message);
}

void Lexer::push(std::vector<Token>& out, Tok type, std::size_t width)
{
    Token t;
    t.type = type;
    t.line = line_;
    out.push_back(std::move(t));
    pos_ += width;
}

std::vector<Token> Lexer::tokenize()
{
    std::vector<Token> out;
    // A UTF-8 byte order mark is not part of the program.
    if (src_.size() >= 3 && src_[0] == '\xEF' && src_[1] == '\xBB' && src_[2] == '\xBF')
        pos_ = 3;
    lineStart(out);
    while (pos_ < src_.size())
        readToken(out);
    if (!out.empty() && out.back().type != Tok::Newline)
        push(out, Tok::Newline, 0);
    while (indents_.size() > 1) {
        indents_.pop_back();
        push(out, Tok::Dedent, 0);
    }
    push(out, Tok::Eof, 0);
    return out;
}

/// At the start of a physical line outside any bracket: measure the
/// indentation, skip blank and comment-only lines, and emit the indentation
/// tokens the column implies.
void Lexer::lineStart(std::vector<Token>& out)
{
    for (;;) {
        int column = 0;
        std::size_t i = pos_;
        while (i < src_.size() && (src_[i] == ' ' || src_[i] == '\t' || src_[i] == '\f')) {
            if (src_[i] == '\t')
                column = (column / 8 + 1) * 8;
            else if (src_[i] == ' ')
                ++column;
            ++i;
        }
        if (i >= src_.size()) {
            pos_ = i;
            return;
        }
        const char c = src_[i];
        if (c == '\n' || c == '\r' || c == '#') {
            // Blank or comment-only: no token, no indentation change.
            while (i < src_.size() && src_[i] != '\n')
                ++i;
            if (i < src_.size()) {
                ++i;
                ++line_;
            }
            pos_ = i;
            continue;
        }
        if (c == '\\' && i + 1 < src_.size() && (src_[i + 1] == '\n' || src_[i + 1] == '\r')) {
            pos_ = i;
            return;  // the line continues; readToken handles the backslash
        }
        pos_ = i;
        if (column > indents_.back()) {
            indents_.push_back(column);
            push(out, Tok::Indent, 0);
        } else {
            while (column < indents_.back()) {
                indents_.pop_back();
                push(out, Tok::Dedent, 0);
            }
            if (column != indents_.back())
                fail("unindent does not match any outer indentation level");
        }
        return;
    }
}

void Lexer::readToken(std::vector<Token>& out)
{
    const char c = peek();
    switch (c) {
        case ' ':
        case '\t':
        case '\f': ++pos_; return;
        case '#':
            while (pos_ < src_.size() && src_[pos_] != '\n')
                ++pos_;
            return;
        case '\\':
            if (peek(1) == '\n' || peek(1) == '\r') {
                pos_ += peek(1) == '\r' && peek(2) == '\n' ? 3 : 2;
                ++line_;
                return;
            }
            fail("unexpected character after line continuation");
        case '\r':
            ++pos_;
            if (peek() == '\n')
                return;  // the \n does the work
            [[fallthrough]];
        case '\n':
            if (c == '\n')
                ++pos_;
            if (depth_ == 0) {
                if (!out.empty() && out.back().type != Tok::Newline &&
                    out.back().type != Tok::Indent && out.back().type != Tok::Dedent) {
                    Token t;
                    t.type = Tok::Newline;
                    t.line = line_;
                    out.push_back(std::move(t));
                }
                ++line_;
                lineStart(out);
            } else {
                ++line_;
            }
            return;
        default: break;
    }

    if (isDigit(c) || (c == '.' && isDigit(peek(1)))) {
        out.push_back(readNumber());
        return;
    }
    if (isNameStart(c)) {
        // A string prefix: one or two of r, b, u, f in either case, then a quote.
        std::size_t n = 0;
        while (n < 2 && std::strchr("rRbBuUfF", peek(n)) && peek(n) != '\0')
            ++n;
        if (n > 0 && (peek(n) == '\'' || peek(n) == '"')) {
            std::string_view prefix = src_.substr(pos_, n);
            pos_ += n;
            out.push_back(readString(prefix));
            return;
        }
        out.push_back(readName());
        return;
    }
    if (c == '\'' || c == '"') {
        out.push_back(readString({}));
        return;
    }

    struct Op
    {
        const char* text;
        Tok token;
    };
    // Longest first, so `**=` wins over `**` and `*`.
    static constexpr Op ops[] = {
        {"**=", Tok::StarStarAssign}, {"//=", Tok::DoubleSlashAssign}, {">>=", Tok::ShrAssign},
        {"<<=", Tok::ShlAssign},      {"...", Tok::Ellipsis},          {"->", Tok::Arrow},
        {":=", Tok::Walrus},          {"**", Tok::StarStar},           {"//", Tok::DoubleSlash},
        {">>", Tok::Shr},             {"<<", Tok::Shl},                {"<=", Tok::Le},
        {">=", Tok::Ge},              {"==", Tok::EqEq},               {"!=", Tok::NotEq},
        {"+=", Tok::PlusAssign},      {"-=", Tok::MinusAssign},        {"*=", Tok::StarAssign},
        {"/=", Tok::SlashAssign},     {"%=", Tok::PercentAssign},      {"&=", Tok::AmpAssign},
        {"|=", Tok::PipeAssign},      {"^=", Tok::CaretAssign},        {"@=", Tok::AtAssign},
        {"(", Tok::LParen},           {")", Tok::RParen},              {"[", Tok::LBracket},
        {"]", Tok::RBracket},         {"{", Tok::LBrace},              {"}", Tok::RBrace},
        {",", Tok::Comma},            {":", Tok::Colon},               {".", Tok::Dot},
        {";", Tok::Semicolon},        {"@", Tok::At},                  {"+", Tok::Plus},
        {"-", Tok::Minus},            {"*", Tok::Star},                {"/", Tok::Slash},
        {"%", Tok::Percent},          {"&", Tok::Amp},                 {"|", Tok::Pipe},
        {"^", Tok::Caret},            {"~", Tok::Tilde},               {"<", Tok::Lt},
        {">", Tok::Gt},               {"=", Tok::Assign},
    };
    for (const Op& op : ops) {
        const std::size_t n = std::strlen(op.text);
        if (src_.compare(pos_, n, op.text) == 0) {
            if (op.token == Tok::LParen || op.token == Tok::LBracket || op.token == Tok::LBrace)
                ++depth_;
            else if (op.token == Tok::RParen || op.token == Tok::RBracket ||
                     op.token == Tok::RBrace) {
                if (depth_ > 0)
                    --depth_;
            }
            push(out, op.token, n);
            return;
        }
    }
    fail(std::string("unexpected character '") + c + "'");
}

Token Lexer::readNumber()
{
    Token t;
    t.type = Tok::Number;
    t.line = line_;
    std::string digits;
    const std::size_t start = pos_;
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X' || peek(1) == 'o' || peek(1) == 'O' ||
                          peek(1) == 'b' || peek(1) == 'B')) {
        const char kind = char(peek(1) | 0x20);
        const int base = kind == 'x' ? 16 : kind == 'o' ? 8 : 2;
        pos_ += 2;
        while (base::hexValue(peek()) >= 0 || peek() == '_') {
            if (peek() != '_') {
                if (base::hexValue(peek()) >= base)
                    fail("invalid digit in number literal");
                digits.push_back(peek());
            }
            ++pos_;
        }
        if (digits.empty())
            fail("invalid number literal");
        double value = 0;
        for (char d : digits)
            value = value * base + base::hexValue(d);
        t.number = value;
        return t;
    }
    bool isFloat = false;
    while (isDigit(peek()) || peek() == '_') {
        if (peek() != '_')
            digits.push_back(peek());
        ++pos_;
    }
    if (peek() == '.' ) {
        isFloat = true;
        digits.push_back('.');
        ++pos_;
        while (isDigit(peek()) || peek() == '_') {
            if (peek() != '_')
                digits.push_back(peek());
            ++pos_;
        }
    }
    if ((peek() == 'e' || peek() == 'E') &&
        (isDigit(peek(1)) || ((peek(1) == '+' || peek(1) == '-') && isDigit(peek(2))))) {
        isFloat = true;
        digits.push_back('e');
        ++pos_;
        if (peek() == '+' || peek() == '-')
            digits.push_back(src_[pos_++]);
        while (isDigit(peek()))
            digits.push_back(src_[pos_++]);
    }
    if (peek() == 'j' || peek() == 'J')
        fail("complex numbers are not supported");
    if (isNameChar(peek()))
        fail("invalid number literal");
    (void)start;
    (void)isFloat;
    t.number = base::parseDouble(digits).value;
    return t;
}

Token Lexer::readName()
{
    Token t;
    t.line = line_;
    const std::size_t start = pos_;
    while (isNameChar(peek()))
        ++pos_;
    t.text = fromUtf8(src_.substr(start, pos_ - start));
    t.type = keywordOf(t.text);
    return t;
}

/// One escape sequence, with `i` on the backslash; leaves `i` after it.
void Lexer::readEscape(String& out, std::size_t& i, std::string_view body) const
{
    const char e = i + 1 < body.size() ? body[i + 1] : '\0';
    i += 2;
    switch (e) {
        case 'n': out.push_back(u'\n'); return;
        case 't': out.push_back(u'\t'); return;
        case 'r': out.push_back(u'\r'); return;
        case '\\': out.push_back(u'\\'); return;
        case '\'': out.push_back(u'\''); return;
        case '"': out.push_back(u'"'); return;
        case 'a': out.push_back(u'\a'); return;
        case 'b': out.push_back(u'\b'); return;
        case 'f': out.push_back(u'\f'); return;
        case 'v': out.push_back(u'\v'); return;
        case '0': out.push_back(u'\0'); return;
        case '\n': return;  // a backslash-newline joins the lines
        case '\r':
            if (i < body.size() && body[i] == '\n')
                ++i;
            return;
        case 'x':
        case 'u':
        case 'U': {
            const int count = e == 'x' ? 2 : e == 'u' ? 4 : 8;
            std::uint32_t cp = 0;
            for (int k = 0; k < count; ++k) {
                const int h = i < body.size() ? base::hexValue(body[i]) : -1;
                if (h < 0)
                    fail("truncated escape sequence in string literal");
                cp = cp * 16 + std::uint32_t(h);
                ++i;
            }
            base::appendCodePointUtf16(out, cp);
            return;
        }
        default:
            // Python keeps an unknown escape verbatim.
            out.push_back(u'\\');
            i -= 1;
            return;
    }
}

String Lexer::cook(std::string_view raw, bool isRaw) const
{
    if (isRaw)
        return fromUtf8(raw);
    String out;
    std::size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] == '\\') {
            readEscape(out, i, raw);
            continue;
        }
        // Copy one UTF-8 sequence at a time.
        std::size_t n = 1;
        const unsigned char lead = static_cast<unsigned char>(raw[i]);
        if (lead >= 0xF0)
            n = 4;
        else if (lead >= 0xE0)
            n = 3;
        else if (lead >= 0xC0)
            n = 2;
        out += fromUtf8(raw.substr(i, n));
        i += n;
    }
    return out;
}

Token Lexer::readString(std::string_view prefix)
{
    bool isRaw = false;
    bool isF = false;
    for (char p : prefix) {
        if (p == 'r' || p == 'R')
            isRaw = true;
        else if (p == 'f' || p == 'F')
            isF = true;
        else if (p == 'b' || p == 'B')
            fail("bytes literals are not supported");
    }
    const char quote = peek();
    const bool triple = peek(1) == quote && peek(2) == quote;
    pos_ += triple ? 3 : 1;
    const std::size_t start = pos_;
    const std::uint32_t line = line_;
    for (;;) {
        if (pos_ >= src_.size())
            fail(triple ? "unterminated triple-quoted string" : "unterminated string literal");
        const char c = src_[pos_];
        if (c == '\\') {
            if (pos_ + 1 < src_.size() && src_[pos_ + 1] == '\n')
                ++line_;
            pos_ += 2;
            continue;
        }
        if (c == '\n') {
            if (!triple)
                fail("unterminated string literal");
            ++line_;
            ++pos_;
            continue;
        }
        if (c == quote) {
            if (!triple)
                break;
            if (peek(1) == quote && peek(2) == quote)
                break;
        }
        ++pos_;
    }
    const std::string_view body = src_.substr(start, pos_ - start);
    pos_ += triple ? 3 : 1;

    Token t;
    t.line = line;
    if (isF) {
        t.type = Tok::FString;
        splitFString(body, isRaw, t);
    } else {
        t.type = Tok::String;
        t.text = cook(body, isRaw);
    }
    return t;
}

/// `parts[0] {exprs[0]:specs[0]} parts[1] ...`. Doubled braces are literal.
void Lexer::splitFString(std::string_view body, bool isRaw, Token& token) const
{
    std::string chunk;
    std::size_t i = 0;
    while (i < body.size()) {
        const char c = body[i];
        if (c == '{' && i + 1 < body.size() && body[i + 1] == '{') {
            chunk.push_back('{');
            i += 2;
            continue;
        }
        if (c == '}' && i + 1 < body.size() && body[i + 1] == '}') {
            chunk.push_back('}');
            i += 2;
            continue;
        }
        if (c == '}')
            fail("single '}' is not allowed in an f-string");
        if (c != '{') {
            if (c == '\\' && !isRaw && i + 1 < body.size()) {
                chunk.push_back(c);
                chunk.push_back(body[i + 1]);
                i += 2;
                continue;
            }
            chunk.push_back(c);
            ++i;
            continue;
        }
        // A replacement field: expression, optional !conversion, optional :spec.
        ++i;
        const std::size_t exprStart = i;
        int depth = 0;
        char inString = 0;
        std::size_t exprEnd = std::string_view::npos;
        std::size_t specStart = std::string_view::npos;
        char conv = 0;
        for (; i < body.size(); ++i) {
            const char d = body[i];
            if (inString) {
                if (d == '\\')
                    ++i;
                else if (d == inString)
                    inString = 0;
                continue;
            }
            if (d == '\'' || d == '"') {
                inString = d;
                continue;
            }
            if (d == '(' || d == '[' || d == '{') {
                ++depth;
                continue;
            }
            if (d == ')' || d == ']') {
                --depth;
                continue;
            }
            if (d == '}') {
                if (depth == 0)
                    break;
                --depth;
                continue;
            }
            if (depth == 0 && d == '!' && i + 1 < body.size() && body[i + 1] != '=' &&
                exprEnd == std::string_view::npos) {
                exprEnd = i;
                conv = body[i + 1];
                i += 1;
                continue;
            }
            if (depth == 0 && d == ':' && specStart == std::string_view::npos) {
                if (exprEnd == std::string_view::npos)
                    exprEnd = i;
                specStart = i + 1;
                // The spec runs to the closing brace of this field; it may
                // hold fields of its own (`{x:{width}}`), which nest.
                int specDepth = 0;
                ++i;
                for (; i < body.size(); ++i) {
                    if (body[i] == '{')
                        ++specDepth;
                    else if (body[i] == '}') {
                        if (specDepth == 0)
                            break;
                        --specDepth;
                    }
                }
                break;
            }
        }
        if (i >= body.size())
            fail("unterminated replacement field in f-string");
        if (exprEnd == std::string_view::npos)
            exprEnd = i;
        std::string expr(body.substr(exprStart, exprEnd - exprStart));
        // Trim so `{ x }` and `{x=}` do not confuse the expression parser.
        while (!expr.empty() && (expr.back() == ' ' || expr.back() == '\t'))
            expr.pop_back();
        if (expr.empty())
            fail("empty expression in f-string");
        bool selfDocumenting = false;
        if (expr.back() == '=') {
            selfDocumenting = true;
            expr.pop_back();
            while (!expr.empty() && (expr.back() == ' ' || expr.back() == '\t'))
                expr.pop_back();
        }
        if (selfDocumenting) {
            chunk += body.substr(exprStart, exprEnd - exprStart);
            if (!conv)
                conv = 'r';
        }
        token.parts.push_back(cook(chunk, isRaw));
        chunk.clear();
        token.exprs.push_back(expr);
        token.specs.emplace_back(specStart == std::string_view::npos
                                     ? std::string()
                                     : std::string(body.substr(specStart, i - specStart)));
        token.convs.push_back(conv);
        ++i;  // the closing brace
    }
    token.parts.push_back(cook(chunk, isRaw));
}

}  // namespace python
}  // namespace internal
}  // namespace script
