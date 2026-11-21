#include "cpu_compressed.h"

// 原始编码的 [hi:lo] 放到结果的 dst 位起，便于与 RVC 格式图逐段对照。
static uint32_t field(uint32_t value, unsigned hi, unsigned lo, unsigned dst = 0)
{
    return ((value >> lo) & ((1u << (hi - lo + 1)) - 1u)) << dst;
}

static uint32_t signed_bits(uint32_t value, unsigned width)
{
    const uint32_t sign = 1u << (width - 1);
    return (value ^ sign) - sign;
}

static uint32_t i_type(unsigned opcode, unsigned func, unsigned rd, unsigned rs1, uint32_t imm)
{
    return (imm << 20) | (rs1 << 15) | (func << 12) | (rd << 7) | opcode;
}

static uint32_t r_type(unsigned opcode, unsigned func, unsigned high, unsigned rd,
                       unsigned rs1, unsigned rs2)
{
    return (high << 25) | (rs2 << 20) | (rs1 << 15) | (func << 12) | (rd << 7) | opcode;
}

static uint32_t store(unsigned func, unsigned base, unsigned source, uint32_t imm, unsigned opcode = 0x23)
{
    return field(imm, 11, 5, 25) | (source << 20) | (base << 15) | (func << 12)
        | field(imm, 4, 0, 7) | opcode;
}

static uint32_t branch_zero(unsigned func, unsigned source, uint32_t offset)
{
    return field(offset, 12, 12, 31) | field(offset, 10, 5, 25) | (source << 15) | (func << 12)
        | field(offset, 4, 1, 8) | field(offset, 11, 11, 7) | 0x63;
}

static uint32_t jump(uint32_t offset)
{
    return field(offset, 20, 20, 31) | field(offset, 10, 1, 21) | field(offset, 11, 11, 20)
        | field(offset, 19, 12, 12) | 0x6f;
}

static uint32_t quadrant_zero(uint16_t code)
{
    const unsigned operation = code >> 13;
    // 三位短寄存器编号 000..111 对应 x8..x15。
    const unsigned reg = 8 + field(code, 4, 2);
    const unsigned base = 8 + field(code, 9, 7);
    switch (operation) {
    case 0: { // C.ADDI4SPN
        const unsigned imm = field(code, 12, 11, 4) | field(code, 10, 7, 6)
            | field(code, 6, 6, 2) | field(code, 5, 5, 3);
        return imm ? i_type(0x13, 0, reg, 2, imm) : 0;
    }
    case 2: case 6: { // C.LW / C.SW
        const unsigned imm = field(code, 12, 10, 3) | field(code, 6, 6, 2) | field(code, 5, 5, 6);
        return operation == 2 ? i_type(0x03, 2, reg, base, imm) : store(2, base, reg, imm);
    }
    case 1: case 3: case 5: case 7: { // C.FLD / C.LD / C.FSD / C.SD
        const unsigned imm = field(code, 12, 10, 3) | field(code, 6, 5, 6);
        if (operation < 4) return i_type(operation == 1 ? 0x07 : 0x03, 3, reg, base, imm);
        return store(3, base, reg, imm, operation == 5 ? 0x27 : 0x23);
    }
    case 4: { // Zcb：字节偏移的 bit 0 在编码 bit 6，bit 1 在编码 bit 5。
        const unsigned half_offset = field(code, 5, 5, 1);
        const unsigned bit6 = field(code, 6, 6);
        const unsigned byte_offset = half_offset | bit6;
        switch (field(code, 12, 10)) {
        case 0: return i_type(0x03, 4, reg, base, byte_offset);               // C.LBU
        case 1: return i_type(0x03, bit6 ? 1 : 5, reg, base, half_offset);    // C.LH / C.LHU
        case 2: return store(0, base, reg, byte_offset);                      // C.SB
        case 3: return bit6 ? 0 : store(1, base, reg, half_offset);           // C.SH
        default: return 0;
        }
    }
    default: return 0; // 浮点或其他扩展的编码。
    }
}

static uint32_t compact_alu(uint16_t code, uint32_t imm6)
{
    const unsigned rd = 8 + field(code, 9, 7);
    switch (field(code, 11, 10)) {
    case 0: return i_type(0x13, 5, rd, rd, imm6);                 // C.SRLI
    case 1: return i_type(0x13, 5, rd, rd, 0x400 | imm6);         // C.SRAI
    case 2: return i_type(0x13, 7, rd, rd, signed_bits(imm6, 6)); // C.ANDI
    }
    const unsigned rs2 = 8 + field(code, 4, 2);
    switch (field(code, 12, 12, 2) | field(code, 6, 5)) {
    case 0: return r_type(0x33, 0, 0x20, rd, rd, rs2); // C.SUB
    case 1: return r_type(0x33, 4, 0, rd, rd, rs2);    // C.XOR
    case 2: return r_type(0x33, 6, 0, rd, rd, rs2);    // C.OR
    case 3: return r_type(0x33, 7, 0, rd, rd, rs2);    // C.AND
    case 4: return r_type(0x3b, 0, 0x20, rd, rd, rs2); // C.SUBW
    case 5: return r_type(0x3b, 0, 0, rd, rd, rs2);    // C.ADDW
    case 6: return r_type(0x33, 0, 1, rd, rd, rs2);    // C.MUL
    case 7: // Zcb 单操作数格式将原来的 rs2 字段用作操作编号。
        switch (field(code, 4, 2)) {
        case 0: return i_type(0x13, 7, rd, rd, 0x0ff);     // C.ZEXT.B -> ANDI
        case 1: return i_type(0x13, 1, rd, rd, 0x604);     // C.SEXT.B -> SEXT.B
        case 2: return r_type(0x3b, 4, 0x04, rd, rd, 0);   // C.ZEXT.H -> ZEXT.H
        case 3: return i_type(0x13, 1, rd, rd, 0x605);     // C.SEXT.H -> SEXT.H
        case 4: return r_type(0x3b, 0, 0x04, rd, rd, 0);   // C.ZEXT.W -> ADD.UW rd,rd,x0
        case 5: return i_type(0x13, 4, rd, rd, 0xfff);     // C.NOT -> XORI
        default: return 0;
        }
    default: return 0;
    }
}

