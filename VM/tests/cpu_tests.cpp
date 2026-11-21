#include "../cpu.h"
#include "../reservation.h"

#include <array>
#include <cstdio>
#include <cstdlib>

struct Bus
{
    std::array<uint8_t, 65536> ram;
    unsigned loads;
    Reservation reservation;
};

Reservation *bus_reservation(Bus *bus)
{
    return &bus->reservation;
}

bool bus_is_ram(const Bus *bus, uint64_t address, unsigned width)
{
    return address <= bus->ram.size() - width;
}

bool bus_load(Bus *bus, uint64_t address, unsigned width, uint64_t *value)
{
    if (address > bus->ram.size() - width) {
        return false;
    }
    ++bus->loads;
    *value = 0;
    for (unsigned i = 0; i < width; ++i) {
        *value |= uint64_t{bus->ram[address + i]} << (8 * i);
    }
    return true;
}

bool bus_store(Bus *bus, uint64_t address, unsigned width, uint64_t value)
{
    if (address > bus->ram.size() - width) {
        return false;
    }
    for (unsigned i = 0; i < width; ++i) {
        bus->ram[address + i] = static_cast<uint8_t>(value >> (8 * i));
    }
    reservation_after_write(&bus->reservation, address, width);
    return true;
}

static void check(bool condition, const char *description)
{
    if (!condition) {
        std::fprintf(stderr, "CPU check failed: %s\n", description);
        std::exit(1);
    }
}

static void run_one(Cpu *cpu, Bus *bus, uint32_t instruction)
{
    check(bus_store(bus, cpu->pc, 4, instruction), "install test instruction");
    cpu_step(cpu, bus);
}

static void test_alu()
{
    // 每组输入放在 x1、x2，结果写入 x3。
    constexpr uint32_t registers = (2u << 20) | (1u << 15) | (3u << 7);
    struct Case {
        uint32_t opcode, funct3, funct7;
        uint64_t a, b, expected;
        const char *name;
    };
    constexpr uint64_t min_i64 = uint64_t{1} << 63;
    const Case cases[] = {
        {0x33, 0, 0, UINT64_MAX, 1, 0, "ADD wraps"},
        {0x33, 0, 0x20, 0, 1, UINT64_MAX, "SUB wraps"},
        {0x33, 1, 0, 1, 127, min_i64, "SLL masks shift count"},
        {0x33, 2, 0, min_i64, 0, 1, "SLT signed"},
        {0x33, 3, 0, min_i64, 0, 0, "SLTU unsigned"},
        {0x33, 5, 0x20, min_i64, 63, UINT64_MAX, "SRA fills the sign"},
        {0x33, 1, 1, UINT64_MAX, UINT64_MAX, 0, "MULH negative times negative"},
        {0x33, 2, 1, UINT64_MAX, UINT64_MAX, UINT64_MAX, "MULHSU mixed signs"},
        {0x33, 3, 1, UINT64_MAX, UINT64_MAX, UINT64_MAX - 1, "MULHU carries"},
        {0x33, 3, 1, 0x0123456789abcdef, 0xfedcba9876543210,
                      0x0121fa00ad77d742, "MULHU unequal halves"},
        {0x33, 4, 1, min_i64, UINT64_MAX, min_i64, "DIV signed overflow"},
        {0x33, 6, 1, min_i64, UINT64_MAX, 0, "REM signed overflow"},
        {0x33, 4, 1, 33, 0, UINT64_MAX, "DIV zero"},
        {0x33, 7, 1, 33, 0, 33, "REMU zero"},
        {0x3b, 0, 0, 0x7fffffff, 1, 0xffffffff80000000, "ADDW sign extends"},
        {0x3b, 5, 0, UINT64_MAX, 1, 0x7fffffff, "SRLW clears upper input"},
        {0x3b, 5, 0x20, 0x80000000, 31, UINT64_MAX, "SRAW uses bit 31"},
        {0x3b, 5, 1, 0x80000000, 1, 0xffffffff80000000, "DIVUW sign extends"},
        {0x3b, 4, 1, 0x80000000, 0xffffffff, 0xffffffff80000000, "DIVW overflow"},
    };
    for (const Case &t : cases) {
        Cpu cpu{};
        Bus bus{};
        cpu.x[1] = t.a;
        cpu.x[2] = t.b;
        run_one(&cpu, &bus, (t.funct7 << 25) | registers | (t.funct3 << 12) | t.opcode);
        check(!cpu.trapped && cpu.x[3] == t.expected && cpu.minstret == 1, t.name);
    }
}

