#include "vector_state.h"
#include "cpu_integer.h"

#include <algorithm>

static uint64_t low_mask(unsigned bits)
{
    return UINT64_MAX >> (64 - bits);
}

static int64_t signed_lane(uint64_t x, unsigned bits)
{
    return std::bit_cast<int64_t>(sign_extend(x, bits));
}

static bool increment(uint64_t x, unsigned shift, unsigned vxrm)
{
    if (!shift) {
        return false;
    }
    uint64_t half = uint64_t{1} << (shift - 1);
    bool tie_bit = x & half, lower = x & (half - 1), odd = (x >> shift) & 1;
    switch (vxrm) {
    case 0:
        return tie_bit; // RNU
    case 1:
        return tie_bit && (lower || odd); // RNE
    case 3:
        return !odd && (tie_bit || lower); // ROD
    default:
        return false; // RDN
    }
}

static uint64_t high_product(uint64_t a, uint64_t b, unsigned bits, bool sa, bool sb)
{
    uint64_t hi = bits == 64 ? mul_high_unsigned(a, b) : (a * b) >> bits;
    if (sa && (a >> (bits - 1))) {
        hi -= b;
    }
    if (sb && (b >> (bits - 1))) {
        hi -= a;
    }
    return hi & low_mask(bits);
}

static uint64_t sat(Cpu *cpu, uint64_t bound)
{
    cpu->vcsr |= 1;
    return bound;
}

static bool ordinary_form(VectorOp o)
{
    // OPIVV/OPIVX/OPIVI 共用 funct6。有些操作只分配了其中一两种操作数形式。
    bool imm = o.form == 3;
    switch (o.code) {
    case 0:
    case 9:
    case 10:
    case 11:
    case 12:
    case 14:
    case 16:
    case 17:
    case 23:
    case 24:
    case 25:
    case 28:
    case 29:
    case 32:
    case 33:
    case 37:
    case 40:
    case 41:
    case 42:
    case 43:
    case 44:
    case 45:
    case 46:
    case 47:
        return true;
    case 2:
    case 4:
    case 5:
    case 6:
    case 7:
    case 18:
    case 19:
    case 26:
    case 27:
    case 34:
    case 35:
    case 39:
        return !imm;
    case 3:
    case 15:
    case 30:
    case 31:
        return !o.vv();
    default:
        return false;
    }
}

static uint64_t integer_lane(Cpu *cpu, unsigned op, unsigned bits, uint64_t a, uint64_t b, bool carry)
{
    uint64_t mask = low_mask(bits), sign = uint64_t{1} << (bits - 1);
    unsigned shift = b & (bits - 1), vxrm = cpu->vcsr >> 1;
    int64_t sa = signed_lane(a, bits), sb = signed_lane(b, bits);
    switch (op) {
    case 0:
        return a + b;
    case 2:
        return a - b;
    case 3:
        return b - a;
    case 4:
        return std::min(a, b);
    case 5:
        return std::min(sa, sb);
    case 6:
        return std::max(a, b);
    case 7:
        return std::max(sa, sb);
    case 9:
        return a & b;
    case 10:
        return a | b;
    case 11:
        return a ^ b;
    case 16:
        return a + b + carry;
    case 17:
        return a > mask - b || (carry && a == mask - b);
    case 18:
        return a - b - carry;
    case 19:
        return a < b || (carry && a == b);
    case 24:
        return a == b;
    case 25:
        return a != b;
    case 26:
        return a < b;
    case 27:
        return sa < sb;
    case 28:
        return a <= b;
    case 29:
        return sa <= sb;
    case 30:
        return a > b;
    case 31:
        return sa > sb;
    case 32:
        return a > mask - b ? sat(cpu, mask) : a + b;
    case 34:
        return a < b ? sat(cpu, 0) : a - b;
    case 33:
    case 35: {
        bool sub = op == 35;
        uint64_t r = sub ? a - b : a + b;
        bool overflow = ((a ^ r) & (sub ? a ^ b : ~(a ^ b)) & sign) != 0;
        return overflow ? sat(cpu, a & sign ? sign : sign - 1) : r;
    }
    case 37:
        return a << shift;
    case 39: { // vsmul：有符号双宽乘积右移 SEW-1，只有 min * min 饱和。
        if (a == sign && b == sign) {
            return sat(cpu, sign - 1);
        }
        uint64_t lo = a * b, hi = high_product(a, b, bits, true, true);
        return (hi << 1 | ((lo & mask) >> (bits - 1))) + increment(lo, bits - 1, vxrm);
    }
    case 40:
        return a >> shift;
    case 41:
        return sa >> shift;
    case 42:
        return (a >> shift) + increment(a, shift, vxrm);
    default:
        return (sa >> shift) + uint64_t{increment(a, shift, vxrm)}; // vssra
    }
}

