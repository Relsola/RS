#include "bus.h"
#include "cpu_privilege.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

static constexpr uint64_t CODE = RAM_BASE, DATA = RAM_BASE + 0x5000;
static constexpr uint32_t CBO_ZERO_X1 = 0x0040a00f;
static constexpr uint32_t CBO_INVAL_X1 = 0x0000a00f, CBO_CLEAN_X1 = 0x0010a00f, CBO_FLUSH_X1 = 0x0020a00f;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::fprintf(stderr, "Cache block check failed: %s\n", message);
        std::exit(1);
    }
}

static void step(Cpu *cpu, Bus *bus, uint32_t instruction)
{
    check(bus_store(bus, CODE, 4, instruction), "install instruction");
    cpu->pc = CODE;
    cpu_step(cpu, bus);
}

static Cpu enabled(Privilege mode)
{
    Cpu cpu{};
    cpu.privilege = mode;
    cpu.menvcfg = CBZE;
    cpu.senvcfg = CBZE;
    cpu.pmpcfg[0] = 0x0f;
    cpu.pmpaddr[0] = (DATA + 4096) >> 2;
    return cpu;
}

static void test_blocks(Bus *bus)
{
    uint8_t *bytes = ram_bytes(bus, DATA, 3 * 64);
    for (unsigned offset : {0u, 1u, 31u, 63u}) {
        std::fill_n(bytes, 3 * 64, 0xab);
        Cpu cpu = enabled(USER);
        cpu.x[1] = DATA + 64 + offset;
        bus->reservation = {DATA + 64 + 56, 8};
        step(&cpu, bus, CBO_ZERO_X1);
        check(!cpu.trapped && cpu.minstret == 1 && cpu.x[1] == DATA + 64 + offset,
              "unaligned operand retires once");
        check(bus->reservation.width == 0, "overlapping LR reservation invalidated");
        for (unsigned i = 0; i < 3 * 64; ++i) {
            check(bytes[i] == (i / 64 == 1 ? 0 : 0xab), "zero exactly the containing block");
        }
    }

    Cpu cpu = enabled(SUPERVISOR);
    uint8_t *last = ram_bytes(bus, DATA + 4096 - 65, 65);
    std::fill_n(last, 65, 0xaa);
    cpu.x[1] = DATA + 4095;
    bus->reservation = {DATA + 4096 - 72, 8};
    step(&cpu, bus, CBO_ZERO_X1);
    check(!cpu.trapped && last[0] == 0xaa && bus->reservation.width == 8 &&
          std::all_of(last + 1, last + 65, [](auto b) { return b == 0; }),
          "final byte selects the final RAM block, leaving adjacent LR intact");

    // PMP permits the byte at rs1, but its range ends halfway through the block.
    cpu = enabled(SUPERVISOR);
    cpu.pmpaddr[0] = (DATA + 32) >> 2;
    cpu.x[1] = DATA + 1;
    step(&cpu, bus, CBO_ZERO_X1);
    check(cpu.mcause == STORE_FAULT && cpu.mtval == DATA + 1 && cpu.minstret == 0,
          "whole-block PMP check reports original operand");
    check(std::all_of(bytes, bytes + 64, [](auto b) { return b == 0xab; }), "no partial write");

    for (uint64_t address : {UART_BASE + 7, RESET_PC, FINISHER_BASE, DATA + 4096}) {
        cpu = enabled(MACHINE);
        bus->uart_scratch = 0x7a;
        cpu.x[1] = address;
        step(&cpu, bus, CBO_ZERO_X1);
        check(cpu.mcause == STORE_FAULT && cpu.mtval == address &&
              bus->uart_scratch == 0x7a && !bus->stopped, "non-RAM access fault without MMIO effects");
    }
}

