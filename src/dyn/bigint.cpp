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

Env envFor(Runtime& rt)
{
    return Env{rt.ctx,
               [](void* owner, ErrorCode code, const char* message) { throwError(*static_cast<Runtime*>(owner), code, message); },
               &rt,
               [](void* owner, std::size_t bytes) { checkLongLoopSlow(*static_cast<Runtime*>(owner), bytes); }};
}

namespace {

using Limb = BigIntCell::Limb;
using Wide = unsigned __int128;
using Digits = Vec<Limb>;
using Limbs = BigIntCell::Limbs;
constexpr int kLimbBits = 64;
constexpr std::size_t kMaxDigits = (std::size_t(1) << 30) / kLimbBits;  // a billion bits

void trim(Digits& d)
{
    while (!d.empty() && d.back() == 0)
        d.pop_back();
}

[[noreturn]] void tooBig(Env& rt) { rt.fail(ErrorCode::RangeError, "BigInt too large"); }

/// A new BigInt with room for `n` limbs, which the caller writes; `finish` then drops the zeros at the top.
BigIntCell* room(Env& rt, std::size_t n, Value& handle)
{
    if (n > kMaxDigits + 1)
        tooBig(rt);
    auto* cell = rt.ctx.newCellSized<BigIntCell>(n * sizeof(Limb), std::uint32_t(n));
    handle = detail::Access::adopt(Type::BigInt, cell);
    return cell;
}

Value finish(Env& rt, Value handle, BigIntCell* cell, bool negative)
{
    std::uint32_t n = cell->count;
    while (n && cell->limbs()[n - 1] == 0)
        --n;
    if (n > kMaxDigits)
        tooBig(rt);
    cell->count = n;
    cell->negative = negative && n;
    return handle;
}

Value make(Env& rt, bool negative, Limbs digits)
{
    std::size_t n = digits.size();
    while (n && digits[n - 1] == 0)
        --n;
    Value handle;
    BigIntCell* cell = room(rt, n, handle);
    if (n)
        std::memcpy(cell->limbs(), digits.begin(), n * sizeof(Limb));
    return finish(rt, std::move(handle), cell, negative);
}

Value make(Env& rt, bool negative, const Digits& digits) { return make(rt, negative, Limbs(digits)); }

int compareMagnitude(Limbs a, Limbs b) noexcept
{
    if (a.size() != b.size())
        return a.size() < b.size() ? -1 : 1;
    for (std::size_t i = a.size(); i-- > 0;) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

/// `a` + `b` into `out`, which has room for one limb more than the longer of them.
void addInto(Limbs a, Limbs b, Limb* out) noexcept
{
    const Limbs big = a.size() >= b.size() ? a : b;
    const Limbs small = a.size() >= b.size() ? b : a;
    Limb carry = 0;
    for (std::size_t i = 0; i < big.size(); ++i) {
        const Wide sum = Wide(big[i]) + (i < small.size() ? small[i] : 0) + carry;
        out[i] = Limb(sum);
        carry = Limb(sum >> kLimbBits);
    }
    out[big.size()] = carry;
}

Digits addMagnitude(Limbs a, Limbs b)
{
    Digits out(std::max(a.size(), b.size()) + 1);
    addInto(a, b, out.data());
    trim(out);
    return out;
}

/// `a` - `b` into `out`, with `a` the larger and `out` as long.
void subInto(Limbs a, Limbs b, Limb* out) noexcept
{
    Limb borrow = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const Limb x = a[i], y = i < b.size() ? b[i] : 0;
        const Limb diff = x - y;
        out[i] = diff - borrow;
        borrow = (x < y) | (diff < borrow);
    }
}

/// `a` - `b`, with `a` the larger.
Digits subMagnitude(Limbs a, Limbs b)
{
    Digits out(a.size());
    subInto(a, b, out.data());
    trim(out);
    return out;
}

/// `a` * `b` into `out`, which has room for both their limbs; neither is zero.
void mulInto(Env& rt, Limbs a, Limbs b, Limb* out)
{
    std::memset(out, 0, (a.size() + b.size()) * sizeof(Limb));
    for (std::size_t i = 0; i < a.size(); ++i) {
        rt.checkLoop(i, (a.size() + b.size()) * sizeof(Limb));
        Limb carry = 0;
        for (std::size_t j = 0; j < b.size(); ++j) {
            const Wide cur = Wide(a[i]) * b[j] + out[i + j] + carry;
            out[i + j] = Limb(cur);
            carry = Limb(cur >> kLimbBits);
        }
        out[i + b.size()] = carry;
    }
}

Digits mulMagnitude(Env& rt, Limbs a, Limbs b)
{
    if (a.empty() || b.empty())
        return Digits();
    if (a.size() + b.size() > kMaxDigits + 1)
        tooBig(rt);
    Digits out(a.size() + b.size());
    mulInto(rt, a, b, out.data());
    trim(out);
    return out;
}

/// A divisor with its top bit set, and its reciprocal: floor((2^128 - 1) / d) - 2^64.
struct Divisor
{
    Limb d, inverse;
    explicit Divisor(Limb by) noexcept : d(by)
    {
        // The reciprocal takes a division of two limbs by one, which no instruction of arm64 does: the last divisor's
        // is kept (a program that reduces by one modulus divides by the same top limb every time).
        thread_local Limb lastBy = 0, lastInverse = 0;
        if (by != lastBy) {
            lastInverse = Limb(~Wide(0) / by - (Wide(1) << kLimbBits));
            lastBy = by;
        }
        inverse = lastInverse;
    }
    /// (`high`, `low`) / d, with `high` below d: two multiplications (Moeller and Granlund, "Improved division by
    /// invariant integers", algorithm 4).
    Limb divide(Limb high, Limb low, Limb* rem) const noexcept
    {
        const Wide q = Wide(inverse) * high + ((Wide(high + 1) << kLimbBits) | low);
        Limb q1 = Limb(q >> kLimbBits);
        const Limb q0 = Limb(q);
        Limb r = low - q1 * d;
        if (r > q0) {
            --q1;
            r += d;
        }
        if (r >= d) {
            ++q1;
            r -= d;
        }
        *rem = r;
        return q1;
    }
};

Limb divSmall(Digits& a, Limb by)
{
    // Divisor and dividend shifted so that the divisor's top bit is set; the quotient is the same.
    const int shift = __builtin_clzll(by);
    const Divisor d(by << shift);
    const std::size_t n = a.size();
    Limb rem = shift && n ? a[n - 1] >> (kLimbBits - shift) : 0;
    for (std::size_t i = n; i-- > 0;) {
        const Limb low = (a[i] << shift) | (shift && i ? a[i - 1] >> (kLimbBits - shift) : 0);
        a[i] = d.divide(rem, low, &rem);
    }
    trim(a);
    return rem >> shift;
}

/// Knuth's algorithm D: the quotient comes back, and `rem` is set to the remainder.
Digits divMagnitude(Env& rt, Limbs a, Limbs b, Digits& rem)
{
    if (compareMagnitude(a, b) < 0) {
        rem.assign(a.begin(), a.end());
        return Digits();
    }
    if (b.size() == 1) {
        Digits q(a.begin(), a.end());
        const Limb r = divSmall(q, b[0]);
        if (r)
            rem.push_back(r);
        return q;
    }
    const int shift = __builtin_clzll(b.back());
    const auto shifted = [shift](Limbs d, std::size_t extra) {
        Digits out(d.size() + extra);
        Limb carry = 0;
        for (std::size_t i = 0; i < d.size(); ++i) {
            out[i] = (d[i] << shift) | carry;
            carry = shift ? d[i] >> (kLimbBits - shift) : 0;
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
    const Limb top = v[n - 1];
    const Limb next = v[n - 2];
    const Divisor byTop(top);
    for (std::size_t j = m + 1; j-- > 0;) {
        rt.checkLoop(j, u.size() * sizeof(Limb));
        Limb qhat, rhat;
        bool rhatTooBig = false;  // past a limb: the guess cannot be too large
        if (u[j + n] >= top) {
            qhat = ~Limb(0);
            const Wide r = ((Wide(u[j + n]) << kLimbBits) | u[j + n - 1]) - Wide(qhat) * top;
            rhatTooBig = (r >> kLimbBits) != 0;
            rhat = Limb(r);
        } else {
            qhat = byTop.divide(u[j + n], u[j + n - 1], &rhat);
        }
        while (!rhatTooBig && Wide(qhat) * next > ((Wide(rhat) << kLimbBits) | u[j + n - 2])) {
            --qhat;
            rhat += top;
            rhatTooBig = rhat < top;
        }
        Limb borrow = 0, carry = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const Wide p = Wide(qhat) * v[i] + carry;
            carry = Limb(p >> kLimbBits);
            const Limb x = u[i + j], y = Limb(p);
            const Limb diff = x - y;
            u[i + j] = diff - borrow;
            borrow = (x < y) | (diff < borrow);
        }
        const Wide owed = Wide(carry) + borrow;
        const bool negative = Wide(u[j + n]) < owed;
        u[j + n] = Limb(Wide(u[j + n]) - owed);
        if (negative) {
            --qhat;
            Limb add = 0;
            for (std::size_t i = 0; i < n; ++i) {
                const Wide sum = Wide(u[i + j]) + v[i] + add;
                u[i + j] = Limb(sum);
                add = Limb(sum >> kLimbBits);
            }
            u[j + n] += add;
        }
        q[j] = qhat;
    }
    // What is left in the low limbs of `u`, shifted back, in place.
    for (std::size_t i = 0; i < n; ++i)
        u[i] = shift ? (u[i] >> shift) | (u[i + 1] << (kLimbBits - shift)) : u[i];
    u.resize(n);
    trim(u);
    rem = std::move(u);
    trim(q);
    return q;
}

Value addSigned(Env& rt, const BigIntCell* a, const BigIntCell* b, bool flipB)
{
    const bool bNegative = b->negative != flipB;
    const Limbs x = a->digits(), y = b->digits();
    Value handle;
    if (a->negative == bNegative) {
        BigIntCell* cell = room(rt, std::max(x.size(), y.size()) + 1, handle);
        addInto(x, y, cell->limbs());
        return finish(rt, std::move(handle), cell, a->negative);
    }
    const int order = compareMagnitude(x, y);
    if (order == 0)
        return make(rt, false, Digits());
    BigIntCell* cell = room(rt, order > 0 ? x.size() : y.size(), handle);
    if (order > 0)
        subInto(x, y, cell->limbs());
    else
        subInto(y, x, cell->limbs());
    return finish(rt, std::move(handle), cell, order > 0 ? a->negative : bNegative);
}

Digits shiftLeft(Env& rt, Limbs a, std::uint64_t by)
{
    if (a.empty())
        return Digits();
    const std::uint64_t words = by / kLimbBits;
    if (words > kMaxDigits)
        tooBig(rt);
    const int bits = int(by % kLimbBits);
    Digits out(std::size_t(words) + a.size() + 1);
    Limb carry = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        out[std::size_t(words) + i] = (a[i] << bits) | carry;
        carry = bits ? a[i] >> (kLimbBits - bits) : 0;
    }
    out[std::size_t(words) + a.size()] = carry;
    return out;
}

/// Toward negative infinity, as the language rounds a shift of a negative number.
Value shiftRight(Env& rt, const BigIntCell* a, std::uint64_t by)
{
    const Limbs d = a->digits();
    const std::uint64_t words = by / kLimbBits;
    const int bits = int(by % kLimbBits);
    if (words >= d.size())
        return a->negative ? fromInt64(rt, -1) : make(rt, false, Digits());
    Digits out(d.size() - std::size_t(words));
    bool lost = false;
    for (std::size_t i = 0; i < std::size_t(words); ++i)
        lost = lost || d[i];
    if (bits && (d[std::size_t(words)] & ((Limb(1) << bits) - 1)))
        lost = true;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const Limb low = d[std::size_t(words) + i];
        const Limb high = std::size_t(words) + i + 1 < d.size() ? d[std::size_t(words) + i + 1] : 0;
        out[i] = bits ? (low >> bits) | (high << (kLimbBits - bits)) : low;
    }
    trim(out);
    if (a->negative && lost) {
        Digits one(1, Limb(1));
        out = addMagnitude(out, one);
    }
    return make(rt, a->negative, std::move(out));
}

