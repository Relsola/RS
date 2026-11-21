#include "cpu_bitmanip.h"
#include "cpu_integer.h"

#include <algorithm>
#include <bit>

static constexpr unsigned operation(unsigned funct7, unsigned funct3)
{
    return (funct7 << 3) | funct3;
}

static bool single_bit(unsigned funct7, unsigned funct3, uint64_t value, unsigned index, uint64_t *out)
{
    uint64_t mask = uint64_t{1} << index;
    switch (operation(funct7, funct3)) {
    case operation(0x24, 1): *out = value & ~mask; break; // BCLR
    case operation(0x24, 5): *out = (value >> index) & 1; break; // BEXT
    case operation(0x34, 1): *out = value ^ mask; break; // BINV
    case operation(0x14, 1): *out = value | mask; break; // BSET
    default: return false;
    }
    return true;
}

static bool register_operation(uint32_t insn, bool word, uint64_t lhs, uint64_t rhs, uint64_t *out)
{
    const unsigned funct7 = insn >> 25;
    const unsigned funct3 = (insn >> 12) & 7;
    if (funct7 == 0x10 && (funct3 == 2 || funct3 == 4 || funct3 == 6)) {
        // SH[1-3]ADD.UW 先零扩展 rs1，移位和加法都按 64 位进行。
        uint64_t index = word ? static_cast<uint32_t>(lhs) : lhs;
        *out = (index << (funct3 / 2)) + rhs;
        return true;
    }
    if (word) {
        const uint32_t lo = static_cast<uint32_t>(lhs);
        const int shift = static_cast<int>(rhs & 31);
        switch (operation(funct7, funct3)) {
        case operation(0x04, 0): *out = rhs + lo; break; // ADD.UW
        case operation(0x04, 4):                          // ZEXT.H 固定 rs2=x0
            if (((insn >> 20) & 31) != 0) {
                return false;
            }
            *out = lhs & 0xffff;
            break;
        case operation(0x30, 1): *out = sign_extend(std::rotl(lo, shift), 32); break; // ROLW
        case operation(0x30, 5): *out = sign_extend(std::rotr(lo, shift), 32); break; // RORW
        default: return false;
        }
        return true;
    }

    const int shift = static_cast<int>(rhs & 63);
    const int64_t signed_lhs = std::bit_cast<int64_t>(lhs);
    const int64_t signed_rhs = std::bit_cast<int64_t>(rhs);
    switch (operation(funct7, funct3)) {
    case operation(0x20, 7): *out = lhs & ~rhs; break;                  // ANDN
    case operation(0x20, 6): *out = lhs | ~rhs; break;                  // ORN
    case operation(0x20, 4): *out = ~(lhs ^ rhs); break;                // XNOR
    case operation(0x05, 4): *out = static_cast<uint64_t>(std::min(signed_lhs, signed_rhs)); break;
    case operation(0x05, 5): *out = std::min(lhs, rhs); break;          // MINU
    case operation(0x05, 6): *out = static_cast<uint64_t>(std::max(signed_lhs, signed_rhs)); break;
    case operation(0x05, 7): *out = std::max(lhs, rhs); break;          // MAXU
    case operation(0x30, 1): *out = std::rotl(lhs, shift); break;       // ROL
    case operation(0x30, 5): *out = std::rotr(lhs, shift); break;       // ROR
    default: return single_bit(funct7, funct3, lhs, static_cast<unsigned>(shift), out);
    }
    return true;
}

static bool immediate_operation(uint32_t insn, bool word, uint64_t lhs, uint64_t *out)
{
    const unsigned funct3 = (insn >> 12) & 7;
    const unsigned imm = insn >> 20;
    const unsigned funct6 = imm >> 6;
    const unsigned shift = imm & 63;
    const uint32_t lo = static_cast<uint32_t>(lhs);
    if (funct3 == 1) {
        switch (imm) {
        case 0x600: *out = word ? std::countl_zero(lo) : std::countl_zero(lhs); return true;
        case 0x601: *out = word ? std::countr_zero(lo) : std::countr_zero(lhs); return true;
        case 0x602: *out = word ? std::popcount(lo) : std::popcount(lhs); return true;
        }
    }
    if (word) {
        if (funct3 == 1 && funct6 == 2) { // SLLI.UW 的移位量为 6 位。
            *out = uint64_t{lo} << shift;
            return true;
        }
        if (funct3 == 5 && imm >> 5 == 0x30) { // RORIW 只有 5 位移位量。
            *out = sign_extend(std::rotr(lo, static_cast<int>(shift)), 32);
            return true;
        }
        return false;
    }
    if (funct3 == 1 && (imm == 0x604 || imm == 0x605)) {
        *out = sign_extend(lhs, imm == 0x604 ? 8 : 16); // SEXT.B / SEXT.H
        return true;
    }
    if (funct3 == 5) {
        if (imm == 0x6b8) {
            *out = std::byteswap(lhs); // REV8
            return true;
        }
        if (imm == 0x287) { // ORC.B：每个非零字节独立扩展成 0xff。
            *out = 0;
            for (unsigned bit = 0; bit < 64; bit += 8) {
                const uint64_t byte = uint64_t{0xff} << bit;
                if (lhs & byte) {
                    *out |= byte;
                }
            }
            return true;
        }
        if (funct6 == 0x18) {
            *out = std::rotr(lhs, static_cast<int>(shift)); // RORI
            return true;
        }
    }
    // 立即数编码的 bit 25 是 shamt[5]；寄存器编码的这一位属于固定 funct7。
    return single_bit(funct6 << 1, funct3, lhs, shift, out);
}

bool cpu_bitmanip(uint32_t instruction, uint64_t lhs, uint64_t rhs, uint64_t *result)
{
    switch (instruction & 0x7f) {
    case 0x33: return register_operation(instruction, false, lhs, rhs, result);
    case 0x3b: return register_operation(instruction, true, lhs, rhs, result);
    case 0x13: return immediate_operation(instruction, false, lhs, result);
    case 0x1b: return immediate_operation(instruction, true, lhs, result);
    default: return false;
    }
}