static bool ordinary(Cpu *cpu, VectorOp o, VectorType t)
{
    if (!ordinary_form(o)) {
        return false;
    }
    unsigned w = t.bytes, bits = 8 * w, group = vector_group(t, w);
    bool mask_dest = o.code == 17 || o.code == 19 || (o.code >= 24 && o.code <= 31);
    bool carry_in = o.code >= 16 && o.code <= 19;
    bool merge = o.code == 23, narrow = o.code >= 44;
    unsigned aw = narrow ? w * 2 : w;
    if (aw > 8 || (!mask_dest && !valid_group(o.vd, group)) ||
        (!o.unmasked && !mask_dest && o.vd == 0) || ((o.code == 16 || o.code == 18) && o.unmasked) ||
        (merge && o.unmasked && o.vs2 != 0)) {
        return false;
    }
    if (!(merge && o.unmasked) && !vector_operand(t, o.vs2, aw)) {
        return false;
    }
    if (narrow && !legal_overlap(t, o.vd, w, o.vs2, aw)) {
        return false;
    }
    unsigned bw = o.code == 14 && o.vv() ? 2 : w; // vrgatherei16
    if (o.vv() && !vector_operand(t, o.vs1, bw)) {
        return false;
    }
    if (mask_dest &&
        ((o.vd > o.vs2 && o.vd < o.vs2 + group) || (o.vv() && o.vd > o.vs1 && o.vd < o.vs1 + group))) {
        return false;
    }
    bool gather = o.code == 12 || (o.code == 14 && o.vv());
    if ((gather || o.code == 14) && overlaps(o.vd, group, o.vs2, group)) {
        return false;
    }
    if (gather && o.vv() && overlaps(o.vd, group, o.vs1, vector_group(t, bw))) {
        return false;
    }
    for (unsigned i = cpu->vstart; i < cpu->vl; ++i) {
        bool m = !o.unmasked && vector_mask(cpu, 0, i);
        if (!o.unmasked && !m && !carry_in && !merge) {
            continue;
        }
        uint64_t a = vector_read(cpu, o.vs2, i, aw);
        uint64_t b = o.vv() ? vector_read(cpu, o.vs1, i, bw)
                            : (o.form == 3 ? sign_extend(o.vs1, 5) : cpu->x[o.vs1]);
        uint64_t v;
        if (gather || o.code == 14 || o.code == 15) {
            if (o.form == 3) {
                b = o.vs1; // 滑移和收集的立即数为无符号。
            }
            if (o.code == 14 && !gather && i < b) {
                continue;
            }
            uint64_t index = gather ? b : (o.code == 14 ? i - b : b + i);
            v = index < t.capacity && (o.code != 15 || index >= b) ? vector_read(cpu, o.vs2, index, w)
                                                                   : 0;
        } else if (merge) {
            v = o.unmasked || m ? b : a;
        } else if (narrow) {
            unsigned shift = b & (bits * 2 - 1);
            bool signed_op = o.code & 1;
            v = signed_op ? static_cast<uint64_t>(signed_lane(a, bits * 2) >> shift) : a >> shift;
            if (o.code >= 46) {
                v += increment(a, shift, cpu->vcsr >> 1);
                uint64_t bound = low_mask(bits);
                if (!signed_op && v > bound) {
                    v = sat(cpu, bound);
                }
                if (signed_op) {
                    int64_t s = std::bit_cast<int64_t>(v), max = static_cast<int64_t>(bound >> 1);
                    if (s > max) {
                        v = sat(cpu, max);
                    }
                    if (s < -max - 1) {
                        v = sat(cpu, -max - 1);
                    }
                }
            }
        } else {
            v = integer_lane(cpu, o.code, bits, a, b & low_mask(bits), m);
        }
        if (mask_dest) {
            vector_mask_write(cpu, o.vd, i, v != 0);
        } else {
            vector_write(cpu, o.vd, i, w, v);
        }
    }
    return true;
}