static void test_enable(Bus *bus)
{
    for (Privilege mode : {MACHINE, SUPERVISOR, USER}) {
        for (unsigned bits = 0; bits < 4; ++bits) {
            Cpu cpu = enabled(mode);
            cpu.menvcfg = bits & 1 ? CBZE : 0;
            cpu.senvcfg = bits & 2 ? CBZE : 0;
            const bool allowed = mode == MACHINE || (mode == SUPERVISOR && (bits & 1)) ||
                                 (mode == USER && bits == 3);
            cpu.x[1] = allowed ? DATA : UINT64_MAX;
            step(&cpu, bus, CBO_ZERO_X1);
            check(allowed ? !cpu.trapped : (cpu.mcause == ILLEGAL_INSTRUCTION &&
                  cpu.mepc == CODE && cpu.mtval == CBO_ZERO_X1), "CBZE gates before memory access");
        }
    }
    Cpu cpu = enabled(MACHINE);
    step(&cpu, bus, CBO_ZERO_X1 | (1u << 7));
    check(cpu.trapped && cpu.mcause == ILLEGAL_INSTRUCTION &&
          cpu.mtval == (CBO_ZERO_X1 | (1u << 7)), "CBO.ZERO requires rd=x0");
    step(&cpu, bus, CBO_ZERO_X1 | (1u << 20));
    check(cpu.trapped && cpu.mcause == ILLEGAL_INSTRUCTION &&
          cpu.mtval == (CBO_ZERO_X1 | (1u << 20)), "reserved cache operation");

    // Zicbop encodes its three hints as ORI x0. Even an unmapped target must not fault.
    cpu = enabled(USER);
    cpu.x[11] = UINT64_MAX;
    for (uint32_t hint : {0x0605e013u, 0x0015e013u, 0xfe35e013u}) {
        step(&cpu, bus, hint);
        check(!cpu.trapped && cpu.x[0] == 0 && cpu.x[11] == UINT64_MAX, "prefetch hint retires");
    }
}

static void test_management(Bus *bus)
{
    uint8_t *bytes = ram_bytes(bus, DATA, 64);
    std::fill_n(bytes, 64, 0x35);
    for (uint32_t op : {CBO_INVAL_X1, CBO_CLEAN_X1, CBO_FLUSH_X1}) {
        // 00/01/11 只控制 INVAL，CBCFE 单独控制 CLEAN/FLUSH。
        for (Privilege mode : {MACHINE, SUPERVISOR, USER}) {
            for (uint64_t machine : {0u, 0x10u, 0x30u, 0x40u}) {
                for (uint64_t supervisor : {0u, 0x10u, 0x30u, 0x40u}) {
                    Cpu cpu = enabled(mode);
                    cpu.menvcfg = machine;
                    cpu.senvcfg = supervisor;
                    uint64_t mask = op == CBO_INVAL_X1 ? 0x30 : 0x40;
                    bool allowed = (mode == MACHINE || (machine & mask)) &&
                                   (mode != USER || (supervisor & mask));
                    cpu.x[1] = allowed ? DATA + 63 : UINT64_MAX;
                    step(&cpu, bus, op);
                    check(allowed ? !cpu.trapped && cpu.minstret == 1 :
                          cpu.trapped && cpu.mcause == ILLEGAL_INSTRUCTION && cpu.mtval == op &&
                          cpu.minstret == 0, "gate instruction before address translation");
                }
            }
        }

        Cpu cpu = enabled(MACHINE);
        bus->block.status = 15;
        bus->block.interrupts = 1;
        cpu.x[1] = VIRTIO_BASE + 0x53; // Contains QUEUE_NOTIFY. A management op never notifies.
        step(&cpu, bus, op);
        check(!cpu.trapped && bus->block.status == 15 && bus->block.interrupts == 1,
              "management on MMIO preserves device status and pending interrupt");
        for (uint64_t address : {UART_BASE + 7, RESET_PC, FINISHER_BASE, DATA + 4096}) {
            cpu.x[1] = address;
            step(&cpu, bus, op);
            check(cpu.trapped && cpu.mcause == STORE_FAULT && cpu.mtval == address,
                  "missing full physical block reports a store access fault");
        }
    }
    check(std::all_of(bytes, bytes + 64, [](auto b) { return b == 0x35; }),
          "management preserves RAM bytes");
}

