#include "platform.h"

static volatile uint8_t *const uart = (volatile uint8_t *)UINT64_C(0x10000000);

enum {
    UART_DATA = 0,
    UART_LINE_STATUS = 5,
    UART_DATA_READY = 1,
    UART_TX_EMPTY = 1 << 5,
};

void uart_putc(char ch)
{
    while ((uart[UART_LINE_STATUS] & UART_TX_EMPTY) == 0) {
    }
    uart[UART_DATA] = (uint8_t)ch;
}

void uart_puts(const char *text)
{
    while (*text) {
        if (*text == '\n') {
            uart_putc('\r');
        }
        uart_putc(*text++);
    }
}

char uart_getc(void)
{
    while ((uart[UART_LINE_STATUS] & UART_DATA_READY) == 0) {
    }
    return (char)uart[UART_DATA];
}

[[noreturn]] void poweroff(bool failed)
{
    volatile uint32_t *const finisher = (volatile uint32_t *)UINT64_C(0x100000);
    io_fence();
    *finisher = failed ? UINT32_C(0x13333) : UINT32_C(0x5555);
    for (;;) {
    }
}

void uart_put_hex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    uart_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        uart_putc(digits[(value >> shift) & 15]);
    }
}

void uart_put_u64(uint64_t value)
{
    char digits[20];
    unsigned used = 0;
    do {
        digits[used++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (used) {
        uart_putc(digits[--used]);
    }
}

[[noreturn]] void handle_trap(uint64_t cause, uint64_t pc, uint64_t value)
{
#ifdef RS_SUPERVISOR
    uart_puts("S trap: scause=");
#else
    uart_puts("M trap: mcause=");
#endif
    uart_put_hex(cause);
    uart_puts(" epc=");
    uart_put_hex(pc);
    uart_puts(" tval=");
    uart_put_hex(value);
    uart_puts("\n");
    poweroff(true);
}