static void test_load_and_branch()
{
    Cpu cpu{};
    Bus bus{};
    cpu.x[1] = 0x100;
    check(bus_store(&bus, 0x100, 1, 0x81), "install byte");
    run_one(&cpu, &bus, 0x00008103); // lb x2, 0(x1)
    check(cpu.x[2] == UINT64_MAX - 126, "LB sign extends");
    run_one(&cpu, &bus, 0x0000c003); // lbu x0, 0(x1)
    check(cpu.x[0] == 0 && bus.loads == 6, "load into x0 still accesses the bus");
    run_one(&cpu, &bus, 0xfe000ee3); // beq x0, x0, -4
    check(cpu.pc == 4, "negative B immediate");
    run_one(&cpu, &bus, 0x002000ef); // jal x1, +2
    check(!cpu.trapped && cpu.pc == 6 && cpu.x[1] == 8, "JAL supports halfword-aligned target");

    cpu.pc = 8;
    run_one(&cpu, &bus, 0x00102063); // reserved branch funct3
    check(cpu.trapped && cpu.mcause == 2, "reserved branch encoding");
    cpu.x[1] = 3;
    run_one(&cpu, &bus, 0x0000a103); // lw x2,0(x1)
    check(!cpu.trapped, "misaligned RAM word load");
    cpu.x[1] = bus.ram.size();
    run_one(&cpu, &bus, 0x0000b023); // sd x0,0(x1)
    check(cpu.trapped && cpu.mcause == 7 && cpu.mtval == bus.ram.size(), "store access fault");
}

static void test_zcb_memory()
{
    Bus bus{};
    Cpu cpu{};
    cpu.x[11] = 0x100;
    cpu.x[10] = 0xabcdef01234580da;
    check(bus_store(&bus, 0x100, 8, UINT64_MAX), "initialize surrounding bytes");
    run_one(&cpu, &bus, 0x8da8); // c.sh a0, 2(a1)
    run_one(&cpu, &bus, 0x89c8); // c.sb a0, 1(a1)
    uint64_t stored = 0;
    check(bus_load(&bus, 0x100, 8, &stored) && stored == 0xffffffff80dadaff, "Zcb exact stores");
    run_one(&cpu, &bus, 0x85a8); // c.lhu a0, 2(a1)
    check(cpu.x[10] == 0x80da, "C.LHU zero extends");
    run_one(&cpu, &bus, 0x85e8); // c.lh a0, 2(a1)
    check(cpu.x[10] == 0xffffffffffff80da, "C.LH sign extends");
    run_one(&cpu, &bus, 0x81e8); // c.lbu a0, 3(a1)
    check(cpu.x[10] == 0x80 && cpu.pc == 10 && cpu.minstret == 5, "C.LBU retires one halfword");
    cpu.x[11] = bus.ram.size();
    run_one(&cpu, &bus, 0x8da8);
    check(cpu.trapped && cpu.mcause == 7 && cpu.mtval == bus.ram.size() + 2 &&
          cpu.mepc == 10 && cpu.minstret == 5, "C.SH has precise store access fault");
}

static void test_zicond()
{
    Cpu cpu{};
    Bus bus{};
    const uint64_t high = uint64_t{1} << 63;
    cpu.x[10] = high;
    cpu.x[11] = high + 9;
    run_one(&cpu, &bus, 0x0ea5d533); // czero.eqz a0,a1,a0，条件只有最高位为 1。
    check(cpu.x[10] == high + 9, "CZERO.EQZ preserves rs1 for nonzero rs2 alias");
    run_one(&cpu, &bus, 0x0eb57533); // czero.nez a0,a0,a1
    check(cpu.x[10] == 0, "CZERO.NEZ clears rs1 alias for nonzero rs2");
    run_one(&cpu, &bus, 0x0ea5d533);
    check(cpu.x[10] == 0, "CZERO.EQZ clears value for zero rs2");
    cpu.x[10] = high;
    cpu.x[11] = 0;
    run_one(&cpu, &bus, 0x0eb57533);
    check(cpu.x[10] == high && cpu.pc == 16 && cpu.minstret == 4, "CZERO.NEZ preserves full XLEN");

    for (uint32_t encoding : {0x0ea5c533u, 0x0ea5d53bu}) { // 未分配的 funct3 及不存在的 W 形式。
        cpu.pc = 16;
        run_one(&cpu, &bus, encoding);
        check(cpu.trapped && cpu.mcause == 2 && cpu.mepc == 16 && cpu.mtval == encoding &&
              cpu.x[10] == high && cpu.minstret == 4, "reserved Zicond encoding is illegal");
    }
}

