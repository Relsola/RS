#include "cpu_privilege.h"
#include "cpu_integer.h"
#include "float_bits.h"

static FloatFormat scalar_format(unsigned fmt)
{
    return fmt == 0 ? BINARY32 : (fmt == 1 ? BINARY64 : BINARY16);
}

bool cpu_float(Cpu *cpu, Bus *bus, uint32_t insn, uint64_t *rd, bool *write_x)
{
    if (!(cpu->mstatus & FS)) {
        return false;
    }
    unsigned opcode = insn & 127, dest = (insn >> 7) & 31, funct = (insn >> 12) & 7;
    unsigned rs1 = (insn >> 15) & 31, rs2 = (insn >> 20) & 31, format = (insn >> 25) & 3;
    *write_x = false;
    if (opcode == 7 || opcode == 0x27) {
        unsigned width = 1u << funct;
        if (opcode == 7) {
            uint64_t value;
            if (cpu_memory(cpu, bus, Access::Load, cpu->x[rs1] + sign_extend(insn >> 20, 12), width,
                           &value)) {
                cpu->f[dest] = fp_box(width == 2 ? BINARY16 : (width == 4 ? BINARY32 : BINARY64), value);
                cpu->mstatus |= FS;
            }
        } else {
            uint64_t offset = sign_extend(((insn >> 25) << 5) | dest, 12);
            uint64_t value = cpu->f[rs2]; // 窄存储保留原始低位，包括 NaN 载荷。
            cpu_memory(cpu, bus, Access::Store, cpu->x[rs1] + offset, width, &value);
        }
        return true;
    }
    if (format > 2) {
        return false;
    }
    unsigned family = insn >> 27;
    if (format == 2 && (opcode != 0x53 || (family != 8 && family != 30 && !(family == 28 && !funct)))) {
        return false;
    }
    FloatFormat f = scalar_format(format);
    uint64_t a = fp_unbox(f, cpu->f[rs1]), b = fp_unbox(f, cpu->f[rs2]);
    unsigned rm = funct == 7 ? cpu->fcsr >> 5 : funct;
    FloatResult result{};
    if (opcode != 0x53) {
        if (rm > 4) {
            return false;
        }
        uint64_t c = fp_unbox(f, cpu->f[insn >> 27]);
        if (opcode & 8) {
            a ^= f.sign();
        }
        if (opcode & 4) {
            c ^= f.sign();
        }
        result = fp_fma(f, a, b, c, rm);
    } else {
        switch (insn >> 27) {
        case 0:
        case 1:
        case 2:
        case 3: {
            constexpr FloatOp op[] = {FloatOp::Add, FloatOp::Sub, FloatOp::Mul, FloatOp::Div};
            if (rm > 4) {
                return false;
            }
            result = fp_binary(f, op[insn >> 27], a, b, rm);
            break;
        }
        case 4: // FSGNJ / FSGNJN / FSGNJX
            if (funct > 2) {
                return false;
            }
            if (funct == 1) {
                b = ~b;
            }
            if (funct == 2) {
                b ^= a;
            }
            result.bits = (a & ~f.sign()) | (b & f.sign());
            break;
        case 5:
            if (funct > 3) {
                return false;
            }
            result = fp_binary(f, (funct & 1) ? FloatOp::Max : FloatOp::Min, a, b, 0);
            if ((funct & 2) && ((a & (f.sign() - 1)) > f.inf() || (b & (f.sign() - 1)) > f.inf())) {
                result.bits = f.nan();
            }
            break;
        case 8: {
            if (format != 2 && (rs2 == 4 || rs2 == 5) && rm <= 4) {
                result = fp_round_integral(f, a, rm, rs2 == 5);
                break;
            }
            if (rs2 > 2 || rs2 == format || rm > 4) {
                return false;
            }
            FloatFormat from = scalar_format(rs2);
            result = fp_convert(f, from, fp_unbox(from, cpu->f[rs1]), rm);
            break;
        }
        case 11:
            if (rs2 || rm > 4) {
                return false;
            }
            result = fp_sqrt(f, a, rm);
            break;
        case 20: {
            if (funct > 2 && funct != 4 && funct != 5) {
                return false;
            }
            constexpr FloatOp op[] = {FloatOp::Le, FloatOp::Lt, FloatOp::Eq};
            result = fp_binary(f, op[funct & 3], a, b, 0);
            if (funct >= 4 && result.flags && fp_class(f, a) != 256 && fp_class(f, b) != 256) {
                result.flags = 0; // 安静比较只对 signaling NaN 置 NV。
            }
            *write_x = true;
            break;
        }
        case 24:
        case 26: {
            if (family == 24 && format == 1 && rs2 == 8 && funct == 1) {
                result = fp_mod_word(a);
                *write_x = true;
                break;
            }
            if (rs2 > 3 || rm > 4) {
                return false;
            }
            unsigned width = rs2 < 2 ? 32 : 64;
            if ((insn >> 27) == 24) {
                result = fp_to_int(f, a, width, rs2 & 1, rm);
                if (width == 32) {
                    result.bits = sign_extend(static_cast<uint32_t>(result.bits), 32);
                }
                *write_x = true;
            } else {
                result = fp_from_int(f, cpu->x[rs1], width, rs2 & 1, rm);
            }
            break;
        }
        case 28: // FMV.X.W/D 传递原始位，FCLASS 按 NaN-boxing 解释输入。
            if (rs2 || funct > 1) {
                return false;
            }
            result.bits = funct ? fp_class(f, a) : sign_extend(cpu->f[rs1], 1 + f.exponent + f.fraction);
            *write_x = true;
            break;
        case 30:
            if (rs2 == 1 && !funct && format != 2) {
                result.bits = fp_immediate(f, rs1);
                break;
            }
            if (rs2 || funct) {
                return false;
            }
            result.bits = cpu->x[rs1];
            break;
        default:
            return false;
        }
    }
    if (*write_x) {
        *rd = result.bits;
    } else {
        cpu->f[dest] = fp_box(f, result.bits);
        cpu->mstatus |= FS;
    }
    if (result.flags) {
        cpu->fcsr |= result.flags;
        cpu->mstatus |= FS;
    }
    return true;
}
