#include "bus.h"

// auipc t0,0 ; ld t0,16(t0) ; jalr zero,0(t0) ; nop ; .dword RAM_BASE
// 绝对地址置于指令后面，避免 RV64 的 LUI 把 0x80000000 符号扩展。
static constexpr uint8_t RESET_ROM[] = {
    0x97, 0x02, 0x00, 0x00, 0x83, 0xb2, 0x02, 0x01,
    0x67, 0x80, 0x02, 0x00, 0x13, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
};

static bool in_ram(const Bus *bus, uint64_t address, size_t size)
{
    if (address < RAM_BASE || size > bus->ram.size()) {
        return false;
    }
    return address - RAM_BASE <= bus->ram.size() - size;
}

bool bus_is_ram(const Bus *bus, uint64_t address, unsigned width)
{
    return in_ram(bus, address, width);
}

bool bus_can_manage_block(const Bus *bus, uint64_t address, unsigned width)
{
    // 每个 virtio 插槽整页可读。ROM、UART 和关机寄存器都占不满一个块。
    return in_ram(bus, address, width) ||
           (address >= VIRTIO_BASE && width <= VIRTIO_WINDOW_BYTES &&
            address - VIRTIO_BASE <= VIRTIO_WINDOW_BYTES - width);
}

Reservation *bus_reservation(Bus *bus)
{
    return &bus->reservation;
}

uint8_t *ram_bytes(Bus *bus, uint64_t address, size_t size)
{
    if (!in_ram(bus, address, size)) {
        return nullptr;
    }
    return bus->ram.data() + static_cast<size_t>(address - RAM_BASE);
}

uint64_t read_le(const uint8_t *p, unsigned width)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) {
        value |= uint64_t{p[i]} << (8 * i);
    }
    return value;
}

void write_le(uint8_t *p, unsigned width, uint64_t value)
{
    for (unsigned i = 0; i < width; ++i) {
        p[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

static uint8_t uart_read(Bus *bus, unsigned offset)
{
    const bool divisor_latch = (bus->uart_lcr & 0x80) != 0;
    switch (offset) {
    case 0: return divisor_latch ? bus->uart_dll : console_read(&bus->console);
    case 1: return divisor_latch ? bus->uart_dlm : bus->uart_ier;
    case 2: return 0x01; // IIR: polled UART, no interrupt pending.
    case 3: return bus->uart_lcr;
    case 5: {
        bool has_byte = console_ready(&bus->console);
        // 连续空读区分“管道结束时仍在打印启动消息”和“内核正等待下一条命令”。
        if (bus->console.eof && bus->console.empty_polls >= 1024) {
            bus->stopped = true;
            bus->exit_code = 2;
        }
        return static_cast<uint8_t>(0x60 | unsigned(has_byte)); // THRE, TEMT, DR
    }
    case 7: return bus->uart_scratch;
    default: return 0;
    }
}

static void uart_write(Bus *bus, unsigned offset, uint8_t value)
{
    const bool divisor_latch = (bus->uart_lcr & 0x80) != 0;
    switch (offset) {
    case 0:
        if (divisor_latch) {
            bus->uart_dll = value;
        } else {
            console_write(&bus->console, value);
        }
        break;
    case 1:
        if (divisor_latch) {
            bus->uart_dlm = value;
        } else {
            bus->uart_ier = value;
        }
        break;
    case 3: bus->uart_lcr = value; break;
    case 7: bus->uart_scratch = value; break;
    }
}

bool bus_load(Bus *bus, uint64_t address, unsigned width, uint64_t *value)
{
    if (width != 1 && width != 2 && width != 4 && width != 8) {
        return false;
    }
    if (uint8_t *p = ram_bytes(bus, address, width)) {
        *value = read_le(p, width);
    } else if (address >= RESET_PC && address - RESET_PC <= sizeof(RESET_ROM) - width) {
        *value = read_le(RESET_ROM + (address - RESET_PC), width);
    } else if (address >= UART_BASE && address < UART_BASE + 8 && width == 1) {
        *value = uart_read(bus, static_cast<unsigned>(address - UART_BASE));
    } else if (address >= VIRTIO_BASE && address < VIRTIO_BASE + VIRTIO_WINDOW_BYTES &&
               width == 4 && (address & 3) == 0) {
        uint32_t offset = static_cast<uint32_t>(address - VIRTIO_BASE);
        if (offset < 0x1000) {
            *value = block_read(bus, offset);
        } else {
            // 空插槽仍有 MMIO 传输头，DeviceID 为 0。
            switch (offset & 0xfff) {
            case 0: *value = 0x74726976; break;
            case 4: *value = 2; break;
            default: *value = 0; break;
            }
        }
    } else {
        return false;
    }
    return true;
}

bool bus_store(Bus *bus, uint64_t address, unsigned width, uint64_t value)
{
    if (width != 1 && width != 2 && width != 4 && width != 8) {
        return false;
    }
    if (uint8_t *p = ram_bytes(bus, address, width)) {
        write_le(p, width, value);
        reservation_after_write(&bus->reservation, address, width);
    } else if (address >= UART_BASE && address < UART_BASE + 8 && width == 1) {
        uart_write(bus, static_cast<unsigned>(address - UART_BASE), static_cast<uint8_t>(value));
    } else if (address >= VIRTIO_BASE && address < VIRTIO_BASE + 0x1000 &&
               width == 4 && (address & 3) == 0) {
        block_write(bus, static_cast<uint32_t>(address - VIRTIO_BASE), static_cast<uint32_t>(value));
    } else if (address == FINISHER_BASE && (width == 2 || width == 4)) {
        if ((value & 0xffff) == 0x5555 || (value & 0xffff) == 0x3333) {
            bus->stopped = true;
            bus->exit_code = (value & 0xffff) == 0x5555 ? 0 : 1;
        }
    } else {
        return false;
    }
    return true;
}
