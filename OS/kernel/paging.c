#include "paging.h"

#include <stdint.h>

enum {
    PAGE_SHIFT = 12, MEGAPAGE_SHIFT = 21, REGION_SHIFT = 30,
    TABLE_ENTRIES = 512,
    VALID = 1, READ = 2, WRITE = 4, EXECUTE = 8, USER_PAGE = 16, ACCESSED = 64, DIRTY = 128,
    COW = 1 << 8, /* Sv39 的 RSW 位，由内核解释；硬件只看到 W=0。 */
    ANON_WINDOW_PAGES = 64, ANON_POOL_PAGES = 32, MAX_AREAS = 8,
};

#define ANON_BASE (USER_ENTRY + 64 * USER_PAGE_SIZE)
static_assert((USER_ENTRY & ((1u << MEGAPAGE_SHIFT) - 1)) == 0);
static_assert(ANON_BASE >= USER_ELF_BASE + USER_ELF_PAGES * USER_PAGE_SIZE);
static_assert((ANON_BASE - USER_ENTRY) / USER_PAGE_SIZE + ANON_WINDOW_PAGES <= TABLE_ENTRIES);

alignas(4096) static uint64_t kernel_root[TABLE_ENTRIES];
alignas(4096) static uint64_t device_l1[TABLE_ENTRIES];
alignas(4096) static uint64_t control_l0[TABLE_ENTRIES];
alignas(4096) static uint64_t uart_l0[TABLE_ENTRIES];
alignas(4096) static uint64_t ram_l1[TABLE_ENTRIES];
extern const uint8_t user_image_page[USER_PAGE_SIZE];

typedef struct {
    unsigned first; /* 相对 ANON_BASE 的页号。 */
    unsigned count;
} VmArea;

/* 监管态映射在各根页表中相同，每个任务有自己的用户末级表。 */
typedef struct {
    alignas(USER_PAGE_SIZE) uint64_t root[TABLE_ENTRIES];
    alignas(USER_PAGE_SIZE) uint64_t user_l1[TABLE_ENTRIES];
    alignas(USER_PAGE_SIZE) uint64_t user_l0[TABLE_ENTRIES];
    alignas(USER_PAGE_SIZE) uint8_t stack[USER_PAGE_SIZE];
    alignas(USER_PAGE_SIZE) uint8_t data[USER_PAGE_SIZE];
    alignas(USER_PAGE_SIZE) uint8_t program_memory[USER_ELF_PAGES * USER_PAGE_SIZE];
    VmArea areas[MAX_AREAS]; /* 按 first 递增，相邻的两次申请可以各自释放。 */
    unsigned area_count;
} AddressSpace;

static AddressSpace spaces[USER_TASK_COUNT];
alignas(USER_PAGE_SIZE) static uint8_t frames[ANON_POOL_PAGES][USER_PAGE_SIZE];
static unsigned frame_refs[ANON_POOL_PAGES];
static VmStats stats = {.pool_pages = ANON_POOL_PAGES};

/* PPN 从物理地址的 bit 12 开始，写入表项时从 bit 10 开始。 */
static uint64_t entry(uintptr_t address, unsigned flags)
{
    return ((address >> PAGE_SHIFT) << 10) | flags | VALID;
}

static unsigned index_at(uintptr_t address, unsigned shift)
{
    return (unsigned)((address >> shift) & (TABLE_ENTRIES - 1));
}

static uint8_t *physical_page(uint64_t pte)
{
    return (uint8_t *)(uintptr_t)((pte >> 10) << PAGE_SHIFT);
}

static unsigned anonymous_frame(uint64_t pte)
{
    return (unsigned)(((uintptr_t)physical_page(pte) - (uintptr_t)frames) >> PAGE_SHIFT);
}

static void copy_page(uint8_t *destination, const uint8_t *source)
{
    for (size_t i = 0; i < USER_PAGE_SIZE; ++i) {
        destination[i] = source[i];
    }
}

uint64_t install_page_table(uint64_t satp);
void invalidate_user_page(uintptr_t address);

VmStats vm_stats(void)
{
    return stats;
}

uintptr_t user_map(unsigned slot, size_t bytes)
{
    if (!bytes || bytes > ANON_WINDOW_PAGES * USER_PAGE_SIZE) {
        return 0;
    }
    AddressSpace *space = &spaces[slot];
    if (space->area_count == MAX_AREAS) {
        return 0;
    }
    unsigned needed = (unsigned)((bytes + USER_PAGE_SIZE - 1) / USER_PAGE_SIZE);
    unsigned start = 0, i = 0;
    while (i < space->area_count) {
        VmArea area = space->areas[i];
        if (area.first - start >= needed) {
            break;
        }
        start = area.first + area.count;
        ++i;
    }
    if (ANON_WINDOW_PAGES - start < needed) {
        return 0;
    }
    for (unsigned n = space->area_count; n > i; --n) {
        space->areas[n] = space->areas[n - 1];
    }
    space->areas[i] = (VmArea){start, needed};
    ++space->area_count;
    stats.reserved_pages += needed;
    return ANON_BASE + start * USER_PAGE_SIZE;
}

