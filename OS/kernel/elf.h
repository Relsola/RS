#ifndef RS_ELF_H
#define RS_ELF_H

#include "user/layout.h"
#include <stddef.h>
#include <stdint.h>

/* 单个前台 ELF 在这段连续用户地址内装入。每个物理页只归一个段。 */
enum { USER_ELF_PAGES = 16, ELF_EXECUTE = 1, ELF_WRITE = 2, ELF_READ = 4 };
#define USER_ELF_BASE (USER_ENTRY + 16 * USER_PAGE_SIZE)

typedef struct {
    const uint8_t *bytes;
    uintptr_t address;
    size_t file_size;
    size_t memory_size;
    unsigned flags;
} ElfSegment;

typedef struct {
    uintptr_t entry;
    unsigned count;
    ElfSegment segments[USER_ELF_PAGES];
} ElfImage;

/* 只检查文件并记录装载计划，成功返回 NULL。调用者在完成复制前保留文件缓冲区。 */
const char *elf_parse(const uint8_t *file, size_t size, ElfImage *image);

#endif
