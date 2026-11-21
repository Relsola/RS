#include "cpu_privilege.h"
#include "cpu_integer.h"
#include "reservation.h"

#include <bit>

enum AtomicFunction : unsigned
{
    ADD = 0, SWAP = 1, LR = 2, SC = 3, XOR = 4, OR = 8, AND = 12,
    MIN = 16, MAX = 20, MINU = 24, MAXU = 28
};

static uint64_t amo_result(unsigned function, unsigned width, uint64_t old, uint64_t operand)
{
    if (width == 4) {
        operand = static_cast<uint32_t>(operand);
    }
    switch (function) {
    case ADD:  return old + operand;
    case SWAP: return operand;
    case XOR:  return old ^ operand;
    case OR:   return old | operand;
    case AND:  return old & operand;
    case MINU: return old < operand ? old : operand;
    case MAXU: return old > operand ? old : operand;
    default: { // MIN / MAX：先按运算宽度解释符号，再比较。
        const int64_t left = std::bit_cast<int64_t>(width == 4 ? sign_extend(old, 32) : old);
        const int64_t right = std::bit_cast<int64_t>(width == 4 ? sign_extend(operand, 32) : operand);
        const bool keep_old = function == MIN ? left < right : left > right;
        return keep_old ? old : operand;
    }
    }
}

bool cpu_atomic(Cpu *cpu, Bus *bus, uint32_t insn, uint64_t address, uint64_t operand, uint64_t *rd)
{
    const unsigned size_code = (insn >> 12) & 7;
    const unsigned function = insn >> 27;
    // A 2.1 的 funct5 除了 SWAP、LR、SC，其余均为 0..28 中 4 的倍数。
    // aq/rl 均可执行：单 hart 按序执行，每条指令和本板的 DMA 在返回前完成。
    if ((size_code != 2 && size_code != 3) || (function > SC && (function & 3) != 0) ||
        (function == LR && ((insn >> 20) & 31) != 0)) {
        return false;
    }
    const unsigned width = 1u << size_code;
    const bool load = function == LR;
    Reservation *reserved = bus_reservation(bus);
    Reservation previous{};
    if (function == SC) {
        // 是否仍有保留与是否能写这个地址是两个检查。即使失败的 SC 也要检查存储权限。
        previous = *reserved;
        reserved->width = 0;
    }
    if ((address & (width - 1)) != 0) {
        cpu_trap(cpu, load ? LOAD_MISALIGNED : STORE_MISALIGNED, address);
        return true;
    }
    uint64_t physical = 0;
    const Access access = load ? Access::Load : (function == SC ? Access::Store : Access::Amo);
    if (!cpu_translate(cpu, bus, access, address, width, &physical)) {
        return true;
    }
    if (!bus_is_ram(bus, physical, width)) {
        cpu_trap(cpu, load ? LOAD_FAULT : STORE_FAULT, address);
        return true;
    }
    if (function == SC) {
        const bool covered = previous.width >= width && physical >= previous.address &&
                             physical - previous.address <= previous.width - width;
        if (covered && !bus_store(bus, physical, width, operand)) {
            cpu_trap(cpu, STORE_FAULT, address);
            return true;
        }
        *rd = covered ? 0 : 1;
        return true;
    }
    uint64_t old = 0;
    if (!bus_load(bus, physical, width, &old)) {
        cpu_trap(cpu, load ? LOAD_FAULT : STORE_FAULT, address);
        return true;
    }
    if (load) {
        *reserved = {physical, width};
    } else if (!bus_store(bus, physical, width, amo_result(function, width, old, operand))) {
        cpu_trap(cpu, STORE_FAULT, address);
        return true;
    }
    *rd = width == 4 ? sign_extend(old, 32) : old;
    return true;
}
