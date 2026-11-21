#include "paging.h"

#include <stdint.h>

enum {
    PAGE_SHIFT = 12, MEGAPAGE_SHIFT = 21, REGION_SHIFT = 30,
    TABLE_ENTRIES = 512,
    VALID = 1, READ = 2, WRITE = 4, EXECUTE = 8, USER_PAGE = 16, ACCESSED = 64, DIRTY = 128,
};

alignas(4096) static uint64_t kernel_root[TABLE_ENTRIES];
alignas(4096) static uint64_t device_l1[TABLE_ENTRIES];
alignas(4096) static uint64_t control_l0[TABLE_ENTRIES];
alignas(4096) static uint64_t uart_l0[TABLE_ENTRIES];
alignas(4096) static uint64_t ram_l1[TABLE_ENTRIES];
extern const uint8_t user_image_page[USER_PAGE_SIZE];

/* 监管态映射在各根页表中相同，用户地址的末级表和可写页归各任务所有。 */
typedef struct {
    alignas(USER_PAGE_SIZE) uint64_t root[TABLE_ENTRIES];
    alignas(USER_PAGE_SIZE) uint64_t user_l1[TABLE_ENTRIES];
    alignas(USER_PAGE_SIZE) uint64_t user_l0[TABLE_ENTRIES];
    alignas(USER_PAGE_SIZE) uint8_t stack[USER_PAGE_SIZE];
    alignas(USER_PAGE_SIZE) uint8_t data[USER_PAGE_SIZE];
} AddressSpace;

static AddressSpace spaces[USER_TASK_COUNT];
/* 磁盘程序占用前台槽 0，在其退出后这些物理页可供下次 run 复用。 */
alignas(USER_PAGE_SIZE) static uint8_t program_memory[USER_ELF_PAGES * USER_PAGE_SIZE];

/* PPN 从物理地址的 bit 12 开始，写入表项时从 bit 10 开始。 */
static uint64_t entry(uintptr_t address, unsigned flags)
{
    return ((address >> PAGE_SHIFT) << 10) | flags | VALID;
}

static unsigned index_at(uintptr_t address, unsigned shift)
{
    return (unsigned)((address >> shift) & (TABLE_ENTRIES - 1));
}

uint64_t install_page_table(uint64_t satp);

static uint64_t root_satp(const uint64_t *root)
{
    return (UINT64_C(8) << 60) | ((uintptr_t)root >> PAGE_SHIFT); /* Sv39，ASID=0。 */
}

void kernel_activate(void)
{
    install_page_table(root_satp(kernel_root));
}

void user_activate(unsigned slot)
{
    install_page_table(root_satp(spaces[slot].root));
}

static void reset_user_pages(AddressSpace *space)
{
    space->user_l0[index_at(USER_ENTRY, PAGE_SHIFT)] = 0;
    space->user_l0[index_at(RS_USER_DATA_BASE, PAGE_SHIFT)] = 0;
    for (unsigned i = 0; i < USER_ELF_PAGES; ++i) {
        space->user_l0[index_at(USER_ELF_BASE, PAGE_SHIFT) + i] = 0;
    }
    for (size_t i = 0; i < USER_PAGE_SIZE; ++i) {
        space->stack[i] = 0;
    }
}

void user_prepare(unsigned slot)
{
    AddressSpace *space = &spaces[slot];
    reset_user_pages(space);
    space->user_l0[index_at(USER_ENTRY, PAGE_SHIFT)] =
        entry((uintptr_t)user_image_page, USER_PAGE | READ | EXECUTE | ACCESSED);
    space->user_l0[index_at(RS_USER_DATA_BASE, PAGE_SHIFT)] =
        entry((uintptr_t)space->data, USER_PAGE | READ | WRITE | ACCESSED | DIRTY);
    for (size_t i = 0; i < USER_PAGE_SIZE; ++i) {
        space->data[i] = 0;
    }
}

