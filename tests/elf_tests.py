"""Boot one kernel with replaceable ELF files and observe each load from its shell."""
import argparse
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from boot import run_guest
from serial_session import command_output


BASE = 0x40010000
ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
PROGRAM_HEADER = struct.Struct("<IIQQQQQQ")


def executable(code, *, flags=5, entry=None, memory_size=None, segment_type=1):
    # A static ELF with one page-aligned mapping containing its headers and RV64 code.
    offset = ELF_HEADER.size + PROGRAM_HEADER.size
    size = offset + len(code)
    ident = b"\x7fELF\x02\x01\x01".ljust(16, b"\0")
    header = ELF_HEADER.pack(ident, 2, 243, 1, BASE + offset if entry is None else entry,
                             ELF_HEADER.size, 0, 0, ELF_HEADER.size, PROGRAM_HEADER.size, 1, 0, 0, 0)
    segment = PROGRAM_HEADER.pack(segment_type, flags, 0, BASE, BASE, size,
                                  size if memory_size is None else memory_size, 4096)
    return header + segment + code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("vm", "firmware", "disk"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    # li a0,37 / li a7,3 (exit) / ecall. This program exists only on the replacement disk.
    exit_code = struct.pack("<III", 0x02500513, 0x00300893, 0x00000073)
    # auipc t0,0 / sw zero,0(t0). A write to the current RX code page must fault.
    write_text = struct.pack("<II", 0x00000297, 0x0002a023)
    fixtures = {
        "exit37": executable(exit_code),
        "readonly": executable(write_text),
        "bad-entry": executable(exit_code, entry=BASE + 4096),
        "rwx": executable(exit_code, flags=7),
        "too-large": executable(exit_code, memory_size=2**64 - 1),
        "short-segment": executable(exit_code, memory_size=1),
        "dynamic": executable(exit_code, segment_type=2),
        "truncated": executable(exit_code)[:80],
    }
    for name, abi in (("single-abi", 2), ("double-abi", 4), ("quad-abi", 6), ("rve-abi", 8)):
        file = bytearray(executable(exit_code))
        struct.pack_into("<I", file, 48, abi)
        fixtures[name] = file
    images = args.disk.parent
    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory)
        disk = temp / "replaceable.img"
        packer = Path(__file__).resolve().parents[1] / "tools/mkdisk.py"
        pack = [sys.executable, str(packer), images / "kernel.bin", disk,
                "--file", f"hello={images / 'hello.elf'}"]
        for name, content in fixtures.items():
            path = temp / name
            path.write_bytes(content)
            pack.extend(["--file", f"{name}={path}"])
        subprocess.run(pack, check=True)
        # Run hello twice around other images and rejected loads to exercise remapping and BSS.
        names = ["hello", *fixtures, "hello"]
        commands = b"".join(b"run " + name.encode() + b"\n" for name in names)
        rc, serial, error = run_guest(args.vm, args.firmware, disk, commands + b"halt\n")
    if rc or b"System halted.\n" not in serial:
        raise AssertionError(f"guest exit {rc}: {serial!r} {error!r}")
    hello = b"hello from disk ELF\nuser exit: 0\n"
    if serial.count(b"rs> run hello\n" + hello + b"rs> ") != 2:
        raise AssertionError(f"expected two fresh loads of hello: {serial!r}")
    expected = {
        "exit37": "user exit: 37",
        "single-abi": "user exit: 37",
        "double-abi": "user exit: 37",
        "quad-abi": "run: expected a static RISC-V ELF64 executable",
        "rve-abi": "run: expected a static RISC-V ELF64 executable",
        "readonly": "user fault: cause=15 epc=0x000000004001007c tval=0x0000000040010078",
        "bad-entry": "run: ELF entry is not an aligned instruction in executable data",
        "rwx": "run: unsupported ELF segment permissions",
        "too-large": "run: ELF segment outside available user pages",
        "short-segment": "run: invalid ELF segment layout",
        "dynamic": "run: unsupported ELF segment type",
        "truncated": "run: invalid ELF program header table",
    }
    for name, line in expected.items():
        body = command_output(serial, b"run " + name.encode())
        if body != line.encode() + b"\n":
            raise AssertionError(f"{name}: {body!r}")


if __name__ == "__main__":
    main()
