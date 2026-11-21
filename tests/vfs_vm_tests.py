"""Run user memory accesses and VFS reads on both a standard and a replacement disk."""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from boot import run_guest
from serial_session import command_output


ROOT = Path(__file__).resolve().parents[1]
STATS = re.compile(rb"pool_pages (\d+)\nresident_pages (\d+)\nreserved_pages (\d+)\nfaults (\d+)\n")
DONE = b"memory.elf: file copied, pages released\nuser exit: 0\n"


def stats(output):
    return [tuple(map(int, match)) for match in STATS.findall(output)]


def expect(label, actual, expected):
    if actual != expected:
        raise AssertionError(f"{label}: expected {expected!r}, got {actual!r}")


def boot(args, disk, commands):
    rc, output, error = run_guest(args.vm, args.firmware, disk, commands + b"halt\n")
    if rc or b"System halted.\n" not in output:
        raise AssertionError(f"exit={rc}\nserial={output!r}\nstderr={error!r}")
    return output


def standard_disk(args):
    commands = b"cat /proc/meminfo\nls /proc\ncat /readme.txt\nrun /memory.elf\ncat /proc/meminfo\n"
    serial = boot(args, args.disk, commands)
    expect("proc directory", command_output(serial, b"ls /proc"), b"meminfo\ndiskstats\n")
    readme = command_output(serial, b"cat /readme.txt")
    expect("readme copy", command_output(serial, b"run /memory.elf"), readme + DONE)
    # Two U loads touch fresh anonymous mappings. The adjacent page for read() is
    # backed by copyout in S mode and does not increment the U page fault counter.
    expect("memory.elf frame accounting", stats(serial), [(32, 0, 0, 0), (32, 0, 0, 2)])


# Open meminfo before reserving pages. Read the first part, change VM state,
# then finish reading the same descriptor. A new descriptor sees the new state.
SNAPSHOT = """
    addi sp, sp, -128
    la a0, path
    li a1, 13
    call user_open
    mv s0, a0
    mv a1, sp
    li a2, 8
    call user_read
    mv a1, a0
    mv a0, sp
    call user_write
    li a0, 8192
    call user_map
    mv s1, a0
    li t0, 77
    sb t0, 0(s1)
    mv a0, s0
    mv a1, sp
    li a2, 128
    call user_read
    mv a1, a0
    mv a0, sp
    call user_write
    la a0, path
    li a1, 13
    call user_open
    mv a1, sp
    li a2, 128
    call user_read
    mv a1, a0
    mv a0, sp
    call user_write
    lbu a0, 0(s1)
    call user_exit
    .section .rodata
path: .ascii "/proc/meminfo"
"""

# The second mapping is still live when the old address faults. Fault teardown
# must return its frame to the pool and discard both file and task state.
AFTER_UNMAP = """
    li a0, 4096
    call user_map
    mv s0, a0
    sb zero, 0(s0)
    li a0, 4096
    call user_map
    sb zero, 0(a0)
    mv a0, s0
    call user_unmap
    .globl fault_site
fault_site:
    lbu a0, 0(s0)
    call user_exit
"""

# Reservation can exceed the frame pool. The 33rd store reaches a valid VMA
# but cannot acquire a frame, so it terminates this task at that instruction.
EXHAUST = """
    li a0, 33 * 4096
    call user_map
    li t0, 33
    li t1, 4096
    .globl fault_site
fault_site:
    sb zero, 0(a0)
    add a0, a0, t1
    addi t0, t0, -1
    bnez t0, fault_site
    li a0, 1
    call user_exit
"""


BOUNDARY_C = """
#include "user/api.h"
#include "user/layout.h"

[[noreturn]] void program_entry(void)
{
    static const char name[] = "/readme.txt";
    long fd[4];
    for (unsigned i = 0; i < 4; ++i) {
        fd[i] = user_open(name, sizeof(name) - 1);
        if (fd[i] < 0) {
            user_exit(1);
        }
    }
    if (user_open(name, sizeof(name) - 1) != -1 || user_close(fd[2]) != 0 ||
        user_open(name, sizeof(name) - 1) != fd[2]) {
        user_exit(2);
    }
    uint8_t *page = user_map(USER_PAGE_SIZE);
    if (!page || user_map(0) || user_unmap(page + 1) != -1) {
        user_exit(3);
    }
    uint8_t *tail = page + USER_PAGE_SIZE - 1;
    *(volatile uint8_t *)tail = 0x5a;
    /* The second byte is outside the mapping. Neither the first byte nor the
       descriptor offset may change on this failed read. */
    if (user_read(fd[0], tail, 2) != -1 || *(volatile uint8_t *)tail != 0x5a ||
        user_read(fd[0], page, 2) != 2 || page[0] != 'A' || page[1] != 'B') {
        user_exit(4);
    }
    void *hole = user_map(1);
    void *neighbor = user_map(1);
    if (!hole || !neighbor || user_unmap(hole) != 0 ||
        user_unmap(hole) != -1 || user_map(1) != hole) {
        user_exit(5);
    }
    user_exit(0); /* Leave all four descriptors and three mappings to task teardown. */
}
"""


