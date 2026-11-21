"""Replace the image of a running process with another disk ELF in the same slot."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

from reference import run_reference
from serial_session import command_output
from vfs_vm_tests import ROOT, boot, compile_guest, expect, stats


CHILD_EXEC = r"""
#include "user/api.h"
#include "user/layout.h"

void program_entry(void)
{
    const char file[] = "/stream";
    for (long fd = 3; fd <= 6; ++fd) {
        if (user_open(file, sizeof(file) - 1) != fd) { user_exit(1); }
    }
    char first;
    if (user_read(3, &first, 1) != 1 || first != 'x') { user_exit(2); }
    char *region = user_map(2 * USER_PAGE_SIZE);
    if (!region) { user_exit(3); }
    region[0] = 'P';
    char *path = region + USER_PAGE_SIZE - 3;
    const char name[] = "/callee";
    for (unsigned i = 0; i < sizeof(name) - 1; ++i) { path[i] = name[i]; }
    long child = user_fork();
    if (child < 0) { user_exit(4); }
    if (child == 0) {
        /* Failure must preserve the caller, its anonymous mappings and descriptors. */
        const char invalid[] = {'/', 'a', '\0', 'b'};
        if (user_exec(invalid, sizeof(invalid)) != -1 || user_exec("/missing", 8) != -1 ||
            user_exec("/proc", 5) != -1 || user_exec("/stream", 7) != -1 ||
            user_exec((const char *)(uintptr_t)-1, 2) != -1 ||
            user_exec(path, 0) != -1 || user_exec(path, 128) != -1 ||
            region[0] != 'P') { user_exit(5); }
        user_exec(path, sizeof(name) - 1);
        user_exit(6);
    }
    if (user_wait(child) != 73 || region[0] != 'P' || user_read(3, &first, 1) != 1 ||
        first != 'z' || user_close(4) != 0) { user_exit(7); }
    user_exit(0);
}
"""

# Observe the initial registers before a compiler prologue can change them.
CALLEE = "\n".join(f"    bnez x{reg}, failed" for reg in range(1, 32) if reg != 2) + """
    li t0, 0x40004000
    bne sp, t0, failed
    ld t1, -8(sp)
    bnez t1, failed
    li a0, 0x40040000
    call user_unmap
    li t0, -1
    bne a0, t0, failed
    li a0, 3
    addi a1, sp, -8
    li a2, 1
    call user_read
    li t0, 1
    bne a0, t0, failed
    lbu t1, -8(sp)
    li t0, 'y'
    bne t1, t0, failed
    li a0, 4
    call user_close
    bnez a0, failed
    li a0, 73
    tail user_exit
failed:
    li a0, 99
    tail user_exit
"""

# Once the parent has replaced its image, only the child's reservation remains.
# It can now exercise code and read-only data originally backed by the parent slot.
PARENT_EXEC = r"""
#include "user/api.h"

static bool parent_replaced(void)
{
    static const char marker[] = "reserved_pages 1\n";
    char buf[128];
    long fd = user_open("/proc/meminfo", 13);
    long n = user_read(fd, buf, sizeof(buf));
    if (n <= 0 || user_close(fd) != 0) { user_exit(8); }
    for (long i = 0; i <= n - (long)(sizeof(marker) - 1); ++i) {
        size_t j = 0;
        while (j < sizeof(marker) - 1 && buf[i + j] == marker[j]) { ++j; }
        if (j == sizeof(marker) - 1) { return true; }
    }
    return false;
}

void program_entry(void)
{
    volatile char *page = user_map(1);
    if (!page) { user_exit(1); }
    *page = 'K';
    long child = user_fork();
    if (child < 0) { user_exit(2); }
    if (child > 0) {
        user_exec("/waiter", 7);
        user_exit(3);
    }
    while (!parent_replaced()) { user_yield(); }
    static const char line[] = "child kept original image\n";
    if (*page != 'K' || user_write(line, sizeof(line) - 1) != (long)(sizeof(line) - 1)) {
        user_exit(4);
    }
    user_exit(29);
}
"""

WAITER = """
#include "user/api.h"
void program_entry(void)
{
    /* This is the first fork on a freshly booted disk. */
    user_exit(user_wait(1) == 29 ? 0 : 9);
}
"""

SELF_EXEC = """
#include "user/api.h"
static volatile unsigned initialized = 17, zero;
void program_entry(void)
{
    char byte;
    if (initialized != 17 || zero != 0) { user_exit(1); }
    initialized = zero = 99;
    if (user_read(3, &byte, 1) == 1) {
        user_exit(byte == 'y' ? 42 : 2);
    }
    if (user_open("/stream", 7) != 3 || user_read(3, &byte, 1) != 1 || byte != 'x') {
        user_exit(3);
    }
    user_exec("/self", 5);
    user_exit(4);
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    runner = parser.add_mutually_exclusive_group(required=True)
    runner.add_argument("vm", type=Path, nargs="?", help="native interpreter")
    runner.add_argument("--qemu", type=Path, help="QEMU RVA23S64 reference executable")
    parser.add_argument("firmware", type=Path)
    parser.add_argument("disk", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory)
        disk = temp / "exec.img"
        stream = temp / "stream"
        stream.write_bytes(b"xyz")
        pack = [sys.executable, ROOT / "tools/mkdisk.py", args.disk.with_name("kernel.bin"),
                disk, "--file", f"stream={stream}"]
        for name, source in (("caller", CHILD_EXEC), ("callee", CALLEE), ("parent", PARENT_EXEC),
                             ("waiter", WAITER), ("self", SELF_EXEC)):
            elf = compile_guest(temp, name, source, assembly=name == "callee")
            pack.extend(["--file", f"{name}={elf}"])
        subprocess.run(pack, check=True)
        for name, expected in (
                (b"caller", b"child exit: 73\nuser exit: 0\n"),
                (b"parent", b"child kept original image\nchild exit: 29\nuser exit: 0\n"),
                (b"self", b"user exit: 42\n")):
            command = b"run " + name
            commands = command + b"\ncat /proc/meminfo\n"
            if args.qemu:
                output = run_reference(args.qemu, args.firmware, disk, commands + b"halt\n")
                output = output.replace(b"\r\n", b"\n")
            else:
                output = boot(args, disk, commands)
            expect(name, command_output(output, command), expected)
            expect("resources after exec and exit", stats(output)[0][:3], (32, 0, 0))


if __name__ == "__main__":
    main()
