"""Boot real ELF programs to check fork, wait, inherited files and COW frame usage."""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from serial_session import command_output
from vfs_vm_tests import ROOT, boot, compile_guest, expect, stats


HEADER = """
#include "user/api.h"
#include "user/layout.h"
"""

MEMINFO = """
static long memory_info(char *buffer, size_t capacity)
{
    static const char path[] = "/proc/meminfo";
    long fd = user_open(path, sizeof(path) - 1);
    if (fd < 0) {
        user_exit(90);
    }
    long n = user_read(fd, buffer, capacity);
    if (n <= 0 || user_close(fd) != 0) {
        user_exit(91);
    }
    return n;
}
"""

ACCOUNTING = MEMINFO + """
#include <stdatomic.h>

static void show_memory(void)
{
    char buffer[128];
    long n = memory_info(buffer, sizeof(buffer));
    if (user_write(buffer, (size_t)n) != n) {
        user_exit(1);
    }
}

void program_entry(void)
{
    const size_t page = USER_PAGE_SIZE;
    volatile uint8_t *p = user_map(4 * page);
    long fd = user_open("/stream", 7);
    if (!p || fd < 0) { user_exit(2); }
    _Atomic uint32_t *counter = (_Atomic uint32_t *)p;
    atomic_store_explicit(counter, 11, memory_order_relaxed);
    p[page] = 22;
    p[2 * page] = 33;
    long child = user_fork();
    if (child < 0) { user_exit(3); }
    if (child == 0) {
        show_memory();                           /* 3 physical frames, 8 reserved pages */
        /* This AMO takes a COW store fault. Retrying it must add exactly once. */
        if (atomic_fetch_add_explicit(counter, 33, memory_order_relaxed) != 11 ||
            atomic_load_explicit(counter, memory_order_relaxed) != 44) { user_exit(7); }
        if (user_read(fd, (void *)(p + 2 * page - 1), 2) != 2 ||
            p[2 * page - 1] != 'x' || p[2 * page] != 'y') { user_exit(4); }
        show_memory();                           /* kernel copyout split two more pages */
        if (p[3 * page] != 0) { user_exit(5); } /* previously untouched in both processes */
        p[3 * page] = 55;
        show_memory();
        user_exit(12);
    }
    if (user_wait(child) != 12 || p[0] != 11 || p[page] != 22 ||
        p[2 * page - 1] != 0 || p[2 * page] != 33 || p[3 * page] != 0) { user_exit(6); }
    show_memory();
    user_exit(0);
}
"""

FULL_POOL = """
void program_entry(void)
{
    const size_t page = USER_PAGE_SIZE;
    volatile uint8_t *p = user_map(31 * page);
    long fd = user_open("/stream", 7);
    if (!p || fd < 0) { user_exit(1); }
    for (unsigned i = 0; i < 31; ++i) { p[i * page] = 37; }
    p[page - 1] = 98;
    long child = user_fork();
    if (child < 0) { user_exit(2); }
    if (child == 0) {
        /* Two shared pages must be split, but only one frame is available. */
        char first;
        if (user_read(fd, (void *)(p + page - 1), 2) != -1 || p[page - 1] != 98 ||
            p[page] != 37 || user_read(fd, &first, 1) != 1 || first != 'x') { user_exit(3); }
        volatile uint8_t *last = user_map(1);
        if (!last) { user_exit(4); }
        *last = 23;
        p[0] = 99; /* shared page, full pool: a store page fault */
        user_exit(5);
    }
    if (user_wait(child) != -(256 + 15) || p[0] != 37) { user_exit(6); }
    volatile uint8_t *last = user_map(1);
    if (!last) { user_exit(7); }
    *last = 42;
    p[0] = 55; /* full pool, one remaining owner: upgrade in place */
    if (user_read(fd, (void *)(p + page - 1), 2) != 2 || p[page - 1] != 'y' ||
        p[page] != 'z' || p[0] != 55) { user_exit(8); }
    user_exit(0);
}
"""

LIFETIME = """
void program_entry(void)
{
    if (user_wait(99) != -1) { user_exit(1); }
    long first = user_fork();
    if (first < 0) { user_exit(2); }
    if (first == 0) {
        if (user_fork() != -1 || user_wait(1) != -1) { user_exit(3); }
        user_exit(0xffffffffu);
    }
    user_yield(); /* allow child exit before wait */
    if (user_fork() != -1 || user_wait(first) != 0xffffffffL ||
        user_wait(first) != -1) { user_exit(4); }
    long next = user_fork();
    if (next == 0) { user_exit(18); }
    if (next < 0 || next == first || user_wait(first) != -1 ||
        user_wait(next) != 18) { user_exit(5); }
    user_exit(0);
}
"""

