#include "platform.h"
#include "vfs.h"
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

static const char *list_files(const char *argument)
{
    VfsDir directory;
    const char *error = vfs_opendir(*argument ? argument : "/", &directory);
    if (error) {
        return error;
    }
    VfsEntry entry;
    while (vfs_readdir(&directory, &entry)) {
        uart_puts(entry.name);
        if (entry.directory) {
            uart_puts("/");
        } else if (entry.size != SIZE_MAX) {
            uart_puts("  ");
            uart_put_u64(entry.size);
            uart_puts(" bytes");
        }
        uart_puts("\n");
    }
    return nullptr;
}

static const char *print_file(VfsFile *file)
{
    uint8_t chunk[128];
    uint8_t previous = 0;
    for (;;) {
        size_t size;
        const char *error = vfs_read(file, chunk, sizeof(chunk), &size);
        if (error || !size) {
            return error;
        }
        for (size_t i = 0; i < size; ++i) {
            if (chunk[i] == '\n' && previous != '\r') {
                uart_putc('\r');
            }
            uart_putc((char)chunk[i]);
            previous = chunk[i];
        }
    }
}

static const char *file_command(const char *command, const char *name)
{
    if (equal(command, "ls")) {
        return list_files(name);
    }
    if (equal(command, "run")) {
        return run_elf(name);
    }
    if (!*name) {
        return "usage: cat NAME";
    }
    VfsFile file;
    const char *error = vfs_open(name, &file);
    return error ? error : print_file(&file);
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
                  "ls [PATH]   list a directory\n"
                  "cat PATH    print a file\n"
                  "run PATH [ARG ...]  load and execute an ELF\n"
                  "ticks       show timer interrupt count\n"
                  "user        run the user-mode program\n"
                  "user fault  demonstrate user-mode memory protection\n"
                  "tasks MODE  run two tasks: yield, preempt, fault, or memory\n"
                  "halt        shut down the machine\n");
    } else if (equal(line, "echo")) {
        uart_puts(argument);
        uart_puts("\n");
    } else if (equal(line, "info")) {
        uart_puts("RS OS | RV64 supervisor mode | MMU: Sv39 | boot: virtio-blk | console: 16550 UART\n");
    } else if (equal(line, "ls") || equal(line, "cat") || equal(line, "run")) {
        const char *error = file_command(line, argument);
        if (error) {
            uart_puts(line);
            uart_puts(": ");
            uart_puts(error);
            uart_puts("\n");
        }
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
        } else if (equal(argument, "memory")) {
            run_tasks(TASKS_MEMORY);
        } else {
            uart_puts("usage: tasks yield|preempt|fault|memory\n");
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
    vfs_mount();
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
