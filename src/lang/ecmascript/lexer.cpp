// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Tokenizer.

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

#include "base/ascii.hpp"
#include "base/number.hpp"
#include "base/utf16.hpp"
#include "script/internal/dyn/lexer.hpp"
#include "script/internal/dyn/numconv.hpp"

namespace script {
namespace internal {
namespace dyn {

namespace {

bool isIdStart(unsigned char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 0x80;
}
bool isIdPart(unsigned char c) noexcept
{
    return isIdStart(c) || (c >= '0' && c <= '9');
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
        {"break", Tok::Break},
        {"case", Tok::Case},
        {"catch", Tok::Catch},
        {"class", Tok::Class},
        {"const", Tok::Const},
        {"continue", Tok::Continue},
        {"debugger", Tok::Debugger},
        {"default", Tok::Default},
        {"delete", Tok::Delete},
        {"do", Tok::Do},
        {"else", Tok::Else},
        {"extends", Tok::Extends},
        {"finally", Tok::Finally},
        {"for", Tok::For},
        {"function", Tok::Function},
        {"if", Tok::If},
        {"in", Tok::In},
        {"instanceof", Tok::InstanceOf},
        {"let", Tok::Let},
        {"new", Tok::New},
        {"return", Tok::Return},
        {"static", Tok::Static},
        {"super", Tok::Super},
        {"switch", Tok::Switch},
        {"this", Tok::This},
        {"throw", Tok::Throw},
        {"try", Tok::Try},
        {"typeof", Tok::TypeOf},
        {"var", Tok::Var},
        {"void", Tok::Void},
        {"while", Tok::While},
        {"with", Tok::With},
        {"yield", Tok::Yield},
        {"async", Tok::Async},
        {"await", Tok::Await},
        {"of", Tok::Of},
        {"get", Tok::Get},
        {"set", Tok::Set},
        {"true", Tok::True},
        {"false", Tok::False},
        {"null", Tok::Null},

        {"interface", Tok::Interface},
        {"type", Tok::Type},
        {"enum", Tok::Enum},
        {"implements", Tok::Implements},
        {"declare", Tok::Declare},
        {"namespace", Tok::Namespace},
        {"abstract", Tok::Abstract},
        {"readonly", Tok::Readonly},
        {"keyof", Tok::KeyOf},
        {"as", Tok::As},
        {"satisfies", Tok::Satisfies},
        {"is", Tok::Is},
        {"public", Tok::Public},
        {"private", Tok::Private},
        {"protected", Tok::Protected},
        {"override", Tok::Override},
        {"export", Tok::Export},
    };
    return table;
}

}  // namespace

const char* tokenName(Tok t) noexcept
{
    switch (t) {
        case Tok::Eof: return "end of input";
        case Tok::Identifier: return "identifier";
        case Tok::PrivateName: return "private name";
        case Tok::Number: return "number";
        case Tok::StringLit: return "string";
        case Tok::Template: return "template literal";
        case Tok::Regex: return "regular expression";
        case Tok::LBrace: return "'{'";
        case Tok::RBrace: return "'}'";
        case Tok::LParen: return "'('";
        case Tok::RParen: return "')'";
        case Tok::LBracket: return "'['";
        case Tok::RBracket: return "']'";
        case Tok::Semicolon: return "';'";
        case Tok::Comma: return "','";
        case Tok::Dot: return "'.'";
        case Tok::Ellipsis: return "'...'";
        case Tok::Colon: return "':'";
        case Tok::Arrow: return "'=>'";
        case Tok::Assign: return "'='";
        default: return "token";
    }
}

Tok keywordOf(StringView word) noexcept
{
    if (word.empty() || word.size() > 10)
        return Tok::Identifier;
    std::string narrow;
    narrow.reserve(word.size());
    for (Char c : word) {
        if (c > 0x7f)
            return Tok::Identifier;
        narrow.push_back(char(c));
    }
    const auto& table = keywordTable();
    auto it = table.find(narrow);
    return it == table.end() ? Tok::Identifier : it->second;
}

Lexer::Lexer(std::string_view source, std::string name) : src_(source), name_(std::move(name)) {}

void Lexer::fail(const std::string& message) const
{
    fail(name_, line_, message);
}

void Lexer::fail(const std::string& sourceName, std::uint32_t line, const std::string& message)
{
    throw Error(ErrorCode::SyntaxError, sourceName + ":" + std::to_string(line) + ": " + message);
}

/// The characters beyond ASCII that separate tokens, in UTF-8, stepping over one if it is next.
bool Lexer::unicodeSpace(bool& sawNewline)
{
    const auto a = static_cast<unsigned char>(peek());
    const auto b = static_cast<unsigned char>(peek(1));
    const auto c = static_cast<unsigned char>(peek(2));
    if (a == 0xC2 && b == 0xA0) {  // no-break space
        pos_ += 2;
        return true;
    }
    if (a == 0xE2 && b == 0x80 && (c == 0xA8 || c == 0xA9)) {  // line and paragraph separators
        ++line_;
        sawNewline = true;
        pos_ += 3;
        return true;
    }
    const bool space = (a == 0xEF && b == 0xBB && c == 0xBF) || (a == 0xE1 && b == 0x9A && c == 0x80) ||
                       (a == 0xE2 && b == 0x80 && ((c >= 0x80 && c <= 0x8A) || c == 0xAF)) ||
                       (a == 0xE2 && b == 0x81 && c == 0x9F) || (a == 0xE3 && b == 0x80 && c == 0x80);
    if (space)
        pos_ += 3;
    return space;
}

bool Lexer::skipSpace()
{
    bool sawNewline = false;
    for (;;) {
        const char c = peek();
        if (c == '\n') {
            ++line_;
            sawNewline = true;
            ++pos_;
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
                if (src_[pos_] == '\n') {
                    ++line_;
                    sawNewline = true;
                } else if (src_[pos_] == '*' && peek(1) == '/') {
                    pos_ += 2;
                    break;
                }
                ++pos_;
            }
        } else if (static_cast<unsigned char>(c) >= 0xC2 && unicodeSpace(sawNewline)) {
            // Taken as it went by.
        } else {
            return sawNewline;
        }
    }
}

void Lexer::readEscape(String& out)
{
    const char c = peek();
    ++pos_;
    switch (c) {
        case 'n': out.push_back(u'\n'); return;
        case 't': out.push_back(u'\t'); return;
        case 'r': out.push_back(u'\r'); return;
        case 'b': out.push_back(u'\b'); return;
        case 'f': out.push_back(u'\f'); return;
        case 'v': out.push_back(u'\v'); return;
        case '0':
            if (!isDigit(peek())) {
                out.push_back(u'\0');
                return;
            }
            fail("octal escapes are not supported");
            return;
        case '\n': ++line_; return;  // line continuation
        case 'x': {
            if (!isHex(peek()) || !isHex(peek(1)))
                fail("malformed \\x escape");
            out.push_back(Char(base::hexValue(peek()) * 16 + base::hexValue(peek(1))));
            pos_ += 2;
            return;
        }
        case 'u': {
            if (peek() == '{') {
                ++pos_;
                char32_t cp = 0;
                if (!isHex(peek()))
                    fail("malformed \\u{} escape");
                while (isHex(peek())) {
                    cp = cp * 16 + char32_t(base::hexValue(peek()));
                    ++pos_;
                    if (cp > 0x10ffff)
                        fail("code point out of range");
                }
                if (peek() != '}')
                    fail("unterminated \\u{} escape");
                ++pos_;
                base::appendCodePointUtf16(out, cp);
                return;
            }
            for (int i = 0; i < 4; ++i) {
                if (!isHex(peek(std::size_t(i))))
                    fail("malformed \\u escape");
            }
            Char v = 0;
            for (int i = 0; i < 4; ++i)
                v = Char(v * 16 + base::hexValue(peek(std::size_t(i))));
            pos_ += 4;
            out.push_back(v);
            return;
        }
        default: out += fromUtf8(std::string_view(src_.data() + pos_ - 1, 1)); return;
    }
}

Token Lexer::readString(char quote)
{
    Token t;
    t.type = Tok::StringLit;
    t.line = line_;
    ++pos_;
    for (;;) {
        if (pos_ >= src_.size())
            fail("unterminated string");
        const char c = src_[pos_];
        if (c == quote) {
            ++pos_;
            break;
        }
        if (c == '\n')
            fail("unterminated string");
        if (c == '\\') {
            ++pos_;
            readEscape(t.text);
            continue;
        }
        std::size_t start = pos_;
        while (pos_ < src_.size() && src_[pos_] != quote && src_[pos_] != '\\' &&
               src_[pos_] != '\n')
            ++pos_;
        t.text += fromUtf8(src_.substr(start, pos_ - start));
    }
    return t;
}

Token Lexer::readTemplate()
{
    Token t;
    t.type = Tok::Template;
    t.line = line_;
    ++pos_;  // opening backtick
    String chunk;
    std::size_t rawStart = pos_;
    const auto keepRaw = [&](std::size_t end) {
        std::string raw(src_.substr(rawStart, end - rawStart));
        // Line endings come out as one, whatever they were in the file.
        std::string plain;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\r') {
                plain.push_back('\n');
                if (i + 1 < raw.size() && raw[i + 1] == '\n')
                    ++i;
            } else {
                plain.push_back(raw[i]);
            }
        }
        t.raws.push_back(fromUtf8(plain));
    };
    for (;;) {
        if (pos_ >= src_.size())
            fail("unterminated template literal");
        const char c = src_[pos_];
        if (c == '`') {
            keepRaw(pos_);
            ++pos_;
            t.parts.push_back(chunk);
            break;
        }
        if (c == '\\') {
            ++pos_;
            readEscape(chunk);
            continue;
        }
        if (c == '$' && peek(1) == '{') {
            t.parts.push_back(chunk);
            keepRaw(pos_);
            chunk.clear();
            pos_ += 2;
            const std::size_t start = pos_;
            // Scan to the matching brace, stepping over nested braces, strings
            // and nested template literals.
            int depth = 1;
            while (pos_ < src_.size() && depth > 0) {
                const char e = src_[pos_];
                if (e == '{') {
                    ++depth;
                } else if (e == '}') {
                    if (--depth == 0)
                        break;
                } else if (e == '\'' || e == '"') {
                    const char q = e;
                    ++pos_;
                    while (pos_ < src_.size() && src_[pos_] != q) {
                        if (src_[pos_] == '\\')
                            ++pos_;
                        ++pos_;
                    }
                } else if (e == '`') {
                    ++pos_;
                    int nested = 0;
                    while (pos_ < src_.size()) {
                        if (src_[pos_] == '\\') {
                            ++pos_;
                        } else if (src_[pos_] == '$' && pos_ + 1 < src_.size() &&
                                   src_[pos_ + 1] == '{') {
                            ++nested;
                            ++pos_;
                        } else if (src_[pos_] == '}' && nested > 0) {
                            --nested;
                        } else if (src_[pos_] == '`' && nested == 0) {
                            break;
                        } else if (src_[pos_] == '\n') {
                            ++line_;
                        }
                        ++pos_;
                    }
                } else if (e == '\n') {
                    ++line_;
                }
                ++pos_;
            }
            if (pos_ >= src_.size())
                fail("unterminated template substitution");
            t.exprs.emplace_back(src_.substr(start, pos_ - start));
            ++pos_;  // closing brace
            rawStart = pos_;
            continue;
        }
        if (c == '\n')
            ++line_;
        const std::size_t start = pos_;
        while (pos_ < src_.size() && src_[pos_] != '`' && src_[pos_] != '\\' &&
               !(src_[pos_] == '$' && peek(1) == '{')) {
            if (src_[pos_] == '\n' && pos_ != start)
                ++line_;
            ++pos_;
        }
        chunk += fromUtf8(src_.substr(start, pos_ - start));
    }
    return t;
}

