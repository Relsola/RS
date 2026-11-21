#include "bus.h"
#include "cpu.h"

#include <cstdio>
#include <cstdlib>

static constexpr uint64_t CODE = RAM_BASE + 0x100, DATA = RAM_BASE + 0x800;

static void check(bool ok, const char *reason)
{
    if (!ok) {
        std::fprintf(stderr, "Atomic check failed: %s\n", reason);
        std::exit(1);
    }
}

static constexpr uint32_t instruction(unsigned funct5, unsigned width, unsigned rs2 = 2,
                                      unsigned rd = 3, unsigned order = 0)
{
    return (funct5 << 27) | (order << 25) | (rs2 << 20) | (1u << 15) |
           ((width == 4 ? 2u : 3u) << 12) | (rd << 7) | 0x2f;
}

static void step(Cpu *cpu, Bus *bus, uint32_t encoding)
{
    cpu->pc = CODE;
    check(bus_store(bus, CODE, 4, encoding), "write instruction");
    cpu_step(cpu, bus);
}

static uint64_t load(Bus *bus, uint64_t address, unsigned width)
{
    uint64_t value = 0;
    check(bus_load(bus, address, width, &value), "read test memory");
    return value;
}

static void test_amos(Bus *bus)
{
    const struct {
        unsigned function;
        uint32_t expected_w;
        uint64_t expected_d;
    } cases[] = {
        {0x00, 0x80000012, 0x8000000000000012}, // add
        {0x01, 13, 13},                        // swap
        {0x04, 0x80000008, 0x8000000000000008}, // xor
        {0x08, 0x8000000d, 0x800000000000000d}, // or
        {0x0c, 5, 5},                          // and
        {0x10, 0x80000005, 0x8000000000000005}, // signed min
        {0x14, 13, 13},                        // signed max
        {0x18, 13, 13},                        // unsigned min
        {0x1c, 0x80000005, 0x8000000000000005}, // unsigned max
    };
    for (const auto &op : cases) {
        for (unsigned width : {4u, 8u}) {
            for (unsigned order = 0; order < 4; ++order) {
                Cpu cpu{};
                const uint64_t original = width == 4 ? 0x1122334480000005 : 0x8000000000000005;
                check(bus_store(bus, DATA, 8, original), "initialize operand");
                cpu.x[1] = DATA;
                cpu.x[2] = width == 4 ? 0xffffffff0000000d : 13;
                // rd==rs1；仍应向原来的地址写回。
                step(&cpu, bus, instruction(op.function, width, 2, 1, order));
                check(!cpu.trapped && cpu.minstret == 1, "AMO retires for each aq/rl combination");
                check(cpu.x[1] == (width == 4 ? 0xffffffff80000005 : original), "return old value");
                const uint64_t expected = width == 4 ? 0x1122334400000000 | op.expected_w : op.expected_d;
                check(load(bus, DATA, 8) == expected, "AMO result and adjacent bytes");
            }
        }
    }
    Cpu cpu{};
    cpu.x[1] = DATA;
    cpu.x[2] = 1;
    check(bus_store(bus, DATA, 8, UINT64_MAX), "initialize max integer");
    step(&cpu, bus, instruction(0, 8, 2, 0)); // amoadd.d x0,x2,(x1)
    check(!cpu.trapped && cpu.x[0] == 0 && load(bus, DATA, 8) == 0, "AMO rd=x0 still writes");
    step(&cpu, bus, 0x301021f3); // csrr x3,misa
    check(cpu.x[3] & 1, "misa.A");
}

static void test_lr_sc(Bus *bus)
{
    Cpu cpu{};
    cpu.x[1] = DATA;
    check(bus_store(bus, DATA, 8, 0xfeedface80000005), "initialize LR");
    step(&cpu, bus, instruction(2, 4, 0)); // LR.W
    check(!cpu.trapped && cpu.x[3] == 0xffffffff80000005, "LR.W sign extension");
    check(bus_store(bus, DATA + 4, 4, 0x12345678), "write next word");
    cpu.x[2] = 99;
    step(&cpu, bus, instruction(3, 4, 2, 2)); // SC.W rd==rs2
    check(cpu.x[2] == 0 && load(bus, DATA, 8) == 0x1234567800000063, "disjoint store preserves LR");
    step(&cpu, bus, instruction(3, 4));
    check(cpu.x[3] == 1 && load(bus, DATA, 4) == 99, "unpaired SC fails without store");

    step(&cpu, bus, instruction(2, 8, 0, 0)); // LR.D rd==x0
    check(bus_store(bus, DATA + 3, 1, 0), "overlap by one byte, same value");
    step(&cpu, bus, instruction(3, 8));
    check(cpu.x[3] == 1, "even an overlapping store of identical bits invalidates LR");
    step(&cpu, bus, instruction(2, 8, 0));
    cpu.x[1] = DATA + 8;
    step(&cpu, bus, instruction(3, 8));
    check(cpu.x[3] == 1, "SC outside reservation fails");
    cpu.x[1] = DATA;
    step(&cpu, bus, instruction(3, 8));
    check(cpu.x[3] == 1, "failed SC consumes the reservation");

    step(&cpu, bus, instruction(2, 8, 0));
    cpu.x[1] += 4;
    cpu.x[2] = 0xabcd;
    step(&cpu, bus, instruction(3, 4));
    check(cpu.x[3] == 0 && load(bus, DATA + 4, 4) == 0xabcd, "contained SC writes reserved bytes");
    cpu.x[1] = DATA;
    step(&cpu, bus, instruction(2, 8, 0));
    cpu.x[1] += 8;
    step(&cpu, bus, instruction(2, 8, 0));
    cpu.x[1] = DATA;
    step(&cpu, bus, instruction(3, 8));
    check(cpu.x[3] == 1, "a new LR replaces the old one");
}

