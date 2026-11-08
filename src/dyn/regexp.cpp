// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/dyn/regexp.hpp"

#include <algorithm>
#include <cstring>
#include <deque>

#include "script/internal/dyn/unicode.hpp"

namespace script {
namespace internal {
namespace dyn {
namespace regex {

namespace {

using unicode::Ranges;

constexpr std::uint32_t kInfinity = 0xffffffffu;
constexpr int kMaxNesting = 400;

bool isHigh(char32_t c) noexcept { return c >= 0xd800 && c <= 0xdbff; }
bool isLow(char32_t c) noexcept { return c >= 0xdc00 && c <= 0xdfff; }
bool isSurrogate(char32_t c) noexcept { return c >= 0xd800 && c <= 0xdfff; }
char32_t combine(char32_t hi, char32_t lo) noexcept { return 0x10000 + ((hi - 0xd800) << 10) + (lo - 0xdc00); }

// ---- sets of code points -----------------------------------------------------------------------------------------

void normalize(Ranges& r)
{
    if (r.size() < 2)
        return;
    std::sort(r.begin(), r.end());
    std::size_t out = 0;
    for (std::size_t i = 1; i < r.size(); ++i) {
        if (r[i].first <= r[out].second || r[i].first == r[out].second + 1)
            r[out].second = std::max(r[out].second, r[i].second);
        else
            r[++out] = r[i];
    }
    r.resize(out + 1);
}

/// Of a normalized set.
Ranges complementOf(const Ranges& r, char32_t top)
{
    Ranges out;
    char32_t at = 0;
    for (const auto& [lo, hi] : r) {
        if (lo > top)
            break;
        if (lo > at)
            out.emplace_back(at, lo - 1);
        at = hi + 1;
    }
    if (at <= top)
        out.emplace_back(at, top);
    return out;
}

bool contains(const Ranges& r, char32_t c)
{
    auto it = std::upper_bound(r.begin(), r.end(), c, [](char32_t v, const std::pair<char32_t, char32_t>& x) { return v < x.first; });
    return it != r.begin() && c <= std::prev(it)->second;
}

Ranges intersect(const Ranges& a, const Ranges& b)
{
    Ranges out;
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const char32_t lo = std::max(a[i].first, b[j].first);
        const char32_t hi = std::min(a[i].second, b[j].second);
        if (lo <= hi)
            out.emplace_back(lo, hi);
        if (a[i].second < b[j].second)
            ++i;
        else
            ++j;
    }
    return out;
}

void addAll(Ranges& to, const Ranges& from) { to.insert(to.end(), from.begin(), from.end()); }

/// Everything that canonicalizes as something in the set does. What makes a set of one case match the other.
void closeOverCase(Ranges& r, bool foldMode)
{
    normalize(r);
    if (r.size() == 1 && r[0].first == 0 && r[0].second >= 0xffff)
        return;
    bool ascii = !foldMode;
    for (const auto& range : r)
        ascii = ascii && range.second < 128;
    if (ascii) {
        Ranges more;
        for (const auto& [lo, hi] : r) {
            for (char32_t c = lo; c <= hi; ++c) {
                if (c >= 'a' && c <= 'z')
                    more.emplace_back(c - 32, c - 32);
                else if (c >= 'A' && c <= 'Z')
                    more.emplace_back(c + 32, c + 32);
            }
        }
        addAll(r, more);
    } else {
        Ranges more;
        for (const std::vector<char32_t>& group : unicode::caseGroups(foldMode)) {
            bool any = false;
            for (char32_t member : group)
                any = any || contains(r, member);
            if (any) {
                for (char32_t member : group)
                    more.emplace_back(member, member);
            }
        }
        addAll(r, more);
    }
    normalize(r);
}

char32_t canonical(char32_t c, bool foldMode)
{
    if (c < 128 && !foldMode)
        return c >= 'a' && c <= 'z' ? c - 32 : c;
    return foldMode ? unicode::simpleFold(c) : unicode::legacyCanonical(c);
}

bool isAsciiWord(char32_t c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

Ranges digitSet() { return {{'0', '9'}}; }
Ranges wordSet() { return {{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}}; }
Ranges spaceSet()
{
    return {{9, 13}, {32, 32}, {0xa0, 0xa0}, {0x1680, 0x1680}, {0x2000, 0x200a}, {0x2028, 0x2029}, {0x202f, 0x202f}, {0x205f, 0x205f}, {0x3000, 0x3000}, {0xfeff, 0xfeff}};
}

// ---- the tree of a pattern ------------------------------------------------------------------------------------------

enum class Kind : std::uint8_t
{
    Empty,
    Char,
    Set,
    Seq,
    Alt,
    Capture,
    Repeat,
    LineStart,
    LineEnd,
    WordBoundary,
    Look,
    BackRef,
};

struct Node
{
    Kind kind = Kind::Empty;
    bool flag = false;       ///< ignores case (Char, BackRef); greedy (Repeat); multiline (line ends); negated (Look, WordBoundary)
    bool behind = false;     ///< Look
    bool wordFold = false;   ///< WordBoundary with the extra word characters of `iu`
    char32_t code = 0;       ///< Char
    std::uint32_t group = 0; ///< Capture
    std::uint32_t min = 0, max = 0;           ///< Repeat
    std::uint32_t firstGroup = 0, endGroup = 0;  ///< Repeat: the groups inside
    Ranges set;              ///< Set
    std::vector<Node*> kids;
    std::vector<std::uint32_t> refs;  ///< BackRef
    std::u16string name;     ///< BackRef by name, until the names are all known
};

struct ClassValue
{
    Ranges set;
    std::vector<std::u32string> strings;  ///< of other than one code point
};

}  // namespace

// ---- the program ----------------------------------------------------------------------------------------------------

namespace {

enum Op : std::uint32_t
{
    OpMatch,
    OpLit,        // unit
    OpLitBack,    // unit
    OpLit2,       // unit unit
    OpLit2Back,   // unit unit
    OpStr,        // offset length
    OpSet,        // set
    OpSetBack,    // set
    OpSetU,       // set
    OpSetUBack,   // set
    OpGreedy,     // set min max: the whole loop of a set, one unit at a time
    OpGreedyU,    // set min max: the same, a code point at a time
    OpSplit,      // target: try on, and that if it fails
    OpSplitLazy,  // target: try that, and on if it fails
    OpJump,       // target
    OpSave,       // register
    OpClear,      // first end: registers
    OpMark,       // register: the position, to know a loop's pass took nothing
    OpCheckProgress,  // register
    OpCheckCounted,   // register counter min: the same, once the minimum is done
    OpLoopInit,   // counter
    OpLoopEnter,  // counter min max greedy exit
    OpLoopNext,   // counter
    OpLineStart,
    OpLineStartM,
    OpLineEnd,
    OpLineEndM,
    OpWord,       // negated folds
    OpBackRef,    // group
    OpBackRefBack,
    OpBackRefFold,  // group: ignoring case
    OpBackRefFoldBack,
    OpLookBegin,  // register
    OpLookEnd,    // register
    OpNegBegin,   // register target
    OpNegEnd,     // register
};

struct CodeSet
{
    std::uint32_t low[8] = {};  ///< below 256
    Ranges high;                ///< the rest, as bare pairs to search
    bool has(std::uint32_t c) const noexcept
    {
        if (c < 256)
            return low[c >> 5] >> (c & 31) & 1;
        return !high.empty() && contains(high, c);
    }
    explicit CodeSet(const Ranges& r)
    {
        for (const auto& [lo, hi] : r) {
            for (char32_t c = lo; c <= hi && c < 256; ++c)
                low[c >> 5] |= 1u << (c & 31);
            if (hi >= 256)
                high.emplace_back(std::max<char32_t>(lo, 256), hi);
        }
    }
};

}  // namespace

struct Program
{
    std::vector<std::uint32_t> code;
    std::vector<CodeSet> sets;
    std::u16string literals;
    std::uint32_t groups = 1;
    std::uint32_t registers = 2;
    std::vector<NamedGroup> names;
    unsigned flags = 0;
    bool anchored = false;       ///< only the start of the input can begin a match
    std::int32_t firstSet = -1;  ///< every match begins with one of these units
};

std::uint32_t groupCount(const Program& program) { return program.groups; }
const std::vector<NamedGroup>& namedGroups(const Program& program) { return program.names; }

bool parseFlags(std::u16string_view text, unsigned& out)
{
    out = 0;
    for (char16_t c : text) {
        unsigned bit = 0;
        switch (c) {
            case 'd': bit = kHasIndices; break;
            case 'g': bit = kGlobal; break;
            case 'i': bit = kIgnoreCase; break;
            case 'm': bit = kMultiline; break;
            case 's': bit = kDotAll; break;
            case 'u': bit = kUnicode; break;
            case 'v': bit = kUnicodeSets; break;
            case 'y': bit = kSticky; break;
            default: return false;
        }
        if (out & bit)
            return false;
        out |= bit;
    }
    return !((out & kUnicode) && (out & kUnicodeSets));
}

namespace {

// ---- the parser -----------------------------------------------------------------------------------------------------

struct BadPattern
{
    const char* message;
};

class PatternParser
{
public:
    PatternParser(std::u16string_view source, unsigned flags, std::vector<NamedGroup>& names)
        : s_(source), flags_(flags), names_(names), unicode_(flags & (kUnicode | kUnicodeSets)), sets_(flags & kUnicodeSets)
    {
    }