/// Two's complement in `size` words.
Digits twos(const BigIntCell* a, std::size_t size)
{
    Digits out(size);
    std::copy_n(a->digits().begin(), std::min(size, a->digits().size()), out.begin());
    if (a->negative) {
        Limb carry = 1;
        for (std::size_t i = 0; i < size; ++i) {
            out[i] = ~out[i] + carry;
            carry = carry && out[i] == 0;
        }
    }
    return out;
}

Value fromTwos(Env& rt, Digits d)
{
    const bool negative = !d.empty() && (d.back() >> (kLimbBits - 1));
    if (negative) {
        Limb carry = 1;
        for (Limb& word : d) {
            word = ~word + carry;
            carry = carry && word == 0;
        }
    }
    return make(rt, negative, std::move(d));
}

Value bitwise(Env& rt, BinaryOp op, const BigIntCell* a, const BigIntCell* b)
{
    const std::size_t size = std::max(a->digits().size(), b->digits().size()) + 1;
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

std::uint64_t shiftCount(Env& rt, const BigIntCell* b)
{
    if (b->digits().size() > 1)
        tooBig(rt);
    return b->digits().empty() ? 0 : b->digits()[0];
}

Value power(Env& rt, const BigIntCell* a, const BigIntCell* b)
{
    if (b->negative)
        rt.fail(ErrorCode::RangeError, "a BigInt raised to a negative power");
    if (b->digits().empty())
        return fromInt64(rt, 1);
    if (a->digits().empty())
        return make(rt, false, Digits());
    if (a->digits().size() == 1 && a->digits()[0] == 1) {
        const bool odd = b->digits()[0] & 1;
        return fromInt64(rt, a->negative && odd ? -1 : 1);
    }
    if (b->digits().size() > 1 || b->digits()[0] >> 32)
        tooBig(rt);
    std::uint32_t exponent = std::uint32_t(b->digits()[0]);
    Digits result(1, Limb(1));
    Digits base(a->digits().begin(), a->digits().end());
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

Value fromUint64(Env& rt, std::uint64_t value)
{
    Digits d;
    if (value)
        d.push_back(value);
    return make(rt, false, std::move(d));
}

Value fromInt64(Env& rt, std::int64_t value)
{
    const std::uint64_t magnitude = value < 0 ? ~std::uint64_t(value) + 1 : std::uint64_t(value);
    Digits d;
    if (magnitude)
        d.push_back(magnitude);
    return make(rt, value < 0, std::move(d));
}

Value fromDouble(Env& rt, double value)
{
    const bool negative = value < 0;
    double m = std::fabs(value);
    if (m < 1)
        return make(rt, false, Digits());
    int exponent = 0;
    const double fraction = std::frexp(m, &exponent);
    // fraction * 2^53 is a whole number of 53 bits; the rest is a shift.
    const std::uint64_t bits = std::uint64_t(std::ldexp(fraction, 53));
    Digits d(1, bits);
    if (exponent >= 53)
        return make(rt, negative, shiftLeft(rt, d, std::uint64_t(exponent - 53)));
    d[0] = bits >> (53 - exponent);
    return make(rt, negative, std::move(d));
}

bool parseDigits(Env& rt, StringView digits, int radix, bool negative, Value& out)
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
        Limb carry = Limb(value);
        for (Limb& word : d) {
            const Wide cur = Wide(word) * Limb(radix) + carry;
            word = Limb(cur);
            carry = Limb(cur >> kLimbBits);
        }
        if (carry)
            d.push_back(carry);
        rt.checkLoop(++step, d.size() * sizeof(Limb));
    }
    out = make(rt, negative, std::move(d));
    return true;
}

