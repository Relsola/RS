#ifndef RS_PAGING_H
#define RS_PAGING_H

#include "user/layout.h"

#include <stddef.h>
#include <stdint.h>

enum { USER_TASK_COUNT = 2 };

bool paging_enable(void);
void kernel_activate(void);
void user_activate(unsigned slot);
void user_prepare(unsigned slot);
bool user_copy_in(unsigned slot, void *destination, uintptr_t address, size_t length);

#endif
