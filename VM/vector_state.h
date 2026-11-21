#pragma once

#include "cpu_privilege.h"
#include <bit>

constexpr uint64_t VILL = uint64_t{1} << 63;

struct VectorType {
    unsigned bytes, capacity;
    int lmul; // log2(LMUL)，负数表示分数分组。
};

inline VectorType vector_type(uint64_t type)
{
    int lmul = static_cast<int>(type & 7);
    if (lmul & 4) {
        lmul -= 8;
    }
    unsigned bytes = 1u << ((type >> 3) & 7);
    if (type > 255 || lmul == -4 || bytes > (8u >> (lmul < 0 ? -lmul : 0))) {
        return {};
    }
    unsigned group_bytes = lmul < 0 ? Cpu::VLEN_BYTES >> -lmul : Cpu::VLEN_BYTES << lmul;
    return {bytes, group_bytes / bytes, lmul};
}

// 返回实际跨越的架构寄存器数。分数 EMUL 只使用首个寄存器的低字节。
inline unsigned vector_group(VectorType type, unsigned width)
{
    int emul = type.lmul + std::countr_zero(width) - std::countr_zero(type.bytes);
    return emul < -3 || emul > 3 ? 0 : 1u << (emul > 0 ? emul : 0);
}

inline bool valid_group(unsigned reg, unsigned group, unsigned fields = 1)
{
    return group && reg % group == 0 && reg + fields * group <= 32;
}

inline uint64_t vector_read(const Cpu *cpu, unsigned reg, unsigned index, unsigned width)
{
    unsigned offset = index * width;
    const uint8_t *p = &cpu->v[reg + offset / Cpu::VLEN_BYTES][offset % Cpu::VLEN_BYTES];
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) {
        value |= uint64_t{p[i]} << (8 * i);
    }
    return value;
}

inline void vector_write(Cpu *cpu, unsigned reg, unsigned index, unsigned width, uint64_t value)
{
    unsigned offset = index * width;
    uint8_t *p = &cpu->v[reg + offset / Cpu::VLEN_BYTES][offset % Cpu::VLEN_BYTES];
    for (unsigned i = 0; i < width; ++i) {
        p[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

inline bool vector_mask(const Cpu *cpu, unsigned reg, unsigned i)
{
    return ((cpu->v[reg][i / 8] >> (i % 8)) & 1) != 0;
}

inline void vector_mask_write(Cpu *cpu, unsigned reg, unsigned i, bool bit)
{
    uint8_t &byte = cpu->v[reg][i / 8];
    unsigned mask = 1u << (i % 8);
    byte = static_cast<uint8_t>(bit ? byte | mask : byte & ~mask);
}

bool vector_alu(Cpu *cpu, uint32_t insn, VectorType type, uint64_t *rd, bool *write_x);

struct VectorOp {
    unsigned code, form, vd, vs1, vs2;
    bool unmasked;
    bool vv() const { return form <= 2; }
};

inline bool vector_operand(VectorType t, unsigned reg, unsigned width)
{
    return valid_group(reg, vector_group(t, width));
}

inline bool overlaps(unsigned a, unsigned na, unsigned b, unsigned nb)
{
    return a < b + nb && b < a + na;
}

// 不同 EEW 的读写若重叠，只允许窄目的占源组低端，或整数源组占宽目的高端。
inline bool legal_overlap(VectorType t, unsigned dest, unsigned dw, unsigned src, unsigned sw)
{
    unsigned dg = vector_group(t, dw), sg = vector_group(t, sw);
    if (dw == sw || !overlaps(dest, dg, src, sg)) {
        return true;
    }
    if (dw < sw) {
        return dest == src;
    }
    int source_lmul = t.lmul + std::countr_zero(sw) - std::countr_zero(t.bytes);
    return source_lmul >= 0 && dest + dg == src + sg;
}

bool vector_float(Cpu *cpu, VectorOp op, VectorType type);