static void test_csr_and_trap()
{
    Cpu cpu{};
    Bus bus{};
    cpu.x[1] = 0x80;
    run_one(&cpu, &bus, 0x30509073); // csrw mtvec,x1
    check(cpu.mtvec == 0x80, "install trap vector");
    run_one(&cpu, &bus, 0x30046073); // csrsi mstatus,8
    run_one(&cpu, &bus, 0xf1402173); // csrr x2,mhartid
    check(!cpu.trapped && cpu.x[2] == 0, "read-only CSR via CSRRS with x0");
    cpu.x[1] = 0;
    run_one(&cpu, &bus, 0xf140a173); // csrrs x2,mhartid,x1
    check(cpu.trapped && cpu.pc == 0x80 && cpu.mcause == 2 && cpu.mepc == 12 &&
          cpu.mtval == 0xf140a173 && cpu.minstret == 3 && cpu.mstatus == 0x1880,
          "CSRRS with zero-valued x1 still attempts to write");
    cpu.x[1] = 16;
    run_one(&cpu, &bus, 0x34109073); // csrw mepc,x1
    run_one(&cpu, &bus, 0x30200073); // mret
    check(!cpu.trapped && cpu.pc == 16 && cpu.mstatus == 0x88, "MRET restores MIE");
    cpu.x[1] = 90;
    run_one(&cpu, &bus, 0xb0209073); // csrw minstret,x1
    check(cpu.minstret == 90, "explicit minstret write supersedes retirement");
    run_one(&cpu, &bus, 0x00000073); // ecall
    check(cpu.mcause == 11 && cpu.mepc == 20 && cpu.minstret == 90, "M-mode ECALL");
    run_one(&cpu, &bus, 0x0000100f); // fence.i
    check(!cpu.trapped, "FENCE.I on an uncached interpreter");
    run_one(&cpu, &bus, 0x0200101b); // slliw with reserved bit 25
    check(cpu.trapped && cpu.mcause == 2, "reserved W shift traps");
}

static void test_privilege()
{
    Cpu cpu{};
    Bus bus{};
    cpu.x[1] = UINT64_MAX;
    run_one(&cpu, &bus, 0x3b009073); // csrw pmpaddr0,x1
    cpu.x[1] = 0x0f; // TOR, R/W/X
    run_one(&cpu, &bus, 0x3a009073);
    cpu.x[1] = 0x100;
    run_one(&cpu, &bus, 0x30209073); // medeleg: guest delegates U ECALL
    run_one(&cpu, &bus, 0x10509073); // stvec=0x100
    cpu.x[1] = 0x80;
    run_one(&cpu, &bus, 0x34109073); // mepc=0x80; MPP=U after reset
    run_one(&cpu, &bus, 0x30200073);
    check(cpu.privilege == USER && cpu.pc == 0x80, "MRET enters U");
    run_one(&cpu, &bus, 0x00000073);
    check(cpu.privilege == SUPERVISOR && cpu.scause == 8 && cpu.sepc == 0x80 &&
          cpu.pc == 0x100 && cpu.mcause == 0, "delegate U ECALL to S");
    cpu.x[1] = cpu.sepc + 4;
    run_one(&cpu, &bus, 0x14109073); // csrw sepc,x1
    run_one(&cpu, &bus, 0x10200073); // sret
    check(cpu.privilege == USER && cpu.pc == 0x84, "SRET returns to U");
    run_one(&cpu, &bus, 0x300020f3); // csrr x1,mstatus; inaccessible from U
    check(cpu.privilege == MACHINE && cpu.mcause == 2 && cpu.mepc == 0x84, "privileged CSR denied");
}

