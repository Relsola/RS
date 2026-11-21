#include "float_bits.h"

#include <algorithm>
#include <bit>

// 有限数表示为 sign × significand × 2^scale，次正规数与正规数共用一条运算路径。
struct Number {
    bool negative, infinity, nan, signaling;
    uint64_t significand;
    int scale;
};

static Number unpack(FloatFormat f, uint64_t bits)
{
    uint64_t magnitude = bits & (f.sign() - 1);
    uint64_t fraction = magnitude & (f.unit() - 1);
    bool special = magnitude >= f.inf();
    int exp = static_cast<int>(magnitude >> f.fraction);
    return {(bits & f.sign()) != 0,
            magnitude == f.inf(),
            magnitude > f.inf(),
            special && fraction && !(fraction & (f.unit() >> 1)),
            fraction | (exp && !special ? f.unit() : 0),
            std::max(exp, 1) - f.bias() - static_cast<int>(f.fraction)};
}

uint64_t fp_box(FloatFormat f, uint64_t bits)
{
    return bits | ~(f.sign() | (f.sign() - 1));
}

uint64_t fp_unbox(FloatFormat f, uint64_t reg)
{
    uint64_t mask = f.sign() | (f.sign() - 1);
    return (reg | mask) == UINT64_MAX ? reg & mask : f.nan();
}

unsigned fp_class(FloatFormat f, uint64_t bits)
{
    Number n = unpack(f, bits);
    if (n.nan) {
        return n.signaling ? 1u << 8 : 1u << 9;
    }
    if (n.infinity) {
        return n.negative ? 1u : 1u << 7;
    }
    if (!n.significand) {
        return n.negative ? 1u << 3 : 1u << 4;
    }
    if (n.significand < f.unit()) {
        return n.negative ? 1u << 2 : 1u << 5;
    }
    return n.negative ? 1u << 1 : 1u << 6;
}

// 最远的两项是 (最小 binary64 次正规数)^2 和最大 binary64 加数。
// 对齐后的非零位从 2^-2148 到 2^1023，共 3172 位。固定空间使 FMA 只舍入一次。
struct Magnitude {
    uint32_t word[102]{};
    int used{};
};

static void trim(Magnitude &a)
{
    while (a.used && !a.word[a.used - 1]) {
        --a.used;
    }
}

static Magnitude small(uint64_t n)
{
    Magnitude a{{static_cast<uint32_t>(n), static_cast<uint32_t>(n >> 32)}, 2};
    trim(a);
    return a;
}

static Magnitude shifted(const Magnitude &a, int count)
{
    Magnitude out{};
    int words = count / 32, bits = count % 32;
    for (int i = 0; i < a.used; ++i) {
        uint64_t v = uint64_t{a.word[i]} << bits;
        out.word[words + i] |= static_cast<uint32_t>(v);
        out.word[words + i + 1] = static_cast<uint32_t>(v >> 32);
    }
    out.used = a.used ? words + a.used + 1 : 0;
    trim(out);
    return out;
}

static Magnitude product(uint64_t x, uint64_t y)
{
    Magnitude a = small(x), b = small(y), out{};
    for (int i = 0; i < a.used; ++i) {
        uint64_t carry = 0;
        for (int j = 0; j < b.used; ++j) {
            uint64_t v = uint64_t{a.word[i]} * b.word[j] + out.word[i + j] + carry;
            out.word[i + j] = static_cast<uint32_t>(v);
            carry = v >> 32;
        }
        out.word[i + b.used] = static_cast<uint32_t>(carry);
    }
    out.used = a.used + b.used;
    trim(out);
    return out;
}

static int compare(const Magnitude &a, const Magnitude &b)
{
    if (a.used != b.used) {
        return a.used - b.used;
    }
    for (int i = a.used - 1; i >= 0; --i) {
        if (a.word[i] != b.word[i]) {
            return a.word[i] > b.word[i] ? 1 : -1;
        }
    }
    return 0;
}

static Magnitude combine(const Magnitude &a, const Magnitude &b, bool subtract)
{
    Magnitude out{};
    out.used = std::max(a.used, b.used);
    uint64_t carry = 0;
    for (int i = 0; i < out.used; ++i) {
        uint64_t rhs = uint64_t{b.word[i]} + carry;
        uint64_t v = subtract ? a.word[i] - rhs : a.word[i] + rhs;
        out.word[i] = static_cast<uint32_t>(v);
        carry = subtract ? a.word[i] < rhs : v >> 32;
    }
    if (carry) {
        out.word[out.used++] = 1;
    }
    trim(out);
    return out;
}