static uint32_t quadrant_one(uint16_t code)
{
    const unsigned operation = code >> 13;
    const unsigned rd = field(code, 11, 7);
    const unsigned imm6 = field(code, 12, 12, 5) | field(code, 6, 2);
    switch (operation) {
    case 0: return i_type(0x13, 0, rd, rd, signed_bits(imm6, 6));          // C.ADDI / C.NOP
    case 1: return rd ? i_type(0x1b, 0, rd, rd, signed_bits(imm6, 6)) : 0; // C.ADDIW
    case 2: return i_type(0x13, 0, rd, 0, signed_bits(imm6, 6));           // C.LI
    case 3: {
        if (imm6 == 0) {
            return 0;
        }
        if (rd == 2) { // C.ADDI16SP
            unsigned imm = field(code, 12, 12, 9) | field(code, 6, 6, 4)
                | field(code, 5, 5, 6) | field(code, 4, 3, 7) | field(code, 2, 2, 5);
            return i_type(0x13, 0, rd, rd, signed_bits(imm, 10));
        }
        return (signed_bits(imm6, 6) << 12) | (rd << 7) | 0x37; // C.LUI
    }
    case 4: return compact_alu(code, imm6);
    case 5: { // C.J
        unsigned imm = field(code, 12, 12, 11) | field(code, 11, 11, 4) | field(code, 10, 9, 8)
            | field(code, 8, 8, 10) | field(code, 7, 7, 6) | field(code, 6, 6, 7)
            | field(code, 5, 3, 1) | field(code, 2, 2, 5);
        return jump(signed_bits(imm, 12));
    }
    default: { // C.BEQZ / C.BNEZ
        unsigned imm = field(code, 12, 12, 8) | field(code, 11, 10, 3)
            | field(code, 6, 5, 6) | field(code, 4, 3, 1) | field(code, 2, 2, 5);
        return branch_zero(operation == 7, 8 + field(code, 9, 7), signed_bits(imm, 9));
    }
    }
}

static uint32_t quadrant_two(uint16_t code)
{
    const unsigned rd = field(code, 11, 7);
    const unsigned rs2 = field(code, 6, 2);
    const unsigned high = field(code, 12, 12);
    switch (code >> 13) {
    case 0: return i_type(0x13, 1, rd, rd, (high << 5) | rs2); // C.SLLI
    case 2: { // C.LWSP
        unsigned imm = (high << 5) | field(code, 6, 4, 2) | field(code, 3, 2, 6);
        return rd ? i_type(0x03, 2, rd, 2, imm) : 0;
    }
    case 1: case 3: { // C.FLDSP 允许 f0 作为目的寄存器。
        unsigned imm = (high << 5) | field(code, 6, 5, 3) | field(code, 4, 2, 6);
        if ((code >> 13) == 1) return i_type(0x07, 3, rd, 2, imm);
        return rd ? i_type(0x03, 3, rd, 2, imm) : 0;
    }
    case 4:
        if (rs2 != 0) { // C.MV / C.ADD；rd=x0 时为 HINT。
            return r_type(0x33, 0, 0, rd, high ? rd : 0, rs2);
        }
        if (rd != 0) { // C.JR / C.JALR
            return i_type(0x67, 0, high, rd, 0);
        }
        return high ? 0x00100073 : 0; // C.EBREAK / 保留
    case 6: { // C.SWSP
        unsigned imm = field(code, 12, 9, 2) | field(code, 8, 7, 6);
        return store(2, 2, rs2, imm);
    }
    case 5: case 7: { // C.FSDSP / C.SDSP
        unsigned imm = field(code, 12, 10, 3) | field(code, 9, 7, 6);
        return store(3, 2, rs2, imm, (code >> 13) == 5 ? 0x27 : 0x23);
    }
    default: return 0;
    }
}

bool cpu_expand_compressed(uint16_t encoded, uint32_t *expanded)
{
    // 合法 HINT 展开为写 x0 或保持原值的计算，和普通指令一样退休。
    switch (encoded & 3) {
    case 0: *expanded = quadrant_zero(encoded); break;
    case 1: *expanded = quadrant_one(encoded); break;
    case 2: *expanded = quadrant_two(encoded); break;
    default: *expanded = 0; break;
    }
    return *expanded != 0;
}
