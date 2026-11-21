#pragma once

#include "cpu.h"

enum TrapCause : uint64_t
{
    FETCH_MISALIGNED = 0, FETCH_FAULT = 1, ILLEGAL_INSTRUCTION = 2,
    BREAKPOINT = 3, LOAD_MISALIGNED = 4, LOAD_FAULT = 5,
    STORE_MISALIGNED = 6, STORE_FAULT = 7, USER_ECALL = 8, SUPERVISOR_ECALL = 9,
    MACHINE_ECALL = 11, FETCH_PAGE_FAULT = 12, LOAD_PAGE_FAULT = 13, STORE_PAGE_FAULT = 15
};

constexpr uint64_t SIE = 1u << 1, MIE = 1u << 3, SPIE = 1u << 5, MPIE = 1u << 7;
constexpr uint64_t SPP = 1u << 8, MPP = 3u << 11, MPRV = 1u << 17;
constexpr uint64_t SUM = 1u << 18, MXR = 1u << 19;
constexpr uint64_t FS = 3u << 13, VS = 3u << 9;
constexpr uint64_t TVM = 1u << 20, TW = 1u << 21, TSR = 1u << 22;
constexpr uint64_t PPN_MASK = (uint64_t{1} << 44) - 1;
constexpr uint64_t CBIE_FLUSH = 1u << 4, CBIE_MASK = 3u << 4, CBCFE = 1u << 6, CBZE = 1u << 7;

enum class Access { Fetch, Load, Store, Amo, ZeroBlock, ManageBlock };

void cpu_trap(Cpu *cpu, TrapCause cause, uint64_t value);
bool cpu_take_timer_interrupt(Cpu *cpu);
bool cpu_system(Cpu *cpu, uint32_t insn, uint64_t *next_pc, uint64_t *result, bool *wrote_instret);
bool cpu_memory(Cpu *cpu, Bus *bus, Access access, uint64_t address, unsigned width, uint64_t *value);
bool cpu_translate(Cpu *cpu, Bus *bus, Access access, uint64_t va, unsigned width, uint64_t *pa);
bool cpu_cache_block(Cpu *cpu, Bus *bus, unsigned operation, uint64_t address);
bool cpu_atomic(Cpu *cpu, Bus *bus, uint32_t insn, uint64_t address, uint64_t operand, uint64_t *rd);
bool cpu_float(Cpu *cpu, Bus *bus, uint32_t insn, uint64_t *rd, bool *write_x);
bool cpu_vector(Cpu *cpu, Bus *bus, uint32_t insn, uint64_t *rd, bool *write_x);
