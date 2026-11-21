#include "vector_state.h"
#include <algorithm>

static bool configure(Cpu *cpu, uint32_t insn, uint64_t *rd)
{
    unsigned rs1 = (insn >> 15) & 31, dest = (insn >> 7) & 31;
    uint64_t raw, avl;
    if ((insn >> 30) == 3) {
        raw = (insn >> 20) & 1023;
        avl = rs1; // vsetivli 的零是 AVL=0。
    } else {
        if (!(insn >> 31)) {
            raw = (insn >> 20) & 2047;
        } else if ((insn >> 25) == 0x40) {
            raw = cpu->x[(insn >> 20) & 31];
        } else {
            return false;
        }
        avl = rs1 ? cpu->x[rs1] : (dest ? UINT64_MAX : cpu->vl);
        if (!dest && !rs1 && vector_type(raw).capacity != vector_type(cpu->vtype).capacity) {
            return false;
        }
    }
    VectorType type = vector_type(raw);
    cpu->vtype = type.capacity ? raw : VILL;
    cpu->vl = static_cast<unsigned>(std::min<uint64_t>(avl, type.capacity));
    *rd = cpu->vl;
    return true;
}

static bool memory(Cpu *cpu, Bus *bus, uint32_t insn, VectorType type)
{
    unsigned encoding = (insn >> 12) & 7;
    if ((encoding && encoding < 5) || ((insn >> 28) & 1)) {
        return false;
    }
    unsigned width = encoding ? 1u << (encoding - 4) : 1;
    unsigned reg = (insn >> 7) & 31, aux = (insn >> 20) & 31;
    unsigned fields = (insn >> 29) + 1, mode = (insn >> 26) & 3;
    bool load = (insn & 0x7f) == 7, unmasked = (insn >> 25) & 1;
    bool whole = mode == 0 && aux == 8, mask = mode == 0 && aux == 11;
    bool first = mode == 0 && aux == 16 && load;
    bool indexed = (mode & 1) != 0;
    uint64_t base = cpu->x[(insn >> 15) & 31];
    unsigned count = cpu->vl, group = 1, data_width = width;
    if (whole) {
        if (!unmasked || !std::has_single_bit(fields) || (!load && width != 1) ||
            !valid_group(reg, fields)) {
            return false;
        }
        count = fields * Cpu::VLEN_BYTES / width;
        fields = 1;
    } else {
        if (!type.capacity || (mode == 0 && aux && !mask && !first)) {
            return false;
        }
        if (mask) {
            if (fields != 1 || !unmasked || width != 1) {
                return false;
            }
            count = (count + 7) / 8;
        } else {
            data_width = indexed ? type.bytes : width;
            group = vector_group(type, data_width);
            if (fields * group > 8 || !valid_group(reg, group, fields) ||
                (load && !unmasked && reg == 0)) {
                return false;
            }
            if (indexed) {
                unsigned index_group = vector_group(type, width);
                if (!valid_group(aux, index_group)) {
                    return false;
                }
                bool overlap = reg < aux + index_group && aux < reg + fields * group;
                if (load && overlap &&
                    (fields > 1 || (data_width < width && reg != aux) ||
                     (data_width > width &&
                      (reg + group != aux + index_group ||
                       type.lmul + std::countr_zero(width) - std::countr_zero(type.bytes) < 0)))) {
                    return false;
                }
            }
        }
    }
    // fault-only-first 后续元素允许缩短 vl。探测使用相同的总线，异常只写临时的陷阱现场。
    Cpu probe;
    if (first) {
        probe = *cpu;
    }
    for (unsigned i = cpu->vstart; i < count; ++i) {
        if (!unmasked && !vector_mask(cpu, 0, i)) {
            continue;
        }
        uint64_t offset = uint64_t{i} * fields * data_width;
        if (mode == 2) {
            offset = i * cpu->x[aux];
        }
        if (indexed) {
            offset = vector_read(cpu, aux, i, width);
        }
        for (unsigned field = 0; field < fields; ++field) {
            unsigned target = reg + field * group;
            uint64_t value = load ? 0 : vector_read(cpu, target, i, data_width);
            uint64_t address = base + offset + field * data_width;
            Cpu *fault_target = first && i ? &probe : cpu;
            if (!cpu_memory(fault_target, bus, load ? Access::Load : Access::Store, address, data_width,
                            &value)) {
                if (fault_target == cpu) {
                    cpu->vstart = i;
                    cpu->mstatus |= VS;
                } else {
                    cpu->vl = i;
                }
                return true;
            }
            if (load) {
                vector_write(cpu, target, i, data_width, value);
            }
        }
    }
    return true;
}

bool cpu_vector(Cpu *cpu, Bus *bus, uint32_t insn, uint64_t *rd, bool *write_x)
{
    if (!(cpu->mstatus & VS)) {
        return false;
    }
    *write_x = false;
    VectorType type = vector_type(cpu->vtype);
    bool ok;
    if ((insn & 127) != 0x57) {
        ok = memory(cpu, bus, insn, type);
    } else if (((insn >> 12) & 7) == 7) {
        ok = configure(cpu, insn, rd);
        *write_x = true;
    } else {
        ok = type.capacity && vector_alu(cpu, insn, type, rd, write_x);
    }
    if (ok && !cpu->trapped) {
        cpu->vstart = 0;
        cpu->mstatus |= VS;
    }
    return ok;
}