static void test_faults(Bus *bus)
{
    const struct {
        uint32_t encoding;
        uint64_t address, cause;
    } cases[] = {
        {instruction(2, 8, 0), DATA + 4, 4}, {instruction(3, 8), DATA + 4, 6},
        {instruction(1, 4), DATA + 1, 6}, {instruction(2, 4, 0), VIRTIO_BASE, 5},
        {instruction(3, 4), VIRTIO_BASE, 7}, {instruction(0, 4), VIRTIO_BASE, 7},
        {instruction(1, 4), RESET_PC, 7}, {instruction(2, 8, 1), DATA, 2},
        {instruction(5, 8), DATA, 2}, {instruction(0, 4) ^ (3u << 12), DATA, 2},
    };
    for (const auto &c : cases) {
        Cpu cpu{};
        cpu.mtvec = RAM_BASE + 0x200;
        cpu.x[1] = c.address;
        cpu.x[3] = 0x1234;
        step(&cpu, bus, c.encoding);
        check(cpu.trapped && cpu.mcause == c.cause && cpu.mepc == CODE && cpu.pc == cpu.mtvec &&
              cpu.mtval == (c.cause == 2 ? c.encoding : c.address) && cpu.x[3] == 0x1234 &&
              cpu.minstret == 0, "fault type, trap CSRs and unchanged rd");
    }
}

static void test_retry_loop(Bus *bus)
{
    const uint32_t loop[] = {
        instruction(2, 8, 0, 2, 2), // lr.d.aq x2,(x1)
        0x00110113,                  // addi x2,x2,1
        instruction(3, 8, 2, 3, 1), // sc.d.rl x3,x2,(x1)
        0xfe019ae3,                  // bnez x3,-12
    };
    for (unsigned i = 0; i < 4; ++i) {
        check(bus_store(bus, CODE + 4 * i, 4, loop[i]), "install LR/SC loop");
    }
    Cpu cpu{};
    cpu.pc = CODE;
    cpu.x[1] = DATA;
    check(bus_store(bus, DATA, 8, 10), "initialize LR/SC counter");
    for (unsigned i = 0; i < 8; ++i) {
        if (i == 2) {
            // 模拟设备在第一次 LR 与 SC 之间改写计数器，第二轮必须读到新值。
            check(bus_store(bus, DATA, 8, 20), "inject one intervening write");
        }
        cpu_step(&cpu, bus);
        check(!cpu.trapped, "constrained LR/SC loop executes");
    }
    check(cpu.pc == CODE + sizeof(loop) && cpu.x[3] == 0 && load(bus, DATA, 8) == 21,
          "an undisturbed constrained LR/SC sequence completes after one retry");
}

static void test_physical_alias_and_protection(Bus *bus)
{
    constexpr uint64_t root = RAM_BASE + 0x1000, middle = root + 0x1000, leaf = middle + 0x1000;
    constexpr uint64_t pa = RAM_BASE + 0x4000, va = 0x1000, alias = 0x2000;
    check(bus_store(bus, root, 8, (middle >> 2) | 1) &&
          bus_store(bus, middle, 8, (leaf >> 2) | 1) &&
          bus_store(bus, leaf + 8, 8, (pa >> 2) | 0xc7) &&
          bus_store(bus, leaf + 16, 8, (pa >> 2) | 0xc7), "install Sv39 aliases");
    Cpu cpu{};
    cpu.satp = (uint64_t{8} << 60) | (root >> 12);
    cpu.pmpcfg[0] = 0xf;
    cpu.pmpaddr[0] = (RAM_BASE + bus->ram.size()) >> 2;
    cpu.mstatus = (1u << 17) | (1u << 11); // M instruction fetch, S-mode data access.
    cpu.x[1] = va;
    step(&cpu, bus, instruction(2, 8, 0));
    cpu.x[1] = alias;
    cpu.x[2] = 0x5678;
    step(&cpu, bus, instruction(3, 8));
    check(!cpu.trapped && !cpu.x[3] && load(bus, pa, 8) == 0x5678, "aliased VA covers same reservation");
    check(bus_store(bus, leaf + 16, 8, (pa >> 2) | 0x43), "mark alias read-only");
    for (unsigned function : {0u, 3u}) {
        Cpu probe = cpu;
        probe.x[3] = 42;
        step(&probe, bus, instruction(function, 8));
        check(probe.trapped && probe.mcause == 15 && probe.mtval == alias && probe.x[3] == 42 &&
              load(bus, pa, 8) == 0x5678, "AMO and unpaired SC fault on read-only PTE");
    }
    cpu.satp = 0;
    cpu.x[1] = pa;
    cpu.pmpcfg[0] = 0x09; // TOR, R only
    step(&cpu, bus, instruction(1, 8));
    check(cpu.trapped && cpu.mcause == 7 && cpu.mtval == pa, "PMP on AMO reports store access fault");
}

int main()
{
    Bus bus{};
    bus.ram.resize(0x6000);
    test_amos(&bus);
    test_lr_sc(&bus);
    test_retry_loop(&bus);
    test_faults(&bus);
    test_physical_alias_and_protection(&bus);
    std::puts("RV64 atomic tests passed");
}
