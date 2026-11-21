"""Check Zicbom in U mode, batched virtio reads and procfs snapshots on one disk."""
import argparse
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

from boot import run_guest
from elf_tests import executable
from reference import run_reference
from rsfs_tests import full_cache_elf
from serial_session import command_output
from vfs_vm_tests import ROOT, compile_guest, expect


METRICS = rb"read_requests (\d+)\nread_sectors (\d+)\nread_errors (\d+)\n"

# Read a prefix of diskstats. Loading "tail" changes the live counters while
# our descriptor still owns the original snapshot. Print both complete records.
SNAPSHOT = """
#include "user/api.h"
void program_entry(void)
{
    static const char path[] = "/proc/diskstats";
    char buf[128], byte;
    long fd = user_open(path, sizeof(path) - 1);
    if (user_read(fd, buf, 5) != 5) { user_exit(1); }
    long data = user_open("/tail", 5);
    if (user_read(data, &byte, 1) != 1 || byte != 0x7f || user_close(data)) { user_exit(2); }
    long n = user_read(fd, buf + 5, sizeof(buf) - 5);
    if (n <= 0 || user_close(fd) || user_write(buf, 5 + n) != 5 + n) { user_exit(3); }
    fd = user_open(path, sizeof(path) - 1);
    n = user_read(fd, buf, sizeof(buf));
    if (n <= 0 || user_close(fd) || user_write(buf, n) != n) { user_exit(4); }
    user_exit(0);
}
"""


def check_short_read(vm, firmware, temp, image):
    # Truncate only our private copy after the guest has read its capacity and
    # mounted the directory. The last file is "wide", with 11 sectors left:
    # one full batch succeeds, then the device transfers 3 of the next 8 sectors.
    scratch = temp / "short.img"
    scratch.write_bytes(image)
    with subprocess.Popen([vm, "--firmware", firmware, "--disk", scratch, "--max-steps", "8000000"],
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as proc:
        prefix = bytearray()
        try:
            while not prefix.endswith(b"rs> "):
                byte = proc.stdout.read(1)
                if not byte:
                    raise AssertionError(f"no initial prompt: {prefix!r}")
                prefix.extend(byte)
            with scratch.open("r+b") as stream:
                stream.truncate(len(image) - 65536 + 11 * 512)
            commands = b"run tail\nrun wide\ncat /proc/diskstats\nrun /tail\ncat //proc/diskstats\nhalt\n"
            output, err = proc.communicate(commands, timeout=15)
        finally:
            if proc.poll() is None:
                proc.kill()
    if proc.returncode != 0:
        raise AssertionError(f"short-read VM failed: {output!r} {err!r}")
    serial = (prefix + output).replace(b"\r\n", b"\n")
    expect("failed batch never supplies an ELF", command_output(serial, b"run wide"), b"run: file read failed\n")
    expect("previously cached program can be read again", command_output(serial, b"run /tail"),
           b"user exit: 41\n")
    expect("failed batch, then reloaded tail", re.findall(METRICS, serial),
           [(b"6", b"19", b"1"), (b"8", b"28", b"1")])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", action="store_true", help="use the RVA23S64 reference machine")
    for name in ("machine", "firmware", "disk"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory)
        # RX text has A=1, D=0. Use an unaligned pointer within this page and
        # execute INVAL, CLEAN, FLUSH before returning status 41.
        code = struct.pack("<8I", 0x00000297, 0x00328293, 0x0002a00f, 0x0012a00f,
                           0x0022a00f, 0x02900513, 0x00300893, 0x00000073)
        (temp / "tail").write_bytes(executable(code).ljust(4097, b"!"))
        (temp / "wide").write_bytes(full_cache_elf())
        (temp / "empty").write_bytes(b"")
        snapshot = compile_guest(temp, "snapshot", SNAPSHOT, assembly=False)
        disk = temp / "batches.img"
        subprocess.run([sys.executable, ROOT / "tools/mkdisk.py", args.disk.with_name("kernel.bin"),
                        disk, "--file", f"tail={temp / 'tail'}", "--file", f"empty={temp / 'empty'}",
                        "--file", f"snap={snapshot}", "--file", f"wide={temp / 'wide'}"], check=True)
        probe = b"cat /proc/diskstats\n"
        commands = (probe + b"cat empty\n" + probe + b"run tail\n" + probe + b"run /tail\n"
                    + probe + b"run wide\n" + probe + b"run tail\n" + probe
                    + b"run snap\n" + probe + b"halt\n")
        if args.qemu:
            serial = run_reference(args.machine, args.firmware, disk, commands).replace(b"\r\n", b"\n")
        else:
            rc, serial, err = run_guest(args.machine, args.firmware, disk, commands, max_steps=12_000_000)
            if rc:
                raise AssertionError(f"exit={rc}, serial={serial!r}, stderr={err!r}")
            check_short_read(args.machine, args.firmware, temp, disk.read_bytes())

    expect("U-mode cache ops on RX", command_output(serial, b"run tail"), b"user exit: 41\n")
    expect("cache hit repeats RX code", command_output(serial, b"run /tail"), b"user exit: 41\n")
    expect("all 128 sectors verified", command_output(serial, b"run wide"), b"user exit: 37\n")
    snapshots = [tuple(map(int, row)) for row in re.findall(b"rs> " + probe + METRICS, serial)]
    expect("empty, short tail, cache hit, ring wrap, reload", snapshots[:-1],
           [(2, 2, 0), (2, 2, 0), (4, 11, 0), (4, 11, 0), (20, 139, 0), (22, 148, 0)])
    body = command_output(serial, b"run snap")
    expect("snapshot program exit", body.splitlines()[-1], b"user exit: 0")
    records = [tuple(map(int, row)) for row in re.findall(METRICS, body)]
    expect("old and new handles", len(records), 2)
    before, after = records
    expect("old handle retains its suffix across I/O",
           tuple(b - a for a, b in zip(before, after)), (2, 9, 0))
    expect("fresh snapshot matches the shell", after, snapshots[-1])


if __name__ == "__main__":
    main()
