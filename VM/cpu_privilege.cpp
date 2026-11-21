#include "cpu_privilege.h"

constexpr uint64_t SSTATUS_WRITABLE = SIE | SPIE | SPP | SUM | MXR;
constexpr uint64_t MSTATUS_WRITABLE = SSTATUS_WRITABLE | MIE | MPIE | MPP | MPRV | TVM | TW | TSR;
constexpr uint64_t UXL64 = uint64_t{2} << 32, SXL64 = uint64_t{2} << 34;
constexpr uint64_t PMPADDR_MASK = (uint64_t{1} << 54) - 1;
constexpr unsigned TIMER_CAUSE = 5;
constexpr uint64_t STI = uint64_t{1} << TIMER_CAUSE;
constexpr uint64_t STCE = uint64_t{1} << 63;
constexpr uint64_t TM = 1u << 1;

static void enter_trap(Cpu *cpu, bool delegated, uint64_t cause, uint64_t value)
{
    const uint64_t status = cpu->mstatus;
    if (delegated) {
        cpu->sepc = cpu->pc & ~uint64_t{1};
        cpu->scause = cause;
        cpu->stval = value;
        cpu->mstatus = (status & ~(SIE | SPIE | SPP)) | ((status & SIE) ? SPIE : 0)
                     | (cpu->privilege == SUPERVISOR ? SPP : 0);
        cpu->privilege = SUPERVISOR;
        cpu->pc = cpu->stvec;
    } else {
        cpu->mepc = cpu->pc & ~uint64_t{1};
        cpu->mcause = cause;
        cpu->mtval = value;
        cpu->mstatus = (status & ~(MIE | MPIE | MPP)) | ((status & MIE) ? MPIE : 0)
                     | (uint64_t{cpu->privilege} << 11);
        cpu->privilege = MACHINE;
        cpu->pc = cpu->mtvec;
    }
    cpu->trapped = true;
}

void cpu_trap(Cpu *cpu, TrapCause cause, uint64_t value)
{
    const bool delegated = cpu->privilege != MACHINE && (cpu->medeleg & (uint64_t{1} << cause));
    enter_trap(cpu, delegated, cause, value);
}

static uint64_t timer_pending(const Cpu *cpu)
{
    if (cpu->menvcfg & STCE) {
        return cpu->counter_time >= cpu->stimecmp ? STI : 0;
    }
    return cpu->software_stip;
}

bool cpu_take_timer_interrupt(Cpu *cpu)
{
    if ((cpu->mie & timer_pending(cpu)) == 0) {
        return false;
    }
    const bool delegated = (cpu->mideleg & STI) != 0;
    const Privilege target = delegated ? SUPERVISOR : MACHINE;
    const uint64_t global_enable = delegated ? SIE : MIE;
    // 低特权代码可被更高特权级打断，即使目标级自己的全局使能位为零。
    if (cpu->privilege > target || (cpu->privilege == target && !(cpu->mstatus & global_enable))) {
        return false;
    }
    enter_trap(cpu, delegated, (uint64_t{1} << 63) | TIMER_CAUSE, 0);
    return true;
}

static bool counter_read_allowed(const Cpu *cpu, unsigned csr)
{
    const unsigned bit = 1u << (csr & 3);
    if (cpu->privilege < MACHINE && (cpu->mcounteren & bit) == 0) {
        return false;
    }
    return cpu->privilege != USER || (cpu->scounteren & bit) != 0;
}