static bool reserved_page(const AddressSpace *space, uintptr_t address)
{
    if (address < ANON_BASE || address >= ANON_BASE + ANON_WINDOW_PAGES * USER_PAGE_SIZE) {
        return false;
    }
    unsigned page = (unsigned)((address - ANON_BASE) / USER_PAGE_SIZE);
    for (unsigned i = 0; i < space->area_count; ++i) {
        VmArea area = space->areas[i];
        if (page >= area.first && page < area.first + area.count) {
            return true;
        }
    }
    return false;
}

/* 进入此处的叶项只有两种：尚未分配，或写入时需要恢复写权的 COW 页。 */
static bool make_anonymous_writable(AddressSpace *space, uintptr_t address)
{
    uint64_t *leaf = &space->user_l0[index_at(address, PAGE_SHIFT)];
    uint64_t old = *leaf;
    if (old && frame_refs[anonymous_frame(old)] == 1) {
        *leaf = (old | WRITE) & ~(uint64_t)COW;
        invalidate_user_page(address);
        return true;
    }
    for (unsigned frame = 0; frame < ANON_POOL_PAGES; ++frame) {
        if (!frame_refs[frame]) {
            if (old) {
                copy_page(frames[frame], physical_page(old));
            } else {
                for (size_t i = 0; i < USER_PAGE_SIZE; ++i) {
                    frames[frame][i] = 0;
                }
            }
            frame_refs[frame] = 1;
            ++stats.resident_pages;
            *leaf = entry((uintptr_t)frames[frame], USER_PAGE | READ | WRITE | ACCESSED | DIRTY);
            invalidate_user_page(address);
            if (old) {
                --frame_refs[anonymous_frame(old)];
            }
            return true;
        }
    }
    return false;
}

bool user_page_fault(unsigned slot, uintptr_t address, uint64_t cause)
{
    AddressSpace *space = &spaces[slot];
    if ((cause != 13 && cause != 15) || !reserved_page(space, address)) {
        return false;
    }
    uint64_t old = space->user_l0[index_at(address, PAGE_SHIFT)];
    if (old && !(cause == 15 && (old & COW))) {
        return false;
    }
    if (!make_anonymous_writable(space, address)) {
        return false;
    }
    ++stats.faults;
    return true;
}

static void drop_area(AddressSpace *space, VmArea area)
{
    for (unsigned i = 0; i < area.count; ++i) {
        uintptr_t address = ANON_BASE + (area.first + i) * USER_PAGE_SIZE;
        uint64_t *leaf = &space->user_l0[index_at(address, PAGE_SHIFT)];
        if (*leaf) {
            unsigned frame = anonymous_frame(*leaf);
            *leaf = 0;
            /* 本 hart 可能缓存了这个映射。先撤销翻译，再把物理页交给其他任务。 */
            invalidate_user_page(address);
            if (--frame_refs[frame] == 0) {
                --stats.resident_pages;
            }
        }
    }
    stats.reserved_pages -= area.count;
}

bool user_unmap(unsigned slot, uintptr_t base)
{
    AddressSpace *space = &spaces[slot];
    for (unsigned i = 0; i < space->area_count; ++i) {
        VmArea area = space->areas[i];
        if (base == ANON_BASE + area.first * USER_PAGE_SIZE) {
            drop_area(space, area);
            --space->area_count;
            for (unsigned j = i; j < space->area_count; ++j) {
                space->areas[j] = space->areas[j + 1];
            }
            return true;
        }
    }
    return false;
}

void user_release(unsigned slot)
{
    AddressSpace *space = &spaces[slot];
    while (space->area_count) {
        drop_area(space, space->areas[--space->area_count]);
    }
}

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

static void reset_user_pages(unsigned slot)
{
    AddressSpace *space = &spaces[slot];
    user_release(slot);
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
    reset_user_pages(slot);
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
    reset_user_pages(0);
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
            uint8_t *page = space->program_memory + n * USER_PAGE_SIZE;
            /* 页首尾的段外字节也会对 U 可见，因此清空整个页。 */
            for (size_t j = 0; j < USER_PAGE_SIZE; ++j) {
                page[j] = 0;
            }
            space->user_l0[index_at(USER_ELF_BASE, PAGE_SHIFT) + n] = entry((uintptr_t)page, flags);
        }
        for (size_t j = 0; j < segment->file_size; ++j) {
            space->program_memory[start + j] = segment->bytes[j];
        }
    }
}

/* 固定布局的可写页使用子槽的预留内存，克隆这些页无需占用匿名页池。 */
static uint64_t clone_image_page(uint64_t source, uint8_t *backing)
{
    if (!(source & WRITE)) {
        return source; /* 未映射、R 或 RX。 */
    }
    copy_page(backing, physical_page(source));
    return entry((uintptr_t)backing, (unsigned)(source & 0x3ff));
}

