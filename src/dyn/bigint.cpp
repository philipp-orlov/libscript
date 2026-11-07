// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/dyn/bigint.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/vm.hpp"

namespace script {
namespace internal {
namespace dyn {
namespace bigint {

namespace {

using Digits = Vec<std::uint32_t>;
constexpr std::size_t kMaxDigits = (std::size_t(1) << 30) / 32;  // a billion bits

void trim(Digits& d)
{
    while (!d.empty() && d.back() == 0)
        d.pop_back();
}

[[noreturn]] void tooBig(Runtime& rt) { throwError(rt, ErrorCode::RangeError, "BigInt too large"); }

Value make(Runtime& rt, bool negative, Digits digits)
{
    trim(digits);
    if (digits.size() > kMaxDigits)
        tooBig(rt);
    auto* cell = rt.newCell<BigIntCell>();
    Value handle = detail::Access::adopt(Type::BigInt, cell);
    cell->negative = negative && !digits.empty();
    rt.ctx.reserve(digits.size() * sizeof(std::uint32_t));
    cell->digits = std::move(digits);
    return handle;
}

int compareMagnitude(const Digits& a, const Digits& b) noexcept
{
    if (a.size() != b.size())
        return a.size() < b.size() ? -1 : 1;
    for (std::size_t i = a.size(); i-- > 0;) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

Digits addMagnitude(const Digits& a, const Digits& b)
{
    const Digits& big = a.size() >= b.size() ? a : b;
    const Digits& small = a.size() >= b.size() ? b : a;
    Digits out(big.size() + 1);
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < big.size(); ++i) {
        const std::uint64_t sum = std::uint64_t(big[i]) + (i < small.size() ? small[i] : 0) + carry;
        out[i] = std::uint32_t(sum);
        carry = sum >> 32;
    }
    out[big.size()] = std::uint32_t(carry);
    trim(out);
    return out;
}

/// `a` - `b`, with `a` the larger.
Digits subMagnitude(const Digits& a, const Digits& b)
{
    Digits out(a.size());
    std::int64_t borrow = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const std::int64_t diff = std::int64_t(a[i]) - (i < b.size() ? std::int64_t(b[i]) : 0) - borrow;
        out[i] = std::uint32_t(diff);
        borrow = diff < 0 ? 1 : 0;
    }
    trim(out);
    return out;
}

Digits mulMagnitude(Runtime& rt, const Digits& a, const Digits& b)
{
    if (a.empty() || b.empty())
        return Digits();
    if (a.size() + b.size() > kMaxDigits + 1)
        tooBig(rt);
    Digits out(a.size() + b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        checkLongLoop(rt, i, out.size() * sizeof(std::uint32_t));
        std::uint64_t carry = 0;
        for (std::size_t j = 0; j < b.size(); ++j) {
            const std::uint64_t cur = std::uint64_t(a[i]) * b[j] + out[i + j] + carry;
            out[i + j] = std::uint32_t(cur);
            carry = cur >> 32;
        }
        out[i + b.size()] += std::uint32_t(carry);
    }
    trim(out);
    return out;
}

std::uint32_t divSmall(Digits& a, std::uint32_t by)
{
    std::uint64_t rem = 0;
    for (std::size_t i = a.size(); i-- > 0;) {
        const std::uint64_t cur = (rem << 32) | a[i];
        a[i] = std::uint32_t(cur / by);
        rem = cur % by;
    }
    trim(a);
    return std::uint32_t(rem);
}

/// Knuth's algorithm D. `a` becomes the remainder, and what comes back is the quotient.
Digits divMagnitude(Runtime& rt, Digits& a, const Digits& b)
{
    if (compareMagnitude(a, b) < 0)
        return Digits();
    if (b.size() == 1) {
        Digits q = a;
        const std::uint32_t r = divSmall(q, b[0]);
        a.clear();
        if (r)
            a.push_back(r);
        return q;
    }
    const int shift = __builtin_clz(b.back());
    const auto shifted = [shift](const Digits& d, std::size_t extra) {
        Digits out(d.size() + extra);
        std::uint32_t carry = 0;
        for (std::size_t i = 0; i < d.size(); ++i) {
            out[i] = (d[i] << shift) | carry;
            carry = shift ? d[i] >> (32 - shift) : 0;
        }
        if (extra)
            out[d.size()] = carry;
        return out;
    };
    Digits u = shifted(a, 1);
    const Digits v = shifted(b, 0);
    const std::size_t n = v.size();
    const std::size_t m = a.size() - n;
    Digits q(m + 1);
    const std::uint64_t top = v[n - 1];
    const std::uint64_t next = v[n - 2];
    for (std::size_t j = m + 1; j-- > 0;) {
        checkLongLoop(rt, j, u.size() * sizeof(std::uint32_t));
        const std::uint64_t numerator = (std::uint64_t(u[j + n]) << 32) | u[j + n - 1];
        std::uint64_t qhat = numerator / top;
        std::uint64_t rhat = numerator % top;
        while (qhat >> 32 || qhat * next > ((rhat << 32) | u[j + n - 2])) {
            --qhat;
            rhat += top;
            if (rhat >> 32)
                break;
        }
        std::int64_t borrow = 0;
        std::uint64_t carry = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint64_t p = qhat * v[i] + carry;
            carry = p >> 32;
            const std::int64_t diff = std::int64_t(u[i + j]) - std::int64_t(std::uint32_t(p)) - borrow;
            u[i + j] = std::uint32_t(diff);
            borrow = diff < 0 ? 1 : 0;
        }
        const std::int64_t diff = std::int64_t(u[j + n]) - std::int64_t(carry) - borrow;
        u[j + n] = std::uint32_t(diff);
        if (diff < 0) {
            --qhat;
            std::uint64_t add = 0;
            for (std::size_t i = 0; i < n; ++i) {
                const std::uint64_t sum = std::uint64_t(u[i + j]) + v[i] + add;
                u[i + j] = std::uint32_t(sum);
                add = sum >> 32;
            }
            u[j + n] += std::uint32_t(add);
        }
        q[j] = std::uint32_t(qhat);
    }
    a.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i)
        a[i] = shift ? (u[i] >> shift) | (u[i + 1] << (32 - shift)) : u[i];
    trim(a);
    trim(q);
    return q;
}

Value addSigned(Runtime& rt, const BigIntCell* a, const BigIntCell* b, bool flipB)
{
    const bool bNegative = b->negative != flipB;
    if (a->negative == bNegative)
        return make(rt, a->negative, addMagnitude(a->digits, b->digits));
    const int order = compareMagnitude(a->digits, b->digits);
    if (order == 0)
        return make(rt, false, Digits());
    if (order > 0)
        return make(rt, a->negative, subMagnitude(a->digits, b->digits));
    return make(rt, bNegative, subMagnitude(b->digits, a->digits));
}

Digits shiftLeft(Runtime& rt, const Digits& a, std::uint64_t by)
{
    if (a.empty())
        return Digits();
    const std::uint64_t words = by / 32;
    if (words > kMaxDigits)
        tooBig(rt);
    const int bits = int(by % 32);
    Digits out(std::size_t(words) + a.size() + 1);
    std::uint32_t carry = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        out[std::size_t(words) + i] = (a[i] << bits) | carry;
        carry = bits ? a[i] >> (32 - bits) : 0;
    }
    out[std::size_t(words) + a.size()] = carry;
    return out;
}

/// Toward negative infinity, as the language rounds a shift of a negative number.
Value shiftRight(Runtime& rt, const BigIntCell* a, std::uint64_t by)
{
    const Digits& d = a->digits;
    const std::uint64_t words = by / 32;
    const int bits = int(by % 32);
    if (words >= d.size())
        return a->negative ? fromInt64(rt, -1) : make(rt, false, Digits());
    Digits out(d.size() - std::size_t(words));
    bool lost = false;
    for (std::size_t i = 0; i < std::size_t(words); ++i)
        lost = lost || d[i];
    if (bits && (d[std::size_t(words)] & ((1u << bits) - 1)))
        lost = true;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::uint32_t low = d[std::size_t(words) + i];
        const std::uint32_t high = std::size_t(words) + i + 1 < d.size() ? d[std::size_t(words) + i + 1] : 0;
        out[i] = bits ? (low >> bits) | (high << (32 - bits)) : low;
    }
    trim(out);
    if (a->negative && lost) {
        Digits one(1, 1u);
        out = addMagnitude(out, one);
    }
    return make(rt, a->negative, std::move(out));
}