Token Lexer::readNumber()
{
    Token t;
    t.type = Tok::Number;
    t.line = line_;
    const std::size_t start = pos_;

    auto digits = [&](bool (*ok)(char)) {
        while (pos_ < src_.size() && (ok(src_[pos_]) || src_[pos_] == '_'))
            ++pos_;
    };

    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X' || peek(1) == 'o' || peek(1) == 'O' ||
                          peek(1) == 'b' || peek(1) == 'B')) {
        const char kind = peek(1);
        pos_ += 2;
        const std::size_t digitStart = pos_;
        digits(+[](char c) { return isHex(c); });
        const int radix = (kind == 'x' || kind == 'X') ? 16 : (kind == 'o' || kind == 'O') ? 8 : 2;
        double acc = 0;
        bool any = false;
        for (std::size_t i = digitStart; i < pos_; ++i) {
            if (src_[i] == '_')
                continue;
            const int v = base::hexValue(src_[i]);
            if (v >= radix)
                fail("invalid digit in numeric literal");
            acc = acc * radix + v;
            any = true;
        }
        if (!any)
            fail("missing digits in numeric literal");
        t.number = acc;
    } else {
        digits(+[](char c) { return isDigit(c); });
        if (peek() == '.') {
            ++pos_;
            digits(+[](char c) { return isDigit(c); });
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-')
                ++pos_;
            if (!isDigit(peek()))
                fail("missing exponent digits");
            digits(+[](char c) { return isDigit(c); });
        }
        std::string text;
        for (std::size_t i = start; i < pos_; ++i) {
            if (src_[i] != '_')
                text.push_back(src_[i]);
        }
        t.number = base::parseDouble(text).value;
    }

    if (peek() == 'n')
        fail("BigInt literals are not supported");
    if (isIdStart(static_cast<unsigned char>(peek())))
        fail("identifier immediately after a number");
    return t;
}

