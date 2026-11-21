#ifndef RS_KERNEL_CONTEXT_H
#define RS_KERNEL_CONTEXT_H

#define RS_FS_INITIAL (1 << 13)
#define RS_VS_INITIAL (1 << 9)
#define RS_FP_CSR_OFFSET 256

#ifndef __ASSEMBLER__
#include <stdint.h>

bool context_init(void);
uint64_t context_reset(unsigned slot);
void context_select(unsigned slot);
void context_fork(unsigned parent, unsigned child);
void context_drop(unsigned slot);
#endif
#endif