/// Two's complement in `size` words.
Digits twos(const BigIntCell* a, std::size_t size)
{
    Digits out(size);
    std::copy_n(a->digits.begin(), std::min(size, a->digits.size()), out.begin());
    if (a->negative) {
        std::uint64_t carry = 1;
        for (std::size_t i = 0; i < size; ++i) {
            const std::uint64_t cur = std::uint64_t(~out[i]) + carry;
            out[i] = std::uint32_t(cur);
            carry = cur >> 32;
        }
    }
    return out;
}

Value fromTwos(Runtime& rt, Digits d)
{
    const bool negative = !d.empty() && (d.back() >> 31);
    if (negative) {
        std::uint64_t carry = 1;
        for (std::uint32_t& word : d) {
            const std::uint64_t cur = std::uint64_t(~word) + carry;
            word = std::uint32_t(cur);
            carry = cur >> 32;
        }
    }
    return make(rt, negative, std::move(d));
}

Value bitwise(Runtime& rt, BinaryOp op, const BigIntCell* a, const BigIntCell* b)
{
    const std::size_t size = std::max(a->digits.size(), b->digits.size()) + 1;
    Digits x = twos(a, size);
    const Digits y = twos(b, size);
    for (std::size_t i = 0; i < size; ++i) {
        if (op == BinaryOp::And)
            x[i] &= y[i];
        else if (op == BinaryOp::Or)
            x[i] |= y[i];
        else
            x[i] ^= y[i];
    }
    return fromTwos(rt, std::move(x));
}