bool parseString(Env& rt, StringView text, Value& out)
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
    Limb chunk = Limb(radix);
    int perChunk = 1;
    while (Wide(chunk) * Limb(radix) <= ~Limb(0)) {
        chunk *= Limb(radix);
        ++perChunk;
    }
    String out;
    while (!d.empty()) {
        Limb rem = divSmall(d, chunk);
        for (int i = 0; i < perChunk; ++i) {
            if (d.empty() && rem == 0)
                break;
            const unsigned digit = unsigned(rem % Limb(radix));
            out.push_back(Char(digit < 10 ? '0' + digit : 'a' + digit - 10));
            rem /= Limb(radix);
        }
    }
    if (negative)
        out.push_back('-');
    std::reverse(out.begin(), out.end());
    return out;
}

}  // namespace

String toString(const BigIntCell* a, int radix) { return digitsToString(Digits(a->digits().begin(), a->digits().end()), a->negative, radix); }

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
        Limb carry = Limb(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
        for (Limb& word : d) {
            const Wide cur = Wide(word) * Limb(radix) + carry;
            word = Limb(cur);
            carry = Limb(cur >> kLimbBits);
        }
        if (carry)
            d.push_back(carry);
    }
    return digitsToString(std::move(d), false, 10);
}

double toDouble(const BigIntCell* a) noexcept
{
    const Limbs d = a->digits();
    if (d.empty())
        return 0;
    const std::size_t bitLength = d.size() * kLimbBits - std::size_t(__builtin_clzll(d.back()));
    if (bitLength > 1024)
        return a->negative ? -HUGE_VAL : HUGE_VAL;
    // The top 64 bits, and whether anything below them is set, round correctly.
    std::uint64_t top = 0;
    bool sticky = false;
    const std::size_t shift = bitLength > 64 ? bitLength - 64 : 0;
    for (std::size_t i = d.size(); i-- > 0;) {
        const std::size_t bitAt = i * kLimbBits;
        if (bitAt >= shift + 64)
            continue;
        if (bitAt >= shift) {
            top |= d[i] << (bitAt - shift);
        } else if (bitAt + kLimbBits > shift) {
            const std::size_t inner = shift - bitAt;
            top |= d[i] >> inner;
            if (d[i] & ((Limb(1) << inner) - 1))
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
    const std::uint64_t magnitude = a->digits().empty() ? 0 : a->digits()[0];
    return a->negative ? ~magnitude + 1 : magnitude;
}

Value binary(Env& rt, BinaryOp op, const BigIntCell* a, const BigIntCell* b)
{
    switch (op) {
        case BinaryOp::Add: return addSigned(rt, a, b, false);
        case BinaryOp::Sub: return addSigned(rt, a, b, true);
        case BinaryOp::Mul: {
            const Limbs x = a->digits(), y = b->digits();
            if (x.empty() || y.empty())
                return make(rt, false, Digits());
            Value handle;
            BigIntCell* cell = room(rt, x.size() + y.size(), handle);
            mulInto(rt, x, y, cell->limbs());
            return finish(rt, std::move(handle), cell, a->negative != b->negative);
        }
        case BinaryOp::Div:
        case BinaryOp::Mod: {
            if (b->digits().empty())
                rt.fail(ErrorCode::RangeError, "division by zero");
            Digits rem;
            Digits quotient = divMagnitude(rt, a->digits(), b->digits(), rem);
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
                return make(rt, a->negative, shiftLeft(rt, a->digits(), a->digits().empty() ? 0 : shiftCount(rt, b)));
            if (b->digits().size() > 1)
                return a->negative ? fromInt64(rt, -1) : make(rt, false, Digits());
            return shiftRight(rt, a, shiftCount(rt, b));
        }
    }
    return Value::undefined();
}

Value negate(Env& rt, const BigIntCell* a) { return make(rt, !a->negative, a->digits()); }

Value bitNot(Env& rt, const BigIntCell* a)
{
    // ~x is -x - 1.
    Digits one(1, Limb(1));
    if (a->negative)
        return make(rt, false, subMagnitude(a->digits(), one));
    return make(rt, true, addMagnitude(a->digits(), one));
}

Value increment(Env& rt, const BigIntCell* a, int by)
{
    Digits one(1, Limb(1));
    const bool towardZero = a->negative == (by > 0);
    if (a->digits().empty())
        return make(rt, by < 0, std::move(one));
    if (!towardZero)
        return make(rt, a->negative, addMagnitude(a->digits(), one));
    return make(rt, a->negative, subMagnitude(a->digits(), one));
}

Value asIntN(Env& rt, std::uint64_t bits, const BigIntCell* a, bool isSigned)
{
    if (bits == 0)
        return make(rt, false, Digits());
    const std::uint64_t magnitudeBits = a->digits().empty() ? 0 : a->digits().size() * kLimbBits - std::uint64_t(__builtin_clzll(a->digits().back()));
    // What fits as it is stays as it is.
    if (!a->negative && magnitudeBits < bits)
        return make(rt, false, a->digits());
    if (a->negative && isSigned && magnitudeBits < bits)
        return make(rt, true, a->digits());
    if (bits > kMaxDigits * kLimbBits) {
        if (!isSigned && a->negative)
            tooBig(rt);
        return make(rt, a->negative, a->digits());
    }
    const std::size_t words = std::size_t((bits + kLimbBits - 1) / kLimbBits) + 1;
    Digits d = twos(a, std::max(words, a->digits().size() + 1));
    d.resize(words);
    const int top = int(bits % kLimbBits);
    const std::size_t last = std::size_t((bits - 1) / kLimbBits);
    const bool signBit = d[last] >> ((bits - 1) % kLimbBits) & 1;
    for (std::size_t i = last + 1; i < words; ++i)
        d[i] = 0;
    if (top)
        d[last] &= (Limb(1) << top) - 1;
    if (!isSigned || !signBit)
        return make(rt, false, std::move(d));
    // Negative: extend the sign and take the two's complement back.
    if (top)
        d[last] |= ~((Limb(1) << top) - 1);
    for (std::size_t i = last + 1; i < words; ++i)
        d[i] = ~Limb(0);
    return fromTwos(rt, std::move(d));
}

int compare(const BigIntCell* a, const BigIntCell* b) noexcept
{
    if (a->negative != b->negative)
        return a->negative ? -1 : 1;
    const int order = compareMagnitude(a->digits(), b->digits());
    return a->negative ? -order : order;
}

int compareDouble(const BigIntCell* a, double d) noexcept
{
    if (std::isnan(d))
        return 2;
    if (std::isinf(d))
        return d > 0 ? -1 : 1;
    if (a->digits().empty())
        return d > 0 ? -1 : d < 0 ? 1 : 0;
    if (a->negative != (d < 0) || d == 0)
        return a->negative ? -1 : 1;
    // Same sign, neither zero. Compare the whole part as an integer, then what is left of the fraction.
    const double whole = std::trunc(std::fabs(d));
    const std::size_t bitLength = a->digits().size() * kLimbBits - std::size_t(__builtin_clzll(a->digits().back()));
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
        for (std::size_t i = a->digits().size(); i-- > 0 && magnitudeOrder == 0;) {
            const int bitAt = int(i) * kLimbBits;
            Limb word = 0;
            if (below >= 0) {
                if (bitAt + kLimbBits > below && bitAt < below + 53) {
                    const int offset = bitAt - below;
                    word = offset >= 0 ? bits >> offset : bits << -offset;
                }
            } else {
                word = bitAt == 0 ? bits >> -below : 0;
            }
            if (a->digits()[i] != word)
                magnitudeOrder = a->digits()[i] < word ? -1 : 1;
        }
        if (magnitudeOrder == 0 && std::fabs(d) > whole)
            magnitudeOrder = -1;
    }
    return a->negative ? -magnitudeOrder : magnitudeOrder;
}

bool equal(const BigIntCell* a, const BigIntCell* b) noexcept
{
    return a->negative == b->negative && a->digits() == b->digits();
}

}  // namespace bigint
}  // namespace dyn
}  // namespace internal
}  // namespace script