static void test_paging()
{
    Cpu cpu{};
    Bus bus{};
    // 根页表 0x1000 -> 二级 0x2000 -> 末级 0x3000。VA 0 -> PA 0x4000。
    // 地址 0x1fff 的非对齐读取跨到第二张物理数据页，页间有间隙。
    check(bus_store(&bus, 0x1000, 8, (2u << 10) | 1), "root PTE");
    check(bus_store(&bus, 0x2000, 8, (3u << 10) | 1), "middle PTE");
    check(bus_store(&bus, 0x3000, 8, (4u << 10) | 0x4b), "code PTE V/R/X/A");
    check(bus_store(&bus, 0x3008, 8, (5u << 10) | 0xc7), "data PTE V/R/W/A/D");
    check(bus_store(&bus, 0x3010, 8, (7u << 10) | 0xc7), "second data PTE");
    check(bus_store(&bus, 0x5fff, 1, 0x78) && bus_store(&bus, 0x7000, 1, 0x56), "data");
    check(bus_store(&bus, 0x4000, 4, 0x0000d103), "lhu x2,0(x1)");
    check(bus_store(&bus, 0x4004, 4, 0x00209023), "sh x2,0(x1)");
    cpu.pmpcfg[0] = 0x0f;
    cpu.pmpaddr[0] = 65536 >> 2;
    cpu.satp = (uint64_t{8} << 60) | 1;
    cpu.privilege = SUPERVISOR;
    cpu.x[1] = 0x1fff;
    cpu_step(&cpu, &bus);
    check(!cpu.trapped && cpu.x[2] == 0x5678 && cpu.pc == 4, "cross-page misaligned LHU");
    check(bus_store(&bus, 0x3010, 8, (7u << 10) | 0x47), "clear second page dirty bit");
    cpu.x[2] = 0xdead;
    cpu_step(&cpu, &bus);
    check(cpu.trapped && cpu.mcause == 15 && cpu.mtval == 0x2000 && bus.ram[0x5fff] == 0x78,
          "Svade reports faulting VA without partial write");
    cpu.pc = 0;
    cpu.privilege = SUPERVISOR;
    cpu.pmpcfg[0] = 0;
    cpu_step(&cpu, &bus);
    check(cpu.trapped && cpu.mcause == 1, "PMP blocks implicit PTE fetch as instruction access fault");
    cpu.pmpcfg[0] = 0x0f;
    cpu.privilege = SUPERVISOR;
    cpu.pc = uint64_t{1} << 38;
    cpu_step(&cpu, &bus);
    check(cpu.trapped && cpu.mcause == 12 && cpu.mtval == (uint64_t{1} << 38), "noncanonical Sv39 VA");
    // 在中间层安装物理页号低 9 位非零的叶子，它不能映射 2 MiB。
    check(bus_store(&bus, 0x2000, 8, (4u << 10) | 0x4b), "misaligned superpage");
    cpu.pc = 0;
    cpu.privilege = SUPERVISOR;
    cpu_step(&cpu, &bus);
    check(cpu.trapped && cpu.mcause == 12, "superpage alignment");
    check(bus_store(&bus, 0x2000, 8, 0xcb), "valid identity superpage");
    check(bus_store(&bus, 0x4000, 4, 0x00100113), "addi x2,x0,1");
    cpu.pc = 0x4000;
    cpu.privilege = SUPERVISOR;
    cpu_step(&cpu, &bus);
    check(!cpu.trapped && cpu.x[2] == 1, "valid 2 MiB superpage");
}

static void test_instruction_fetch()
{
    Cpu cpu{};
    Bus bus{};
    check(bus_store(&bus, 0x1000, 8, (2u << 10) | 1), "root PTE");
    check(bus_store(&bus, 0x2000, 8, (3u << 10) | 1), "middle PTE");
    check(bus_store(&bus, 0x3000, 8, (4u << 10) | 0x4b), "first code PTE");
    // 在第一页末尾放 c.nop。取指不得预先访问第二页。
    check(bus_store(&bus, 0x4ffe, 2, 0x0001), "c.nop");
    cpu.pmpcfg[0] = 0x0f;
    cpu.pmpaddr[0] = 65536 >> 2;
    cpu.satp = (uint64_t{8} << 60) | 1;
    cpu.privilege = SUPERVISOR;
    cpu.pc = 0xffe;
    cpu_step(&cpu, &bus);
    check(!cpu.trapped && cpu.pc == 0x1000, "compressed instruction does not fetch next page");
    check(bus_store(&bus, 0x4ffe, 2, 0x0113), "addi x2,x0,1 first half");
    cpu.pc = 0xffe;
    cpu_step(&cpu, &bus);
    check(cpu.trapped && cpu.mcause == 12 && cpu.mtval == 0x1000 && cpu.mepc == 0xffe,
          "second parcel fault keeps start in EPC and failing VA in TVAL");
    check(bus_store(&bus, 0x3008, 8, (7u << 10) | 0x4b), "second code PTE");
    check(bus_store(&bus, 0x7000, 2, 0x0010), "addi x2,x0,1 second half");
    cpu.pc = cpu.mepc;
    cpu.privilege = SUPERVISOR;
    cpu_step(&cpu, &bus);
    check(!cpu.trapped && cpu.pc == 0x1002 && cpu.x[2] == 1, "32-bit instruction across pages");
    check(bus_store(&bus, 0x7002, 2, 0x9082), "c.jalr x1");
    cpu.x[1] = 0x1002;
    cpu_step(&cpu, &bus);
    check(!cpu.trapped && cpu.pc == 0x1002 && cpu.x[1] == 0x1004,
          "compressed JALR links pc+2 and uses original rs1");
    check(bus_store(&bus, 0x7002, 2, 0x8002), "reserved compressed JR with rs1=x0");
    cpu_step(&cpu, &bus);
    check(cpu.trapped && cpu.mcause == 2 && cpu.mtval == 0x8002, "illegal compressed trap stores raw bits");
}