def compile_guest(temp, name, body, *, assembly=True):
    source = temp / (name + (".S" if assembly else ".c"))
    elf = temp / (name + ".elf")
    if assembly:
        body = ".text\n.globl program_entry\nprogram_entry:\n" + body
    source.write_text(body, encoding="ascii")
    subprocess.run(["clang", "--target=riscv64-unknown-elf", "-march=rv64imac", "-mabi=lp64",
                    "-mcmodel=medany", "-mno-relax", "-nostdlib", "-fuse-ld=lld", "-std=c23",
                    "-ffreestanding", "-Os", "-msmall-data-limit=0",
                    "-Wl,--defsym=APP_BASE=0x40010000", f"-Wl,-T,{ROOT / 'OS/user/disk.ld'}",
                    f"-I{ROOT / 'OS'}", str(source), str(ROOT / "OS/user/syscall.S"),
                    "-o", str(elf)], check=True)
    return elf


def fault_at(elf, cause, address):
    symbols = subprocess.check_output(["llvm-nm", "--format=posix", str(elf)], text=True)
    fields = next(line.split() for line in symbols.splitlines() if line.startswith("fault_site "))
    pc = int(fields[2], 16)
    return f"user fault: cause={cause} epc=0x{pc:016x} tval=0x{address:016x}\n".encode()


def replacement_disk(args, temp):
    images = args.disk.parent
    disk = temp / "replacement.img"
    readme = temp / "text"
    data = bytes(range(65, 91)) * 6 + b"!\n"
    readme.write_bytes(data)
    snapshot = compile_guest(temp, "snapshot", SNAPSHOT)
    unmapped = compile_guest(temp, "after-unmap", AFTER_UNMAP)
    exhausted = compile_guest(temp, "exhaust", EXHAUST)
    boundary = compile_guest(temp, "boundary", BOUNDARY_C, assembly=False)
    subprocess.run([sys.executable, ROOT / "tools/mkdisk.py", images / "kernel.bin", disk,
                    "--file", f"readme.txt={readme}", "--file", f"memory.elf={images / 'memory.elf'}",
                    "--file", f"snap={snapshot}", "--file", f"stale={unmapped}",
                    "--file", f"exhaust={exhausted}", "--file", f"boundary={boundary}"], check=True)
    serial = boot(args, disk, b"run snap\ncat /proc/meminfo\nrun stale\ncat /proc/meminfo\n"
                             b"run exhaust\ncat /proc/meminfo\nrun boundary\nrun boundary\n"
                             b"cat /proc/meminfo\nrun memory.elf\ncat /proc/meminfo\n")
    snap = command_output(serial, b"run snap")
    expect("post-fault store", snap.splitlines()[-1:], [b"user exit: 77"])
    expect("read after unmap", command_output(serial, b"run stale"), fault_at(unmapped, 13, 0x40040000))
    expect("frame pool exhausted", command_output(serial, b"run exhaust"), fault_at(exhausted, 15, 0x40060000))
    expect("descriptor reuse across exits", serial.count(b"rs> run boundary\nuser exit: 0\n"), 2)
    expect("replacement file", command_output(serial, b"run memory.elf"), data + DONE)
    expect("snapshot and teardown accounting", stats(serial),
           [(32, 0, 0, 0), (32, 1, 2, 1), (32, 0, 0, 1), (32, 0, 0, 3),
            (32, 0, 0, 35), (32, 0, 0, 37), (32, 0, 0, 39)])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("vm", "firmware", "disk"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    standard_disk(args)
    with tempfile.TemporaryDirectory() as directory:
        replacement_disk(args, Path(directory))


if __name__ == "__main__":
    main()
