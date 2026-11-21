"""Exercise the complete reset -> disk -> supervisor kernel path."""
import argparse
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path

from user_session import COMMANDS, check_user_session
from task_session import TASK_COMMANDS, check_task_session
from file_session import FILE_COMMANDS, check_file_session


def run_guest(vm, firmware, disk, commands, *, max_steps=6_000_000):
    process = subprocess.run(
        [vm, "--firmware", firmware, "--disk", disk, "--max-steps", str(max_steps)],
        input=commands, capture_output=True, timeout=15,
    )
    return process.returncode, process.stdout.replace(b"\r\n", b"\n"), process.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("case", choices=("console", "user", "tasks", "files", "ring-wrap", "bad-header", "bad-crc",
                                         "bad-size", "short-disk"))
    parser.add_argument("vm", type=Path)
    parser.add_argument("firmware", type=Path)
    parser.add_argument("disk", type=Path)
    args = parser.parse_args()

    if args.case == "console":
        commands = (b"help\r\ninfo\necho hellx\x08o\r\nwhat\n"
                    + b"x" * 128 + b"\necho after overflow\nhalt\n")
        rc, output, error = run_guest(args.vm, args.firmware, args.disk, commands)
        expected = (b"RS firmware: kernel verified\n", b"RS OS ready.",
                    b"echo TEXT   print TEXT", b"RV64 supervisor mode | MMU: Sv39",
                    b"\nhello\nrs> ", b"unknown command: what\n", b"line too long\n",
                    b"\nafter overflow\nrs> ", b"System halted.\n")
        passed = rc == 0 and all(fragment in output for fragment in expected)
    elif args.case == "user":
        rc, output, error = run_guest(args.vm, args.firmware, args.disk, COMMANDS)
        if rc == 0:
            check_user_session(output)
        passed = rc == 0
    elif args.case == "tasks":
        rc, output, error = run_guest(args.vm, args.firmware, args.disk, TASK_COMMANDS + b"halt\n")
        if rc == 0:
            check_task_session(output, args.disk.with_name("user.elf"))
        passed = rc == 0
    elif args.case == "files":
        # Each ELF load reads sectors and checks the entire file's CRC in guest code.
        rc, output, error = run_guest(args.vm, args.firmware, args.disk, FILE_COMMANDS + b"halt\n",
                                      max_steps=10_000_000)
        if rc == 0:
            check_file_session(output, args.disk.parent)
        passed = rc == 0
    elif args.case == "ring-wrap":
        # Eight sectors per request, eight slots per ring. The extra 65 sectors
        # force a reused slot even if the original kernel becomes much smaller.
        image = args.disk.read_bytes()
        size, = struct.unpack_from("<I", image, 8)
        kernel = image[512:512 + size] + b"".join(bytes([i]) * 512 for i in range(1, 66))
        header = struct.pack("<8sII", b"RSDISK01", len(kernel), zlib.crc32(kernel))
        with tempfile.TemporaryDirectory() as tmp:
            disk = Path(tmp) / "many-sectors.img"
            disk.write_bytes(header.ljust(512, b"\0") + kernel + bytes(-len(kernel) % 512))
            rc, output, error = run_guest(args.vm, args.firmware, disk, b"halt\n")
        passed = rc == 0 and b"kernel verified" in output and b"System halted." in output
    else:
        image = bytearray(args.disk.read_bytes())
        if args.case == "bad-header":
            image[0] ^= 1
            expected = b"invalid disk magic"
        elif args.case == "bad-crc":
            image[512] ^= 1
            expected = b"kernel CRC mismatch"
        elif args.case == "bad-size":
            struct.pack_into("<I", image, 8, 0xffffffff)
            expected = b"invalid kernel size"
        else:
            del image[512:]
            expected = b"truncated kernel"
        with tempfile.TemporaryDirectory() as tmp:
            disk = Path(tmp) / "broken.img"
            disk.write_bytes(image)
            rc, output, error = run_guest(args.vm, args.firmware, disk, b"halt\n")
        passed = rc == 1 and b"boot error: " + expected in output and b"RS OS ready" not in output
    if not passed:
        raise SystemExit(f"exit={rc}\nserial={output!r}\nstderr={error!r}")


if __name__ == "__main__":
    main()