void user_prepare_elf(const ElfImage *image)
{
    AddressSpace *space = &spaces[0];
    reset_user_pages(space);
    for (unsigned i = 0; i < image->count; ++i) {
        const ElfSegment *segment = &image->segments[i];
        size_t start = segment->address - USER_ELF_BASE;
        size_t first_page = start / USER_PAGE_SIZE;
        size_t last_page = (start + segment->memory_size - 1) / USER_PAGE_SIZE;
        unsigned flags = USER_PAGE | READ | ACCESSED;
        if (segment->flags & ELF_EXECUTE) {
            flags |= EXECUTE;
        }
        if (segment->flags & ELF_WRITE) {
            flags |= WRITE | DIRTY;
        }
        for (size_t n = first_page; n <= last_page; ++n) {
            uint8_t *page = program_memory + n * USER_PAGE_SIZE;
            /* 页首尾的段外字节也会对 U 可见，因此清空整个页。 */
            for (size_t j = 0; j < USER_PAGE_SIZE; ++j) {
                page[j] = 0;
            }
            space->user_l0[index_at(USER_ELF_BASE, PAGE_SHIFT) + n] = entry((uintptr_t)page, flags);
        }
        for (size_t j = 0; j < segment->file_size; ++j) {
            program_memory[start + j] = segment->bytes[j];
        }
    }
}

bool user_copy_in(unsigned slot, void *destination, uintptr_t address, size_t length)
{
    /* 检查调用者的每个页表项。sys_write 在整个缓冲区复制成功后才输出。 */
    uint8_t *target = destination;
    while (length) {
        if ((address >> MEGAPAGE_SHIFT) != (USER_ENTRY >> MEGAPAGE_SHIFT)) {
            return false;
        }
        uint64_t pte = spaces[slot].user_l0[index_at(address, PAGE_SHIFT)];
        const unsigned required = VALID | USER_PAGE | READ;
        if ((pte & required) != required) {
            return false;
        }
        uintptr_t offset = address & (USER_PAGE_SIZE - 1);
        size_t chunk = USER_PAGE_SIZE - offset;
        if (chunk > length) {
            chunk = length;
        }
        const uint8_t *source = (const uint8_t *)(uintptr_t)(((pte >> 10) << PAGE_SHIFT) + offset);
        for (size_t i = 0; i < chunk; ++i) {
            target[i] = source[i];
        }
        address += chunk;
        target += chunk;
        length -= chunk;
    }
    return true;
}

bool paging_enable(void)
{
    const uintptr_t control = 0x00100000;
    const uintptr_t uart = 0x10000000;
    const uintptr_t ram = 0x80000000;
    const unsigned rw = READ | WRITE | ACCESSED | DIRTY;
    const unsigned device_region = index_at(control, REGION_SHIFT);
    const unsigned ram_region = index_at(ram, REGION_SHIFT);
    kernel_root[device_region] = entry((uintptr_t)device_l1, 0);
    device_l1[index_at(control, MEGAPAGE_SHIFT)] = entry((uintptr_t)control_l0, 0);
    control_l0[index_at(control, PAGE_SHIFT)] = entry(control, rw);
    device_l1[index_at(uart, MEGAPAGE_SHIFT)] = entry((uintptr_t)uart_l0, 0);
    /* 同一张末级表映射 UART 和相邻的八个 virtio 端口，每个占一页。 */
    for (unsigned port = 0; port <= 8; ++port) {
        uintptr_t address = uart + ((uintptr_t)port << PAGE_SHIFT);
        uart_l0[index_at(address, PAGE_SHIFT)] = entry(address, rw);
    }
    kernel_root[ram_region] = entry((uintptr_t)ram_l1, 0);
    for (unsigned i = 0; i < 64; ++i) {
        uintptr_t address = ram + ((uintptr_t)i << MEGAPAGE_SHIFT);
        ram_l1[index_at(address, MEGAPAGE_SHIFT)] = entry(address, rw | EXECUTE);
    }
    for (unsigned slot = 0; slot < USER_TASK_COUNT; ++slot) {
        AddressSpace *space = &spaces[slot];
        space->root[device_region] = kernel_root[device_region];
        space->root[ram_region] = kernel_root[ram_region];
        space->root[index_at(USER_ENTRY, REGION_SHIFT)] = entry((uintptr_t)space->user_l1, 0);
        space->user_l1[index_at(USER_ENTRY, MEGAPAGE_SHIFT)] = entry((uintptr_t)space->user_l0, 0);
        space->user_l0[index_at(USER_STACK_TOP - USER_PAGE_SIZE, PAGE_SHIFT)] =
            entry((uintptr_t)space->stack, USER_PAGE | rw);
    }
    uint64_t satp = root_satp(kernel_root);
    return install_page_table(satp) == satp;
}