static void test_translation(Bus *bus)
{
    constexpr uint64_t root = RAM_BASE + 0x1000, middle = root + 4096, leaves = middle + 4096;
    constexpr uint64_t va = 0x40000000;
    check(bus_store(bus, root + 8, 8, (middle >> 2) | 1) &&
          bus_store(bus, middle, 8, (leaves >> 2) | 1), "install nonidentity translation");
    Cpu cpu = enabled(MACHINE);
    cpu.satp = (uint64_t{8} << 60) | (root >> 12);
    cpu.mstatus = MPRV; // M-mode fetch; memory access uses MPP=U via Sv39.
    cpu.menvcfg = cpu.senvcfg = 0;
    cpu.x[1] = va + 255;
    uint8_t *bytes = ram_bytes(bus, DATA + 192, 64);
    std::fill_n(bytes, 64, 0x67);
    for (unsigned flags : {0u, 0x53u, 0x57u, 0x97u, 0xc7u}) {
        check(bus_store(bus, leaves, 8, (DATA >> 2) | flags), "install protected leaf");
        step(&cpu, bus, CBO_ZERO_X1);
        check(cpu.mcause == STORE_PAGE_FAULT && cpu.mtval == va + 255 && cpu.mepc == CODE &&
              bytes[63] == 0x67 && cpu.minstret == 0, "missing V/W/D/A/U preserves original state");
        cpu.mstatus = MPRV; // Trap entry wrote MPP=M.
    }
    check(bus_store(bus, leaves, 8, (DATA >> 2) | 0xd7), "install writable leaf");
    step(&cpu, bus, CBO_ZERO_X1);
    check(!cpu.trapped && std::all_of(bytes, bytes + 64, [](auto b) { return b == 0; }),
          "MPRV applies to translation, current M privilege controls CBZE");

    struct PageCase { unsigned flags; bool mxr, allowed; };
    constexpr PageCase pages[] = {
        {0x53, false, true}, {0x57, false, true}, // U/R or U/RW, A=1 and D=0.
        {0x59, false, false}, {0x59, true, true}, // Execute-only, MXR determines readability.
        {0x13, false, false}, {0x43, false, false}, {0, false, false}, // No A, no U, no PTE.
    };
    cpu.pmpcfg[0] = 0x09; // Read-only PMP covers page tables and the final physical block.
    for (uint32_t op : {CBO_INVAL_X1, CBO_CLEAN_X1, CBO_FLUSH_X1}) {
        for (const auto &page : pages) {
            uint64_t pte = (DATA >> 2) | page.flags;
            check(bus_store(bus, leaves, 8, pte), "install management leaf");
            cpu.mstatus = MPRV | (page.mxr ? MXR : 0);
            step(&cpu, bus, op);
            check(page.allowed ? !cpu.trapped : cpu.trapped && cpu.mcause == STORE_PAGE_FAULT &&
                  cpu.mtval == va + 255 && cpu.mepc == CODE, "management tests read access and A");
            uint64_t after = 0;
            check(bus_load(bus, leaves, 8, &after) && after == pte, "management never sets D");
        }
    }
    check(bus_store(bus, leaves, 8, (DATA >> 2) | 0x53), "restore readable mapping");
    constexpr uint64_t supervisor_data = MPRV | (uint64_t{SUPERVISOR} << 11);
    cpu.mstatus = supervisor_data;
    step(&cpu, bus, CBO_CLEAN_X1);
    check(cpu.trapped && cpu.mcause == STORE_PAGE_FAULT && cpu.mtval == va + 255,
          "supervisor management of a user page requires SUM");
    cpu.mstatus = supervisor_data | SUM;
    step(&cpu, bus, CBO_CLEAN_X1);
    check(!cpu.trapped, "SUM allows management of a readable user page");
    cpu.pmpaddr[0] = (DATA + 224) >> 2; // Split the selected physical block at its midpoint.
    cpu.mstatus = MPRV;
    step(&cpu, bus, CBO_CLEAN_X1);
    check(cpu.trapped && cpu.mcause == STORE_FAULT && cpu.mtval == va + 255,
          "management checks the whole PMP range and keeps the virtual operand");
}

int main()
{
    Bus bus{};
    bus.ram.resize(0x6000);
    test_blocks(&bus);
    test_enable(&bus);
    test_management(&bus);
    test_translation(&bus);
    std::puts("Cache block tests passed");
}
