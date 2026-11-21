#include "vector_state.h"
#include "float_bits.h"

static FloatFormat format(unsigned width)
{
    return width == 8 ? BINARY64 : (width == 4 ? BINARY32 : BINARY16);
}

// 指令规定的 7 位查找结果：在输入区间中点取估计并舍入到最近整数。
// 平方比较避免使用宿主 sqrt。这里的范围足以完全用 32 位无符号数计算。
static unsigned estimate_entry(bool root, unsigned index, bool odd)
{
    if (!root) {
        return (65536u + 128 + index) / (257 + 2 * index) - 128;
    }
    unsigned denom = 129 + 2 * index, numer = (odd ? 4u : 2u) * 128 * 128 * 128;
    unsigned low = 128, high = 256;
    while (low < high) {
        unsigned mid = (low + high) / 2, boundary = 2 * mid + 1;
        if (boundary * boundary * denom <= 4 * numer) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return low - 128;
}

static FloatResult estimate(FloatFormat f, uint64_t a, bool root, unsigned rm)
{
    unsigned cls = fp_class(f, a);
    uint64_t sign = a & f.sign(), mag = a & (f.sign() - 1);
    if (cls & 0x300) {
        return {f.nan(), cls == 0x100 ? FP_NV : 0u};
    }
    if (!mag) {
        return {sign | f.inf(), FP_DZ};
    }
    if (root && sign) {
        return {f.nan(), FP_NV};
    }
    if (mag == f.inf()) {
        return {sign, 0};
    }
    int exp = static_cast<int>(mag >> f.fraction);
    uint64_t frac = mag & (f.unit() - 1);
    if (!exp) {
        int shift = static_cast<int>(f.fraction) + 1 - static_cast<int>(std::bit_width(frac));
        exp = 1 - shift;
        frac = (frac << shift) & (f.unit() - 1);
    }
    unsigned index = static_cast<unsigned>(frac >> (f.fraction - (root ? 6 : 7)));
    uint64_t mantissa = uint64_t{estimate_entry(root, index, exp & 1)} << (f.fraction - 7);
    int out = root ? (3 * f.bias() - 1 - exp) / 2 : 2 * f.bias() - 1 - exp;
    if (out > 2 * f.bias()) {
        bool inf = rm == 0 || rm == 4 || (sign ? rm == 2 : rm == 3);
        return {sign | (f.inf() - !inf), FP_OF | FP_NX};
    }
    if (out <= 0) {
        return {sign | ((f.unit() | mantissa) >> (1 - out)), 0};
    }
    return {sign | (uint64_t(out) << f.fraction) | mantissa, 0};
}

static void accrue(Cpu *cpu, unsigned flags)
{
    if (flags) {
        cpu->fcsr |= flags;
        cpu->mstatus |= FS;
    }
}

static FloatResult operation(VectorOp o, FloatFormat f, uint64_t a, uint64_t b, uint64_t old,
                             unsigned rm)
{
    switch (o.code) {
    case 0:
    case 48:
    case 52:
        return fp_binary(f, FloatOp::Add, a, b, rm);
    case 2:
    case 50:
    case 54:
        return fp_binary(f, FloatOp::Sub, a, b, rm);
    case 4:
        return fp_binary(f, FloatOp::Min, a, b, rm);
    case 6:
        return fp_binary(f, FloatOp::Max, a, b, rm);
    case 8:
    case 9:
    case 10: {
        uint64_t sign = o.code == 8 ? b : (o.code == 9 ? ~b : a ^ b);
        return {(a & ~f.sign()) | (sign & f.sign()), 0};
    }
    case 19:
        if (!o.vs1) {
            return fp_sqrt(f, a, rm);
        }
        if (o.vs1 == 16) {
            return {fp_class(f, a), 0};
        }
        return estimate(f, a, o.vs1 == 4, rm);
    case 24:
        return fp_binary(f, FloatOp::Eq, a, b, rm);
    case 25:
        return fp_binary(f, FloatOp::Le, a, b, rm);
    case 27:
        return fp_binary(f, FloatOp::Lt, a, b, rm);
    case 28: {
        FloatResult r = fp_binary(f, FloatOp::Eq, a, b, rm);
        r.bits ^= 1;
        return r;
    }
    case 29:
        return fp_binary(f, FloatOp::Lt, b, a, rm);
    case 31:
        return fp_binary(f, FloatOp::Le, b, a, rm);
    case 32:
        return fp_binary(f, FloatOp::Div, a, b, rm);
    case 33:
        return fp_binary(f, FloatOp::Div, b, a, rm);
    case 36:
    case 56:
        return fp_binary(f, FloatOp::Mul, a, b, rm);
    case 39:
        return fp_binary(f, FloatOp::Sub, b, a, rm);
    default: {
        uint64_t x = o.code < 44 ? old : a, c = o.code < 44 ? a : old;
        if (o.code & 1) {
            x ^= f.sign();
        }
        if (((o.code ^ (o.code >> 1)) & 1)) {
            c ^= f.sign();
        }
        return fp_fma(f, x, b, c, rm);
    }
    }
}

static bool arithmetic_form(VectorOp o)
{
    switch (o.code) {
    case 0:
    case 2:
    case 4:
    case 6:
    case 8:
    case 9:
    case 10:
    case 24:
    case 25:
    case 27:
    case 28:
    case 32:
    case 36:
    case 40:
    case 41:
    case 42:
    case 43:
    case 44:
    case 45:
    case 46:
    case 47:
    case 48:
    case 50:
    case 52:
    case 54:
    case 56:
    case 60:
    case 61:
    case 62:
    case 63:
        return true;
    case 14:
    case 15:
    case 23:
    case 29:
    case 31:
    case 33:
    case 39:
        return !o.vv();
    case 19:
        return o.vv() && (o.vs1 == 0 || o.vs1 == 4 || o.vs1 == 5 || o.vs1 == 16);
    default:
        return false;
    }
}

static bool convert_vector(Cpu *cpu, VectorOp o, VectorType t, unsigned rm)
{
    if (!o.vv() || o.vs1 >= 24) {
        return false;
    }
    unsigned sub = o.vs1 & 7;
    bool wide = o.vs1 >= 8 && o.vs1 < 16, narrow = o.vs1 >= 16;
    unsigned from = t.bytes * (narrow ? 2 : 1), to = t.bytes * (wide ? 2 : 1);
    if ((sub == 4 && !wide && !narrow) || (sub == 5 && !narrow) || from > 8 || to > 8) {
        return false;
    }
    // Zvfhmin 仅增加 binary16 与 binary32 之间的宽化及窄化转换。
    bool float_to_float = sub == 4 || sub == 5;
    if (float_to_float ? t.bytes < 2 : ((sub == 2 || sub == 3) ? to < 4 : from < 4)) {
        return false;
    }
    if (sub == 5 && t.bytes < 4) {
        return false;
    }
    if (!vector_operand(t, o.vd, to) || !vector_operand(t, o.vs2, from) ||
        !legal_overlap(t, o.vd, to, o.vs2, from) || (!o.unmasked && !o.vd)) {
        return false;
    }
    for (unsigned i = cpu->vstart; i < cpu->vl; ++i) {
        if (!o.unmasked && !vector_mask(cpu, 0, i)) {
            continue;
        }
        uint64_t a = vector_read(cpu, o.vs2, i, from);
        FloatResult r;
        if (float_to_float) {
            r = fp_convert(format(to), format(from), a, sub == 5 ? 1 : rm);
            if (sub == 5 && (r.flags & FP_NX)) {
                r.bits |= 1; // round-to-odd
            }
        } else if (sub == 2 || sub == 3) {
            r = fp_from_int(format(to), a, from * 8, sub == 2, rm);
        } else {
            r = fp_to_int(format(from), a, to * 8, !(sub & 1), sub >= 6 ? 1 : rm);
        }
        accrue(cpu, r.flags);
        vector_write(cpu, o.vd, i, to, r.bits);
    }
    return true;
}

bool vector_float(Cpu *cpu, VectorOp o, VectorType t)
{
    unsigned rm = cpu->fcsr >> 5;
    if (!(cpu->mstatus & FS) || rm > 4) {
        return false;
    }
    if (o.code == 18) {
        return convert_vector(cpu, o, t, rm);
    }
    unsigned w = t.bytes;
    if (w < 4) {
        return false;
    }
    FloatFormat f = format(w);
    if (o.code == 16) {
        if (!o.unmasked || (o.vv() ? o.vs1 : o.vs2)) {
            return false;
        }
        if (o.vv()) {
            cpu->f[o.vd] = fp_box(f, vector_read(cpu, o.vs2, 0, w));
            cpu->mstatus |= FS;
        } else if (cpu->vstart < cpu->vl) {
            vector_write(cpu, o.vd, 0, w, fp_unbox(f, cpu->f[o.vs1]));
        }
        return true;
    }
    bool wide = o.code >= 48;
    unsigned dw = wide ? 2 * w : w;
    if (dw > 8) {
        return false;
    }
    FloatFormat dest = format(dw);
    if (o.vv() &&
        (o.code == 1 || o.code == 3 || o.code == 5 || o.code == 7 || o.code == 49 || o.code == 51)) {
        if (cpu->vstart || !vector_operand(t, o.vs2, w)) {
            return false;
        }
        uint64_t acc = vector_read(cpu, o.vs1, 0, dw);
        FloatOp op = o.code == 5 ? FloatOp::Min : (o.code == 7 ? FloatOp::Max : FloatOp::Add);
        for (unsigned i = 0; i < cpu->vl; ++i) {
            if (!o.unmasked && !vector_mask(cpu, 0, i)) {
                continue;
            }
            uint64_t a = vector_read(cpu, o.vs2, i, w);
            if (wide) {
                auto r = fp_convert(dest, f, a, rm);
                a = r.bits;
                accrue(cpu, r.flags);
            }
            auto r = fp_binary(dest, op, acc, a, rm);
            acc = r.bits;
            accrue(cpu, r.flags);
        }
        if (cpu->vl) {
            vector_write(cpu, o.vd, 0, dw, acc);
        }
        return true;
    }
    if (!arithmetic_form(o)) {
        return false;
    }
    bool mask_result = o.code >= 24 && o.code <= 31, merge = o.code == 23, unary = o.code == 19;
    unsigned aw = (o.code == 52 || o.code == 54) ? dw : w;
    if (merge && o.unmasked && o.vs2) {
        return false;
    }
    if ((!mask_result && (!vector_operand(t, o.vd, dw) || (!o.unmasked && !o.vd))) ||
        !vector_operand(t, o.vs2, aw) || (o.vv() && !unary && !vector_operand(t, o.vs1, w)) ||
        (!mask_result && (!legal_overlap(t, o.vd, dw, o.vs2, aw) ||
                          (o.vv() && !unary && !legal_overlap(t, o.vd, dw, o.vs1, w)))) ||
        (o.code == 14 && o.vd == o.vs2)) {
        return false;
    }
    unsigned group = vector_group(t, w);
    if (mask_result &&
        ((o.vd > o.vs2 && o.vd < o.vs2 + group) || (o.vv() && o.vd > o.vs1 && o.vd < o.vs1 + group))) {
        return false;
    }
    for (unsigned i = cpu->vstart; i < cpu->vl; ++i) {
        bool active = o.unmasked || vector_mask(cpu, 0, i);
        if (!active && !merge) {
            continue;
        }
        uint64_t a = vector_read(cpu, o.vs2, i, aw);
        uint64_t b = o.vv() ? (unary ? 0 : vector_read(cpu, o.vs1, i, w)) : fp_unbox(f, cpu->f[o.vs1]);
        FloatResult r{};
        if (merge) {
            r.bits = active ? b : a;
        } else if (o.code == 14) {
            r.bits = i ? vector_read(cpu, o.vs2, i - 1, w) : b;
        } else if (o.code == 15) {
            r.bits = i + 1 < cpu->vl ? vector_read(cpu, o.vs2, i + 1, w) : b;
        } else {
            if (wide) {
                if (aw != dw) {
                    auto v = fp_convert(dest, f, a, rm);
                    a = v.bits;
                    accrue(cpu, v.flags);
                }
                auto v = fp_convert(dest, f, b, rm);
                b = v.bits;
                accrue(cpu, v.flags);
            }
            uint64_t old = mask_result ? 0 : vector_read(cpu, o.vd, i, dw);
            r = operation(o, dest, a, b, old, rm);
        }
        accrue(cpu, r.flags);
        if (mask_result) {
            vector_mask_write(cpu, o.vd, i, r.bits != 0);
        } else {
            vector_write(cpu, o.vd, i, dw, r.bits);
        }
    }
    return true;
}