    Node* parse()
    {
        scanGroups();
        Node* root = disjunction();
        if (more())
            bad(peek() == ')' ? "unmatched ')'" : "unexpected character");
        for (Node* ref : namedRefs_) {
            bool found = false;
            for (const NamedGroup& g : names_) {
                if (g.name == ref->name) {
                    ref->refs = g.groups;
                    found = true;
                }
            }
            if (!found)
                bad("invalid named reference");
        }
        return root;
    }
    std::uint32_t groupsSeen() const noexcept { return nextGroup_; }

private:
    [[noreturn]] static void bad(const char* message) { throw BadPattern{message}; }

    bool more() const noexcept { return p_ < s_.size(); }
    /// The next character, -1 at the end, a whole code point when the pattern is read by them.
    std::int32_t peek() const noexcept
    {
        if (p_ >= s_.size())
            return -1;
        const char32_t c = s_[p_];
        if (unicode_ && isHigh(c) && p_ + 1 < s_.size() && isLow(s_[p_ + 1]))
            return std::int32_t(combine(c, s_[p_ + 1]));
        return std::int32_t(c);
    }
    std::int32_t unitAt(std::size_t at) const noexcept { return at < s_.size() ? std::int32_t(s_[at]) : -1; }
    void step() noexcept { p_ += peek() > 0xffff ? 2 : 1; }
    bool eat(char c) noexcept
    {
        if (unitAt(p_) != c)
            return false;
        ++p_;
        return true;
    }

    Node* node(Kind kind)
    {
        arena_.emplace_back();
        arena_.back().kind = kind;
        return &arena_.back();
    }
    Node* charNode(char32_t c)
    {
        Node* n = node(Kind::Char);
        n->code = c;
        n->flag = flags_ & kIgnoreCase;
        return n;
    }
    bool foldMode() const noexcept { return unicode_; }
    char32_t topCode() const noexcept { return unicode_ ? 0x10ffff : 0xffff; }
    /// `\w` and `\b` take in the two characters whose folding lands in ASCII, under `iu`.
    bool wordFolds() const noexcept { return unicode_ && (flags_ & kIgnoreCase); }

    /// Counts the groups ahead: `\2` means something else if there is no second group, and `\k` if there are no names.
    void scanGroups()
    {
        int classDepth = 0;
        for (std::size_t i = 0; i < s_.size(); ++i) {
            const char16_t c = s_[i];
            if (c == '\\') {
                ++i;
            } else if (classDepth) {
                if (c == ']')
                    --classDepth;
                else if (c == '[' && sets_)
                    ++classDepth;
            } else if (c == '[') {
                classDepth = 1;
            } else if (c == '(') {
                if (unitAt(i + 1) != '?') {
                    ++totalGroups_;
                } else if (unitAt(i + 2) == '<' && unitAt(i + 3) != '=' && unitAt(i + 3) != '!') {
                    ++totalGroups_;
                    hasNames_ = true;
                }
            }
        }
    }

    Node* disjunction()
    {
        if (++depth_ > kMaxNesting)
            bad("regular expression too deeply nested");
        const std::size_t base = visibleNames_.size();
        std::vector<std::u16string> declared;
        Node* first = alternative();
        Node* result = first;
        while (eat('|')) {
            // A name may be used again in another alternative: it is the same group for whoever refers to it.
            declared.insert(declared.end(), visibleNames_.begin() + std::ptrdiff_t(base), visibleNames_.end());
            visibleNames_.resize(base);
            Node* next = alternative();
            if (result == first) {
                result = node(Kind::Alt);
                result->kids.push_back(first);
            }
            result->kids.push_back(next);
        }
        visibleNames_.insert(visibleNames_.end(), declared.begin(), declared.end());
        --depth_;
        return result;
    }

    Node* alternative()
    {
        Node* seq = node(Kind::Seq);
        while (more()) {
            const std::int32_t c = peek();
            if (c == '|' || c == ')')
                break;
            seq->kids.push_back(term());
        }
        if (seq->kids.size() == 1)
            return seq->kids[0];
        return seq;
    }

    Node* term()
    {
        const std::uint32_t groupsBefore = nextGroup_;
        const std::int32_t c = peek();
        Node* atom = nullptr;
        bool quantifiable = true;
        switch (c) {
            case '^':
                ++p_;
                atom = node(Kind::LineStart);
                atom->flag = flags_ & kMultiline;
                quantifiable = false;
                break;
            case '$':
                ++p_;
                atom = node(Kind::LineEnd);
                atom->flag = flags_ & kMultiline;
                quantifiable = false;
                break;
            case '(': atom = group(quantifiable); break;
            case '.': {
                ++p_;
                atom = node(Kind::Set);
                if (flags_ & kDotAll)
                    atom->set = {{0, topCode()}};
                else
                    atom->set = complementOf({{10, 10}, {13, 13}, {0x2028, 0x2029}}, topCode());
                break;
            }
            case '[': atom = classAtom(); break;
            case '\\':
                if (unitAt(p_ + 1) == 'b' || unitAt(p_ + 1) == 'B') {
                    atom = node(Kind::WordBoundary);
                    atom->flag = unitAt(p_ + 1) == 'B';
                    atom->wordFold = wordFolds();
                    p_ += 2;
                    quantifiable = false;
                } else {
                    atom = atomEscape();
                }
                break;
            case '*':
            case '+':
            case '?': bad("nothing to repeat");
            case '{': {
                if (unicode_)
                    bad("nothing to repeat");
                std::size_t save = p_;
                std::uint32_t lo, hi;
                if (bracedQuantifier(lo, hi))
                    bad("nothing to repeat");
                p_ = save;
                ++p_;
                atom = charNode('{');
                break;
            }
            case '}':
            case ']':
                if (unicode_)
                    bad("lone quantifier bracket");
                [[fallthrough]];
            default:
                step();
                atom = charNode(char32_t(c));
                break;
        }
        return quantifier(atom, quantifiable, groupsBefore);
    }

    Node* quantifier(Node* atom, bool quantifiable, std::uint32_t groupsBefore)
    {
        std::uint32_t lo, hi;
        const std::int32_t c = peek();
        if (c == '*') {
            ++p_;
            lo = 0, hi = kInfinity;
        } else if (c == '+') {
            ++p_;
            lo = 1, hi = kInfinity;
        } else if (c == '?') {
            ++p_;
            lo = 0, hi = 1;
        } else if (c == '{') {
            const std::size_t save = p_;
            if (!bracedQuantifier(lo, hi)) {
                p_ = save;
                if (unicode_)
                    bad("incomplete quantifier");
                return atom;
            }
        } else {
            return atom;
        }
        if (!quantifiable)
            bad("nothing to repeat");
        Node* repeat = node(Kind::Repeat);
        repeat->min = lo;
        repeat->max = hi;
        repeat->flag = !eat('?');
        repeat->firstGroup = groupsBefore;
        repeat->endGroup = nextGroup_;
        repeat->kids.push_back(atom);
        return repeat;
    }

    /// `{n}`, `{n,}` or `{n,m}` here, moved past; false when it is something else. The numbers can be any size, and any
    /// past what a string can be long all mean the same.
    bool bracedQuantifier(std::uint32_t& lo, std::uint32_t& hi)
    {
        if (!eat('{'))
            return false;
        std::u16string_view a, b;
        if (!digits(a))
            return false;
        bool hasSecond = false;
        bool open = false;
        if (eat(',')) {
            hasSecond = digits(b);
            open = !hasSecond;
        }
        if (!eat('}'))
            return false;
        lo = clampCount(a);
        hi = open ? kInfinity : hasSecond ? clampCount(b) : lo;
        if (hasSecond && compareDigits(a, b) > 0)
            bad("numbers out of order in quantifier");
        return true;
    }
    bool digits(std::u16string_view& out)
    {
        const std::size_t start = p_;
        while (unitAt(p_) >= '0' && unitAt(p_) <= '9')
            ++p_;
        out = s_.substr(start, p_ - start);
        return p_ > start;
    }
    static std::u16string_view stripZeros(std::u16string_view d)
    {
        while (d.size() > 1 && d[0] == '0')
            d.remove_prefix(1);
        return d;
    }
    static int compareDigits(std::u16string_view a, std::u16string_view b)
    {
        a = stripZeros(a);
        b = stripZeros(b);
        if (a.size() != b.size())
            return a.size() < b.size() ? -1 : 1;
        return a.compare(b);
    }
    static std::uint32_t clampCount(std::u16string_view d)
    {
        std::uint64_t value = 0;
        for (char16_t c : d) {
            value = value * 10 + std::uint64_t(c - '0');
            if (value >= kInfinity)
                return kInfinity;
        }
        return std::uint32_t(value);
    }