static bool read_csr(const Cpu *cpu, unsigned csr, uint64_t *value)
{
    if (csr >= 0x3b0 && csr < 0x3c0) {
        *value = cpu->pmpaddr[csr - 0x3b0];
        return true;
    }
    if (csr == 0x3a0 || csr == 0x3a2) {
        const unsigned first = (csr - 0x3a0) * 4;
        *value = 0;
        for (unsigned i = 0; i < 8; ++i) {
            *value |= uint64_t{cpu->pmpcfg[first + i]} << (i * 8);
        }
        return true;
    }
    switch (csr) {
    case 0x100: *value = (cpu->mstatus & SSTATUS_WRITABLE) | UXL64; break;
    case 0x104: *value = cpu->mie & cpu->mideleg; break; // sie 仅暴露已委托的中断位
    case 0x105: *value = cpu->stvec; break;
    case 0x106: *value = cpu->scounteren; break;
    case 0x140: *value = cpu->sscratch; break;
    case 0x141: *value = cpu->sepc; break;
    case 0x142: *value = cpu->scause; break;
    case 0x143: *value = cpu->stval; break;
    case 0x144: *value = timer_pending(cpu) & cpu->mideleg; break; // sip
    case 0x14d: // stimecmp
        if (cpu->privilege < MACHINE && (!(cpu->menvcfg & STCE) || !(cpu->mcounteren & TM))) {
            return false;
        }
        *value = cpu->stimecmp;
        break;
    case 0x180: *value = cpu->satp; break;
    case 0x300: *value = cpu->mstatus | UXL64 | SXL64; break;
    case 0x301: // misa：RV64、A、B、C、I、M、S、U。Zicsr、Zifencei 无单独位。
        *value = (uint64_t{2} << 62) | (1u << 20) | (1u << 18) | (1u << 12)
               | (1u << 8) | (1u << 2) | (1u << 1) | 1u;
        break;
    case 0x302: *value = cpu->medeleg; break;
    case 0x303: *value = cpu->mideleg; break;
    case 0x304: *value = cpu->mie; break;
    case 0x305: *value = cpu->mtvec; break;
    case 0x306: *value = cpu->mcounteren; break;
    case 0x30a: *value = cpu->menvcfg; break;
    case 0x340: *value = cpu->mscratch; break;
    case 0x341: *value = cpu->mepc; break;
    case 0x342: *value = cpu->mcause; break;
    case 0x343: *value = cpu->mtval; break;
    case 0x344: *value = timer_pending(cpu); break; // mip
    case 0xb00: *value = cpu->mcycle; break;
    case 0xb02: *value = cpu->minstret; break;
    case 0xc00: case 0xc01: case 0xc02:
        if (!counter_read_allowed(cpu, csr)) {
            return false;
        }
        *value = csr == 0xc00 ? cpu->mcycle : (csr == 0xc01 ? cpu->counter_time : cpu->minstret);
        break;
    case 0xf11: case 0xf12: case 0xf13: case 0xf14: case 0xf15: // 机器信息
        *value = 0;
        break;
    default: return false;
    }
    return true;
}

static void write_csr(Cpu *cpu, unsigned csr, uint64_t value)
{
    if (csr >= 0x3b0 && csr < 0x3c0) {
        const unsigned i = csr - 0x3b0;
        // 锁定的 TOR 区间同时锁住前一项的地址字段，它定义了下界。
        const bool next_locked_tor = i < 15 && (cpu->pmpcfg[i + 1] & 0x98) == 0x88;
        if ((cpu->pmpcfg[i] & 0x80) == 0 && !next_locked_tor) {
            cpu->pmpaddr[i] = value & PMPADDR_MASK;
        }
        return;
    }
    if (csr == 0x3a0 || csr == 0x3a2) {
        const unsigned first = (csr - 0x3a0) * 4;
        for (unsigned i = 0; i < 8; ++i) {
            uint8_t config = static_cast<uint8_t>((value >> (8 * i)) & 0x9f);
            if ((config & 3) == 2) {
                config &= 0xfd; // R=0/W=1 为保留组合。
            }
            if ((cpu->pmpcfg[first + i] & 0x80) == 0) {
                cpu->pmpcfg[first + i] = config;
            }
        }
        return;
    }
    switch (csr) {
    case 0x100: cpu->mstatus = (cpu->mstatus & ~SSTATUS_WRITABLE) | (value & SSTATUS_WRITABLE); break;
    case 0x104: cpu->mie = (cpu->mie & ~cpu->mideleg) | (value & cpu->mideleg); break;
    case 0x105: cpu->stvec = value & ~uint64_t{3}; break;
    case 0x106: cpu->scounteren = value & 7; break;
    case 0x140: cpu->sscratch = value; break;
    case 0x141: cpu->sepc = value & ~uint64_t{1}; break;
    case 0x142: cpu->scause = value; break;
    case 0x143: cpu->stval = value; break;
    case 0x144: break; // sip.STIP 只读；当前板上无其它可写的 sip 位。
    case 0x14d: cpu->stimecmp = value; break;
    case 0x180:
        if (value >> 60 == 0) {
            cpu->satp = 0;
        } else if (value >> 60 == 8) {
            cpu->satp = (uint64_t{8} << 60) | (value & PPN_MASK); // ASIDLEN=0
        }
        break; // 写入未实现的 MODE 时，整个 satp 保持原值。
    case 0x300:
        cpu->mstatus = value & MSTATUS_WRITABLE;
        if ((value & MPP) == (uint64_t{2} << 11)) {
            cpu->mstatus &= ~MPP;
        }
        break;
    case 0x302: cpu->medeleg = value & 0xb3ffu; break; // M-mode ECALL 永不委托。
    case 0x303: cpu->mideleg = value & STI; break;
    case 0x304: cpu->mie = value & STI; break;
    case 0x305: cpu->mtvec = value & ~uint64_t{3}; break;
    case 0x306: cpu->mcounteren = value & 7; break;
    case 0x30a: cpu->menvcfg = value & STCE; break;
    case 0x340: cpu->mscratch = value; break;
    case 0x341: cpu->mepc = value & ~uint64_t{1}; break;
    case 0x342: cpu->mcause = value; break;
    case 0x343: cpu->mtval = value; break;
    case 0x344:
        if (!(cpu->menvcfg & STCE)) {
            cpu->software_stip = value & STI;
        }
        break;
    case 0xb00: cpu->mcycle = value; break;
    case 0xb02: cpu->minstret = value; break;
    default: break; // 固定值的 WARL 字段，例如 misa 和本板未接线的中断位。
    }
}

