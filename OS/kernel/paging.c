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

void user_prepare(unsigned slot)
{
    AddressSpace *space = &spaces[slot];
    for (size_t i = 0; i < USER_PAGE_SIZE; ++i) {
        space->stack[i] = 0;
        space->data[i] = 0;
    }
}

bool user_copy_in(unsigned slot, void *destination, uintptr_t address, size_t length)
{
    if (!length) {
        return true;
    }
    /* 用户映射都在同一个 2 MiB 区间，彼此之间有保护页。先核对完整地址和区间，
       再用调用任务自己的末级表解析物理页，避免把另一个任务同 VA 的数据写入串口。 */
    uintptr_t offset = address & (USER_PAGE_SIZE - 1);
    if ((address >> MEGAPAGE_SHIFT) != (USER_ENTRY >> MEGAPAGE_SHIFT) ||
        length > USER_PAGE_SIZE - offset) {
        return false;
    }
    uint64_t pte = spaces[slot].user_l0[index_at(address, PAGE_SHIFT)];
    const unsigned required = VALID | USER_PAGE | READ;
    if ((pte & required) != required) {
        return false;
    }
    const uint8_t *source = (const uint8_t *)(uintptr_t)(((pte >> 10) << PAGE_SHIFT) + offset);
    uint8_t *target = destination;
    for (size_t i = 0; i < length; ++i) {
        target[i] = source[i];
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
        space->user_l0[index_at(USER_ENTRY, PAGE_SHIFT)] =
            entry((uintptr_t)user_image_page, USER_PAGE | READ | EXECUTE | ACCESSED);
        space->user_l0[index_at(USER_STACK_TOP - USER_PAGE_SIZE, PAGE_SHIFT)] =
            entry((uintptr_t)space->stack, USER_PAGE | rw);
        space->user_l0[index_at(RS_USER_DATA_BASE, PAGE_SHIFT)] =
            entry((uintptr_t)space->data, USER_PAGE | rw);
    }
    uint64_t satp = root_satp(kernel_root);
    return install_page_table(satp) == satp;
}