static int top(const Magnitude &a)
{
    return a.used * 32 - 1 - std::countl_zero(a.word[a.used - 1]);
}

static bool bit(const Magnitude &a, int index)
{
    return index >= 0 && index / 32 < a.used && ((a.word[index / 32] >> (index % 32)) & 1);
}

static bool any_below(const Magnitude &a, int limit)
{
    for (int i = 0; i < a.used && i * 32 < limit; ++i) {
        int n = limit - i * 32;
        if (n >= 32 ? a.word[i] != 0 : (a.word[i] & ((uint32_t{1} << n) - 1)) != 0) {
            return true;
        }
    }
    return false;
}

struct Rounded {
    uint64_t value;
    bool inexact, carry;
};

static Rounded round_integer(const Magnitude &a, int shift, bool sticky, bool negative, unsigned rm)
{
    // 保留 shift 及以上的位。下一位是 half，更低位及除法余数共同决定是否恰在中点。
    uint64_t v = 0;
    for (int i = 0; i < 64; ++i) {
        if (bit(a, shift + i)) {
            v |= uint64_t{1} << i;
        }
    }
    bool half = bit(a, shift - 1);
    bool rest = sticky || any_below(a, shift - 1);
    bool lost = half || rest;
    bool up = (rm == 0 && half && (rest || (v & 1))) || (rm == 4 && half) ||
              (rm == 2 && negative && lost) || (rm == 3 && !negative && lost);
    return {v + up, lost, up && v == UINT64_MAX};
}

static FloatResult pack(FloatFormat f, bool negative, const Magnitude &n, int scale, bool sticky,
                        unsigned rm)
{
    uint64_t sign = negative ? f.sign() : 0;
    if (!n.used) {
        return {sign, 0};
    }
    int highest = top(n), exponent = scale + highest, emin = 1 - f.bias();
    int precision_shift = highest - static_cast<int>(f.fraction);
    int shift = std::max(exponent, emin) - static_cast<int>(f.fraction) - scale;
    Rounded rounded = round_integer(n, shift, sticky, negative, rm);
    uint64_t sig = rounded.value;
    bool tiny = exponent < emin;
    if (tiny && exponent == emin - 1 && sig >= f.unit()) {
        // IEEE 在无限指数范围内按目标精度舍入后判断 tiny，再量化到次正规数的步长。
        tiny = round_integer(n, precision_shift, sticky, negative, rm).value < 2 * f.unit();
    }
    exponent = std::max(exponent, emin);
    if (sig == 2 * f.unit()) {
        sig >>= 1;
        ++exponent;
    }
    if (exponent > f.bias()) {
        bool inf = rm == 0 || rm == 4 || (negative ? rm == 2 : rm == 3);
        return {sign | (f.inf() - !inf), FP_OF | FP_NX};
    }
    uint64_t encoded =
        sig < f.unit() ? sig : (uint64_t(exponent + f.bias()) << f.fraction) | (sig - f.unit());
    unsigned flags = rounded.inexact ? FP_NX | (tiny ? FP_UF : 0u) : 0u;
    return {sign | encoded, flags};
}

static FloatResult sum(FloatFormat f, Magnitude a, int ea, bool sa, Magnitude b, int eb, bool sb,
                       unsigned rm)
{
    int scale = std::min(ea, eb);
    a = shifted(a, ea - scale);
    b = shifted(b, eb - scale);
    if (sa == sb) {
        return pack(f, sa, combine(a, b, false), scale, false, rm);
    }
    int order = compare(a, b);
    if (!order) {
        return {rm == 2 ? f.sign() : 0, 0};
    }
    return pack(f, order > 0 ? sa : sb, order > 0 ? combine(a, b, true) : combine(b, a, true), scale,
                false, rm);
}

