// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "base/number.hpp"
#include "script/internal/typed/lexer.hpp"

#include <cstdlib>
#include <unordered_map>

#include "script/internal/typed/numconv.hpp"

namespace script {
namespace internal {
namespace typed {

namespace {

bool isIdentStart(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '@' ||
           std::uint8_t(c) >= 0x80;
}

bool isIdentPart(char c) noexcept
{
    return isIdentStart(c) || (c >= '0' && c <= '9');
}

bool isDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

bool isHex(char c) noexcept
{
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

const std::unordered_map<std::string, Tok>& keywordTable()
{
    static const std::unordered_map<std::string, Tok> table = {
        {"abstract", Tok::Abstract},   {"as", Tok::As},
        {"base", Tok::Base},           {"bool", Tok::Bool},
        {"break", Tok::Break},         {"byte", Tok::Byte},
        {"case", Tok::Case},           {"catch", Tok::Catch},
        {"char", Tok::Char},           {"checked", Tok::Checked},
        {"class", Tok::Class},         {"const", Tok::Const},
        {"continue", Tok::Continue},   {"decimal", Tok::Decimal},
        {"default", Tok::Default},     {"delegate", Tok::Delegate},
        {"do", Tok::Do},               {"double", Tok::Double},
        {"else", Tok::Else},           {"enum", Tok::Enum},
        {"explicit", Tok::Explicit},   {"extern", Tok::Extern},
        {"false", Tok::False},         {"finally", Tok::Finally},
        {"float", Tok::Float},         {"for", Tok::For},
        {"foreach", Tok::Foreach},     {"goto", Tok::Goto},
        {"if", Tok::If},               {"implicit", Tok::Implicit},
        {"in", Tok::In},               {"int", Tok::Int},
        {"interface", Tok::Interface}, {"internal", Tok::Internal},
        {"is", Tok::Is},               {"lock", Tok::Lock},
        {"long", Tok::Long},           {"namespace", Tok::Namespace},
        {"new", Tok::New},             {"null", Tok::Null},
        {"object", Tok::Object},       {"operator", Tok::Operator},
        {"out", Tok::Out},             {"override", Tok::Override},
        {"params", Tok::Params},       {"private", Tok::Private},
        {"protected", Tok::Protected}, {"public", Tok::Public},
        {"readonly", Tok::Readonly},   {"ref", Tok::Ref},
        {"return", Tok::Return},       {"sbyte", Tok::SByte},
        {"sealed", Tok::Sealed},       {"short", Tok::Short},
        {"sizeof", Tok::Sizeof},       {"static", Tok::Static},
        {"string", Tok::StringKw},     {"struct", Tok::Struct},
        {"switch", Tok::Switch},       {"this", Tok::This},
        {"throw", Tok::Throw},         {"true", Tok::True},
        {"try", Tok::Try},             {"typeof", Tok::Typeof},
        {"uint", Tok::UInt},           {"ulong", Tok::ULong},
        {"unchecked", Tok::Unchecked}, {"unsafe", Tok::Unsafe},
        {"ushort", Tok::UShort},       {"using", Tok::Using},
        {"virtual", Tok::Virtual},     {"void", Tok::Void},
        {"volatile", Tok::Volatile},   {"while", Tok::While},
        // contextual
        {"var", Tok::Var},             {"value", Tok::Value},
        {"get", Tok::Get},             {"set", Tok::Set},
        {"partial", Tok::Partial},     {"where", Tok::Where},
        {"yield", Tok::Yield},         {"async", Tok::Async},
        {"await", Tok::Await},         {"global", Tok::Global},
        {"nameof", Tok::Nameof},       {"when", Tok::When},
        {"record", Tok::Record},       {"init", Tok::Init},
        {"required", Tok::Required},   {"with", Tok::With},
        {"field", Tok::Field},         {"and", Tok::And},
        {"or", Tok::Or},               {"not", Tok::Not},
    };
    return table;
}

}  // namespace

const char* tokenName(Tok t) noexcept
{
    switch (t) {
        case Tok::Eof: return "end of file";
        case Tok::Identifier: return "identifier";
        case Tok::IntLit:
        case Tok::LongLit:
        case Tok::UIntLit:
        case Tok::ULongLit:
        case Tok::FloatLit:
        case Tok::DoubleLit:
        case Tok::DecimalLit: return "number";
        case Tok::CharLit: return "character literal";
        case Tok::StringLit:
        case Tok::InterpolatedLit: return "string literal";
        case Tok::LBrace: return "{";
        case Tok::RBrace: return "}";
        case Tok::LParen: return "(";
        case Tok::RParen: return ")";
        case Tok::LBracket: return "[";
        case Tok::RBracket: return "]";
        case Tok::Semicolon: return ";";
        case Tok::Comma: return ",";
        case Tok::Dot: return ".";
        case Tok::DotDot: return "..";
        case Tok::Colon: return ":";
        case Tok::ColonColon: return "::";
        case Tok::Question: return "?";
        case Tok::QuestionDot: return "?.";
        case Tok::QuestionQuestion: return "??";
        case Tok::QuestionQuestionAssign: return "?\?=";
        case Tok::Arrow: return "=>";
        case Tok::MinusArrow: return "->";
        case Tok::Assign: return "=";
        case Tok::PlusAssign: return "+=";
        case Tok::MinusAssign: return "-=";
        case Tok::StarAssign: return "*=";
        case Tok::SlashAssign: return "/=";
        case Tok::PercentAssign: return "%=";
        case Tok::ShlAssign: return "<<=";
        case Tok::ShrAssign: return ">>=";
        case Tok::AmpAssign: return "&=";
        case Tok::PipeAssign: return "|=";
        case Tok::CaretAssign: return "^=";
        case Tok::Plus: return "+";
        case Tok::Minus: return "-";
        case Tok::Star: return "*";
        case Tok::Slash: return "/";
        case Tok::Percent: return "%";
        case Tok::PlusPlus: return "++";
        case Tok::MinusMinus: return "--";
        case Tok::Shl: return "<<";
        case Tok::Shr: return ">>";
        case Tok::Amp: return "&";
        case Tok::Pipe: return "|";
        case Tok::Caret: return "^";
        case Tok::Tilde: return "~";
        case Tok::Bang: return "!";
        case Tok::Lt: return "<";
        case Tok::Gt: return ">";
        case Tok::Le: return "<=";
        case Tok::Ge: return ">=";
        case Tok::EqEq: return "==";
        case Tok::NotEq: return "!=";
        case Tok::AndAnd: return "&&";
        case Tok::OrOr: return "||";
        default: break;
    }
    for (const auto& entry : keywordTable()) {
        if (entry.second == t)
            return entry.first.c_str();
    }
    return "token";
}

bool identifierLike(Tok t) noexcept
{
    switch (t) {
        case Tok::Identifier:
        case Tok::Var:
        case Tok::Value:
        case Tok::Get:
        case Tok::Set:
        case Tok::Partial:
        case Tok::Where:
        case Tok::Yield:
        case Tok::Async:
        case Tok::Await:
        case Tok::Global:
        case Tok::Nameof:
        case Tok::When:
        case Tok::Record:
        case Tok::Init:
        case Tok::Required:
        case Tok::With:
        case Tok::Field:
        case Tok::And:
        case Tok::Or:
        case Tok::Not: return true;
        default: return false;
    }
}

bool typeKeyword(Tok t) noexcept
{
    switch (t) {
        case Tok::Bool:
        case Tok::Byte:
        case Tok::SByte:
        case Tok::Short:
        case Tok::UShort:
        case Tok::Int:
        case Tok::UInt:
        case Tok::Long:
        case Tok::ULong:
        case Tok::Char:
        case Tok::Float:
        case Tok::Double:
        case Tok::Decimal:
        case Tok::StringKw:
        case Tok::Object:
        case Tok::Void: return true;
        default: return false;
    }
}

Tok keywordOf(StringView word) noexcept
{
    std::string narrow;
    narrow.reserve(word.size());
    for (Char c : word) {
        if (c >= 128)
            return Tok::Identifier;
        narrow.push_back(char(c));
    }
    const auto it = keywordTable().find(narrow);
    return it == keywordTable().end() ? Tok::Identifier : it->second;
}

// --------------------------------------------------------------- lexer ---

Lexer::Lexer(std::string_view source, std::string name) : src_(source), name_(std::move(name))
{
    // A UTF-8 byte order mark is not part of the program.
    if (src_.size() >= 3 && std::uint8_t(src_[0]) == 0xEF && std::uint8_t(src_[1]) == 0xBB &&
        std::uint8_t(src_[2]) == 0xBF)
        pos_ = 3;
}

void Lexer::fail(const std::string& message) const
{
    fail(name_, line_, std::uint32_t(pos_ - lineStart_ + 1), message);
}

void Lexer::fail(const std::string& sourceName, std::uint32_t line, std::uint32_t column,
                 const std::string& message)
{
    throw Error(ErrorCode::SyntaxError, sourceName + "(" + std::to_string(line) + "," +
                                            std::to_string(column) + "): error CS1002: " + message);
}

Token Lexer::make(Tok type, std::size_t length) noexcept
{
    Token t;
    t.type = type;
    t.line = line_;
    t.column = std::uint32_t(pos_ - lineStart_ + 1);
    pos_ += length;
    return t;
}

void Lexer::skipSpace()
{
    for (;;) {
        const char c = peek();
        if (c == '\n') {
            ++line_;
            ++pos_;
            lineStart_ = pos_;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            ++pos_;
        } else if (c == '/' && peek(1) == '/') {
            while (pos_ < src_.size() && src_[pos_] != '\n')
                ++pos_;
        } else if (c == '/' && peek(1) == '*') {
            pos_ += 2;
            for (;;) {
                if (pos_ >= src_.size())
                    fail("unterminated comment");
                if (src_[pos_] == '*' && peek(1) == '/') {
                    pos_ += 2;
                    break;
                }
                if (src_[pos_] == '\n') {
                    ++line_;
                    lineStart_ = pos_ + 1;
                }
                ++pos_;
            }
        } else if (c == '#') {
            // Preprocessor directives are not interpreted; the line is skipped
            // so that `#region`, `#pragma` and friends do not stop a build.
            while (pos_ < src_.size() && src_[pos_] != '\n')
                ++pos_;
        } else {
            return;
        }
    }
}

std::vector<Token> Lexer::tokenize()
{
    std::vector<Token> tokens;
    for (;;) {
        Token t = next();
        const bool done = t.type == Tok::Eof;
        tokens.push_back(std::move(t));
        if (done)
            return tokens;
    }
}

Token Lexer::next()
{
    skipSpace();
    if (pos_ >= src_.size())
        return make(Tok::Eof, 0);
    return readToken();
}

Token Lexer::readToken()
{
    const char c = peek();
    if (isDigit(c) || (c == '.' && isDigit(peek(1))))
        return readNumber();
    if (c == '"' && peek(1) == '"' && peek(2) == '"')
        return readRawString();
    if (c == '"')
        return readString();
    if (c == '\'')
        return readChar();
    if (c == '@' && peek(1) == '"') {
        ++pos_;
        return readVerbatimString();
    }
    if (c == '$' && peek(1) == '"')
        return readInterpolated(false);
    if ((c == '$' && peek(1) == '@' && peek(2) == '"') ||
        (c == '@' && peek(1) == '$' && peek(2) == '"'))
        return readInterpolated(true);
    if (isIdentStart(c))
        return readIdentifier();

    const char c1 = peek(1);
    const char c2 = peek(2);
    switch (c) {
        case '{': return make(Tok::LBrace, 1);
        case '}': return make(Tok::RBrace, 1);
        case '(': return make(Tok::LParen, 1);
        case ')': return make(Tok::RParen, 1);
        case '[': return make(Tok::LBracket, 1);
        case ']': return make(Tok::RBracket, 1);
        case ';': return make(Tok::Semicolon, 1);
        case ',': return make(Tok::Comma, 1);
        case '~': return make(Tok::Tilde, 1);
        case '.':
            // `..` is a range; `.5` is still a number, handled before this.
            if (c1 == '.')
                return make(Tok::DotDot, 2);
            return make(Tok::Dot, 1);
        case ':': return make(c1 == ':' ? Tok::ColonColon : Tok::Colon, c1 == ':' ? 2 : 1);
        case '?':
            if (c1 == '?' && c2 == '=')
                return make(Tok::QuestionQuestionAssign, 3);
            if (c1 == '?')
                return make(Tok::QuestionQuestion, 2);
            // `?.` is only the null-conditional operator when a member name
            // follows; `a ? .5 : 0` is a conditional over a number.
            if (c1 == '.' && !isDigit(c2))
                return make(Tok::QuestionDot, 2);
            return make(Tok::Question, 1);
        case '+':
            if (c1 == '+')
                return make(Tok::PlusPlus, 2);
            return make(c1 == '=' ? Tok::PlusAssign : Tok::Plus, c1 == '=' ? 2 : 1);
        case '-':
            if (c1 == '-')
                return make(Tok::MinusMinus, 2);
            if (c1 == '>')
                return make(Tok::MinusArrow, 2);
            return make(c1 == '=' ? Tok::MinusAssign : Tok::Minus, c1 == '=' ? 2 : 1);
        case '*': return make(c1 == '=' ? Tok::StarAssign : Tok::Star, c1 == '=' ? 2 : 1);
        case '/': return make(c1 == '=' ? Tok::SlashAssign : Tok::Slash, c1 == '=' ? 2 : 1);
        case '%': return make(c1 == '=' ? Tok::PercentAssign : Tok::Percent, c1 == '=' ? 2 : 1);
        case '^': return make(c1 == '=' ? Tok::CaretAssign : Tok::Caret, c1 == '=' ? 2 : 1);
        case '!': return make(c1 == '=' ? Tok::NotEq : Tok::Bang, c1 == '=' ? 2 : 1);
        case '=':
            if (c1 == '=')
                return make(Tok::EqEq, 2);
            return make(c1 == '>' ? Tok::Arrow : Tok::Assign, c1 == '>' ? 2 : 1);
        case '&':
            if (c1 == '&')
                return make(Tok::AndAnd, 2);
            return make(c1 == '=' ? Tok::AmpAssign : Tok::Amp, c1 == '=' ? 2 : 1);
        case '|':
            if (c1 == '|')
                return make(Tok::OrOr, 2);
            return make(c1 == '=' ? Tok::PipeAssign : Tok::Pipe, c1 == '=' ? 2 : 1);
        case '<':
            if (c1 == '<' && c2 == '=')
                return make(Tok::ShlAssign, 3);
            if (c1 == '<')
                return make(Tok::Shl, 2);
            return make(c1 == '=' ? Tok::Le : Tok::Lt, c1 == '=' ? 2 : 1);
        case '>':
            // `>>` is produced whole; the parser splits it where a type
            // argument list can close, which is the only place it matters.
            if (c1 == '>' && c2 == '=')
                return make(Tok::ShrAssign, 3);
            if (c1 == '>')
                return make(Tok::Shr, 2);
            return make(c1 == '=' ? Tok::Ge : Tok::Gt, c1 == '=' ? 2 : 1);
        default: break;
    }
    fail(std::string("unexpected character '") + c + "'");
}

Token Lexer::readIdentifier()
{
    const std::size_t start = pos_;
    const bool verbatim = peek() == '@';
    if (verbatim)
        ++pos_;
    while (pos_ < src_.size() && isIdentPart(src_[pos_]))
        ++pos_;

    Token t;
    t.line = line_;
    t.column = std::uint32_t(start - lineStart_ + 1);
    const std::size_t nameStart = start + (verbatim ? 1 : 0);
    t.text = fromUtf8(src_.substr(nameStart, pos_ - nameStart));
    // `@class` is the identifier `class`, not the keyword.
    t.type = verbatim ? Tok::Identifier : keywordOf(t.text);
    return t;
}

Token Lexer::readNumber()
{
    const std::size_t start = pos_;
    Token t;
    t.line = line_;
    t.column = std::uint32_t(start - lineStart_ + 1);

    int radix = 10;
    std::string digits;
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        radix = 16;
        pos_ += 2;
        while (isHex(peek()) || peek() == '_') {
            if (peek() != '_')
                digits.push_back(src_[pos_]);
            ++pos_;
        }
    } else if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
        radix = 2;
        pos_ += 2;
        while (peek() == '0' || peek() == '1' || peek() == '_') {
            if (peek() != '_')
                digits.push_back(src_[pos_]);
            ++pos_;
        }
    } else {
        bool real = false;
        while (isDigit(peek()) || peek() == '_') {
            if (peek() != '_')
                digits.push_back(src_[pos_]);
            ++pos_;
        }
        if (peek() == '.' && isDigit(peek(1))) {
            real = true;
            digits.push_back('.');
            ++pos_;
            while (isDigit(peek()) || peek() == '_') {
                if (peek() != '_')
                    digits.push_back(src_[pos_]);
                ++pos_;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            real = true;
            digits.push_back('e');
            ++pos_;
            if (peek() == '+' || peek() == '-')
                digits.push_back(src_[pos_++]);
            while (isDigit(peek())) {
                digits.push_back(src_[pos_]);
                ++pos_;
            }
        }
        if (real) {
            t.realValue = base::parseDouble(digits).value;
            t.type = Tok::DoubleLit;
        }
    }

    // Suffixes decide the literal's type, exactly as the language says.
    bool unsignedSuffix = false;
    bool longSuffix = false;
    for (;;) {
        const char s = peek();
        if (s == 'u' || s == 'U') {
            unsignedSuffix = true;
            ++pos_;
        } else if (s == 'l' || s == 'L') {
            longSuffix = true;
            ++pos_;
        } else if (s == 'f' || s == 'F') {
            ++pos_;
            t.realValue = t.type == Tok::DoubleLit ? t.realValue : base::parseDouble(digits).value;
            t.type = Tok::FloatLit;
            return t;
        } else if (s == 'd' || s == 'D') {
            ++pos_;
            t.realValue = t.type == Tok::DoubleLit ? t.realValue : base::parseDouble(digits).value;
            t.type = Tok::DoubleLit;
            return t;
        } else if (s == 'm' || s == 'M') {
            ++pos_;
            t.type = Tok::DecimalLit;
            return t;
        } else {
            break;
        }
    }

    if (t.type == Tok::DoubleLit)
        return t;

    std::uint64_t value = 0;
    if (!parseIntegerLiteral(digits, radix, value))
        fail("integer literal is too large");
    t.intValue = std::int64_t(value);

    if (longSuffix)
        t.type = unsignedSuffix ? Tok::ULongLit : Tok::LongLit;
    else if (unsignedSuffix)
        t.type = value > 0xffffffffull ? Tok::ULongLit : Tok::UIntLit;
    else if (value <= 0x7fffffffull)
        t.type = Tok::IntLit;
    else if (value <= 0xffffffffull)
        t.type = Tok::UIntLit;
    else if (value <= 0x7fffffffffffffffull)
        t.type = Tok::LongLit;
    else
        t.type = Tok::ULongLit;
    return t;
}

Char Lexer::readEscape()
{
    ++pos_;  // the backslash
    const char c = peek();
    ++pos_;
    switch (c) {
        case '0': return u'\0';
        case 'a': return u'\a';
        case 'b': return u'\b';
        case 'f': return u'\f';
        case 'n': return u'\n';
        case 'r': return u'\r';
        case 't': return u'\t';
        case 'v': return u'\v';
        case '\\': return u'\\';
        case '\'': return u'\'';
        case '"': return u'"';
        case 'u':
        case 'x': {
            // `\uXXXX` is exactly four digits, `\xX..` up to four.
            std::uint32_t value = 0;
            int count = 0;
            while (count < 4 && isHex(peek())) {
                const char h = src_[pos_++];
                value = value * 16 +
                        std::uint32_t(isDigit(h) ? h - '0' : (h | 0x20) - 'a' + 10);
                ++count;
            }
            if (count == 0)
                fail("expected hexadecimal digits after the escape");
            return Char(value);
        }
        default:
            fail(std::string("unrecognized escape sequence '\\") + c + "'");
    }
}

Token Lexer::readString()
{
    Token t;
    t.line = line_;
    t.column = std::uint32_t(pos_ - lineStart_ + 1);
    t.type = Tok::StringLit;
    ++pos_;  // the quote
    for (;;) {
        if (pos_ >= src_.size() || src_[pos_] == '\n')
            fail("newline in constant");
        const char c = src_[pos_];
        if (c == '"') {
            ++pos_;
            return t;
        }
        if (c == '\\') {
            t.text.push_back(readEscape());
            continue;
        }
        if (std::uint8_t(c) >= 0x80) {
            // Decode the whole multi-byte sequence in one go.
            std::size_t len = 1;
            while (pos_ + len < src_.size() && (std::uint8_t(src_[pos_ + len]) & 0xC0) == 0x80)
                ++len;
            t.text += fromUtf8(src_.substr(pos_, len));
            pos_ += len;
            continue;
        }
        t.text.push_back(Char(c));
        ++pos_;
    }
}

/// `"""..."""`: no escapes, and the closing delimiter's indentation is
/// stripped from every line, so the text can be laid out with the code.
Token Lexer::readRawString()
{
    Token t;
    t.line = line_;
    t.column = std::uint32_t(pos_ - lineStart_ + 1);
    t.type = Tok::StringLit;

    std::size_t fence = 0;
    while (pos_ + fence < src_.size() && src_[pos_ + fence] == '"')
        ++fence;
    pos_ += fence;

    // A single-line raw string starts right after the opening fence.
    const std::size_t bodyStart = pos_;
    std::size_t end = std::string::npos;
    for (std::size_t i = pos_; i + fence <= src_.size(); ++i) {
        if (src_[i] != '"')
            continue;
        std::size_t run = 0;
        while (i + run < src_.size() && src_[i + run] == '"')
            ++run;
        if (run >= fence) {
            end = i + run - fence;
            break;
        }
        i += run - 1;
    }
    if (end == std::string::npos)
        fail("unterminated raw string literal");

    std::string raw = std::string(src_.substr(bodyStart, end - bodyStart));
    for (char ch : raw) {
        if (ch == '\n') {
            ++line_;
        }
    }
    pos_ = end + fence;
    lineStart_ = pos_;

    // Multi-line form: drop the first and last newlines, then remove the
    // closing fence's indentation from every line.
    if (raw.find('\n') != std::string::npos) {
        std::size_t first = raw.find('\n');
        std::size_t last = raw.rfind('\n');
        std::string indent;
        if (last != std::string::npos)
            indent = raw.substr(last + 1);
        bool allSpace = true;
        for (char ch : indent)
            allSpace = allSpace && (ch == ' ' || ch == '\t');
        if (!allSpace)
            indent.clear();
        std::string body = raw.substr(first + 1, last > first ? last - first - 1 : 0);

        std::string out;
        std::size_t at = 0;
        while (at <= body.size()) {
            std::size_t nl = body.find('\n', at);
            std::string lineText =
                body.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
            if (!indent.empty() && lineText.compare(0, indent.size(), indent) == 0)
                lineText = lineText.substr(indent.size());
            out += lineText;
            if (nl == std::string::npos)
                break;
            out.push_back('\n');
            at = nl + 1;
        }
        raw = out;
    }
    t.text = fromUtf8(raw);
    return t;
}

Token Lexer::readVerbatimString()
{
    Token t;
    t.line = line_;
    t.column = std::uint32_t(pos_ - lineStart_ + 1);
    t.type = Tok::StringLit;
    ++pos_;  // the quote
    std::string raw;
    for (;;) {
        if (pos_ >= src_.size())
            fail("unterminated string literal");
        const char c = src_[pos_];
        if (c == '"') {
            if (peek(1) == '"') {
                raw.push_back('"');
                pos_ += 2;
                continue;
            }
            ++pos_;
            t.text = fromUtf8(raw);
            return t;
        }
        if (c == '\n') {
            ++line_;
            lineStart_ = pos_ + 1;
        }
        raw.push_back(c);
        ++pos_;
    }
}

Token Lexer::readInterpolated(bool verbatim)
{
    Token t;
    t.line = line_;
    t.column = std::uint32_t(pos_ - lineStart_ + 1);
    t.type = Tok::InterpolatedLit;
    while (peek() != '"')
        ++pos_;
    ++pos_;

    String chunk;
    std::string rawChunk;
    auto flush = [&]() {
        if (!rawChunk.empty()) {
            chunk += fromUtf8(rawChunk);
            rawChunk.clear();
        }
    };

    for (;;) {
        if (pos_ >= src_.size())
            fail("unterminated interpolated string");
        const char c = src_[pos_];
        if (c == '"') {
            if (verbatim && peek(1) == '"') {
                rawChunk.push_back('"');
                pos_ += 2;
                continue;
            }
            ++pos_;
            flush();
            t.parts.push_back(chunk);
            return t;
        }
        if (c == '{' && peek(1) == '{') {
            rawChunk.push_back('{');
            pos_ += 2;
            continue;
        }
        if (c == '}' && peek(1) == '}') {
            rawChunk.push_back('}');
            pos_ += 2;
            continue;
        }
        if (c == '{') {
            flush();
            t.parts.push_back(chunk);
            chunk.clear();
            ++pos_;

            // Scan the hole, tracking nesting and string literals so a lambda
            // or an indexer inside it does not close the interpolation early.
            const std::size_t start = pos_;
            int depth = 0;
            std::size_t formatAt = std::string::npos;
            for (;;) {
                if (pos_ >= src_.size())
                    fail("unterminated interpolation");
                const char e = src_[pos_];
                if (e == '"' || e == '\'') {
                    const char quote = e;
                    ++pos_;
                    while (pos_ < src_.size() && src_[pos_] != quote) {
                        if (src_[pos_] == '\\')
                            ++pos_;
                        ++pos_;
                    }
                    ++pos_;
                    continue;
                }
                if (e == '(' || e == '[' || e == '{') {
                    ++depth;
                } else if (e == ')' || e == ']') {
                    --depth;
                } else if (e == '}') {
                    if (depth == 0)
                        break;
                    --depth;
                } else if (e == ':' && depth == 0 && src_[pos_ - 1] != ':' && peek(1) != ':' &&
                           formatAt == std::string::npos) {
                    formatAt = pos_;
                } else if (e == '\n') {
                    ++line_;
                    lineStart_ = pos_ + 1;
                }
                ++pos_;
            }
            const std::size_t end = formatAt == std::string::npos ? pos_ : formatAt;
            t.exprs.push_back(std::string(src_.substr(start, end - start)));
            t.formats.push_back(formatAt == std::string::npos
                                    ? std::string()
                                    : std::string(src_.substr(formatAt + 1, pos_ - formatAt - 1)));
            ++pos_;  // the closing brace
            continue;
        }
        if (c == '\\' && !verbatim) {
            flush();
            chunk.push_back(readEscape());
            continue;
        }
        if (c == '\n') {
            ++line_;
            lineStart_ = pos_ + 1;
        }
        rawChunk.push_back(c);
        ++pos_;
    }
}

Token Lexer::readChar()
{
    Token t;
    t.line = line_;
    t.column = std::uint32_t(pos_ - lineStart_ + 1);
    t.type = Tok::CharLit;
    ++pos_;
    if (peek() == '\\') {
        t.intValue = std::int64_t(readEscape());
    } else if (std::uint8_t(peek()) >= 0x80) {
        const std::size_t start = pos_;
        std::size_t len = 1;
        while (start + len < src_.size() && (std::uint8_t(src_[start + len]) & 0xC0) == 0x80)
            ++len;
        const String decoded = fromUtf8(src_.substr(start, len));
        t.intValue = decoded.empty() ? 0 : std::int64_t(decoded[0]);
        pos_ = start + len;
    } else {
        t.intValue = std::int64_t(std::uint8_t(src_[pos_]));
        ++pos_;
    }
    if (peek() != '\'')
        fail("too many characters in character literal");
    ++pos_;
    return t;
}

}  // namespace typed
}  // namespace internal
}  // namespace script