std::uint64_t shiftCount(Runtime& rt, const BigIntCell* b)
{
    if (b->digits.size() > 2)
        tooBig(rt);
    return b->digits.empty() ? 0 : b->digits.size() == 1 ? b->digits[0] : (std::uint64_t(b->digits[1]) << 32) | b->digits[0];
}

Value power(Runtime& rt, const BigIntCell* a, const BigIntCell* b)
{
    if (b->negative)
        throwError(rt, ErrorCode::RangeError, "a BigInt raised to a negative power");
    if (b->digits.empty())
        return fromInt64(rt, 1);
    if (a->digits.empty())
        return make(rt, false, Digits());
    if (a->digits.size() == 1 && a->digits[0] == 1) {
        const bool odd = b->digits[0] & 1;
        return fromInt64(rt, a->negative && odd ? -1 : 1);
    }
    if (b->digits.size() > 1)
        tooBig(rt);
    std::uint32_t exponent = b->digits[0];
    Digits result(1, 1u);
    Digits base = a->digits;
    const bool negative = a->negative && (exponent & 1);
    while (exponent) {
        if (exponent & 1)
            result = mulMagnitude(rt, result, base);
        exponent >>= 1;
        if (exponent)
            base = mulMagnitude(rt, base, base);
    }
    return make(rt, negative, std::move(result));
}

}  // namespace

Value fromUint64(Runtime& rt, std::uint64_t value)
{
    Digits d;
    if (value)
        d.push_back(std::uint32_t(value));
    if (value >> 32)
        d.push_back(std::uint32_t(value >> 32));
    return make(rt, false, std::move(d));
}

Value fromInt64(Runtime& rt, std::int64_t value)
{
    const std::uint64_t magnitude = value < 0 ? ~std::uint64_t(value) + 1 : std::uint64_t(value);
    Digits d;
    if (magnitude)
        d.push_back(std::uint32_t(magnitude));
    if (magnitude >> 32)
        d.push_back(std::uint32_t(magnitude >> 32));
    return make(rt, value < 0, std::move(d));
}

Value fromDouble(Runtime& rt, double value)
{
    const bool negative = value < 0;
    double m = std::fabs(value);
    if (m < 1)
        return make(rt, false, Digits());
    int exponent = 0;
    const double fraction = std::frexp(m, &exponent);
    // fraction * 2^53 is a whole number of 53 bits; the rest is a shift.
    const std::uint64_t bits = std::uint64_t(std::ldexp(fraction, 53));
    Digits d;
    d.push_back(std::uint32_t(bits));
    d.push_back(std::uint32_t(bits >> 32));
    if (exponent >= 53)
        return make(rt, negative, shiftLeft(rt, d, std::uint64_t(exponent - 53)));
    const std::uint64_t whole = bits >> (53 - exponent);
    d.clear();
    d.push_back(std::uint32_t(whole));
    d.push_back(std::uint32_t(whole >> 32));
    return make(rt, negative, std::move(d));
}