FloatResult fp_fma(FloatFormat f, uint64_t x, uint64_t y, uint64_t z, unsigned rm)
{
    Number a = unpack(f, x), b = unpack(f, y), c = unpack(f, z);
    bool invalid_product = (a.infinity && !b.nan && !b.infinity && !b.significand) ||
                           (b.infinity && !a.nan && !a.infinity && !a.significand);
    bool negative = a.negative != b.negative;
    if (a.nan || b.nan || c.nan || invalid_product) {
        return {f.nan(), invalid_product || a.signaling || b.signaling || c.signaling ? FP_NV : 0u};
    }
    if (a.infinity || b.infinity) {
        if (c.infinity && negative != c.negative) {
            return {f.nan(), FP_NV};
        }
        return {f.inf() | (negative ? f.sign() : 0), 0};
    }
    if (c.infinity) {
        return {z, 0};
    }
    return sum(f, product(a.significand, b.significand), a.scale + b.scale, negative,
               small(c.significand), c.scale, c.negative, rm);
}

FloatResult fp_binary(FloatFormat f, FloatOp op, uint64_t x, uint64_t y, unsigned rm)
{
    if (op == FloatOp::Sub) {
        y ^= f.sign();
        op = FloatOp::Add;
    }
    Number a = unpack(f, x), b = unpack(f, y);
    bool nan = a.nan || b.nan;
    unsigned flags = a.signaling || b.signaling ? FP_NV : 0u;
    bool both_zero = ((x | y) & (f.sign() - 1)) == 0;
    bool equal = x == y || both_zero;
    bool less = a.negative != b.negative ? a.negative : (a.negative ? x > y : x < y);
    if (op == FloatOp::Min || op == FloatOp::Max) {
        if (nan) {
            return {a.nan ? (b.nan ? f.nan() : y) : x, flags};
        }
        return {(op == FloatOp::Min) == less ? x : y, 0};
    }
    if (op == FloatOp::Eq || op == FloatOp::Lt || op == FloatOp::Le) {
        if (nan) {
            return {0, op == FloatOp::Eq ? flags : FP_NV};
        }
        return {op == FloatOp::Eq ? equal : (op == FloatOp::Lt ? less && !equal : less || equal), 0};
    }
    if (nan) {
        return {f.nan(), flags};
    }
    if (op == FloatOp::Add) {
        if (a.infinity && b.infinity && a.negative != b.negative) {
            return {f.nan(), FP_NV};
        }
        if (a.infinity || b.infinity) {
            return {a.infinity ? x : y, 0};
        }
        return sum(f, small(a.significand), a.scale, a.negative, small(b.significand), b.scale,
                   b.negative, rm);
    }
    bool negative = a.negative != b.negative;
    uint64_t sign = negative ? f.sign() : 0;
    if (op == FloatOp::Mul) {
        if (a.infinity || b.infinity) {
            if ((!a.infinity && !a.significand) || (!b.infinity && !b.significand)) {
                return {f.nan(), FP_NV};
            }
            return {sign | f.inf(), 0};
        }
        return pack(f, negative, product(a.significand, b.significand), a.scale + b.scale, false, rm);
    }
    if ((a.infinity && b.infinity) || both_zero) {
        return {f.nan(), FP_NV};
    }
    if (a.infinity) {
        return {sign | f.inf(), 0};
    }
    if (b.infinity) {
        return {sign, 0};
    }
    if (!b.significand) {
        return {sign | f.inf(), FP_DZ};
    }
    if (!a.significand) {
        return {sign, 0};
    }
    // 规整到相同的最高位，商总在 [0.5,2)。除到保护位，再把非零余数交给舍入。
    int la = std::bit_width(a.significand) - 1, lb = std::bit_width(b.significand) - 1;
    uint64_t r = a.significand << (f.fraction - la), d = b.significand << (f.fraction - lb);
    uint64_t quotient = 0;
    int extra = static_cast<int>(f.fraction) + 3;
    for (int i = 0; i <= extra; ++i) {
        bool one = r >= d;
        quotient = (quotient << 1) | one;
        r = (one ? r - d : r) << 1;
    }
    return pack(f, negative, small(quotient), a.scale + la - b.scale - lb - extra, r != 0, rm);
}

FloatResult fp_sqrt(FloatFormat f, uint64_t x, unsigned rm)
{
    Number a = unpack(f, x);
    if (a.nan) {
        return {f.nan(), a.signaling ? FP_NV : 0u};
    }
    if (!(x & (f.sign() - 1))) {
        return {x, 0};
    }
    if (a.negative) {
        return {f.nan(), FP_NV};
    }
    if (a.infinity) {
        return {x, 0};
    }
    int lead = std::bit_width(a.significand) - 1;
    int exp = lead + a.scale;
    int extra = static_cast<int>(f.fraction) + 3;
    Magnitude radicand = shifted(small(a.significand), 2 * extra + (exp & 1) - lead);
    uint64_t root = 0, rem = 0;
    for (int pair = top(radicand) / 2; pair >= 0; --pair) {
        rem = (rem << 2) | (bit(radicand, 2 * pair + 1) << 1) | bit(radicand, 2 * pair);
        uint64_t trial = (root << 2) | 1;
        root <<= 1;
        if (rem >= trial) {
            rem -= trial;
            ++root;
        }
    }
    return pack(f, false, small(root), (exp - (exp & 1)) / 2 - extra, rem != 0, rm);
}