    Node* group(bool& quantifiable)
    {
        ++p_;  // (
        if (!eat('?')) {
            const std::uint32_t number = ++nextGroup_;
            return closeCapture(number);
        }
        const std::int32_t c = unitAt(p_);
        if (c == ':') {
            ++p_;
            return closeGroup();
        }
        if (c == '=' || c == '!') {
            ++p_;
            quantifiable = !unicode_;
            return look(false, c == '!');
        }
        if (c == '<') {
            const std::int32_t d = unitAt(p_ + 1);
            if (d == '=' || d == '!') {
                p_ += 2;
                quantifiable = false;
                return look(true, d == '!');
            }
            ++p_;
            std::u16string name = groupName();
            for (const std::u16string& seen : visibleNames_) {
                if (seen == name)
                    bad("duplicate capture group name");
            }
            visibleNames_.push_back(name);
            const std::uint32_t number = ++nextGroup_;
            bool added = false;
            for (NamedGroup& g : names_) {
                if (g.name == name) {
                    g.groups.push_back(number);
                    added = true;
                }
            }
            if (!added)
                names_.push_back({name, {number}});
            return closeCapture(number);
        }
        return modifiers();
    }

    Node* closeCapture(std::uint32_t number)
    {
        Node* n = node(Kind::Capture);
        n->group = number;
        n->kids.push_back(disjunction());
        if (!eat(')'))
            bad("unterminated group");
        return n;
    }
    Node* closeGroup()
    {
        Node* inner = disjunction();
        if (!eat(')'))
            bad("unterminated group");
        return inner;
    }
    Node* look(bool behind, bool negated)
    {
        Node* n = node(Kind::Look);
        n->behind = behind;
        n->flag = negated;
        n->kids.push_back(disjunction());
        if (!eat(')'))
            bad("unterminated group");
        return n;
    }

    /// `(?ims-ims:`: flags for one group. Everything they affect is settled here in the parser.
    Node* modifiers()
    {
        unsigned on = 0, off = 0;
        unsigned* into = &on;
        bool sawDash = false;
        for (;;) {
            const std::int32_t c = unitAt(p_);
            if (c == ':')
                break;
            if (c == '-' && !sawDash) {
                sawDash = true;
                into = &off;
                ++p_;
                continue;
            }
            const unsigned bit = c == 'i' ? kIgnoreCase : c == 'm' ? kMultiline : c == 's' ? kDotAll : 0;
            if (!bit)
                bad("invalid group");
            if ((on | off) & bit)
                bad("a modifier repeated");
            *into |= bit;
            ++p_;
        }
        if (sawDash && !on && !off)
            bad("a modifier group changes nothing");
        ++p_;
        const unsigned saved = flags_;
        flags_ = (flags_ | on) & ~off;
        Node* inner = closeGroup();
        flags_ = saved;
        return inner;
    }

    std::u16string groupName()
    {
        // The opening '<' is gone. A name is read by code points whatever the mode of the rest.
        std::u16string name;
        bool first = true;
        for (;;) {
            if (!more())
                bad("invalid capture group name");
            char32_t c = s_[p_];
            if (c == '>') {
                ++p_;
                break;
            }
            if (c == '\\') {
                ++p_;
                if (!eat('u'))
                    bad("invalid capture group name");
                std::int32_t code = unicodeEscape(true);
                if (code < 0)
                    bad("invalid Unicode escape");
                c = char32_t(code);
            } else if (isHigh(c) && p_ + 1 < s_.size() && isLow(s_[p_ + 1])) {
                c = combine(c, s_[p_ + 1]);
                p_ += 2;
            } else {
                ++p_;
            }
            const bool fits = first ? (c == '$' || c == '_' || unicode::isIdStart(c)) : (c == '$' || c == 0x200c || c == 0x200d || unicode::isIdContinue(c));
            if (!fits)
                bad("invalid capture group name");
            appendCode(name, c);
            first = false;
        }
        if (name.empty())
            bad("invalid capture group name");
        return name;
    }
    static void appendCode(std::u16string& out, char32_t c)
    {
        if (c > 0xffff) {
            out.push_back(char16_t(0xd800 + ((c - 0x10000) >> 10)));
            out.push_back(char16_t(0xdc00 + ((c - 0x10000) & 0x3ff)));
        } else {
            out.push_back(char16_t(c));
        }
    }

    static int hexValue(std::int32_t c) noexcept
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }
    /// After `\u`: the code, or -1 with nothing moved. `whole` reads it as a mode with code points does.
    std::int32_t unicodeEscape(bool whole)
    {
        const std::size_t save = p_;
        if (whole && unitAt(p_) == '{') {
            ++p_;
            std::uint32_t code = 0;
            int count = 0;
            while (hexValue(unitAt(p_)) >= 0) {
                code = code * 16 + std::uint32_t(hexValue(unitAt(p_)));
                if (code > 0x10ffff) {
                    p_ = save;
                    return -1;
                }
                ++p_;
                ++count;
            }
            if (!count || unitAt(p_) != '}') {
                p_ = save;
                return -1;
            }
            ++p_;
            return std::int32_t(code);
        }
        const std::int32_t hi = fourHex();
        if (hi < 0) {
            p_ = save;
            return -1;
        }
        if (whole && isHigh(char32_t(hi)) && unitAt(p_) == '\\' && unitAt(p_ + 1) == 'u') {
            const std::size_t afterHi = p_;
            p_ += 2;
            const std::int32_t lo = fourHex();
            if (lo >= 0 && isLow(char32_t(lo)))
                return std::int32_t(combine(char32_t(hi), char32_t(lo)));
            p_ = afterHi;
        }
        return hi;
    }
    std::int32_t fourHex()
    {
        std::int32_t code = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const int digit = hexValue(unitAt(p_ + i));
            if (digit < 0)
                return -1;
            code = code * 16 + digit;
        }
        p_ += 4;
        return code;
    }

    static bool isSyntaxCharacter(std::int32_t c) noexcept
    {
        return c == '^' || c == '$' || c == '\\' || c == '.' || c == '*' || c == '+' || c == '?' || c == '(' || c == ')' || c == '[' || c == ']' ||
               c == '{' || c == '}' || c == '|';
    }