static void test_user_page_permissions()
{
    Cpu cpu{};
    Bus bus{};
    check(bus_store(&bus, 0x1000, 8, 0x5b), "user V/R/X/A gigapage");
    check(bus_store(&bus, 0, 4, 0x0000c103), "lbu x2,0(x1)");
    cpu.pmpcfg[0] = 0x0f;
    cpu.pmpaddr[0] = 65536 >> 2;
    cpu.satp = (uint64_t{8} << 60) | 1;
    cpu.privilege = SUPERVISOR;
    cpu.mstatus = 1u << 18; // SUM
    cpu_step(&cpu, &bus);
    check(cpu.trapped && cpu.mcause == 12, "SUM never enables S fetch from user page");
    cpu.pc = 0;
    cpu.privilege = USER;
    cpu.x[1] = 0x100;
    check(bus_store(&bus, 0x1000, 8, 0x59), "user execute-only gigapage");
    cpu_step(&cpu, &bus);
    check(cpu.trapped && cpu.mcause == 13, "execute-only data read needs MXR");
    cpu.pc = 0;
    cpu.privilege = USER;
    cpu.mstatus |= 1u << 19; // MXR
    cpu_step(&cpu, &bus);
    check(!cpu.trapped && cpu.pc == 4, "MXR permits read on executable page");
    check(bus_store(&bus, 0x1000, 8, 0x4b), "supervisor page");
    cpu_step(&cpu, &bus);
    check(cpu.trapped && cpu.mcause == 12, "U fetch denied on supervisor page");
}

static void test_pmp()
{
    Cpu cpu{};
    Bus bus{};
    // Entry 0 = [0x100,0x110), NAPOT, R only. Entry 1 = [0,64 KiB), NAPOT, RWX.
    cpu.pmpcfg[0] = 0x19;
    cpu.pmpaddr[0] = (0x100 >> 2) | 1;
    cpu.pmpcfg[1] = 0x1f;
    cpu.pmpaddr[1] = (65536 / 8) - 1;
    cpu.privilege = USER;
    cpu.x[1] = 0x10c;
    run_one(&cpu, &bus, 0x0000a023); // sw x0,0(x1)
    check(cpu.trapped && cpu.mcause == 7 && cpu.mtval == 0x10c, "lower-number NAPOT match wins");
    cpu.pc = 4;
    cpu.x[1] = 0x110;
    cpu.privilege = USER;
    run_one(&cpu, &bus, 0x0000a023);
    check(!cpu.trapped, "NAPOT upper bound excluded");
    // [0x100,0x104) 为 NA4；一次对齐的 8 字节读取不能跨出首个匹配项。
    cpu.pmpcfg[0] = 0x11;
    cpu.pmpaddr[0] = 0x100 >> 2;
    cpu.x[1] = 0x100;
    run_one(&cpu, &bus, 0x0000b103);
    check(cpu.trapped && cpu.mcause == 5, "partial NA4 match denies complete LD");
    // Entry 1 的 TOR 锁必须同时锁住 pmpaddr0。
    cpu.pmpcfg[0] = 0;
    cpu.pmpcfg[1] = 0x89;
    cpu.pmpaddr[1] = 0x200 >> 2;
    cpu.x[1] = 0;
    run_one(&cpu, &bus, 0x3b009073);
    check(cpu.pmpaddr[0] == (0x100 >> 2), "locked TOR protects previous address");
    run_one(&cpu, &bus, 0x3a009073);
    check(cpu.pmpcfg[1] == 0x89, "locked TOR protects own configuration");
    cpu.x[1] = 0x100;
    run_one(&cpu, &bus, 0x0000a023);
    check(cpu.trapped && cpu.mcause == 7, "locked entry also checks M-mode writes");
}

static void write_csr(Cpu *cpu, Bus *bus, unsigned csr, uint64_t value)
{
    cpu->x[1] = value;
    run_one(cpu, bus, (csr << 20) | 0x09073); // csrw csr,x1
}

static uint64_t read_csr(Cpu *cpu, Bus *bus, unsigned csr)
{
    run_one(cpu, bus, (csr << 20) | 0x02173); // csrr x2,csr
    check(!cpu->trapped, "CSR read");
    return cpu->x[2];
}

