#ifndef RS_KERNEL_STRING_H
#define RS_KERNEL_STRING_H

#include <stddef.h>

void *memcpy(void *restrict destination, const void *restrict source, size_t count);
void *memset(void *destination, int byte, size_t count);

#endif
