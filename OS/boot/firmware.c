#include "disk.h"
#include "disk_format.h"
#include "platform.h"

alignas(16) static uint8_t header[SECTOR_BYTES];

[[noreturn]] static void boot_error(const char *text)
{
    uart_puts("boot error: ");
    uart_puts(text);
    uart_puts("\n");
    poweroff(true);
}

[[noreturn]] void guest_main(void)
{
    uart_puts("RS firmware: booting from virtio-blk\n");
    uint64_t capacity = 0;
    if (!disk_open(&capacity)) {
        boot_error("virtio-blk unavailable");
    }
    if (capacity == 0 || !disk_read_sector(0, header)) {
        boot_error("cannot read sector 0");
    }
    static const char magic[] = "RSDISK01";
    for (unsigned i = 0; i < sizeof(magic) - 1; ++i) {
        if (header[i] != magic[i]) {
            boot_error("invalid disk magic");
        }
    }

    uint32_t size = load_u32_le(header + 8);
    if (size == 0 || size > KERNEL_MAX_BYTES) {
        boot_error("invalid kernel size");
    }
    uint32_t sectors = (size + SECTOR_BYTES - 1) / SECTOR_BYTES;
    if (sectors > capacity - 1) {
        boot_error("truncated kernel");
    }
    uint8_t *const kernel = (uint8_t *)(uintptr_t)RS_KERNEL_BASE;
    for (uint32_t i = 0; i < sectors; ++i) {
        if (!disk_read_sector(1 + i, kernel + i * SECTOR_BYTES)) {
            boot_error("kernel sector read failed");
        }
    }
    if (crc32(kernel, size) != load_u32_le(header + 12)) {
        boot_error("kernel CRC mismatch");
    }
    uart_puts("RS firmware: kernel verified\n");
    enter_kernel(RS_KERNEL_BASE);
}