    /// `\c` and on is one character; the mode decides what the odd ones mean. `p_` is past the backslash.
    char32_t characterEscape(bool inClass)
    {
        const std::int32_t c = peek();
        switch (c) {
            case -1: bad("\\ at end of pattern");
            case 't': ++p_; return 9;
            case 'n': ++p_; return 10;
            case 'v': ++p_; return 11;
            case 'f': ++p_; return 12;
            case 'r': ++p_; return 13;
            case 'c': {
                const std::int32_t d = unitAt(p_ + 1);
                if ((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z')) {
                    p_ += 2;
                    return char32_t(d % 32);
                }
                if (unicode_)
                    bad("invalid unicode escape");
                if (inClass && ((d >= '0' && d <= '9') || d == '_')) {
                    p_ += 2;
                    return char32_t(d % 32);
                }
                // The backslash stands for itself, and the `c` comes next.
                return '\\';
            }
            case 'x': {
                ++p_;
                const int a = hexValue(unitAt(p_));
                const int b = a < 0 ? -1 : hexValue(unitAt(p_ + 1));
                if (b >= 0) {
                    p_ += 2;
                    return char32_t(a * 16 + b);
                }
                if (unicode_)
                    bad("invalid escape");
                return 'x';
            }
            case 'u': {
                ++p_;
                const std::int32_t code = unicodeEscape(unicode_);
                if (code >= 0)
                    return char32_t(code);
                if (unicode_)
                    bad("invalid Unicode escape");
                return 'u';
            }
            default: break;
        }
        if (c >= '0' && c <= '9') {
            if (unicode_) {
                if (c == '0' && !(unitAt(p_ + 1) >= '0' && unitAt(p_ + 1) <= '9')) {
                    ++p_;
                    return 0;
                }
                bad("invalid decimal escape");
            }
            if (c >= '8') {
                ++p_;
                return char32_t(c);
            }
            return legacyOctal();
        }
        if (unicode_) {
            if (isSyntaxCharacter(c) || c == '/' || (inClass && c == '-')) {
                ++p_;
                return char32_t(c);
            }
            bad("invalid escape");
        }
        if (c == 'k' && hasNames_)
            bad("invalid named reference");
        step();
        return char32_t(c);
    }
    char32_t legacyOctal()
    {
        std::uint32_t value = std::uint32_t(unitAt(p_) - '0');
        ++p_;
        if (unitAt(p_) >= '0' && unitAt(p_) <= '7') {
            value = value * 8 + std::uint32_t(unitAt(p_) - '0');
            ++p_;
            if (value < 32 && unitAt(p_) >= '0' && unitAt(p_) <= '7') {
                value = value * 8 + std::uint32_t(unitAt(p_) - '0');
                ++p_;
            }
        }
        return value;
    }

    /// `\d` and its like at `p_` (past the backslash) into `out`. False, with nothing moved, when it is not one.
    bool classEscape(Ranges& out)
    {
        const std::int32_t c = peek();
        switch (c) {
            case 'd': out = digitSet(); break;
            case 'D': out = complementOf(digitSet(), topCode()); break;
            case 's': out = spaceSet(); break;
            case 'S': out = complementOf(spaceSet(), topCode()); break;
            case 'w':
            case 'W': {
                Ranges word = wordSet();
                if (wordFolds()) {
                    word.emplace_back(0x17f, 0x17f);
                    word.emplace_back(0x212a, 0x212a);
                }
                normalize(word);
                out = c == 'w' ? word : complementOf(word, topCode());
                break;
            }
            case 'p':
            case 'P': {
                if (!unicode_)
                    return false;
                ++p_;
                propertyEscape(out, c == 'P');
                return true;
            }
            default: return false;
        }
        ++p_;
        return true;
    }

    void propertyEscape(Ranges& out, bool negated)
    {
        if (!eat('{'))
            bad("invalid property name");
        std::string name, value;
        std::string* into = &name;
        bool sawEquals = false;
        for (;;) {
            const std::int32_t c = unitAt(p_);
            if (c == '}')
                break;
            if (c == '=' && !sawEquals) {
                sawEquals = true;
                into = &value;
                ++p_;
                continue;
            }
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
                bad("invalid property name");
            into->push_back(char(c));
            ++p_;
        }
        ++p_;
        if (name.empty() || (sawEquals && value.empty()))
            bad("invalid property name");
        Ranges found;
        if (!unicode::lookupProperty(name, value, found))
            bad("invalid property name");
        normalize(found);
        // Under `v` the two cases are one thing before anything is taken away from everything.
        if (sets_ && (flags_ & kIgnoreCase))
            closeOverCase(found, true);
        out = negated ? complementOf(found, 0x10ffff) : std::move(found);
    }

    Node* atomEscape()
    {
        ++p_;  // the backslash
        const std::int32_t c = peek();
        if (c == -1)
            bad("\\ at end of pattern");
        if (c >= '1' && c <= '9') {
            const std::size_t save = p_;
            std::uint64_t number = 0;
            while (unitAt(p_) >= '0' && unitAt(p_) <= '9') {
                number = std::min<std::uint64_t>(number * 10 + std::uint64_t(unitAt(p_) - '0'), 1u << 30);
                ++p_;
            }
            if (number <= totalGroups_) {
                Node* n = node(Kind::BackRef);
                n->refs.push_back(std::uint32_t(number));
                n->flag = flags_ & kIgnoreCase;
                return n;
            }
            if (unicode_)
                bad("invalid escape");
            p_ = save;
        }
        if (c == 'k' && (unicode_ || hasNames_)) {
            ++p_;
            if (!eat('<'))
                bad("invalid named reference");
            Node* n = node(Kind::BackRef);
            n->name = groupName();
            n->flag = flags_ & kIgnoreCase;
            namedRefs_.push_back(n);
            return n;
        }
        Ranges set;
        if (classEscape(set)) {
            Node* n = node(Kind::Set);
            n->set = std::move(set);
            finishSet(*n);
            return n;
        }
        return charNode(characterEscape(false));
    }

    void finishSet(Node& n)
    {
        if (flags_ & kIgnoreCase)
            closeOverCase(n.set, foldMode());
        normalize(n.set);
    }

    // ---- classes

    Node* classAtom()
    {
        ++p_;  // [
        if (sets_)
            return setClass();
        const bool negated = eat('^');
        Ranges set;
        for (;;) {
            if (!more())
                bad("unterminated character class");
            if (eat(']'))
                break;
            Ranges aSet;
            const std::int32_t a = classMember(aSet);
            if (unitAt(p_) == '-' && unitAt(p_ + 1) != ']' && unitAt(p_ + 1) != -1) {
                ++p_;
                Ranges bSet;
                const std::int32_t b = classMember(bSet);
                if (a < 0 || b < 0) {
                    if (unicode_)
                        bad("invalid character class");
                    addMember(set, a, aSet);
                    addMember(set, b, bSet);
                    set.emplace_back('-', '-');
                } else {
                    if (a > b)
                        bad("range out of order in character class");
                    set.emplace_back(char32_t(a), char32_t(b));
                }
            } else {
                addMember(set, a, aSet);
            }
        }
        if (flags_ & kIgnoreCase)
            closeOverCase(set, foldMode());
        normalize(set);
        Node* n = node(Kind::Set);
        n->set = negated ? complementOf(set, topCode()) : std::move(set);
        return n;
    }
    static void addMember(Ranges& set, std::int32_t code, Ranges& members)
    {
        if (code >= 0)
            set.emplace_back(char32_t(code), char32_t(code));
        else
            addAll(set, members);
    }
    /// One character of a class, or -1 with a whole set of them in `set`.
    std::int32_t classMember(Ranges& set)
    {
        std::int32_t c = peek();
        if (c != '\\') {
            step();
            return c;
        }
        ++p_;
        c = peek();
        if (c == 'b') {
            ++p_;
            return 8;
        }
        if (c == '-' && unicode_) {
            ++p_;
            return '-';
        }
        if (classEscape(set)) {
            normalize(set);
            return -1;
        }
        return std::int32_t(characterEscape(true));
    }

    // ---- classes of `v`, which are expressions

    Node* setClass()
    {
        const ClassValue value = setClassBody();
        Node* n;
        if (value.strings.empty()) {
            n = node(Kind::Set);
            n->set = value.set;
            return n;
        }
        // The longest string that matches is the one taken, and nothing at all only if nothing else does.
        std::vector<std::u32string> strings = value.strings;
        std::stable_sort(strings.begin(), strings.end(), [](const std::u32string& a, const std::u32string& b) { return a.size() > b.size(); });
        n = node(Kind::Alt);
        bool setDone = value.set.empty();
        const auto addSet = [&]() {
            Node* members = node(Kind::Set);
            members->set = value.set;
            n->kids.push_back(members);
            setDone = true;
        };
        for (const std::u32string& text : strings) {
            if (!setDone && text.size() < 1)
                addSet();
            Node* seq = node(Kind::Seq);
            for (char32_t c : text)
                seq->kids.push_back(charNode(c));
            n->kids.push_back(seq);
        }
        if (!setDone)
            addSet();
        return n;
    }

    /// After `[`, up to and past its `]`.
    ClassValue setClassBody()
    {
        if (++depth_ > kMaxNesting)
            bad("regular expression too deeply nested");
        const bool negated = eat('^');
        ClassValue result;
        if (eat(']')) {
            if (negated)
                result.set = {{0, 0x10ffff}};
            --depth_;
            return result;
        }
        bool firstIsRange = false;
        result = setOperand(firstIsRange, true);
        if (startsWith("&&") || startsWith("--")) {
            if (firstIsRange)
                bad("invalid set operation in character class");
            const bool isAnd = startsWith("&&");
            while (more() && unitAt(p_) != ']') {
                if (!startsWith(isAnd ? "&&" : "--"))
                    bad("invalid set operation in character class");
                p_ += 2;
                if (isAnd && unitAt(p_) == '&')
                    bad("invalid character in character class");
                bool isRange = false;
                ClassValue operand = setOperand(isRange, false);
                if (isRange)
                    bad("invalid set operation in character class");
                combineSets(result, operand, isAnd);
            }
        } else {
            while (more() && unitAt(p_) != ']') {
                if (startsWith("&&") || startsWith("--"))
                    bad("invalid set operation in character class");
                bool isRange = false;
                ClassValue operand = setOperand(isRange, true);
                addAll(result.set, operand.set);
                normalize(result.set);
                for (auto& text : operand.strings) {
                    if (std::find(result.strings.begin(), result.strings.end(), text) == result.strings.end())
                        result.strings.push_back(std::move(text));
                }
            }
        }
        if (!eat(']'))
            bad("unterminated character class");
        normalize(result.set);
        if (negated) {
            if (!result.strings.empty())
                bad("negated character class may contain strings");
            result.set = complementOf(result.set, 0x10ffff);
        }
        --depth_;
        return result;
    }
    bool startsWith(const char* text) const noexcept
    {
        std::size_t at = p_;
        for (; *text; ++text, ++at) {
            if (unitAt(at) != *text)
                return false;
        }
        return true;
    }
    static void combineSets(ClassValue& into, const ClassValue& other, bool isAnd)
    {
        Ranges b = other.set;
        normalize(b);
        normalize(into.set);
        into.set = intersect(into.set, isAnd ? b : complementOf(b, 0x10ffff));
        std::vector<std::u32string> kept;
        for (auto& text : into.strings) {
            const bool inOther = std::find(other.strings.begin(), other.strings.end(), text) != other.strings.end();
            if (inOther == isAnd)
                kept.push_back(std::move(text));
        }
        into.strings = std::move(kept);
    }

    ClassValue setOperand(bool& isRange, bool allowRange)
    {
        ClassValue out;
        if (eat('[')) {
            return foldedValue(setClassBody());
        }
        if (unitAt(p_) == '\\' && unitAt(p_ + 1) == 'q') {
            p_ += 2;
            if (!eat('{'))
                bad("invalid escape");
            stringDisjunction(out);
            return foldedValue(std::move(out));
        }
        if (unitAt(p_) == '\\') {
            const std::size_t save = p_;
            ++p_;
            Ranges set;
            if (classEscape(set)) {
                out.set = std::move(set);
                return foldedValue(std::move(out));
            }
            p_ = save;
        }
        const char32_t lo = setCharacter();
        if (allowRange && unitAt(p_) == '-' && unitAt(p_ + 1) != '-') {
            ++p_;
            const char32_t hi = setCharacter();
            if (lo > hi)
                bad("range out of order in character class");
            out.set.emplace_back(lo, hi);
            isRange = true;
        } else if (unitAt(p_) == '-' && unitAt(p_ + 1) != '-') {
            bad("invalid character class");
        } else {
            out.set.emplace_back(lo, lo);
        }
        return foldedValue(std::move(out));
    }
    /// Under `iv` the two cases are the same member, whichever operation comes next.
    ClassValue foldedValue(ClassValue value)
    {
        normalize(value.set);
        if (flags_ & kIgnoreCase) {
            closeOverCase(value.set, true);
            for (auto& text : value.strings) {
                for (char32_t& c : text)
                    c = unicode::simpleFold(c);
            }
        }
        return value;
    }
    void stringDisjunction(ClassValue& out)
    {
        std::u32string text;
        for (;;) {
            if (!more())
                bad("unterminated \\q");
            const std::int32_t c = unitAt(p_);
            if (c == '}' || c == '|') {
                ++p_;
                if (text.size() == 1) {
                    out.set.emplace_back(text[0], text[0]);
                } else if (std::find(out.strings.begin(), out.strings.end(), text) == out.strings.end()) {
                    out.strings.push_back(text);
                }
                text.clear();
                if (c == '}')
                    return;
                continue;
            }
            text.push_back(setCharacter());
        }
    }
    char32_t setCharacter()
    {
        const std::int32_t c = peek();
        if (c == -1)
            bad("unterminated character class");
        if (c == '\\') {
            ++p_;
            const std::int32_t d = peek();
            if (d == 'b') {
                ++p_;
                return 8;
            }
            if (d == '&' || d == '-' || d == '!' || d == '#' || d == '%' || d == ',' || d == ':' || d == ';' || d == '<' || d == '=' || d == '>' || d == '@' ||
                d == '`' || d == '~') {
                ++p_;
                return char32_t(d);
            }
            return characterEscape(true);
        }
        if (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '-' || c == '|')
            bad("invalid character in character class");
        static const char kDoubled[] = "&!#$%*+,.:;<=>?@^`~";
        if (c < 128 && std::strchr(kDoubled, c) && c != 0 && unitAt(p_ + 1) == c)
            bad("invalid set operation in character class");
        step();
        return char32_t(c);
    }

    std::u16string_view s_;
    std::size_t p_ = 0;
    unsigned flags_;
    std::vector<NamedGroup>& names_;
    bool unicode_;
    bool sets_;
    bool hasNames_ = false;
    int depth_ = 0;
    std::uint32_t totalGroups_ = 0;
    std::uint32_t nextGroup_ = 0;
    std::vector<std::u16string> visibleNames_;
    std::vector<Node*> namedRefs_;
    std::deque<Node> arena_;
};

// ---- the code -------------------------------------------------------------------------------------------------------

class Generator
{
public:
    Generator(Program& program, bool unicodeMode) : p_(program), unicode_(unicodeMode) {}