static bool csr_instruction(Cpu *cpu, uint32_t insn, uint64_t *result, bool *wrote_instret)
{
    const unsigned csr = insn >> 20;
    const unsigned function = (insn >> 12) & 7;
    const unsigned operation = function & 3;
    const unsigned source = (insn >> 15) & 31;
    const uint64_t operand = function & 4 ? source : cpu->x[source];
    const bool write = operation == 1 || source != 0;
    if (cpu->privilege < ((csr >> 8) & 3) ||
        (csr == 0x180 && cpu->privilege == SUPERVISOR && (cpu->mstatus & TVM))) {
        return false;
    }
    // CSRRS/CSRRC 的 rs1=x0 仅作读取。判断寄存器编号，不判断寄存器中的值。
    if (operation == 0 || (write && (csr >> 10) == 3) || !read_csr(cpu, csr, result)) {
        return false;
    }
    if (write) {
        uint64_t value = operand;
        if (operation == 2) {
            value = *result | operand;
        } else if (operation == 3) {
            value = *result & ~operand;
        }
        write_csr(cpu, csr, value);
        *wrote_instret = csr == 0xb02;
    }
    return true;
}

bool cpu_system(Cpu *cpu, uint32_t insn, uint64_t *next_pc, uint64_t *result, bool *wrote_instret)
{
    if (((insn >> 12) & 7) != 0) {
        return csr_instruction(cpu, insn, result, wrote_instret);
    }
    // 这两条指令的 rs1、rs2 分别选择虚拟地址和 ASID。本机每次访存都查表。
    const uint32_t fixed_bits = insn & 0xfe007fff;
    if (fixed_bits == 0x12000073 || fixed_bits == 0x16000073) { // SFENCE.VMA, SINVAL.VMA
        return cpu->privilege == MACHINE ||
               (cpu->privilege == SUPERVISOR && (cpu->mstatus & TVM) == 0);
    }
    const uint64_t status = cpu->mstatus;
    switch (insn) {
    case 0x18000073: // SFENCE.W.INVAL
    case 0x18100073: // SFENCE.INVAL.IR
        // 页表写入和隐式查表都已按序完成。两个排序指令在 S 态均不受 TVM 限制。
        return cpu->privilege != USER;
    case 0x00000073:
        cpu_trap(cpu, static_cast<TrapCause>(8 + cpu->privilege), 0);
        return true;
    case 0x00100073:
        cpu_trap(cpu, BREAKPOINT, cpu->pc);
        return true;
    case 0x30200073: // MRET
        if (cpu->privilege != MACHINE) {
            return false;
        }
        cpu->privilege = static_cast<Privilege>((status & MPP) >> 11);
        cpu->mstatus = (status & ~(MPP | MIE)) | MPIE | ((status & MPIE) ? MIE : 0);
        *next_pc = cpu->mepc;
        break;
    case 0x10200073: // SRET
        if (cpu->privilege == USER || (cpu->privilege == SUPERVISOR && (status & TSR))) {
            return false;
        }
        cpu->privilege = status & SPP ? SUPERVISOR : USER;
        cpu->mstatus = (status & ~(SPP | SIE)) | SPIE | ((status & SPIE) ? SIE : 0);
        *next_pc = cpu->sepc;
        break;
    case 0x10500073: // WFI 可立即恢复；TW 允许上级截获。
        return cpu->privilege == MACHINE || (cpu->privilege == SUPERVISOR && (status & TW) == 0);
    default: return false;
    }
    if (cpu->privilege != MACHINE) {
        cpu->mstatus &= ~MPRV;
    }
    return true;
}