void user_clone(unsigned parent_slot, unsigned child_slot)
{
    AddressSpace *parent = &spaces[parent_slot];
    AddressSpace *child = &spaces[child_slot];
    /* 调度器已回收子槽旧任务的区域。内核在整个 fork 期间关闭 S 中断。 */
    copy_page(child->stack, parent->stack);
    unsigned code = index_at(USER_ENTRY, PAGE_SHIFT);
    unsigned data = index_at(RS_USER_DATA_BASE, PAGE_SHIFT);
    child->user_l0[code] = parent->user_l0[code];
    child->user_l0[data] = clone_image_page(parent->user_l0[data], child->data);
    unsigned elf = index_at(USER_ELF_BASE, PAGE_SHIFT);
    for (unsigned i = 0; i < USER_ELF_PAGES; ++i) {
        child->user_l0[elf + i] = clone_image_page(parent->user_l0[elf + i],
                                                   child->program_memory + i * USER_PAGE_SIZE);
    }
    child->area_count = parent->area_count;
    for (unsigned i = 0; i < parent->area_count; ++i) {
        VmArea area = parent->areas[i];
        child->areas[i] = area;
        stats.reserved_pages += area.count;
        for (unsigned n = 0; n < area.count; ++n) {
            uintptr_t address = ANON_BASE + (area.first + n) * USER_PAGE_SIZE;
            unsigned index = index_at(address, PAGE_SHIFT);
            uint64_t leaf = parent->user_l0[index];
            if (!leaf) {
                continue; /* 两个空间各自在首次访问时申请零页。 */
            }
            uint64_t shared = (leaf | COW) & ~(uint64_t)WRITE;
            ++frame_refs[anonymous_frame(leaf)];
            parent->user_l0[index] = shared;
            child->user_l0[index] = shared;
            /* 当前页表仍属于父进程，立即清除可能缓存的父进程写权。 */
            invalidate_user_page(address);
        }
    }
}

static bool prepare_buffer(AddressSpace *space, uintptr_t address, size_t length, unsigned required)
{
    /* user_l0 只描述这个 2 MiB 范围。先检查区间，再使用末级索引。 */
    const uintptr_t limit = USER_ENTRY + (1u << MEGAPAGE_SHIFT);
    if (address < USER_ENTRY || address >= limit || length > limit - address) {
        return false;
    }
    uintptr_t first = address & ~(uintptr_t)(USER_PAGE_SIZE - 1);
    uintptr_t end = address + length;
    unsigned needed = 0;
    bool write = (required & WRITE) != 0;
    for (uintptr_t page = first; page < end; page += USER_PAGE_SIZE) {
        uint64_t pte = space->user_l0[index_at(page, PAGE_SHIFT)];
        if (!pte && reserved_page(space, page)) {
            ++needed;
        } else if (write && (pte & COW)) {
            if (frame_refs[anonymous_frame(pte)] > 1) {
                ++needed;
            }
        } else if ((pte & required) != required) {
            return false;
        }
    }
    if (needed > stats.pool_pages - stats.resident_pages) {
        return false;
    }
    /* 单 hart 在此次系统调用内独占页池。整段权限和可用页数已检查通过。 */
    for (uintptr_t page = first; page < end; page += USER_PAGE_SIZE) {
        uint64_t pte = space->user_l0[index_at(page, PAGE_SHIFT)];
        if (!pte || (write && (pte & COW))) {
            if (!make_anonymous_writable(space, page)) {
                return false;
            }
        }
    }
    return true;
}

static bool copy_user(unsigned slot, uintptr_t address, size_t length,
                      uint8_t *kernel_destination, const uint8_t *kernel_source)
{
    if (!length) {
        return true;
    }
    AddressSpace *space = &spaces[slot];
    unsigned required = VALID | USER_PAGE | READ;
    if (kernel_source) {
        required |= WRITE;
    }
    if (!prepare_buffer(space, address, length, required)) {
        return false;
    }
    size_t done = 0;
    while (done < length) {
        uint64_t pte = space->user_l0[index_at(address, PAGE_SHIFT)];
        uintptr_t offset = address & (USER_PAGE_SIZE - 1);
        uint8_t *user = physical_page(pte) + offset;
        size_t chunk = USER_PAGE_SIZE - offset;
        if (chunk > length - done) {
            chunk = length - done;
        }
        const uint8_t *source = kernel_source ? kernel_source + done : user;
        uint8_t *destination = kernel_source ? user : kernel_destination + done;
        for (size_t i = 0; i < chunk; ++i) {
            destination[i] = source[i];
        }
        address += chunk;
        done += chunk;
    }
    return true;
}

bool user_copy_in(unsigned slot, void *destination, uintptr_t address, size_t length)
{
    return copy_user(slot, address, length, destination, nullptr);
}

bool user_copy_out(unsigned slot, uintptr_t address, const void *source, size_t length)
{
    return copy_user(slot, address, length, nullptr, source);
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