    void run(Node* root)
    {
        p_.registers = p_.groups * 2;
        gen(*root, false);
        p_.code.push_back(OpMatch);
        findStart(*root);
    }

private:
    std::uint32_t reg(std::uint32_t count = 1)
    {
        const std::uint32_t r = p_.registers;
        p_.registers += count;
        return r;
    }
    void emit(std::uint32_t a) { p_.code.push_back(a); }
    void emit(std::uint32_t a, std::uint32_t b)
    {
        p_.code.push_back(a);
        p_.code.push_back(b);
    }
    std::size_t emitTarget(std::uint32_t op)
    {
        p_.code.push_back(op);
        p_.code.push_back(0);
        return p_.code.size() - 1;
    }
    std::uint32_t here() const noexcept { return std::uint32_t(p_.code.size()); }
    void land(std::size_t slot) { p_.code[slot] = here(); }

    std::uint32_t setIndex(const Ranges& r)
    {
        p_.sets.emplace_back(r);
        return std::uint32_t(p_.sets.size() - 1);
    }

    static bool foldsToOthers(const Node& n, bool foldMode, std::vector<char32_t>& out)
    {
        if (!n.flag)
            return false;
        const char32_t c = n.code;
        if (!foldMode && c < 128) {
            if (c >= 'a' && c <= 'z')
                out = {c - 32, c};
            else if (c >= 'A' && c <= 'Z')
                out = {c, c + 32};
            return !out.empty();
        }
        if (foldMode && c < 128 && c != 'k' && c != 'K' && c != 's' && c != 'S') {
            if (c >= 'a' && c <= 'z')
                out = {c - 32, c};
            else if (c >= 'A' && c <= 'Z')
                out = {c, c + 32};
            return !out.empty();
        }
        if (const std::vector<char32_t>* group = unicode::caseEquivalents(c, foldMode)) {
            out = *group;
            return true;
        }
        return false;
    }

    /// The set a character node stands for, for the loops that run on sets.
    Ranges charSet(const Node& n)
    {
        std::vector<char32_t> others;
        Ranges r;
        if (foldsToOthers(n, unicode_, others)) {
            for (char32_t c : others)
                r.emplace_back(c, c);
        } else {
            r.emplace_back(n.code, n.code);
        }
        normalize(r);
        return r;
    }

    void genSet(const Ranges& r, bool back)
    {
        emit(unicode_ ? (back ? OpSetUBack : OpSetU) : (back ? OpSetBack : OpSet), setIndex(r));
    }

    void genChar(const Node& n, bool back)
    {
        std::vector<char32_t> others;
        if (foldsToOthers(n, unicode_, others)) {
            const bool plain = others.size() == 2 && others[0] <= 0xffff && others[1] <= 0xffff && (!unicode_ || (!isSurrogate(others[0]) && !isSurrogate(others[1])));
            if (plain) {
                emit(back ? OpLit2Back : OpLit2, others[0]);
                emit(others[1]);
                return;
            }
            genSet(charSet(n), back);
            return;
        }
        const char32_t c = n.code;
        if (!unicode_ || (c <= 0xffff && !isSurrogate(c))) {
            emit(back ? OpLitBack : OpLit, c);
        } else if (c > 0xffff) {
            const std::uint32_t hi = 0xd800 + ((c - 0x10000) >> 10);
            const std::uint32_t lo = 0xdc00 + ((c - 0x10000) & 0x3ff);
            emit(back ? OpLitBack : OpLit, back ? lo : hi);
            emit(back ? OpLitBack : OpLit, back ? hi : lo);
        } else {
            // A lone surrogate in a pattern that reads code points is not half of a pair in the input.
            genSet({{c, c}}, back);
        }
    }