Token Lexer::readIdentifier()
{
    Token t;
    t.line = line_;
    const std::size_t start = pos_;
    while (pos_ < src_.size() && isIdPart(static_cast<unsigned char>(src_[pos_])))
        ++pos_;
    t.text = fromUtf8(src_.substr(start, pos_ - start));
    // `\u0061` and `\u{61}` are the letter they name, in any part of a name.
    bool escaped = false;
    while (peek() == '\\') {
        if (peek(1) != 'u')
            fail("unexpected character '\\'");
        pos_ += 2;
        std::uint32_t code = 0;
        if (peek() == '{') {
            ++pos_;
            int digits = 0;
            while (peek() != '}') {
                const int d = std::isxdigit(static_cast<unsigned char>(peek())) ? base::hexValue(peek()) : -1;
                if (d < 0 || (code = code * 16 + std::uint32_t(d)) > 0x10FFFF)
                    fail("invalid Unicode escape in a name");
                ++pos_;
                ++digits;
            }
            ++pos_;
            if (!digits)
                fail("invalid Unicode escape in a name");
        } else {
            for (int i = 0; i < 4; ++i, ++pos_) {
                const int d = std::isxdigit(static_cast<unsigned char>(peek())) ? base::hexValue(peek()) : -1;
                if (d < 0)
                    fail("invalid Unicode escape in a name");
                code = code * 16 + std::uint32_t(d);
            }
        }
        const bool fits = code >= 0x80 ? !(code >= 0xD800 && code <= 0xDFFF)
                          : t.text.empty() ? isIdStart(static_cast<unsigned char>(code)) : isIdPart(static_cast<unsigned char>(code));
        if (!fits || code == 0x2028 || code == 0x2029)
            fail("that escape does not name a letter of a name");
        if (code >= 0x10000) {
            t.text.push_back(Char(0xD800 + ((code - 0x10000) >> 10)));
            t.text.push_back(Char(0xDC00 + ((code - 0x10000) & 0x3FF)));
        } else {
            t.text.push_back(Char(code));
        }
        escaped = true;
        const std::size_t more = pos_;
        while (pos_ < src_.size() && isIdPart(static_cast<unsigned char>(src_[pos_])))
            ++pos_;
        t.text += fromUtf8(src_.substr(more, pos_ - more));
    }
    t.type = keywordOf(t.text);
    // A word spelled with an escape is a name, and not the keyword: a property may be called that, a variable may not.
    if (escaped && t.type != Tok::Identifier) {
        t.escapedKeyword = true;
        t.type = Tok::Identifier;
    }
    return t;
}

