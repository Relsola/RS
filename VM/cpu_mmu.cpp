#include "cpu_privilege.h"
#include "cpu_integer.h"

#include <bit>

static constexpr unsigned BLOCK_BYTES = 64; // RVA23 的 Zic64b

static TrapCause access_fault(Access access)
{
    return access == Access::Fetch ? FETCH_FAULT : (access == Access::Load ? LOAD_FAULT : STORE_FAULT);
}

static TrapCause page_fault(Access access)
{
    return access == Access::Fetch ? FETCH_PAGE_FAULT :
           (access == Access::Load ? LOAD_PAGE_FAULT : STORE_PAGE_FAULT);
}

static bool pmp_allows(const Cpu *cpu, Privilege mode, Access access, uint64_t address, unsigned width)
{
    constexpr uint64_t physical_limit = uint64_t{1} << 56;
    if (address > physical_limit - width) {
        return false;
    }
    unsigned permission = 0;
    switch (access) {
    case Access::Fetch: permission = 4; break;
    case Access::Load:  permission = 1; break;
    case Access::Store: case Access::ZeroBlock: permission = 2; break;
    case Access::Amo:   permission = 3; break; // AMO 同时要求读和写。
    }
    for (unsigned i = 0; i < 16; ++i) {
        const uint8_t cfg = cpu->pmpcfg[i];
        const unsigned range_type = (cfg >> 3) & 3;
        const uint64_t entry = cpu->pmpaddr[i];
        uint64_t first, end;
        switch (range_type) {
        case 1: // TOR：当前地址为开区间上界，前一地址为下界。
            first = i == 0 ? 0 : cpu->pmpaddr[i - 1] << 2;
            end = entry << 2;
            break;
        case 2: // NA4
            first = entry << 2;
            end = first + 4;
            break;
        case 3: { // NAPOT：最低端连续 1 的个数决定自然对齐区域的尺寸。
            const unsigned ones = static_cast<unsigned>(std::countr_one(entry));
            const uint64_t size = ones >= 53 ? physical_limit : uint64_t{8} << ones;
            first = (entry << 2) & ~(size - 1);
            end = first + size;
            break;
        }
        default: continue;
        }
        if (first >= end || address >= end || address + width <= first) {
            continue;
        }
        // 最低编号的重叠项决定结果；访问横跨它的边界也会失败。
        return address >= first && address + width <= end &&
               ((mode == MACHINE && (cfg & 0x80) == 0) || (cfg & permission) == permission);
    }
    return mode == MACHINE;
}

// 失败时返回与原始操作种类相符的异常。读页表失败也是取指/读/写的访问错误，
// 即使失败的隐式操作自身总是一次 8 字节读取。
bool cpu_translate(Cpu *cpu, Bus *bus, Access access, uint64_t va, unsigned width, uint64_t *pa)
{
    Privilege mode = cpu->privilege;
    if (access != Access::Fetch && mode == MACHINE && (cpu->mstatus & MPRV)) {
        mode = static_cast<Privilege>((cpu->mstatus & MPP) >> 11);
    }
    *pa = va;
    if (mode < MACHINE && (cpu->satp >> 60) == 8) {
        bool mapped = false;
        uint64_t table = (cpu->satp & PPN_MASK) << 12;
        if (va == sign_extend(va, 39)) {
            for (int level = 2; level >= 0; --level) {
                const unsigned shift = static_cast<unsigned>(level) * 9;
                const uint64_t index = (va >> (12 + shift)) & 0x1ff;
                const uint64_t slot = table + index * 8;
                uint64_t pte = 0;
                if (!pmp_allows(cpu, SUPERVISOR, Access::Load, slot, 8) ||
                    !bus_is_ram(bus, slot, 8) || !bus_load(bus, slot, 8, &pte)) {
                    cpu_trap(cpu, access_fault(access), va);
                    return false;
                }
                const bool valid = (pte & 1) != 0, read = (pte & 2) != 0;
                const bool write = (pte & 4) != 0, execute = (pte & 8) != 0;
                const bool user = (pte & 16) != 0, accessed = (pte & 64) != 0, dirty = (pte & 128) != 0;
                constexpr uint64_t n_bit = uint64_t{1} << 63;
                const bool napot = (pte & n_bit) != 0;
                if (!valid || (!read && write) || ((pte & ~n_bit) >> 54) != 0) {
                    break;
                }
                uint64_t ppn = (pte >> 10) & PPN_MASK;
                if (!read && !execute) {
                    if (napot || user || accessed || dirty) {
                        break; // 非叶项的 N/U/A/D 位保留。
                    }
                    table = ppn << 12;
                    continue;
                }
                if (napot) {
                    // Svnapot 1.0 的 64 KiB 编码：PPN 低四位为 1000，翻译时用 VPN 低四位替换。
                    if (level != 0 || (ppn & 0xf) != 8) {
                        break;
                    }
                    ppn = (ppn & ~uint64_t{0xf}) | (index & 0xf);
                }
                if ((mode == USER && !user) ||
                    (mode == SUPERVISOR && user && (access == Access::Fetch || !(cpu->mstatus & SUM)))) {
                    break;
                }
                const bool can_read = read || ((cpu->mstatus & MXR) && execute);
                const bool needs_write = access == Access::Store || access == Access::Amo ||
                                         access == Access::ZeroBlock;
                if ((access == Access::Fetch && !execute) || (access == Access::Load && !can_read) ||
                    (needs_write && (!write || !dirty)) || !accessed) {
                    break; // Svade：缺少 A/D 由软件处理。
                }
                const uint64_t page_mask = (uint64_t{1} << (12 + shift)) - 1;
                if (((ppn << 12) & page_mask) != 0) {
                    break; // 大页要求物理基址按同等大小对齐。
                }
                *pa = (ppn << 12) | (va & page_mask);
                mapped = true;
                break;
            }
        }
        if (!mapped) {
            cpu_trap(cpu, page_fault(access), va);
            return false;
        }
    }
    if (access == Access::ZeroBlock) {
        // 块不会跨越 4 KiB 页。只对物理范围向下对齐，异常仍报告 rs1 的原始虚拟地址。
        *pa &= ~uint64_t{BLOCK_BYTES - 1};
    }
    if (!pmp_allows(cpu, mode, access, *pa, width)) {
        cpu_trap(cpu, access_fault(access), va);
        return false;
    }
    return true;
}