    /// Characters with no case to them, side by side, go out as one comparison.
    void genSeq(const Node& n, bool back)
    {
        if (back) {
            for (std::size_t i = n.kids.size(); i-- > 0;)
                gen(*n.kids[i], true);
            return;
        }
        for (std::size_t i = 0; i < n.kids.size();) {
            std::u16string run;
            std::size_t j = i;
            for (; j < n.kids.size(); ++j) {
                const Node& kid = *n.kids[j];
                std::vector<char32_t> others;
                if (kid.kind != Kind::Char || foldsToOthers(kid, unicode_, others))
                    break;
                if (unicode_ && kid.code <= 0xffff && isSurrogate(kid.code))
                    break;
                if (kid.code > 0xffff) {
                    run.push_back(char16_t(0xd800 + ((kid.code - 0x10000) >> 10)));
                    run.push_back(char16_t(0xdc00 + ((kid.code - 0x10000) & 0x3ff)));
                } else {
                    run.push_back(char16_t(kid.code));
                }
            }
            if (run.size() >= 2) {
                emit(OpStr, std::uint32_t(p_.literals.size()));
                emit(std::uint32_t(run.size()));
                p_.literals += run;
                i = j;
            } else {
                gen(*n.kids[i], false);
                ++i;
            }
        }
    }

    static std::uint32_t minWidth(const Node& n)
    {
        switch (n.kind) {
            case Kind::Char:
            case Kind::Set: return 1;
            case Kind::Seq: {
                std::uint64_t total = 0;
                for (const Node* kid : n.kids)
                    total += minWidth(*kid);
                return std::uint32_t(std::min<std::uint64_t>(total, 1u << 30));
            }
            case Kind::Alt: {
                std::uint32_t best = kInfinity;
                for (const Node* kid : n.kids)
                    best = std::min(best, minWidth(*kid));
                return best;
            }
            case Kind::Capture: return minWidth(*n.kids[0]);
            case Kind::Repeat: return std::uint32_t(std::min<std::uint64_t>(std::uint64_t(n.min) * minWidth(*n.kids[0]), 1u << 30));
            default: return 0;
        }
    }
    static std::size_t weight(const Node& n)
    {
        std::size_t total = 1;
        for (const Node* kid : n.kids)
            total += weight(*kid);
        return total;
    }

    void clearGroups(const Node& repeat)
    {
        if (repeat.endGroup > repeat.firstGroup) {
            emit(OpClear, (repeat.firstGroup + 1) * 2);
            emit((repeat.endGroup + 1) * 2);
        }
    }

    void genRepeat(const Node& n, bool back)
    {
        const Node& body = *n.kids[0];
        if (n.max == 0)
            return;
        const bool single = body.kind == Kind::Set || body.kind == Kind::Char;
        if (!back && single && n.flag) {
            const Ranges set = body.kind == Kind::Set ? body.set : charSet(body);
            emit(unicode_ ? OpGreedyU : OpGreedy, setIndex(set));
            emit(n.min, n.max);
            return;
        }
        const bool mayBeEmpty = minWidth(body) == 0;
        std::uint32_t min = n.min;
        std::uint32_t max = n.max;
        // The first few passes are not a loop at all.
        if (min > 0 && min <= 4 && weight(body) * min <= 64) {
            for (std::uint32_t i = 0; i < min; ++i) {
                clearGroups(n);
                gen(body, back);
            }
            if (max != kInfinity)
                max -= min;
            min = 0;
            if (max == 0)
                return;
        }
        if (min == 0 && (max == 1 || max == kInfinity)) {
            const std::uint32_t top = here();
            const std::size_t exit = emitTarget(n.flag ? OpSplit : OpSplitLazy);
            clearGroups(n);
            std::uint32_t mark = 0;
            if (mayBeEmpty) {
                mark = reg();
                emit(OpMark, mark);
            }
            gen(body, back);
            if (mayBeEmpty)
                emit(OpCheckProgress, mark);
            if (max == kInfinity)
                emit(OpJump, top);
            land(exit);
            return;
        }
        const std::uint32_t counter = reg();
        emit(OpLoopInit, counter);
        const std::uint32_t top = here();
        emit(OpLoopEnter, counter);
        emit(min, max);
        emit(n.flag);
        p_.code.push_back(0);
        const std::size_t exit = p_.code.size() - 1;
        clearGroups(n);
        std::uint32_t mark = 0;
        if (mayBeEmpty) {
            mark = reg();
            emit(OpMark, mark);
        }
        gen(body, back);
        if (mayBeEmpty) {
            emit(OpCheckCounted, mark);
            emit(counter, min);
        }
        emit(OpLoopNext, counter);
        emit(OpJump, top);
        land(exit);
    }

    void gen(const Node& n, bool back)
    {
        switch (n.kind) {
            case Kind::Empty: break;
            case Kind::Char: genChar(n, back); break;
            case Kind::Set: genSet(n.set, back); break;
            case Kind::Seq: genSeq(n, back); break;
            case Kind::Alt: {
                std::vector<std::size_t> ends;
                for (std::size_t i = 0; i < n.kids.size(); ++i) {
                    const bool last = i + 1 == n.kids.size();
                    std::size_t next = 0;
                    if (!last)
                        next = emitTarget(OpSplit);
                    gen(*n.kids[i], back);
                    if (!last) {
                        ends.push_back(emitTarget(OpJump));
                        land(next);
                    }
                }
                for (std::size_t slot : ends)
                    land(slot);
                break;
            }
            case Kind::Capture:
                emit(OpSave, n.group * 2 + (back ? 1 : 0));
                gen(*n.kids[0], back);
                emit(OpSave, n.group * 2 + (back ? 0 : 1));
                break;
            case Kind::Repeat: genRepeat(n, back); break;
            case Kind::LineStart: emit(n.flag ? OpLineStartM : OpLineStart); break;
            case Kind::LineEnd: emit(n.flag ? OpLineEndM : OpLineEnd); break;
            case Kind::WordBoundary:
                emit(OpWord, n.flag);
                emit(n.wordFold);
                break;
            case Kind::Look: {
                const std::uint32_t r = reg(2);
                if (n.flag) {
                    emit(OpNegBegin, r);
                    p_.code.push_back(0);
                    const std::size_t after = p_.code.size() - 1;
                    gen(*n.kids[0], n.behind);
                    emit(OpNegEnd, r);
                    land(after);
                } else {
                    emit(OpLookBegin, r);
                    gen(*n.kids[0], n.behind);
                    emit(OpLookEnd, r);
                }
                break;
            }
            case Kind::BackRef:
                // Of groups sharing a name at most one has matched, and the others match nothing and take no room.
                for (std::uint32_t group : n.refs) {
                    if (unicode_ || n.flag)
                        emit(n.flag ? (back ? OpBackRefFoldBack : OpBackRefFold) : (back ? OpBackRefBack : OpBackRef), group);
                    else
                        emit(back ? OpBackRefBack : OpBackRef, group);
                }
                break;
        }
    }

    // ---- where a match can begin

    /// Adds what can come first in `n` to `first`: true when it can match nothing, so what follows counts too. Sets `open`
    /// when anything at all can.
    bool firstOf(const Node& n, Ranges& first, bool& open)
    {
        switch (n.kind) {
            case Kind::Empty:
            case Kind::LineStart:
            case Kind::LineEnd:
            case Kind::WordBoundary:
            case Kind::Look: return true;
            case Kind::Char: addAll(first, charSet(n)); return false;
            case Kind::Set: addAll(first, n.set); return false;
            case Kind::Seq:
                for (const Node* kid : n.kids) {
                    if (!firstOf(*kid, first, open))
                        return false;
                }
                return true;
            case Kind::Alt: {
                bool empty = false;
                for (const Node* kid : n.kids)
                    empty = firstOf(*kid, first, open) || empty;
                return empty;
            }
            case Kind::Capture: return firstOf(*n.kids[0], first, open);
            case Kind::Repeat: {
                if (n.max == 0)
                    return true;
                const bool empty = firstOf(*n.kids[0], first, open);
                return empty || n.min == 0;
            }
            case Kind::BackRef: open = true; return true;
        }
        return true;
    }
    static bool startsAtStart(const Node& n)
    {
        switch (n.kind) {
            case Kind::LineStart: return !n.flag;
            case Kind::Seq: return !n.kids.empty() && startsAtStart(*n.kids[0]);
            case Kind::Capture: return startsAtStart(*n.kids[0]);
            case Kind::Alt:
                for (const Node* kid : n.kids) {
                    if (!startsAtStart(*kid))
                        return false;
                }
                return true;
            default: return false;
        }
    }
    void findStart(const Node& root)
    {
        p_.anchored = startsAtStart(root);
        Ranges first;
        bool open = false;
        if (firstOf(root, first, open) || open)
            return;
        normalize(first);
        // By units: what starts a pair is what the scan looks for.
        Ranges units;
        for (const auto& [lo, hi] : first) {
            if (lo <= 0xffff)
                units.emplace_back(lo, std::min<char32_t>(hi, 0xffff));
            if (hi > 0xffff) {
                const char32_t a = std::max<char32_t>(lo, 0x10000) - 0x10000;
                units.emplace_back(0xd800 + (a >> 10), 0xd800 + ((hi - 0x10000) >> 10));
            }
        }
        normalize(units);
        p_.firstSet = std::int32_t(setIndex(units));
    }

