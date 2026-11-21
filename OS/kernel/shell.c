#include "platform.h"
#include "paging.h"
#include "trap.h"

#include <stddef.h>

static bool equal(const char *left, const char *right)
{
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

static bool is_space(char ch)
{
    return ch == ' ' || ch == '\t';
}

static void execute(char *line)
{
    while (is_space(*line)) {
        ++line;
    }
    if (!*line) {
        return;
    }
    char *argument = line;
    while (*argument && !is_space(*argument)) {
        ++argument;
    }
    if (*argument) {
        *argument++ = '\0';
        while (is_space(*argument)) {
            ++argument;
        }
    }
    if (equal(line, "help")) {
        uart_puts("help        list commands\n"
                  "echo TEXT   print TEXT\n"
                  "info        show boot and console details\n"
                  "ticks       show timer interrupt count\n"
                  "user        run the user-mode program\n"
                  "user fault  demonstrate user-mode memory protection\n"
                  "tasks MODE  run two tasks: yield, preempt, or fault\n"
                  "halt        shut down the machine\n");
    } else if (equal(line, "echo")) {
        uart_puts(argument);
        uart_puts("\n");
    } else if (equal(line, "info")) {
        uart_puts("RS OS | RV64 supervisor mode | MMU: Sv39 | boot: virtio-blk | console: 16550 UART\n");
    } else if (equal(line, "ticks")) {
        uart_puts("timer ticks: ");
        uart_put_u64(timer_ticks());
        uart_puts("\n");
    } else if (equal(line, "user")) {
        if (!*argument || equal(argument, "fault")) {
            run_user(*argument != '\0');
        } else {
            uart_puts("usage: user [fault]\n");
        }
    } else if (equal(line, "tasks")) {
        if (equal(argument, "yield")) {
            run_tasks(TASKS_YIELD);
        } else if (equal(argument, "preempt")) {
            run_tasks(TASKS_PREEMPT);
        } else if (equal(argument, "fault")) {
            run_tasks(TASKS_FAULT);
        } else {
            uart_puts("usage: tasks yield|preempt|fault\n");
        }
    } else if (equal(line, "halt")) {
        uart_puts("System halted.\n");
        poweroff(false);
    } else {
        uart_puts("unknown command: ");
        uart_puts(line);
        uart_puts("\n");
    }
}

[[noreturn]] void guest_main(void)
{
    char line[128];
    size_t length = 0;
    bool discard = false;
    bool after_cr = false;
    if (!paging_enable()) {
        uart_puts("kernel error: Sv39 unavailable\n");
        poweroff(true);
    }
    timer_start();
    uart_puts("RS OS ready. Type help.\nrs> ");
    for (;;) {
        char ch = uart_getc();
        if (after_cr && ch == '\n') {
            after_cr = false;
            continue;
        }
        after_cr = ch == '\r';
        if (ch == '\r' || ch == '\n') {
            uart_puts("\n");
            if (discard) {
                uart_puts("line too long\n");
            } else {
                line[length] = '\0';
                execute(line);
            }
            length = 0;
            discard = false;
            uart_puts("rs> ");
        } else if (discard) {
            continue;
        } else if (ch == '\b' || ch == 127) {
            if (length) {
                --length;
                uart_puts("\b \b");
            }
        } else if ((ch >= ' ' && ch <= '~') || ch == '\t') {
            if (length == sizeof(line) - 1) {
                discard = true;
                uart_putc('\a');
            } else {
                line[length++] = ch;
                uart_putc(ch);
            }
        }
    }
}