static void test_timer_csrs()
{
    constexpr uint64_t sti = 1u << 5, stce = uint64_t{1} << 63;
    Cpu cpu{};
    Bus bus{};
    check(read_csr(&cpu, &bus, 0x14d) == UINT64_MAX, "M can read initial stimecmp");
    write_csr(&cpu, &bus, 0x344, sti); // mip.STIP 可由 M 写入（STCE=0）。
    check(read_csr(&cpu, &bus, 0x344) == sti && read_csr(&cpu, &bus, 0x144) == 0,
          "software STIP stays hidden from sip until delegated");
    write_csr(&cpu, &bus, 0x303, UINT64_MAX);
    check(read_csr(&cpu, &bus, 0x303) == sti && read_csr(&cpu, &bus, 0x144) == sti,
          "delegating only the implemented timer exposes sip.STIP");
    write_csr(&cpu, &bus, 0x144, 0);
    check(read_csr(&cpu, &bus, 0x144) == sti, "sip.STIP is always read-only");
    write_csr(&cpu, &bus, 0x344, 0);
    write_csr(&cpu, &bus, 0x30a, UINT64_MAX);
    check(read_csr(&cpu, &bus, 0x30a) == stce, "menvcfg implements STCE");
    write_csr(&cpu, &bus, 0x344, sti);
    check(read_csr(&cpu, &bus, 0x344) == 0, "mip cannot force a future Sstc deadline");
    write_csr(&cpu, &bus, 0x14d, 0);
    write_csr(&cpu, &bus, 0x344, 0);
    check(read_csr(&cpu, &bus, 0x344) == sti, "mip cannot clear an expired Sstc deadline");
    write_csr(&cpu, &bus, 0x104, UINT64_MAX);
    check(read_csr(&cpu, &bus, 0x304) == sti, "sie.STIE updates the underlying mie.STIE");
    write_csr(&cpu, &bus, 0x303, 0);
    write_csr(&cpu, &bus, 0x104, 0);
    check(read_csr(&cpu, &bus, 0x104) == 0 && read_csr(&cpu, &bus, 0x304) == sti,
          "writes to non-delegated sie bits preserve mie");

    // 单独改变特权级和权限，让每个非法访问都确实由访问 stimecmp 引起。
    const struct {
        Privilege from;
        uint64_t menvcfg, mcounteren;
        bool allowed;
    } cases[] = {
        {SUPERVISOR, 0, 2, false}, {SUPERVISOR, stce, 0, false},
        {SUPERVISOR, stce, 2, true}, {USER, stce, 2, false},
        {MACHINE, 0, 0, true}
    };
    for (const auto &test : cases) {
        Cpu probe{};
        probe.privilege = test.from;
        probe.menvcfg = test.menvcfg;
        probe.mcounteren = test.mcounteren;
        probe.pmpcfg[0] = 0xf;
        probe.pmpaddr[0] = bus.ram.size() >> 2;
        write_csr(&probe, &bus, 0x14d, 12345);
        if (test.allowed) {
            check(!probe.trapped && probe.stimecmp == 12345, "permitted stimecmp write");
        } else {
            check(probe.trapped && probe.mcause == 2 && probe.stimecmp == UINT64_MAX,
                  "stimecmp permission failure leaves compare value intact");
        }
    }
}

static void test_timer_delivery()
{
    constexpr uint64_t sti = 1u << 5, stce = uint64_t{1} << 63;
    constexpr uint64_t timer_interrupt = (uint64_t{1} << 63) | 5u;
    const struct {
        Privilege from;
        bool delegated, globally_enabled, delivered;
    } cases[] = {
        {MACHINE, true, true, false}, {MACHINE, false, false, false},
        {MACHINE, false, true, true}, {SUPERVISOR, false, false, true},
        {SUPERVISOR, true, false, false}, {SUPERVISOR, true, true, true},
        {USER, true, false, true}, {USER, false, false, true}
    };
    for (const auto &test : cases) {
        Cpu cpu{};
        Bus bus{};
        cpu.privilege = test.from;
        cpu.menvcfg = stce;
        cpu.mcounteren = 2;
        cpu.stimecmp = 1; // 第一次 step 时恰好与 time 相等。
        cpu.mideleg = test.delegated ? sti : 0;
        cpu.mie = sti;
        cpu.mstatus = test.globally_enabled ? (1u << 3) | (1u << 1) : 0;
        cpu.pmpcfg[0] = 0xf;
        cpu.pmpaddr[0] = bus.ram.size() >> 2;
        cpu.mtvec = 0x100;
        cpu.stvec = 0x200;
        cpu.pc = 0x42;
        run_one(&cpu, &bus, 0x00118193); // addi x3,x3,1
        if (!test.delivered) {
            check(!cpu.trapped && cpu.x[3] == 1 && cpu.minstret == 1 && cpu.pc == 0x46,
                  "masked timer leaves normal execution intact");
            continue;
        }
        check(cpu.trapped && cpu.minstret == 0 && cpu.x[3] == 0 && bus.loads == 0,
              "interrupt arrives before fetch and retirement");
        if (test.delegated) {
            check(cpu.privilege == SUPERVISOR && cpu.scause == timer_interrupt && cpu.stval == 0 &&
                  cpu.sepc == 0x42 && cpu.pc == cpu.stvec && !(cpu.mstatus & (1u << 1)),
                  "delegated timer saves S trap state");
        } else {
            check(cpu.privilege == MACHINE && cpu.mcause == timer_interrupt && cpu.mtval == 0 &&
                  cpu.mepc == 0x42 && cpu.pc == cpu.mtvec && !(cpu.mstatus & (1u << 3)),
                  "non-delegated timer saves M trap state");
        }
        write_csr(&cpu, &bus, 0x14d, UINT64_MAX);
        run_one(&cpu, &bus, test.delegated ? 0x10200073 : 0x30200073); // SRET / MRET
        check(cpu.privilege == test.from && cpu.pc == 0x42, "xret resumes the interrupted PC");
        cpu_step(&cpu, &bus);
        check(!cpu.trapped && cpu.x[3] == 1 && cpu.minstret == 3, "rearmed timer allows resumption");
    }
}

