#include "elf.h"

enum {
    ELF_HEADER_SIZE = 64, PROGRAM_HEADER_SIZE = 56,
    PT_NULL = 0, PT_LOAD = 1, PT_NOTE = 4, PT_PHDR = 6,
    PT_GNU_STACK = 0x6474e551, PT_RISCV_ATTRIBUTES = 0x70000003,
};

/* 文件头来自字节缓冲区。逐字段解码，也适用于未对齐的程序头。 */
static uint16_t little16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] | (unsigned)p[1] << 8);
}

static uint32_t little32(const uint8_t *p)
{
    return little16(p) | (uint32_t)little16(p + 2) << 16;
}

static uint64_t little64(const uint8_t *p)
{
    return little32(p) | (uint64_t)little32(p + 4) << 32;
}

const char *elf_parse(const uint8_t *file, size_t size, ElfImage *image)
{
    if (size < ELF_HEADER_SIZE || file[0] != 0x7f || file[1] != 'E' || file[2] != 'L' ||
        file[3] != 'F' || file[4] != 2 || file[5] != 1 || file[6] != 1 ||
        file[7] != 0 || file[8] != 0 || little16(file + 16) != 2 ||
        little16(file + 18) != 243 || little32(file + 20) != 1 ||
        (little32(file + 48) & ~UINT32_C(1)) || little16(file + 52) != ELF_HEADER_SIZE) {
        return "expected a static RISC-V ELF64 executable";
    }
    uint64_t table = little64(file + 32);
    unsigned count = little16(file + 56);
    if (little16(file + 54) != PROGRAM_HEADER_SIZE || !count || table < ELF_HEADER_SIZE ||
        table > size || count > (size - table) / PROGRAM_HEADER_SIZE) {
        return "invalid ELF program header table";
    }

    image->entry = little64(file + 24);
    image->count = 0;
    bool entry_in_code = false;
    const uintptr_t limit = USER_ELF_BASE + USER_ELF_PAGES * USER_PAGE_SIZE;
    uintptr_t next_page = USER_ELF_BASE;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *ph = file + table + (size_t)i * PROGRAM_HEADER_SIZE;
        uint32_t type = little32(ph);
        uint32_t flags = little32(ph + 4);
        if (type == PT_NULL || type == PT_NOTE || type == PT_PHDR || type == PT_RISCV_ATTRIBUTES ||
            (type == PT_GNU_STACK && !(flags & ELF_EXECUTE))) {
            continue;
        }
        if (type != PT_LOAD) {
            return "unsupported ELF segment type";
        }
        uint64_t offset = little64(ph + 8), address = little64(ph + 16);
        uint64_t file_size = little64(ph + 32), memory_size = little64(ph + 40);
        uint64_t alignment = little64(ph + 48);
        if (flags != ELF_READ && flags != (ELF_READ | ELF_EXECUTE) && flags != (ELF_READ | ELF_WRITE)) {
            return "unsupported ELF segment permissions";
        }
        if (offset > size || file_size > size - offset || file_size > memory_size ||
            ((offset ^ address) & (USER_PAGE_SIZE - 1)) ||
            (alignment > 1 && ((alignment & (alignment - 1)) || ((offset ^ address) & (alignment - 1))))) {
            return "invalid ELF segment layout";
        }
        if (!memory_size) {
            continue;
        }
        /* PT_LOAD 按虚拟地址递增。向上取整使两个段也不能共用一个权限页。 */
        if (address < next_page || address >= limit || memory_size > limit - address) {
            return "ELF segment outside available user pages";
        }
        next_page = (address + memory_size + USER_PAGE_SIZE - 1) & ~(uintptr_t)(USER_PAGE_SIZE - 1);
        image->segments[image->count++] = (ElfSegment){
            file + offset, address, file_size, memory_size, flags
        };
        if ((flags & ELF_EXECUTE) && image->entry >= address && image->entry - address < file_size) {
            entry_in_code = true;
        }
    }
    if (!entry_in_code || (image->entry & 1)) {
        return "ELF entry is not an aligned instruction in executable data";
    }
    return nullptr;
}