bool cpu_zero_block(Cpu *cpu, Bus *bus, uint64_t address)
{
    if ((cpu->privilege != MACHINE && !(cpu->menvcfg & CBZE)) ||
        (cpu->privilege == USER && !(cpu->senvcfg & CBZE))) {
        return false; // 译码器报告非法指令，先于地址翻译。
    }
    uint64_t base;
    if (!cpu_translate(cpu, bus, Access::ZeroBlock, address, BLOCK_BYTES, &base)) {
        return true;
    }
    if (!bus_is_ram(bus, base, BLOCK_BYTES)) {
        cpu_trap(cpu, STORE_FAULT, address);
        return true;
    }
    // 整块已通过 PMP 和 RAM 边界检查。走总线的普通写入口会使重叠的 LR 保留失效。
    for (unsigned offset = 0; offset < BLOCK_BYTES; offset += 8) {
        bus_store(bus, base + offset, 8, 0);
    }
    return true;
}

bool cpu_memory(Cpu *cpu, Bus *bus, Access access, uint64_t address, unsigned width, uint64_t *value)
{
    if ((address & (width - 1)) == 0) {
        uint64_t pa = 0;
        if (!cpu_translate(cpu, bus, access, address, width, &pa)) {
            return false;
        }
        const bool ok = access == Access::Store ? bus_store(bus, pa, width, *value)
                                                : bus_load(bus, pa, width, value);
        if (!ok) {
            cpu_trap(cpu, access_fault(access), address);
        }
        return ok;
    }
    if (access == Access::Fetch) {
        cpu_trap(cpu, FETCH_MISALIGNED, address);
        return false;
    }
    // 普通 RAM 支持错位访问，且一条访问可覆盖两个不相邻的物理页。
    // 先核对所有字节，再写入。MMIO 有读写副作用，不可拆成多次设备操作。
    uint64_t physical[8]{};
    for (unsigned i = 0; i < width; ++i) {
        if (!cpu_translate(cpu, bus, access, address + i, 1, &physical[i])) {
            return false;
        }
        if (!bus_is_ram(bus, physical[i], 1)) {
            cpu_trap(cpu, access == Access::Load ? LOAD_MISALIGNED : STORE_MISALIGNED, address);
            return false;
        }
    }
    uint64_t loaded = 0;
    for (unsigned i = 0; i < width; ++i) {
        uint64_t byte = 0;
        const bool ok = access == Access::Store ? bus_store(bus, physical[i], 1, (*value >> (i * 8)) & 0xff)
                                                : bus_load(bus, physical[i], 1, &byte);
        if (!ok) {
            cpu_trap(cpu, access_fault(access), address + i);
            return false;
        }
        loaded |= byte << (i * 8);
    }
    if (access == Access::Load) {
        *value = loaded;
    }
    return true;
}