static void test_timer_retrigger()
{
    Cpu cpu{};
    Bus bus{};
    cpu.menvcfg = uint64_t{1} << 63;
    cpu.stimecmp = 2;
    cpu.mstatus = 1u << 3; // MIE
    cpu.pc = 0x40;
    cpu.mtvec = 0x100;
    run_one(&cpu, &bus, 0x13); // time=1, deadline in future
    run_one(&cpu, &bus, 0x13); // time=2, pending but STIE=0
    check(cpu.pc == 0x48 && cpu.minstret == 2, "local STIE masks a pending timer");
    write_csr(&cpu, &bus, 0x304, 1u << 5);
    cpu_step(&cpu, &bus);
    check(cpu.trapped && cpu.mepc == 0x4c && cpu.minstret == 3, "enabling STIE delivers pending timer");
    run_one(&cpu, &bus, 0x30200073); // 未推迟比较值即返回，STIP 仍为 1。
    cpu_step(&cpu, &bus);
    check(cpu.trapped && cpu.mepc == 0x4c && cpu.minstret == 4, "level timer retriggers after MRET");
}

static void test_svinval()
{
    Bus bus{};
    auto probe = [&](Privilege mode, bool tvm, uint32_t word, bool allowed) {
        Cpu cpu{};
        cpu.pmpcfg[0] = 0x0f;
        cpu.pmpaddr[0] = bus.ram.size() >> 2;
        cpu.privilege = mode;
        cpu.mstatus = tvm ? (1u << 20) : 0;
        cpu.pc = 0x40;
        // rs1 甚至可以持有非规范地址，SINVAL.VMA 不会用它发起显式访存。
        cpu.x[1] = uint64_t{1} << 38;
        cpu.x[2] = 7;
        run_one(&cpu, &bus, word);
        if (allowed) {
            check(!cpu.trapped && cpu.pc == 0x44 && cpu.minstret == 1, "Svinval retires");
        } else {
            check(cpu.trapped && cpu.mcause == 2 && cpu.mtval == word &&
                  cpu.mepc == 0x40 && cpu.minstret == 0, "Svinval precise illegal instruction");
        }
    };
    constexpr uint32_t sinval = 0x16000073, before = 0x18000073, after = 0x18100073;
    for (uint32_t word : {sinval, sinval | (1u << 15), sinval | (2u << 20),
                          sinval | (1u << 15) | (2u << 20), 0x12000073u /* SFENCE.VMA */}) {
        probe(MACHINE, true, word, true);
        probe(SUPERVISOR, false, word, true);
        probe(SUPERVISOR, true, word, false);
        probe(USER, false, word, false);
    }
    for (uint32_t word : {before, after}) {
        probe(MACHINE, true, word, true);
        probe(SUPERVISOR, false, word, true);
        probe(SUPERVISOR, true, word, true);
        probe(USER, false, word, false);
    }
    // 固定的 rd、rs1、rs2 字段参与译码。HINVAL 的两种编码需要 H 扩展。
    for (uint32_t word : {sinval | (1u << 7), before | (1u << 15), before | (2u << 20),
                          after | (1u << 7), 0x26000073u, 0x66000073u}) {
        probe(MACHINE, false, word, false);
    }
}