FloatResult fp_convert(FloatFormat to, FloatFormat from, uint64_t x, unsigned rm)
{
    Number a = unpack(from, x);
    if (a.nan) {
        return {to.nan(), a.signaling ? FP_NV : 0u};
    }
    if (a.infinity) {
        return {to.inf() | (a.negative ? to.sign() : 0), 0};
    }
    return pack(to, a.negative, small(a.significand), a.scale, false, rm);
}

uint64_t fp_immediate(FloatFormat f, unsigned index)
{
    if (index == 0) {
        return f.sign() | (uint64_t(f.bias()) << f.fraction);
    }
    if (index == 1) {
        return f.unit();
    }
    if (index >= 30) {
        return index == 30 ? f.inf() : f.nan();
    }
    // FLI 表中的普通值只使用有效数最高的两个小数位。
    constexpr int exponent[] = {-16, -15, -8, -7, -4, -3, -2, -2, -2, -2, -1, -1, -1, -1,
                                0,   0,   0,  0,  1,  1,  1,  2,  3,  4,  7,  8,  15, 16};
    unsigned quarter = index >= 8 && index <= 22 ? (index - 8) % 4 : 0;
    return (uint64_t(exponent[index - 2] + f.bias()) << f.fraction) |
           (uint64_t{quarter} << (f.fraction - 2));
}

FloatResult fp_round_integral(FloatFormat f, uint64_t x, unsigned rm, bool exact)
{
    Number a = unpack(f, x);
    if (a.nan) {
        return {f.nan(), a.signaling ? FP_NV : 0u};
    }
    if (a.infinity || !a.significand || a.scale >= 0) {
        return {x, 0};
    }
    Rounded r = round_integer(small(a.significand), -a.scale, false, a.negative, rm);
    auto result = pack(f, a.negative, small(r.value), 0, false, rm);
    if (exact && r.inexact) {
        result.flags |= FP_NX;
    }
    return result;
}

FloatResult fp_mod_word(uint64_t x)
{
    Number a = unpack(BINARY64, x);
    FloatResult r = fp_to_int(BINARY64, x, 32, false, 1);
    uint32_t low = 0;
    if (!a.infinity && !a.nan && a.scale < 32 && a.scale > -64) {
        low = static_cast<uint32_t>(a.scale >= 0 ? a.significand << a.scale : a.significand >> -a.scale);
        if (a.negative) {
            low = 0u - low;
        }
    }
    r.bits = (uint64_t{low} ^ 0x80000000u) - 0x80000000u;
    return r;
}

FloatResult fp_from_int(FloatFormat f, uint64_t x, unsigned width, bool unsign, unsigned rm)
{
    uint64_t mask = UINT64_MAX >> (64 - width);
    bool negative = !unsign && ((x >> (width - 1)) & 1);
    return pack(f, negative, small((negative ? 0 - x : x) & mask), 0, false, rm);
}

FloatResult fp_to_int(FloatFormat f, uint64_t x, unsigned width, bool unsign, unsigned rm)
{
    Number a = unpack(f, x);
    uint64_t mask = UINT64_MAX >> (64 - width);
    uint64_t positive_max = unsign ? mask : mask >> 1;
    uint64_t negative_max = unsign ? 0 : positive_max + 1;
    uint64_t limit = a.negative && !a.nan ? negative_max : positive_max;
    Magnitude n = small(a.significand);
    if (a.nan || a.infinity || (n.used && top(n) + a.scale >= 64)) {
        return {a.negative && !a.nan ? 0 - limit : limit, FP_NV};
    }
    Rounded r = round_integer(n, -a.scale, false, a.negative, rm);
    if (r.carry || r.value > limit) {
        return {a.negative ? 0 - limit : limit, FP_NV};
    }
    return {a.negative ? 0 - r.value : r.value, r.inexact ? FP_NX : 0u};
}