    Program& p_;
    bool unicode_;
};

// ---- the matcher ----------------------------------------------------------------------------------------------------

constexpr std::uint32_t kUndoTag = 0x80000000u;    // [old value, register | tag]
constexpr std::uint32_t kGreedyTag = 0x40000000u;  // [lowest, position, pc | tag]: can give back one unit
constexpr std::uint32_t kGreedyUTag = 0x20000000u; // the same, a code point at a time
constexpr std::uint32_t kPcMask = 0x1fffffffu;

bool wordAt(const char16_t* s, std::int32_t i, std::int32_t length, bool folds) noexcept
{
    if (i < 0 || i >= length)
        return false;
    const char32_t c = s[i];
    return isAsciiWord(c) || (folds && (c == 0x17f || c == 0x212a));
}

bool isLineEnd(char32_t c) noexcept { return c == 10 || c == 13 || c == 0x2028 || c == 0x2029; }

struct Run
{
    const Program& program;
    Matcher& m;
    const char16_t* s;
    std::int32_t length;
    bool foldMode;

    std::uint32_t* stack;
    std::size_t capacity;
    std::size_t sp = 0;
    std::uint64_t steps = 0;

    __attribute__((noinline)) void grow()
    {
        m.stack.resize(m.stack.size() * 2);
        stack = m.stack.data();
        capacity = m.stack.size();
        if (m.poll)
            m.poll(m.pollContext, capacity * sizeof(std::uint32_t));
    }
    __attribute__((always_inline)) void push2(std::uint32_t a, std::uint32_t b)
    {
        if (sp + 4 > capacity)
            grow();
        stack[sp++] = a;
        stack[sp++] = b;
    }
    __attribute__((always_inline)) void setRegister(std::int32_t* regs, std::uint32_t r, std::int32_t value)
    {
        push2(std::uint32_t(regs[r]), r | kUndoTag);
        regs[r] = value;
    }

    /// Reads the next or previous code point of a pair-aware pattern; false at the edge.
    bool nextCode(std::int32_t& pos, char32_t& c) const noexcept
    {
        if (pos >= length)
            return false;
        c = s[pos++];
        if (isHigh(c) && pos < length && isLow(s[pos]))
            c = combine(c, s[pos++]);
        return true;
    }
    bool previousCode(std::int32_t& pos, char32_t& c) const noexcept
    {
        if (pos <= 0)
            return false;
        c = s[--pos];
        if (isLow(c) && pos > 0 && isHigh(s[pos - 1]))
            c = combine(s[--pos], c);
        return true;
    }

    bool backRef(const std::int32_t* regs, std::uint32_t group, std::int32_t& pos, bool fold, bool back) const
    {
        const std::int32_t a = regs[group * 2];
        const std::int32_t b = regs[group * 2 + 1];
        if (a < 0 || b < 0)
            return true;
        const std::int32_t size = b - a;
        const std::int32_t from = back ? pos - size : pos;
        if (from < 0 || from + size > length)
            return false;
        if (!fold) {
            if (std::memcmp(s + a, s + from, std::size_t(size) * sizeof(char16_t)) != 0)
                return false;
        } else if (foldMode) {
            std::int32_t i = a;
            std::int32_t j = from;
            while (i < b) {
                char32_t x, y;
                nextCode(i, x);
                nextCode(j, y);
                if (x != y && unicode::simpleFold(x) != unicode::simpleFold(y))
                    return false;
            }
        } else {
            for (std::int32_t i = 0; i < size; ++i) {
                if (s[a + i] != s[from + i] && canonical(s[a + i], false) != canonical(s[from + i], false))
                    return false;
            }
        }
        pos = back ? from : from + size;
        return true;
    }