// S 态切换代码在两套页表中指向同一物理页，用户代码和数据则故意映射到不同页。
static void test_satp_switch()
{
    Cpu cpu{};
    Bus bus{};
    constexpr uint64_t s_code = 0x7000;
    constexpr uint64_t user_rw = 0xd7, user_rx = 0x5b, supervisor_rx = 0x4b;
    auto pte = [&](uint64_t slot, uint64_t physical, uint64_t flags) {
        check(bus_store(&bus, slot, 8, ((physical >> 12) << 10) | flags), "install task PTE");
    };
    auto instruction = [&](uint64_t address, uint32_t word) {
        check(bus_store(&bus, address, 4, word), "install task instruction");
    };
    const uint64_t roots[] = {0x1000, 0x4000};
    for (unsigned i = 0; i < 2; ++i) {
        const uint64_t root = roots[i], leaf = root + 0x2000;
        const uint64_t data = 0x8000 + i * 0x1000, code = 0xa000 + i * 0x1000;
        pte(root, root + 0x1000, 1);
        pte(root + 0x1000, leaf, 1);
        pte(leaf, s_code, supervisor_rx);
        pte(leaf + 8, data, user_rw);
        pte(leaf + 16, code, user_rx);
        pte(leaf + 24, 0xc000, i == 0 ? 0x53 : 0x43); // U 可在 A 中读，B 中同页仅 S 可读。
        check(bus_store(&bus, data, 4, i == 0 ? 10 : 20), "initialize private data");
        instruction(code, 0x00012303);                             // lw x6,0(x2)
        instruction(code + 4, i == 0 ? 0x00130313 : 0x00230313);  // addi x6,x6,1 或 2
        instruction(code + 8, 0x00612023);                         // sw x6,0(x2)
        instruction(code + 12, 0x00000073);                        // ecall
        instruction(code + 16, 0x00022303);                        // lw x6,0(x4)
        instruction(code + 20, 0x00000073);                        // ecall
    }
    instruction(s_code, 0x18009073);      // csrw satp,x1
    instruction(s_code + 4, 0x12000073);  // sfence.vma x0,x0
    instruction(s_code + 8, 0x14119073);  // csrw sepc,x3
    instruction(s_code + 12, 0x10200073); // sret
    cpu.pmpcfg[0] = 0x0f;
    cpu.pmpaddr[0] = bus.ram.size() >> 2;
    cpu.medeleg = (1u << 8) | (1u << 13);
    cpu.privilege = SUPERVISOR;
    cpu.satp = (uint64_t{8} << 60) | (roots[0] >> 12);
    cpu.x[2] = 0x1000;
    cpu.x[4] = 0x3000;
    auto enter = [&](unsigned slot, uint64_t user_pc) {
        check(cpu.privilege == SUPERVISOR && cpu.pc == 0, "at shared S trampoline");
        cpu.x[1] = (uint64_t{8} << 60) | (roots[slot] >> 12);
        cpu.x[3] = user_pc;
        for (int i = 0; i < 4; ++i) {
            cpu_step(&cpu, &bus);
        }
        check(cpu.privilege == USER && cpu.pc == user_pc, "resume user in selected address space");
    };
    for (unsigned i = 0; i < 2; ++i) {
        enter(i, 0x2000);
        for (int step = 0; step < 4; ++step) {
            cpu_step(&cpu, &bus);
        }
        check(cpu.scause == 8 && cpu.sepc == 0x200c, "user ECALL after private store");
    }
    uint64_t a = 0, b = 0;
    check(bus_load(&bus, 0x8000, 4, &a) && a == 11 &&
          bus_load(&bus, 0x9000, 4, &b) && b == 22, "same VA uses distinct instructions and data");
    enter(0, 0x2010);
    cpu_step(&cpu, &bus);
    check(cpu.privilege == USER && cpu.pc == 0x2014, "A can read the additional page");
    cpu_step(&cpu, &bus);
    enter(1, 0x2010);
    cpu_step(&cpu, &bus);
    check(cpu.privilege == SUPERVISOR && cpu.scause == 13 && cpu.stval == 0x3000 &&
          cpu.sepc == 0x2010, "B observes its own user permission after SATP switch");
}

int main()
{
    test_svinval();
    test_satp_switch();
    test_alu();
    test_load_and_branch();
    test_zcb_memory();
    test_zicond();
    test_csr_and_trap();
    test_privilege();
    test_paging();
    test_instruction_fetch();
    test_user_page_permissions();
    test_pmp();
    test_timer_csrs();
    test_timer_delivery();
    test_timer_retrigger();
    std::puts("CPU checks passed");
}