bool parseDigits(Runtime& rt, StringView digits, int radix, bool negative, Value& out)
{
    if (digits.empty())
        return false;
    Digits d;
    std::size_t step = 0;
    for (Char c : digits) {
        int value;
        if (c >= '0' && c <= '9')
            value = c - '0';
        else if (c >= 'a' && c <= 'z')
            value = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z')
            value = c - 'A' + 10;
        else
            return false;
        if (value >= radix)
            return false;
        std::uint64_t carry = std::uint64_t(value);
        for (std::uint32_t& word : d) {
            const std::uint64_t cur = std::uint64_t(word) * std::uint64_t(radix) + carry;
            word = std::uint32_t(cur);
            carry = cur >> 32;
        }
        if (carry)
            d.push_back(std::uint32_t(carry));
        checkLongLoop(rt, ++step, d.size() * sizeof(std::uint32_t));
    }
    out = make(rt, negative, std::move(d));
    return true;
}

bool parseString(Runtime& rt, StringView text, Value& out)
{
    const auto isSpace = [](Char c) {
        return c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32 || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 ||
               c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000 || c == 0xfeff;
    };
    while (!text.empty() && isSpace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isSpace(text.back()))
        text.remove_suffix(1);
    if (text.empty()) {
        out = make(rt, false, Digits());
        return true;
    }
    if (text.size() > 2 && text[0] == '0') {
        const Char kind = text[1];
        const int radix = (kind == 'x' || kind == 'X') ? 16 : (kind == 'o' || kind == 'O') ? 8 : (kind == 'b' || kind == 'B') ? 2 : 0;
        if (radix)
            return parseDigits(rt, text.substr(2), radix, false, out);
    }
    bool negative = false;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        text.remove_prefix(1);
    }
    return parseDigits(rt, text, 10, negative, out);
}

namespace {

String digitsToString(Digits d, bool negative, int radix)
{
    if (d.empty())
        return ascii("0");
    // As many digits at a time as fit a word.
    std::uint32_t chunk = std::uint32_t(radix);
    int perChunk = 1;
    while (std::uint64_t(chunk) * std::uint64_t(radix) <= 0xffffffffu) {
        chunk *= std::uint32_t(radix);
        ++perChunk;
    }
    String out;
    while (!d.empty()) {
        std::uint32_t rem = divSmall(d, chunk);
        for (int i = 0; i < perChunk; ++i) {
            if (d.empty() && rem == 0)
                break;
            const std::uint32_t digit = rem % std::uint32_t(radix);
            out.push_back(Char(digit < 10 ? '0' + digit : 'a' + digit - 10));
            rem /= std::uint32_t(radix);
        }
    }
    if (negative)
        out.push_back('-');
    std::reverse(out.begin(), out.end());
    return out;
}

}  // namespace

String toString(const BigIntCell* a, int radix) { return digitsToString(a->digits, a->negative, radix); }

