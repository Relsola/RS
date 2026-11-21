#include "cpu.h"
#include "cpu_compressed.h"
#include "cpu_integer.h"
#include "cpu_privilege.h"

#include <bit>

static bool branch_condition(unsigned operation, uint64_t a, uint64_t b, bool *take)
{
    switch (operation) {
    case 0: *take = a == b; break;
    case 1: *take = a != b; break;
    case 4: *take = std::bit_cast<int64_t>(a) < std::bit_cast<int64_t>(b); break;
    case 5: *take = std::bit_cast<int64_t>(a) >= std::bit_cast<int64_t>(b); break;
    case 6: *take = a < b; break;
    case 7: *take = a >= b; break;
    default: return false;
    }
    return true;
}

static uint64_t branch_offset(uint32_t insn)
{
    const unsigned bits = ((insn >> 31) << 12) | (((insn >> 7) & 1) << 11)
                        | (((insn >> 25) & 63) << 5) | (((insn >> 8) & 15) << 1);
    return sign_extend(bits, 13);
}

static uint64_t jump_offset(uint32_t insn)
{
    const unsigned bits = ((insn >> 31) << 20) | (insn & 0xff000)
                        | (((insn >> 20) & 1) << 11) | (((insn >> 21) & 0x3ff) << 1);
    return sign_extend(bits, 21);
}

void cpu_step(Cpu *cpu, Bus *bus)
{
    cpu->trapped = false;
    ++cpu->mcycle;
    ++cpu->counter_time;
    if (cpu_take_timer_interrupt(cpu)) {
        return;
    }
    uint64_t first = 0;
    if (!cpu_memory(cpu, bus, Access::Fetch, cpu->pc, 2, &first)) {
        return;
    }
    // IALIGN=16：32 位指令也可能从一页的最后半字开始。
    // 半字分别取指；若第二页故障，epc 仍指向整条指令的起点。
    const bool compressed = (first & 3) != 3;
    uint32_t raw = static_cast<uint32_t>(first);
    uint32_t insn = raw;
    if (compressed) {
        if (!cpu_expand_compressed(static_cast<uint16_t>(first), &insn)) {
            cpu_trap(cpu, ILLEGAL_INSTRUCTION, raw);
            return;
        }
    } else {
        uint64_t second = 0;
        if (!cpu_memory(cpu, bus, Access::Fetch, cpu->pc + 2, 2, &second)) {
            return;
        }
        raw |= static_cast<uint32_t>(second) << 16;
        insn = raw;
    }
    const unsigned opcode = insn & 0x7f;
    const unsigned rd = (insn >> 7) & 31;
    const unsigned function = (insn >> 12) & 7;
    const uint64_t a = cpu->x[(insn >> 15) & 31];
    const uint64_t b = cpu->x[(insn >> 20) & 31];
    const uint64_t imm_i = sign_extend(insn >> 20, 12);
    const uint64_t imm_u = sign_extend(insn & 0xfffff000, 32);
    uint64_t next_pc = cpu->pc + (compressed ? 2 : 4);
    uint64_t result = 0;
    bool writes_rd = true;
    bool wrote_instret = false;
    bool legal = true;

    switch (opcode) {
    case 0x37: result = imm_u; break; // LUI
    case 0x17: result = cpu->pc + imm_u; break; // AUIPC
    case 0x6f: // JAL
        result = next_pc;
        next_pc = cpu->pc + jump_offset(insn);
        break;
    case 0x67: // JALR 先读取旧 rs1，最后才写入可能相同的 rd。
        legal = function == 0;
        result = next_pc;
        next_pc = (a + imm_i) & ~uint64_t{1};
        break;
    case 0x63: { // BRANCH
        writes_rd = false;
        bool take = false;
        legal = branch_condition(function, a, b, &take);
        if (take) {
            next_pc = cpu->pc + branch_offset(insn);
        }
        break;
    }
    case 0x03: { // LOAD
        if (function == 7) {
            legal = false;
            break;
        }
        const unsigned width = 1u << (function & 3);
        const uint64_t address = a + imm_i;
        if (!cpu_memory(cpu, bus, Access::Load, address, width, &result)) {
            return;
        }
        if (function < 3) {
            result = sign_extend(result, width * 8);
        }
        break;
    }
    case 0x23: { // STORE
        writes_rd = false;
        if (function > 3) {
            legal = false;
            break;
        }
        const uint64_t offset = sign_extend(((insn >> 25) << 5) | rd, 12);
        const uint64_t address = a + offset;
        const unsigned width = 1u << function;
        uint64_t stored = width == 8 ? b : b & ((uint64_t{1} << (width * 8)) - 1);
        if (!cpu_memory(cpu, bus, Access::Store, address, width, &stored)) {
            return;
        }
        break;
    }
    case 0x2f: // AMO
        legal = cpu_atomic(cpu, bus, insn, a, b, &result);
        if (cpu->trapped) {
            return;
        }
        break;
    case 0x13: case 0x1b: // OP-IMM, OP-IMM-32
    case 0x33: case 0x3b: // OP, OP-32
        legal = cpu_integer(insn, a, b, &result);
        break;
    case 0x0f: // MISC-MEM
        writes_rd = false;
        if ((insn & 0xfff07fff) == 0x0040200f) { // CBO.ZERO (rs1), rd=x0
            legal = cpu_zero_block(cpu, bus, a);
            if (cpu->trapped) {
                return;
            }
        } else {
            legal = function <= 1; // FENCE / FENCE.I：本机按序执行，每次取指都读总线。
        }
        break;
    case 0x73: // SYSTEM
        writes_rd = function != 0;
        legal = cpu_system(cpu, insn, &next_pc, &result, &wrote_instret);
        if (cpu->trapped) {
            return;
        }
        break;
    default: legal = false; break;
    }

    if (!legal) {
        cpu_trap(cpu, ILLEGAL_INSTRUCTION, raw);
        return;
    }
    if ((next_pc & 1) != 0) {
        cpu_trap(cpu, FETCH_MISALIGNED, next_pc);
        return;
    }
    // rd=x0 的 load 仍须访问总线。本处只抑制最终的寄存器写回。
    if (writes_rd && rd != 0) {
        cpu->x[rd] = result;
    }
    cpu->pc = next_pc;
    if (!wrote_instret) {
        ++cpu->minstret;
    }
}