static bool reduction(Cpu *cpu, VectorOp o, VectorType t, bool wide)
{
    unsigned width = t.bytes * (wide ? 2 : 1);
    if (cpu->vstart || width > 8 || !vector_operand(t, o.vs2, t.bytes)) {
        return false;
    }
    uint64_t acc = vector_read(cpu, o.vs1, 0, width);
    for (unsigned i = 0; i < cpu->vl; ++i) {
        if (!o.unmasked && !vector_mask(cpu, 0, i)) {
            continue;
        }
        uint64_t v = vector_read(cpu, o.vs2, i, t.bytes);
        if (wide) {
            acc += (o.code & 1) ? sign_extend(v, t.bytes * 8) : v;
        } else if (o.code == 0) {
            acc += v;
        } else if (o.code == 1) {
            acc &= v;
        } else if (o.code == 2) {
            acc |= v;
        } else if (o.code == 3) {
            acc ^= v;
        } else {
            acc = integer_lane(cpu, o.code, width * 8, acc & low_mask(width * 8), v, false);
        }
    }
    if (cpu->vl) {
        vector_write(cpu, o.vd, 0, width, acc);
    }
    return true;
}

static bool mask_ops(Cpu *cpu, VectorOp o, VectorType t, uint64_t *rd, bool *write_x)
{
    if (o.code == 16 && !o.vs1) {
        if (!o.unmasked) {
            return false;
        }
        *rd = sign_extend(vector_read(cpu, o.vs2, 0, t.bytes), t.bytes * 8);
        *write_x = true;
        return true;
    }
    if (o.code == 16 && o.vs1 != 16 && o.vs1 != 17) {
        return false;
    }
    bool logic = o.code >= 24, compress = o.code == 23;
    bool sequence = o.code == 20, index = sequence && o.vs1 >= 16;
    if (sequence && o.vs1 != 1 && o.vs1 != 2 && o.vs1 != 3 && o.vs1 != 16 &&
        !(o.vs1 == 17 && o.vs2 == 0)) {
        return false;
    }
    unsigned group = vector_group(t, t.bytes);
    if ((logic || compress) && !o.unmasked) {
        return false;
    }
    if (!logic && cpu->vstart && !(index && o.vs1 == 17)) {
        return false;
    }
    if ((index || compress) && !valid_group(o.vd, group)) {
        return false;
    }
    if (compress && (!valid_group(o.vs2, group) || overlaps(o.vd, group, o.vs2, group) ||
                     overlaps(o.vd, group, o.vs1, 1))) {
        return false;
    }
    if (sequence &&
        ((!o.unmasked && o.vd == 0) || (o.vs1 != 17 && overlaps(o.vd, index ? group : 1, o.vs2, 1)))) {
        return false;
    }
    unsigned seen = 0;
    uint64_t first = UINT64_MAX;
    for (unsigned i = cpu->vstart; i < cpu->vl; ++i) {
        if (!o.unmasked && !vector_mask(cpu, 0, i)) {
            continue;
        }
        bool a = vector_mask(cpu, o.vs2, i);
        if (logic) {
            bool b = vector_mask(cpu, o.vs1, i);
            const bool results[] = {a && !b, a && b,    a || b,    a != b,
                                    a || !b, !(a && b), !(a || b), a == b};
            vector_mask_write(cpu, o.vd, i, results[o.code - 24]);
        } else if (compress) {
            if (vector_mask(cpu, o.vs1, i)) {
                vector_write(cpu, o.vd, seen++, t.bytes, vector_read(cpu, o.vs2, i, t.bytes));
            }
        } else {
            if (index) {
                vector_write(cpu, o.vd, i, t.bytes, o.vs1 == 17 ? i : seen);
            } else if (sequence) {
                vector_mask_write(cpu, o.vd, i, !seen && (o.vs1 == 3 || (o.vs1 == 2) == a));
            }
            if (a) {
                if (!seen) {
                    first = i;
                }
                ++seen;
            }
        }
    }
    if (o.code == 16) {
        *write_x = true;
        *rd = o.vs1 == 16 ? seen : first;
    }
    return true;
}