    bool match(std::int32_t start, std::int32_t* regs)
    {
        const std::uint32_t* const code = program.code.data();
        const CodeSet* const sets = program.sets.data();
        std::uint32_t pc = 0;
        std::int32_t pos = start;
        sp = 0;
        for (;;) {
            switch (code[pc]) {
                case OpMatch:
                    regs[0] = start;
                    regs[1] = pos;
                    return true;
                case OpLit:
                    if (pos >= length || s[pos] != code[pc + 1])
                        goto fail;
                    ++pos;
                    pc += 2;
                    continue;
                case OpLitBack:
                    if (pos <= 0 || s[pos - 1] != code[pc + 1])
                        goto fail;
                    --pos;
                    pc += 2;
                    continue;
                case OpLit2:
                    if (pos >= length || (s[pos] != code[pc + 1] && s[pos] != code[pc + 2]))
                        goto fail;
                    ++pos;
                    pc += 3;
                    continue;
                case OpLit2Back:
                    if (pos <= 0 || (s[pos - 1] != code[pc + 1] && s[pos - 1] != code[pc + 2]))
                        goto fail;
                    --pos;
                    pc += 3;
                    continue;
                case OpStr: {
                    const std::int32_t size = std::int32_t(code[pc + 2]);
                    if (length - pos < size || std::memcmp(s + pos, program.literals.data() + code[pc + 1], std::size_t(size) * sizeof(char16_t)) != 0)
                        goto fail;
                    pos += size;
                    pc += 3;
                    continue;
                }
                case OpSet:
                    if (pos >= length || !sets[code[pc + 1]].has(s[pos]))
                        goto fail;
                    ++pos;
                    pc += 2;
                    continue;
                case OpSetBack:
                    if (pos <= 0 || !sets[code[pc + 1]].has(s[pos - 1]))
                        goto fail;
                    --pos;
                    pc += 2;
                    continue;
                case OpSetU: {
                    char32_t c;
                    if (!nextCode(pos, c) || !sets[code[pc + 1]].has(c))
                        goto fail;
                    pc += 2;
                    continue;
                }
                case OpSetUBack: {
                    char32_t c;
                    if (!previousCode(pos, c) || !sets[code[pc + 1]].has(c))
                        goto fail;
                    pc += 2;
                    continue;
                }
                case OpGreedy: {
                    const CodeSet& set = sets[code[pc + 1]];
                    const std::uint32_t min = code[pc + 2];
                    const std::uint32_t max = code[pc + 3];
                    const std::int32_t begin = pos;
                    const std::int64_t limit = max == kInfinity ? length : std::min<std::int64_t>(length, std::int64_t(pos) + max);
                    while (pos < limit && set.has(s[pos]))
                        ++pos;
                    if (std::uint32_t(pos - begin) < min)
                        goto fail;
                    pc += 4;
                    if (std::uint32_t(pos - begin) > min) {
                        if (sp + 4 > capacity)
                            grow();
                        stack[sp++] = std::uint32_t(begin) + min;
                        stack[sp++] = std::uint32_t(pos);
                        stack[sp++] = pc | kGreedyTag;
                    }
                    continue;
                }
                case OpGreedyU: {
                    const CodeSet& set = sets[code[pc + 1]];
                    const std::uint32_t min = code[pc + 2];
                    const std::uint32_t max = code[pc + 3];
                    std::uint32_t count = 0;
                    std::int32_t lowest = pos;
                    while (count < max) {
                        std::int32_t next = pos;
                        char32_t c;
                        if (!nextCode(next, c) || !set.has(c))
                            break;
                        pos = next;
                        ++count;
                        if (count == min)
                            lowest = pos;
                    }
                    if (count < min)
                        goto fail;
                    pc += 4;
                    if (count > min) {
                        if (sp + 4 > capacity)
                            grow();
                        stack[sp++] = std::uint32_t(lowest);
                        stack[sp++] = std::uint32_t(pos);
                        stack[sp++] = pc | kGreedyUTag;
                    }
                    continue;
                }
                case OpSplit:
                    push2(std::uint32_t(pos), code[pc + 1]);
                    pc += 2;
                    continue;
                case OpSplitLazy:
                    push2(std::uint32_t(pos), pc + 2);
                    pc = code[pc + 1];
                    continue;
                case OpJump: pc = code[pc + 1]; continue;
                case OpSave:
                    setRegister(regs, code[pc + 1], pos);
                    pc += 2;
                    continue;
                case OpClear:
                    for (std::uint32_t r = code[pc + 1]; r < code[pc + 2]; ++r) {
                        if (regs[r] != -1)
                            setRegister(regs, r, -1);
                    }
                    pc += 3;
                    continue;
                case OpMark:
                    setRegister(regs, code[pc + 1], pos);
                    pc += 2;
                    continue;
                case OpCheckProgress:
                    if (regs[code[pc + 1]] == pos)
                        goto fail;
                    pc += 2;
                    continue;
                case OpCheckCounted:
                    if (regs[code[pc + 1]] == pos && std::uint32_t(regs[code[pc + 2]]) >= code[pc + 3])
                        goto fail;
                    pc += 4;
                    continue;
                case OpLoopInit:
                    setRegister(regs, code[pc + 1], 0);
                    pc += 2;
                    continue;
                case OpLoopEnter: {
                    const std::uint32_t count = std::uint32_t(regs[code[pc + 1]]);
                    const std::uint32_t min = code[pc + 2];
                    const std::uint32_t max = code[pc + 3];
                    const std::uint32_t exit = code[pc + 5];
                    if (m.poll && !(++steps & 0xfffff))
                        m.poll(m.pollContext, capacity * sizeof(std::uint32_t));
                    if (count < min) {
                        pc += 6;
                    } else if (count >= max) {
                        pc = exit;
                    } else if (code[pc + 4]) {
                        push2(std::uint32_t(pos), exit);
                        pc += 6;
                    } else {
                        push2(std::uint32_t(pos), pc + 6);
                        pc = exit;
                    }
                    continue;
                }
                case OpLoopNext: {
                    const std::uint32_t r = code[pc + 1];
                    setRegister(regs, r, regs[r] + 1);
                    pc += 2;
                    continue;
                }
                case OpLineStart:
                    if (pos != 0)
                        goto fail;
                    ++pc;
                    continue;
                case OpLineStartM:
                    if (pos != 0 && !isLineEnd(s[pos - 1]))
                        goto fail;
                    ++pc;
                    continue;
                case OpLineEnd:
                    if (pos != length)
                        goto fail;
                    ++pc;
                    continue;
                case OpLineEndM:
                    if (pos != length && !isLineEnd(s[pos]))
                        goto fail;
                    ++pc;
                    continue;
                case OpWord: {
                    const bool folds = code[pc + 2];
                    const bool at = wordAt(s, pos - 1, length, folds) != wordAt(s, pos, length, folds);
                    if (at == bool(code[pc + 1]))
                        goto fail;
                    pc += 3;
                    continue;
                }
                case OpBackRef:
                    if (!backRef(regs, code[pc + 1], pos, false, false))
                        goto fail;
                    pc += 2;
                    continue;
                case OpBackRefBack:
                    if (!backRef(regs, code[pc + 1], pos, false, true))
                        goto fail;
                    pc += 2;
                    continue;
                case OpBackRefFold:
                    if (!backRef(regs, code[pc + 1], pos, true, false))
                        goto fail;
                    pc += 2;
                    continue;
                case OpBackRefFoldBack:
                    if (!backRef(regs, code[pc + 1], pos, true, true))
                        goto fail;
                    pc += 2;
                    continue;
                case OpLookBegin: {
                    const std::uint32_t r = code[pc + 1];
                    setRegister(regs, r + 1, pos);
                    setRegister(regs, r, std::int32_t(sp + 2));
                    pc += 2;
                    continue;
                }
                case OpLookEnd: {
                    // Inside is settled: its choices go, what it set stays, and undoing that stays possible.
                    const std::uint32_t r = code[pc + 1];
                    const std::size_t height = std::size_t(regs[r]);
                    pos = regs[r + 1];
                    std::size_t read = sp;
                    std::size_t write = sp;
                    while (read > height) {
                        const std::uint32_t tag = stack[read - 1];
                        if (tag & kUndoTag) {
                            stack[--write] = tag;
                            stack[--write] = stack[read - 2];
                            read -= 2;
                        } else {
                            read -= (tag & (kGreedyTag | kGreedyUTag)) ? 3 : 2;
                        }
                    }
                    const std::size_t kept = sp - write;
                    if (write != height)
                        std::memmove(stack + height, stack + write, kept * sizeof(std::uint32_t));
                    sp = height + kept;
                    pc += 2;
                    continue;
                }
                case OpNegBegin: {
                    const std::uint32_t r = code[pc + 1];
                    setRegister(regs, r, std::int32_t(sp + 2));
                    push2(std::uint32_t(pos), code[pc + 2]);
                    pc += 3;
                    continue;
                }
                case OpNegEnd: {
                    // Inside matched, so this fails: back to before it, and the choice to go on without it goes too.
                    const std::size_t height = std::size_t(regs[code[pc + 1]]);
                    while (sp > height) {
                        const std::uint32_t tag = stack[sp - 1];
                        if (tag & kUndoTag) {
                            regs[tag & ~kUndoTag] = std::int32_t(stack[sp - 2]);
                            sp -= 2;
                        } else {
                            sp -= (tag & (kGreedyTag | kGreedyUTag)) ? 3 : 2;
                        }
                    }
                    goto fail;
                }
            }
        fail:
            if (m.poll && !(++steps & 0xfffff))
                m.poll(m.pollContext, capacity * sizeof(std::uint32_t));
            for (;;) {
                if (sp == 0)
                    return false;
                const std::uint32_t tag = stack[sp - 1];
                if (tag & kUndoTag) {
                    regs[tag & ~kUndoTag] = std::int32_t(stack[sp - 2]);
                    sp -= 2;
                    continue;
                }
                if (tag & kGreedyTag) {
                    const std::int32_t lowest = std::int32_t(stack[sp - 3]);
                    const std::int32_t at = std::int32_t(stack[sp - 2]) - 1;
                    pos = at;
                    pc = tag & kPcMask;
                    if (at == lowest)
                        sp -= 3;
                    else
                        stack[sp - 2] = std::uint32_t(at);
                    break;
                }
                if (tag & kGreedyUTag) {
                    const std::int32_t lowest = std::int32_t(stack[sp - 3]);
                    std::int32_t at = std::int32_t(stack[sp - 2]) - 1;
                    if (at > lowest && isLow(s[at]) && isHigh(s[at - 1]))
                        --at;
                    pos = at;
                    pc = tag & kPcMask;
                    if (at == lowest)
                        sp -= 3;
                    else
                        stack[sp - 2] = std::uint32_t(at);
                    break;
                }
                pos = std::int32_t(stack[sp - 2]);
                pc = tag;
                sp -= 2;
                break;
            }
        }
    }
};

}  // namespace

std::shared_ptr<const Program> compile(std::u16string_view pattern, unsigned flags, std::string& error)
{
    auto program = std::make_shared<Program>();
    program->flags = flags;
    try {
        PatternParser parser(pattern, flags, program->names);
        Node* root = parser.parse();
        program->groups = parser.groupsSeen() + 1;
        Generator(*program, flags & (kUnicode | kUnicodeSets)).run(root);
    } catch (const BadPattern& failure) {
        error = failure.message;
        return nullptr;
    }
    return program;
}

bool literalError(std::u16string_view pattern, std::u16string_view flags, std::string& error)
{
    unsigned bits = 0;
    if (!parseFlags(flags, bits)) {
        error = "invalid flags";
        return true;
    }
    return compile(pattern, bits, error) == nullptr;
}

bool exec(const Program& program, std::u16string_view input, std::size_t start, Matcher& matcher)
{
    const char16_t* const s = input.data();
    const std::int32_t length = std::int32_t(input.size());
    const bool pairs = program.flags & (kUnicode | kUnicodeSets);
    std::int32_t at = std::int32_t(start);
    // In the middle of a pair, the character there is the pair.
    if (pairs && at > 0 && at < length && isLow(s[at]) && isHigh(s[at - 1]))
        --at;
    if (matcher.stack.size() < 256)
        matcher.stack.resize(256);
    matcher.registers.resize(program.registers);
    std::int32_t* const regs = matcher.registers.data();
    Run run{program, matcher, s, length, pairs, matcher.stack.data(), matcher.stack.size()};
    // `^` without `m` can only match where it starts: one try, with no scan ahead for a likely first character, which
    // on a long input would make every failed `/^.../` cost the length of the input.
    const bool once = (program.flags & kSticky) != 0 || (program.anchored && !(program.flags & kMultiline));
    const CodeSet* first = !once && program.firstSet >= 0 ? &program.sets[std::size_t(program.firstSet)] : nullptr;
    for (;;) {
        if (first) {
            if (!pairs) {
                while (at < length && !first->has(s[at]))
                    ++at;
            } else {
                while (at < length && !first->has(s[at]))
                    at += isHigh(s[at]) && at + 1 < length && isLow(s[at + 1]) ? 2 : 1;
            }
            if (at >= length)
                return false;
        }
        std::fill_n(regs, program.registers, -1);
        if (run.match(at, regs))
            return true;
        if (once || at >= length || (program.anchored && !(program.flags & kMultiline)))
            return false;
        at += pairs && isHigh(s[at]) && at + 1 < length && isLow(s[at + 1]) ? 2 : 1;
    }
}

}  // namespace regex
}  // namespace dyn
}  // namespace internal
}  // namespace script