String literalToDecimal(StringView text)
{
    int radix = 10;
    if (text.size() > 2 && text[0] == '0') {
        const Char kind = text[1];
        radix = (kind == 'x' || kind == 'X') ? 16 : (kind == 'o' || kind == 'O') ? 8 : (kind == 'b' || kind == 'B') ? 2 : 10;
        if (radix != 10)
            text.remove_prefix(2);
    }
    Digits d;
    for (Char c : text) {
        std::uint64_t carry = std::uint64_t(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
        for (std::uint32_t& word : d) {
            const std::uint64_t cur = std::uint64_t(word) * std::uint64_t(radix) + carry;
            word = std::uint32_t(cur);
            carry = cur >> 32;
        }
        if (carry)
            d.push_back(std::uint32_t(carry));
    }
    return digitsToString(std::move(d), false, 10);
}

double toDouble(const BigIntCell* a) noexcept
{
    const Digits& d = a->digits;
    if (d.empty())
        return 0;
    const std::size_t bitLength = d.size() * 32 - std::size_t(__builtin_clz(d.back()));
    if (bitLength > 1024)
        return a->negative ? -HUGE_VAL : HUGE_VAL;
    // The top 64 bits, and whether anything below them is set, round correctly.
    std::uint64_t top = 0;
    bool sticky = false;
    const std::size_t shift = bitLength > 64 ? bitLength - 64 : 0;
    for (std::size_t i = d.size(); i-- > 0;) {
        const std::size_t bitAt = i * 32;
        if (bitAt >= shift + 64)
            continue;
        if (bitAt >= shift) {
            top |= std::uint64_t(d[i]) << (bitAt - shift);
        } else if (bitAt + 32 > shift) {
            const std::size_t inner = shift - bitAt;
            top |= std::uint64_t(d[i]) >> inner;
            if (d[i] & ((std::uint64_t(1) << inner) - 1))
                sticky = true;
        } else if (d[i]) {
            sticky = true;
        }
    }
    // Round to 53 bits, half to even, by hand: the conversion to double would do it, but cannot see `sticky`.
    if (bitLength > 53) {
        const int excess = bitLength >= 64 ? 11 : int(bitLength - 53);
        const std::uint64_t mask = (std::uint64_t(1) << excess) - 1;
        const std::uint64_t half = std::uint64_t(1) << (excess - 1);
        const std::uint64_t low = top & mask;
        top &= ~mask;
        if (low > half || (low == half && (sticky || (top >> excess & 1))))
            top += std::uint64_t(1) << excess;  // overflowing 64 bits only when it became a power of two
        if (top == 0) {
            const double r = std::ldexp(1.0, int(shift) + 64);
            return a->negative ? -r : r;
        }
    }
    const double r = std::ldexp(double(top), int(shift));
    return a->negative ? -r : r;
}

std::uint64_t toUint64(const BigIntCell* a) noexcept
{
    std::uint64_t magnitude = 0;
    if (!a->digits.empty())
        magnitude = a->digits[0];
    if (a->digits.size() > 1)
        magnitude |= std::uint64_t(a->digits[1]) << 32;
    return a->negative ? ~magnitude + 1 : magnitude;
}

Value binary(Runtime& rt, BinaryOp op, const BigIntCell* a, const BigIntCell* b)
{
    switch (op) {
        case BinaryOp::Add: return addSigned(rt, a, b, false);
        case BinaryOp::Sub: return addSigned(rt, a, b, true);
        case BinaryOp::Mul: return make(rt, a->negative != b->negative, mulMagnitude(rt, a->digits, b->digits));
        case BinaryOp::Div:
        case BinaryOp::Mod: {
            if (b->digits.empty())
                throwError(rt, ErrorCode::RangeError, "division by zero");
            Digits rem = a->digits;
            Digits quotient = divMagnitude(rt, rem, b->digits);
            if (op == BinaryOp::Div)
                return make(rt, a->negative != b->negative, std::move(quotient));
            return make(rt, a->negative, std::move(rem));
        }
        case BinaryOp::Pow: return power(rt, a, b);
        case BinaryOp::And:
        case BinaryOp::Or:
        case BinaryOp::Xor: return bitwise(rt, op, a, b);
        case BinaryOp::Shl:
        case BinaryOp::Shr: {
            const bool left = (op == BinaryOp::Shl) != b->negative;
            if (left)
                return make(rt, a->negative, shiftLeft(rt, a->digits, a->digits.empty() ? 0 : shiftCount(rt, b)));
            if (b->digits.size() > 2)
                return a->negative ? fromInt64(rt, -1) : make(rt, false, Digits());
            return shiftRight(rt, a, shiftCount(rt, b));
        }
    }
    return Value::undefined();
}

Value negate(Runtime& rt, const BigIntCell* a) { return make(rt, !a->negative, a->digits); }

Value bitNot(Runtime& rt, const BigIntCell* a)
{
    // ~x is -x - 1.
    Digits one(1, 1u);
    if (a->negative)
        return make(rt, false, subMagnitude(a->digits, one));
    return make(rt, true, addMagnitude(a->digits, one));
}

Value increment(Runtime& rt, const BigIntCell* a, int by)
{
    Digits one(1, 1u);
    const bool towardZero = a->negative == (by > 0);
    if (a->digits.empty())
        return make(rt, by < 0, std::move(one));
    if (!towardZero)
        return make(rt, a->negative, addMagnitude(a->digits, one));
    return make(rt, a->negative, subMagnitude(a->digits, one));
}

Value asIntN(Runtime& rt, std::uint64_t bits, const BigIntCell* a, bool isSigned)
{
    if (bits == 0)
        return make(rt, false, Digits());
    const std::uint64_t magnitudeBits = a->digits.empty() ? 0 : a->digits.size() * 32 - std::uint64_t(__builtin_clz(a->digits.back()));
    // What fits as it is stays as it is.
    if (!a->negative && magnitudeBits < bits)
        return make(rt, false, a->digits);
    if (a->negative && isSigned && magnitudeBits < bits)
        return make(rt, true, a->digits);
    if (bits > kMaxDigits * 32) {
        if (!isSigned && a->negative)
            tooBig(rt);
        return make(rt, a->negative, a->digits);
    }
    const std::size_t words = std::size_t((bits + 31) / 32) + 1;
    Digits d = twos(a, std::max(words, a->digits.size() + 1));
    d.resize(words);
    const int top = int(bits % 32);
    const std::size_t last = std::size_t((bits - 1) / 32);
    const bool signBit = d[last] >> ((bits - 1) % 32) & 1;
    for (std::size_t i = last + 1; i < words; ++i)
        d[i] = 0;
    if (top)
        d[last] &= (1u << top) - 1;
    if (!isSigned || !signBit)
        return make(rt, false, std::move(d));
    // Negative: extend the sign and take the two's complement back.
    if (top)
        d[last] |= ~((1u << top) - 1);
    for (std::size_t i = last + 1; i < words; ++i)
        d[i] = 0xffffffffu;
    return fromTwos(rt, std::move(d));
}

int compare(const BigIntCell* a, const BigIntCell* b) noexcept
{
    if (a->negative != b->negative)
        return a->negative ? -1 : 1;
    const int order = compareMagnitude(a->digits, b->digits);
    return a->negative ? -order : order;
}

int compareDouble(const BigIntCell* a, double d) noexcept
{
    if (std::isnan(d))
        return 2;
    if (std::isinf(d))
        return d > 0 ? -1 : 1;
    if (a->digits.empty())
        return d > 0 ? -1 : d < 0 ? 1 : 0;
    if (a->negative != (d < 0) || d == 0)
        return a->negative ? -1 : 1;
    // Same sign, neither zero. Compare the whole part as an integer, then what is left of the fraction.
    const double whole = std::trunc(std::fabs(d));
    const std::size_t bitLength = a->digits.size() * 32 - std::size_t(__builtin_clz(a->digits.back()));
    int exponent = 0;
    if (whole >= 1)
        std::frexp(whole, &exponent);
    int magnitudeOrder;
    if (int(bitLength) != exponent) {
        magnitudeOrder = int(bitLength) < exponent ? -1 : 1;
    } else {
        // At most 1024 bits: place the double's 53 bits and compare word by word.
        int shift = 0;
        const double fraction = std::frexp(whole, &shift);
        const std::uint64_t bits = std::uint64_t(std::ldexp(fraction, 53));
        magnitudeOrder = 0;
        const int below = shift - 53;
        for (std::size_t i = a->digits.size(); i-- > 0 && magnitudeOrder == 0;) {
            const int bitAt = int(i) * 32;
            std::uint64_t word = 0;
            if (below >= 0) {
                if (bitAt + 32 > below && bitAt < below + 53) {
                    const int offset = bitAt - below;
                    word = offset >= 0 ? (bits >> offset) & 0xffffffffu : (bits << -offset) & 0xffffffffu;
                }
            } else {
                word = bitAt < 64 ? ((bits >> -below) >> bitAt) & 0xffffffffu : 0;
            }
            if (a->digits[i] != word)
                magnitudeOrder = a->digits[i] < word ? -1 : 1;
        }
        if (magnitudeOrder == 0 && std::fabs(d) > whole)
            magnitudeOrder = -1;
    }
    return a->negative ? -magnitudeOrder : magnitudeOrder;
}

bool equal(const BigIntCell* a, const BigIntCell* b) noexcept
{
    return a->negative == b->negative && a->digits == b->digits;
}

}  // namespace bigint
}  // namespace dyn
}  // namespace internal
}  // namespace script