static bool multiply_group(Cpu *cpu, VectorOp o, VectorType t)
{
    unsigned w = t.bytes, bits = 8 * w;
    bool wide = o.code >= 48, wide_src = o.code >= 52 && o.code <= 55;
    bool extend = o.code == 18;
    if (extend && (!o.vv() || o.vs1 < 2 || o.vs1 > 7)) {
        return false;
    }
    unsigned dw = wide ? 2 * w : w, aw = wide_src ? 2 * w : w;
    if (extend) {
        aw = w >> (4 - o.vs1 / 2);
    }
    if (!aw || dw > 8 || !vector_operand(t, o.vd, dw) || !vector_operand(t, o.vs2, aw) ||
        (!extend && o.vv() && !vector_operand(t, o.vs1, w)) || (!o.unmasked && !o.vd) ||
        !legal_overlap(t, o.vd, dw, o.vs2, aw) ||
        (!extend && o.vv() && !legal_overlap(t, o.vd, dw, o.vs1, w))) {
        return false;
    }
    if (!(extend || (o.code >= 8 && o.code <= 11) || (o.code >= 32 && o.code <= 39) ||
          (o.code >= 41 && o.code <= 47 && (o.code & 1)) ||
          (wide && o.code != 57 && (o.code != 62 || !o.vv())) ||
          (!o.vv() && (o.code == 14 || o.code == 15)))) {
        return false;
    }
    if (o.code == 14 && o.vs2 == o.vd) {
        return false;
    }
    uint64_t mask = low_mask(bits);
    for (unsigned i = cpu->vstart; i < cpu->vl; ++i) {
        if (!o.unmasked && !vector_mask(cpu, 0, i)) {
            continue;
        }
        uint64_t a = vector_read(cpu, o.vs2, i, aw);
        uint64_t b = o.vv() && !extend ? vector_read(cpu, o.vs1, i, w) : cpu->x[o.vs1] & mask;
        uint64_t old = vector_read(cpu, o.vd, i, dw), v;
        if (extend) {
            v = (o.vs1 & 1) ? sign_extend(a, 8 * aw) : a;
        } else if (wide) {
            // vwmulsu 的符号顺序按 vs2、vs1，累加指令则按 vs1、vs2 命名。
            bool sa = ((o.code & 1) && o.code != 63) || o.code == 58 || o.code == 62;
            bool sb = (o.code & 1) != 0;
            if (sa) {
                a = sign_extend(a, aw * 8);
            }
            if (sb) {
                b = sign_extend(b, bits);
            }
            v = o.code < 56 ? ((o.code & 2) ? a - b : a + b) : a * b;
            if (o.code >= 60) {
                v += old;
            }
        } else if (o.code >= 8 && o.code <= 11) {
            // 在全宽边界也保留求和/求差产生的额外一位。
            bool sub = o.code & 2, signed_op = o.code & 1;
            uint64_t lo = (sub ? a - b : a + b) & mask;
            bool high = sub ? a < b : a > mask - b;
            if (signed_op) {
                high ^= ((a ^ b) >> (bits - 1)) & 1;
            }
            v = (lo >> 1 | (uint64_t{high} << (bits - 1))) + increment(lo, 1, cpu->vcsr >> 1);
        } else {
            switch (o.code) {
            case 14:
                v = i ? vector_read(cpu, o.vs2, i - 1, w) : b;
                break;
            case 15:
                v = i + 1 < cpu->vl ? vector_read(cpu, o.vs2, i + 1, w) : b;
                break;
            case 32:
                v = b ? a / b : mask;
                break;
            case 34:
                v = b ? a % b : a;
                break;
            case 33:
            case 35: {
                int64_t x = signed_lane(a, bits), y = signed_lane(b, bits);
                if (!y) {
                    v = o.code == 33 ? mask : a;
                } else if (x == INT64_MIN && y == -1) {
                    v = o.code == 33 ? a : 0;
                } else {
                    v = o.code == 33 ? x / y : x % y;
                }
                break;
            }
            case 36:
            case 38:
            case 39:
                v = high_product(a, b, bits, o.code != 36, o.code == 39);
                break;
            case 37:
                v = a * b;
                break;
            case 41:
                v = b * old + a;
                break;
            case 43:
                v = a - b * old;
                break;
            case 45:
                v = b * a + old;
                break;
            default:
                v = old - b * a;
                break;
            }
        }
        vector_write(cpu, o.vd, i, dw, v);
    }
    return true;
}

