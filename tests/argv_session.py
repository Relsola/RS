"""Guest fixtures for argument passing on the native and reference machines."""

# Inspect the entry stack before a compiler prologue can move sp. Print one line
# per argument so an empty string is distinguishable from the end of the vector.
SHOW = """
    bne sp, a1, failed
    andi t0, sp, 15
    bnez t0, failed
    li t0, 0x40003000
    bltu sp, t0, failed
    li s3, 0x40004000
    bgeu sp, s3, failed
    li t0, 8
    bltu t0, a0, failed
    mv s0, a1
    slli t0, a0, 3
    add s1, a1, t0
    addi s2, s1, 8
    bltu s3, s2, failed
    ld t0, 0(s1)
    bnez t0, failed
next_arg:
    beq s0, s1, done
    ld a0, 0(s0)
    bltu a0, s2, failed
    mv t0, a0
next_byte:
    bgeu t0, s3, failed
    lbu t1, 0(t0)
    addi t0, t0, 1
    bnez t1, next_byte
    sub a1, t0, a0
    addi a1, a1, -1
    call user_write
    la a0, newline
    li a1, 1
    call user_write
    addi s0, s0, 8
    j next_arg
done:
    li a0, 0
    tail user_exit
failed:
    li a0, 99
    tail user_exit
    .section .rodata
newline: .byte 10
"""

EXECV = r"""
#include "user/api.h"
#include "user/layout.h"

static long snapshot(char *buffer)
{
    long fd = user_open("/proc/meminfo", 13);
    long size = user_read(fd, buffer, 128);
    if (size <= 0 || user_close(fd) != 0) { user_exit(1); }
    return size;
}

static void check_rejections(char **vector, char *lazy, char *end)
{
    char before[128], after[128];
    long size = snapshot(before);
    char *extra[10];
    for (unsigned i = 0; i < 9; ++i) { extra[i] = lazy; }
    extra[9] = nullptr;
    char *invalid[] = {lazy, (char *)UINTPTR_MAX, nullptr};
    if (user_execv("/show", 5, nullptr) != -1 ||
        user_execv("/show", 5, invalid) != -1 ||
        user_execv("/show", 5, extra) != -1 ||
        user_execv("/show", 5, vector) != -1) { user_exit(2); }

    /* Both an unterminated table and an unterminated string reach the guard page. */
    char **last = (char **)(end - sizeof(char *));
    *last = lazy;
    if (user_execv("/show", 5, last) != -1) { user_exit(3); }
    end[-1] = 'x';
    invalid[1] = end - 1;
    if (user_execv("/show", 5, invalid) != -1) { user_exit(4); }
    if (snapshot(after) != size) { user_exit(5); }
    for (long i = 0; i < size; ++i) {
        if (before[i] != after[i]) { user_exit(6); }
    }
}

void program_entry(int argc, char **argv)
{
    if (argc == 2) {
        /* The untouched reservation supplies a readable NULL terminator. */
        user_execv("/show", 5, user_map(USER_PAGE_SIZE));
        user_exit(7);
    }
    if (argc || argv) { user_exit(8); }
    char *pages = user_map(6 * USER_PAGE_SIZE);
    if (!pages) { user_exit(9); }
    char **vector = (char **)(pages + USER_PAGE_SIZE - sizeof(char *));
    char *word = pages + 3 * USER_PAGE_SIZE - 120;
    char *lazy = pages + 4 * USER_PAGE_SIZE;
    char *end = pages + 6 * USER_PAGE_SIZE;
    for (unsigned i = 0; i < 249; ++i) { word[i] = 'z'; }
    word[249] = 0;
    vector[0] = word;
    for (unsigned i = 1; i < 8; ++i) { vector[i] = lazy; }
    vector[8] = nullptr;
    end[-1] = 'x'; /* Make the guard's neighbour resident before taking the snapshot. */
    check_rejections(vector, lazy, end);
    word[248] = 0; /* Now exactly eight strings occupying 256 bytes including NULs. */
    for (unsigned i = 63; i <= 128; ++i) { pages[i] = 'P'; }
    long child = user_fork();
    if (child < 0) { user_exit(10); }
    if (child == 0) {
        /* CBO.ZERO must split the COW page and retry before execv reads its vector. */
        __asm__ volatile (".option push\n.option arch,+zicboz\ncbo.zero (%0)\n.option pop"
                          : : "r"(pages + 95) : "memory");
        for (unsigned i = 63; i <= 128; ++i) {
            if (pages[i] != (i == 63 || i == 128 ? 'P' : 0)) { user_exit(11); }
        }
        user_execv("/show", 5, vector);
        user_exit(12);
    }
    if (user_wait(child) != 0) { user_exit(13); }
    for (unsigned i = 63; i <= 128; ++i) {
        if (pages[i] != 'P') { user_exit(14); }
    }
    user_exit(0);
}
"""


def argument_cases():
    return (
        (b'''show  'two words'\t"" a'bc' "x'y"''', b"show\ntwo words\n\nabc\nx'y\nuser exit: 0\n"),
        (b'show "unfinished', b"run: unclosed quote\n"),
        (b'""', b"run: usage: run PATH [ARG ...]\n"),
        (b"show 1 2 3 4 5 6 7 8", b"run: too many arguments\n"),
        (b"execv empty", b"user exit: 0\n"),
        (b"execv", b"z" * 248 + b"\n" * 8 + b"child exit: 0\nuser exit: 0\n"),
        (b"wc /words", b"1 3 133 /words\nuser exit: 0\n"),
        (b"wc", b"usage: wc.elf PATH\nuser exit: 2\n"),
        (b"wc /absent", b"wc: cannot open file\nuser exit: 1\n"),
    )