ORPHAN = """
void program_entry(void)
{
    volatile char *p = user_map(1);
    if (!p) { user_exit(1); }
    *p = 'K';
    long fd = user_open("/stream", 7);
    long child = user_fork();
    if (child < 0 || fd < 0) { user_exit(2); }
    if (child > 0) { user_exit(23); }
    uint64_t before = user_ticks();
    do { user_yield(); } while (user_ticks() == before);
    char ch;
    if (*p != 'K' || user_read(fd, &ch, 1) != 1 || ch != 'x') { user_exit(3); }
    user_exit(19);
}
"""

PREEMPT = MEMINFO + """
static bool parent_started(void)
{
    char text[128];
    long n = memory_info(text, sizeof(text));
    static const char marker[] = "reserved_pages 3\\n";
    for (long i = 0; i + (long)sizeof(marker) - 1 <= n; ++i) {
        size_t j = 0;
        while (j < sizeof(marker) - 1 && text[i + j] == marker[j]) { ++j; }
        if (j == sizeof(marker) - 1) { return true; }
    }
    return false;
}

void program_entry(void)
{
    volatile char *value = user_map(1);
    if (!value) { user_exit(1); }
    *value = 'B';
    long child = user_fork();
    if (child < 0) { user_exit(1); }
    if (child == 0) {
        while (!parent_started()) { user_yield(); }
        if (*value != 'B') { user_exit(5); }
        *value = 'C';
        user_write("child scheduled\\n", 16);
        user_exit(24);
    }
    user_write("parent computing\\n", 17);
    *value = 'P'; /* The child observes the pre-fork byte after this store. */
    if (!user_map(1)) { user_exit(2); }
    volatile uint32_t acc = 0;
    for (uint32_t i = 1; i <= 100000; ++i) { acc += i; }
    if (acc != 705082704u || *value != 'P') { user_exit(3); }
    user_write("parent done\\n", 12);
    user_exit(user_wait(child) == 24 ? 0 : 4);
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("vm", "firmware", "disk"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory)
        stream = temp / "stream"
        stream.write_bytes(b"xyz")
        disk = temp / "fork.img"
        pack = [sys.executable, ROOT / "tools/mkdisk.py", args.disk.with_name("kernel.bin"),
                disk, "--file", f"stream={stream}"]
        for name, body in (("account", ACCOUNTING), ("pool", FULL_POOL), ("life", LIFETIME),
                           ("orphan", ORPHAN), ("preempt", PREEMPT)):
            elf = compile_guest(temp, name, HEADER + body, assembly=False)
            pack.extend(["--file", f"{name}={elf}"])
        subprocess.run(pack, check=True)
        serial = boot(args, disk, b"run account\nrun pool\nrun life\nrun orphan\ncat /proc/meminfo\n")
        preempt = boot(args, disk, b"run preempt\n")

    account = command_output(serial, b"run account")
    expect("COW physical frames and virtual reservations",
           [item[:3] for item in stats(account)], [(32, 3, 8), (32, 6, 8), (32, 7, 8), (32, 4, 4)])
    expect("parent after isolated child writes", account.splitlines()[-1], b"user exit: 0")
    pool = command_output(serial, b"run pool")
    if not re.fullmatch(rb"child fault: cause=15 epc=0x[0-9a-f]+ tval=0x0000000040040000\nuser exit: 0\n", pool):
        raise AssertionError(f"unexpected full-pool outcome: {pool!r}")
    expect("child-first reaping and slot reuse", command_output(serial, b"run life"),
           b"child exit: 4294967295\nchild exit: 18\nuser exit: 0\n")
    expect("parent exits first", command_output(serial, b"run orphan"), b"user exit: 23\nchild exit: 19\n")
    expect("all anonymous frames released", stats(command_output(serial, b"cat /proc/meminfo"))[0][:3],
           (32, 0, 0))
    body = command_output(preempt, b"run preempt")
    events = [line for line in body.splitlines() if line in (b"parent computing", b"child scheduled", b"parent done")]
    expect("runnable child preempts computing parent", events, [b"parent computing", b"child scheduled", b"parent done"])
    expect("parent waited for preempting child", body.splitlines()[-1], b"user exit: 0")


if __name__ == "__main__":
    main()