Token Lexer::next(bool regexAllowed)
{
    const bool newlineBefore = skipSpace();
    Token t = readToken(regexAllowed);
    t.newlineBefore = newlineBefore;
    return t;
}

Token Lexer::readToken(bool regexAllowed)
{
    Token t;
    t.line = line_;
    if (pos_ >= src_.size())
        return t;

    const char c = src_[pos_];
    if (isIdStart(static_cast<unsigned char>(c)) || c == '\\')
        return readIdentifier();
    if (isDigit(c) || (c == '.' && isDigit(peek(1))))
        return readNumber();
    if (c == '"' || c == '\'')
        return readString(c);
    if (c == '`')
        return readTemplate();
    if (c == '#') {
        ++pos_;
        Token id = readIdentifier();
        id.type = Tok::PrivateName;
        return id;
    }
    if (c == '/' && regexAllowed) {
        const std::size_t save = pos_;
        ++pos_;
        bool inClass = false;
        bool ok = false;
        while (pos_ < src_.size()) {
            const char e = src_[pos_];
            if (e == '\\') {
                pos_ += 2;
                continue;
            }
            if (e == '\n')
                break;
            if (e == '[')
                inClass = true;
            else if (e == ']')
                inClass = false;
            else if (e == '/' && !inClass) {
                ++pos_;
                while (pos_ < src_.size() && isIdPart(static_cast<unsigned char>(src_[pos_])))
                    ++pos_;
                ok = true;
                break;
            }
            ++pos_;
        }
        if (ok) {
            t.type = Tok::Regex;
            t.text = fromUtf8(src_.substr(save, pos_ - save));
            return t;
        }
        pos_ = save;
    }

    auto take = [&](std::size_t n, Tok type) {
        pos_ += n;
        t.type = type;
        return t;
    };

    switch (c) {
        case '{': return take(1, Tok::LBrace);
        case '}': return take(1, Tok::RBrace);
        case '(': return take(1, Tok::LParen);
        case ')': return take(1, Tok::RParen);
        case '[': return take(1, Tok::LBracket);
        case ']': return take(1, Tok::RBracket);
        case ';': return take(1, Tok::Semicolon);
        case ',': return take(1, Tok::Comma);
        case ':': return take(1, Tok::Colon);
        case '~': return take(1, Tok::Tilde);
        case '.':
            if (peek(1) == '.' && peek(2) == '.')
                return take(3, Tok::Ellipsis);
            return take(1, Tok::Dot);
        case '?':
            if (peek(1) == '?' && peek(2) == '=')
                return take(3, Tok::NullishAssign);
            if (peek(1) == '?')
                return take(2, Tok::QuestionQuestion);
            if (peek(1) == '.' && !isDigit(peek(2)))
                return take(2, Tok::QuestionDot);
            return take(1, Tok::Question);
        case '=':
            if (peek(1) == '=' && peek(2) == '=')
                return take(3, Tok::EqEqEq);
            if (peek(1) == '=')
                return take(2, Tok::EqEq);
            if (peek(1) == '>')
                return take(2, Tok::Arrow);
            return take(1, Tok::Assign);
        case '!':
            if (peek(1) == '=' && peek(2) == '=')
                return take(3, Tok::NotEqEq);
            if (peek(1) == '=')
                return take(2, Tok::NotEq);
            return take(1, Tok::Bang);
        case '+':
            if (peek(1) == '+')
                return take(2, Tok::PlusPlus);
            if (peek(1) == '=')
                return take(2, Tok::PlusAssign);
            return take(1, Tok::Plus);
        case '-':
            if (peek(1) == '-')
                return take(2, Tok::MinusMinus);
            if (peek(1) == '=')
                return take(2, Tok::MinusAssign);
            return take(1, Tok::Minus);
        case '*':
            if (peek(1) == '*' && peek(2) == '=')
                return take(3, Tok::StarStarAssign);
            if (peek(1) == '*')
                return take(2, Tok::StarStar);
            if (peek(1) == '=')
                return take(2, Tok::StarAssign);
            return take(1, Tok::Star);
        case '/':
            if (peek(1) == '=')
                return take(2, Tok::SlashAssign);
            return take(1, Tok::Slash);
        case '%':
            if (peek(1) == '=')
                return take(2, Tok::PercentAssign);
            return take(1, Tok::Percent);
        case '&':
            if (peek(1) == '&' && peek(2) == '=')
                return take(3, Tok::AndAssign);
            if (peek(1) == '&')
                return take(2, Tok::AndAnd);
            if (peek(1) == '=')
                return take(2, Tok::AmpAssign);
            return take(1, Tok::Amp);
        case '|':
            if (peek(1) == '|' && peek(2) == '=')
                return take(3, Tok::OrAssign);
            if (peek(1) == '|')
                return take(2, Tok::OrOr);
            if (peek(1) == '=')
                return take(2, Tok::PipeAssign);
            return take(1, Tok::Pipe);
        case '^':
            if (peek(1) == '=')
                return take(2, Tok::CaretAssign);
            return take(1, Tok::Caret);
        case '<':
            if (peek(1) == '<' && peek(2) == '=')
                return take(3, Tok::ShlAssign);
            if (peek(1) == '<')
                return take(2, Tok::Shl);
            if (peek(1) == '=')
                return take(2, Tok::Le);
            return take(1, Tok::Lt);
        case '>':
            if (peek(1) == '>' && peek(2) == '>' && peek(3) == '=')
                return take(4, Tok::UShrAssign);
            if (peek(1) == '>' && peek(2) == '>')
                return take(3, Tok::UShr);
            if (peek(1) == '>' && peek(2) == '=')
                return take(3, Tok::ShrAssign);
            if (peek(1) == '>')
                return take(2, Tok::Shr);
            if (peek(1) == '=')
                return take(2, Tok::Ge);
            return take(1, Tok::Gt);
        default: break;
    }
    fail(std::string("unexpected character '") + c + "'");
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
