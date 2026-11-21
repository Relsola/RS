#include "bus.h"
#include "cpu_privilege.h"

#include <cstdio>
#include <cstdlib>

static constexpr uint64_t ROOT = RAM_BASE + 0x1000, MIDDLE = ROOT + 0x1000, LEAF = MIDDLE + 0x1000;
static constexpr uint64_t VA = 0x40010000, PA = RAM_BASE + 0x30000;
static constexpr uint64_t N = uint64_t{1} << 63, PPN_FIELD = (PA >> 12) << 10;
static constexpr uint64_t NAPOT_PPN = N | PPN_FIELD | (8u << 10);
static constexpr uint64_t USER_RX = 0x5b, USER_RW = 0xd7;

static void check(bool ok, const char *what)
{
    if (!ok) {
        std::fprintf(stderr, "Svnapot check failed: %s\n", what);
        std::exit(1);
    }
}

static void fill_leaves(Bus *bus, uint64_t pte)
{
    // VA 的低 21 位为 0x10000，因此这一组从末级页表的第 16 项开始。
    for (unsigned i = 16; i < 32; ++i) {
        check(bus_store(bus, LEAF + i * 8, 8, pte), "install contiguous PTE");
    }
}

static void expect_fault(Cpu cpu, Bus *bus, Access access, uint64_t address, TrapCause cause)
{
    uint64_t physical;
    check(!cpu_translate(&cpu, bus, access, address, 4, &physical) &&
          cpu.trapped && cpu.scause == cause && cpu.stval == address && cpu.sepc == VA,
          "delegated fault cause and virtual address");
}

static void test_range(Cpu cpu, Bus *bus)
{
    fill_leaves(bus, NAPOT_PPN | USER_RX);
    for (unsigned i = 0; i < 16; ++i) {
        uint64_t offset = i * 4096 + 0x34;
        check(bus_store(bus, PA + offset, 4, (i << 20) | 0x513), "install addi a0,zero,i");
        cpu.x[10] = 0xff;
        cpu.pc = VA + offset;
        cpu_step(&cpu, bus);
        check(!cpu.trapped && cpu.pc == VA + offset + 4 && cpu.x[10] == i,
              "each 4 KiB slice fetches its own instruction");
    }

    fill_leaves(bus, NAPOT_PPN | USER_RW);
    uint64_t value = 0x5678;
    check(cpu_memory(&cpu, bus, Access::Store, VA + 0x7fff, 2, &value),
          "store across two slices");
    check(bus_load(bus, PA + 0x7fff, 2, &value) && value == 0x5678,
          "store reaches matching adjacent physical bytes");
    value = 0;
    check(cpu_memory(&cpu, bus, Access::Load, VA + 0x7fff, 2, &value) && value == 0x5678,
          "load across two slices");

    cpu.pc = VA;
    expect_fault(cpu, bus, Access::Load, VA - 4, LOAD_PAGE_FAULT);
    expect_fault(cpu, bus, Access::Load, VA + 0x10000, LOAD_PAGE_FAULT);
    // PMP 的边界位于范围中间，须对替换低四位之后的物理地址检查权限。
    cpu.pmpaddr[0] = (PA + 0xf000) >> 2;
    expect_fault(cpu, bus, Access::Load, VA + 0xf024, LOAD_FAULT);
}

static void test_encodings(Cpu cpu, Bus *bus)
{
    for (unsigned low = 0; low < 16; ++low) {
        if (low == 8) {
            continue;
        }
        fill_leaves(bus, N | PPN_FIELD | (low << 10) | USER_RW);
        expect_fault(cpu, bus, Access::Load, VA, LOAD_PAGE_FAULT);
    }
    // N 为零时，PPN 尾部的 1000 是普通物理页号的一部分。
    fill_leaves(bus, PPN_FIELD | (8u << 10) | USER_RW);
    uint64_t physical;
    check(cpu_translate(&cpu, bus, Access::Store, VA + 0x1234, 4, &physical) &&
          physical == PA + 0x8234, "ordinary 4 KiB mapping with PPN ending in eight");

    for (unsigned bit = 54; bit <= 62; ++bit) {
        fill_leaves(bus, NAPOT_PPN | USER_RW | (uint64_t{1} << bit));
        expect_fault(cpu, bus, Access::Store, VA, STORE_PAGE_FAULT);
    }
    fill_leaves(bus, NAPOT_PPN | USER_RW);
    check(bus_store(bus, MIDDLE, 8, N | (LEAF >> 2) | 1), "N on non-leaf PTE");
    expect_fault(cpu, bus, Access::Load, VA, LOAD_PAGE_FAULT);

    // 普通 2 MiB/1 GiB 页允许的对齐地址，在 N=1 时仍是保留编码。
    for (uint64_t slot : {MIDDLE, ROOT + 8}) {
        check(bus_store(bus, slot, 8, N | (RAM_BASE >> 2) | USER_RX), "N on upper-level leaf");
        expect_fault(cpu, bus, Access::Fetch, VA, FETCH_PAGE_FAULT);
    }
}

static void test_permissions(Cpu cpu, Bus *bus)
{
    fill_leaves(bus, NAPOT_PPN | (USER_RX & ~0x40u)); // A=0
    expect_fault(cpu, bus, Access::Fetch, VA, FETCH_PAGE_FAULT);
    expect_fault(cpu, bus, Access::Load, VA, LOAD_PAGE_FAULT);
    fill_leaves(bus, NAPOT_PPN | (USER_RW & ~0x80u)); // D=0
    expect_fault(cpu, bus, Access::Store, VA, STORE_PAGE_FAULT);
    expect_fault(cpu, bus, Access::Amo, VA, STORE_PAGE_FAULT);
    fill_leaves(bus, NAPOT_PPN | USER_RX);
    expect_fault(cpu, bus, Access::Store, VA, STORE_PAGE_FAULT);
    fill_leaves(bus, NAPOT_PPN | (USER_RW & ~0x10u)); // U=0
    expect_fault(cpu, bus, Access::Load, VA, LOAD_PAGE_FAULT);
}

int main()
{
    Bus bus{};
    bus.ram.resize(0x40000);
    check(bus_store(&bus, ROOT + 8, 8, (MIDDLE >> 2) | 1) &&
          bus_store(&bus, MIDDLE, 8, (LEAF >> 2) | 1), "install Sv39 non-leaf entries");
    Cpu cpu{};
    cpu.pc = VA;
    cpu.privilege = USER;
    cpu.satp = (uint64_t{8} << 60) | (ROOT >> 12);
    cpu.medeleg = (1u << FETCH_PAGE_FAULT) | (1u << LOAD_PAGE_FAULT) |
                  (1u << STORE_PAGE_FAULT) | (1u << LOAD_FAULT);
    cpu.pmpcfg[0] = 0x0f;
    cpu.pmpaddr[0] = (RAM_BASE + bus.ram.size()) >> 2;

    test_range(cpu, &bus);
    test_permissions(cpu, &bus);
    test_encodings(cpu, &bus);
    std::puts("Svnapot 64 KiB translation tests passed");
}
