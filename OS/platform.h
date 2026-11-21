#ifndef RS_PLATFORM_H
#define RS_PLATFORM_H

#define RS_CACHE_BLOCK_BYTES 64

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>

enum {
    SECTOR_BYTES = 512,
    KERNEL_MAX_BYTES = 8 * 1024 * 1024,
};

void uart_putc(char ch);
void uart_puts(const char *text);
void uart_put_hex(uint64_t value);
void uart_put_u64(uint64_t value);
char uart_getc(void);
[[noreturn]] void poweroff(bool failed);
[[noreturn]] void handle_trap(uint64_t cause, uint64_t pc, uint64_t value);

/* 设备顺序与取指同步由 RISC-V 汇编实现。 */
void io_fence(void);
void dma_clean(const volatile void *start, size_t bytes);
void dma_flush(const volatile void *start, size_t bytes);
void dma_inval(const volatile void *start, size_t bytes);
[[noreturn]] void enter_kernel(uintptr_t address);

#endif
#endif
