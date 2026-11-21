#ifndef RS_PAGING_H
#define RS_PAGING_H

#include "user/layout.h"
#include "elf.h"
#include "exec_args.h"

#include <stddef.h>
#include <stdint.h>

enum { USER_TASK_COUNT = 2 };

typedef struct {
    unsigned pool_pages;
    unsigned resident_pages;
    unsigned reserved_pages;
    uint64_t faults;
} VmStats;

VmStats vm_stats(void);
bool paging_enable(void);
void kernel_activate(void);
void user_activate(unsigned slot);
void user_prepare(unsigned slot);
uintptr_t user_prepare_elf(unsigned slot, const ElfImage *image, const ExecArgs *args);
void user_clone(unsigned parent_slot, unsigned child_slot);
uintptr_t user_map(unsigned slot, size_t bytes);
bool user_unmap(unsigned slot, uintptr_t base);
void user_release(unsigned slot);
bool user_page_fault(unsigned slot, uintptr_t address, uint64_t cause);
bool user_copy_in(unsigned slot, void *destination, uintptr_t address, size_t length);
bool user_peek(unsigned slot, void *destination, uintptr_t address, size_t length);
bool user_copy_path(unsigned slot, char *destination, size_t capacity, uintptr_t address, size_t length);
bool user_copy_out(unsigned slot, uintptr_t address, const void *source, size_t length);

#endif