static bool vector_bits(Cpu *cpu, VectorOp o, VectorType t)
{
    unsigned w = t.bytes, bits = 8 * w;
    bool unary = o.form == 2, wide = o.code == 53;
    unsigned dw = wide ? 2 * w : w;
    if (dw > 8 || !vector_operand(t, o.vd, dw) || !vector_operand(t, o.vs2, w) ||
        (!o.unmasked && !o.vd) || !legal_overlap(t, o.vd, dw, o.vs2, w) ||
        (!unary && o.vv() && (!vector_operand(t, o.vs1, w) || !legal_overlap(t, o.vd, dw, o.vs1, w)))) {
        return false;
    }
    if (unary && (o.vs1 < 8 || o.vs1 > 14 || o.vs1 == 11)) {
        return false;
    }
    if (o.code == 1 && o.form == 3) {
        return false;
    }
    for (unsigned i = cpu->vstart; i < cpu->vl; ++i) {
        if (!o.unmasked && !vector_mask(cpu, 0, i)) {
            continue;
        }
        uint64_t a = vector_read(cpu, o.vs2, i, w), v;
        if (unary) {
            if (o.vs1 == 12) {
                v = std::countl_zero(a) - (64 - bits);
            } else if (o.vs1 == 13) {
                v = std::min(bits, static_cast<unsigned>(std::countr_zero(a)));
            } else if (o.vs1 == 14) {
                v = std::popcount(a);
            } else {
                if (o.vs1 != 9) {
                    constexpr uint64_t pairs = UINT64_C(0x5555555555555555);
                    constexpr uint64_t quads = UINT64_C(0x3333333333333333);
                    constexpr uint64_t nibbles = UINT64_C(0x0f0f0f0f0f0f0f0f);
                    a = ((a & pairs) << 1) | ((a >> 1) & pairs);
                    a = ((a & quads) << 2) | ((a >> 2) & quads);
                    a = ((a & nibbles) << 4) | ((a >> 4) & nibbles);
                }
                v = o.vs1 == 8 ? a : std::byteswap(a) >> (64 - bits);
            }
        } else {
            uint64_t b = o.vv() ? vector_read(cpu, o.vs1, i, w) : (o.form == 3 ? o.vs1 : cpu->x[o.vs1]);
            if (o.code == 1) {
                v = a & ~b;
            } else if (wide) {
                v = a << (b & (2 * bits - 1));
            } else {
                if (o.form == 3) {
                    b |= (o.code & 1) << 5; // vror.vi 的第六个立即数位。
                }
                unsigned right = (o.code == 21 && o.form != 3 ? 0 - b : b) & (bits - 1);
                v = (a >> right) | (a << ((0u - right) & (bits - 1)));
            }
        }
        vector_write(cpu, o.vd, i, dw, v);
    }
    return true;
}

bool vector_alu(Cpu *cpu, uint32_t insn, VectorType t, uint64_t *rd, bool *write_x)
{
    // funct6 选具体操作，funct3 区分整数、浮点、乘除及标量/向量/立即数来源。
    VectorOp o{insn >> 26,        (insn >> 12) & 7,  (insn >> 7) & 31,
               (insn >> 15) & 31, (insn >> 20) & 31, ((insn >> 25) & 1) != 0};
    if (o.form == 1 || o.form == 5) {
        return vector_float(cpu, o, t);
    }
    if ((o.form == 2 && o.code == 18 && o.vs1 >= 8) ||
        ((o.form == 0 || o.form == 3 || o.form == 4) &&
         (o.code == 1 || o.code == 20 || o.code == 21 || o.code == 53))) {
        return vector_bits(cpu, o, t);
    }
    if (o.form == 0 && (o.code == 48 || o.code == 49)) {
        return reduction(cpu, o, t, true);
    }
    if (o.form == 3 && o.code == 39) { // vmv<n>r.v 的有效长度取整组寄存器。
        unsigned regs = o.vs1 + 1;
        if (!o.unmasked || regs > 8 || !std::has_single_bit(regs) || !valid_group(o.vd, regs) ||
            !valid_group(o.vs2, regs)) {
            return false;
        }
        for (unsigned i = cpu->vstart; i < regs * Cpu::VLEN_BYTES / t.bytes; ++i) {
            vector_write(cpu, o.vd, i, t.bytes, vector_read(cpu, o.vs2, i, t.bytes));
        }
        return true;
    }
    if (o.form != 2 && o.form != 6) {
        return ordinary(cpu, o, t);
    }
    if (o.form == 2 && o.code < 8) {
        return reduction(cpu, o, t, false);
    }
    if (o.form == 2 && (o.code == 16 || o.code == 20 || (o.code >= 23 && o.code <= 31))) {
        return mask_ops(cpu, o, t, rd, write_x);
    }
    if (o.form == 6 && o.code == 16) {
        if (!o.unmasked || o.vs2) {
            return false;
        }
        if (cpu->vstart < cpu->vl) {
            vector_write(cpu, o.vd, 0, t.bytes, cpu->x[o.vs1]);
        }
        return true;
    }
    return multiply_group(cpu, o, t);
}
