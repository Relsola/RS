"""Boot real disk variants and check RSFS validation through the supervisor shell."""
import argparse
from pathlib import Path
import struct
import tempfile
import zlib

from boot import run_guest
from serial_session import command_output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("vm", "firmware", "disk"):
        parser.add_argument(argument, type=Path)
    args = parser.parse_args()
    original = args.disk.read_bytes()
    sector = 512
    kernel_length, = struct.unpack_from("<I", original, 8)
    directory = sector * (1 + (kernel_length + sector - 1) // sector)
    if original[directory:directory + 8] != b"RSFS0001":
        raise AssertionError("the test image needs an RSFS directory")
    count, = struct.unpack_from("<I", original, directory + 12)
    entries = {}
    for i in range(count):
        offset = directory + 32 + i * 32
        name, lba, size, _ = struct.unpack_from("<20sIII", original, offset)
        entries[name.rstrip(b"\0")] = offset, lba, size
    text_record, text_lba, text_size = entries[b"readme.txt"]
    elf_record, elf_lba, _ = entries[b"hello.elf"]

    with tempfile.TemporaryDirectory() as temp:
        disk_path = Path(temp) / "fixture.img"

        def boot(label, image, checks):
            disk_path.write_bytes(image)
            commands = b"".join(command + b"\n" for command, _ in checks) + b"halt\n"
            rc, output, errors = run_guest(args.vm, args.firmware, disk_path, commands)
            if rc or b"System halted." not in output:
                raise AssertionError(f"{label}: exit={rc} serial={output!r} stderr={errors!r}")
            for command, expected in checks:
                actual = command_output(output, command)
                if actual != expected:
                    raise AssertionError(f"{label}: {command!r} gave {actual!r}, expected {expected!r}")

        invalid = [(b"ls", b"ls: invalid directory\n"),
                   (b"cat readme.txt", b"cat: invalid directory\n"),
                   (b"echo alive", b"alive\n")]
        image = bytearray(original)
        image[directory + 8] ^= 1
        boot("directory checksum", image, invalid)

        # Use a valid directory checksum so the parser must inspect the record contents.
        for label, offset, replacement in (
                ("duplicate name", elf_record, original[text_record:text_record + 20]),
                ("overlapping files", elf_record + 20, struct.pack("<I", text_lba)),
                ("start past disk", elf_record + 20, struct.pack("<I", 0xffffffff)),
                ("excessive file size", elf_record + 24, struct.pack("<I", 0xffffffff))):
            image = bytearray(original)
            image[offset:offset + len(replacement)] = replacement
            crc = zlib.crc32(image[directory + 12:directory + sector])
            struct.pack_into("<I", image, directory + 8, crc)
            boot(label, image, invalid)

        boot("truncated extent", original[:-sector], invalid)
        boot("missing directory", original[:directory], invalid)
        old_disk = bytearray(original[:directory])
        old_disk[16:24] = bytes(8)
        boot("kernel-only disk", old_disk, [(b"ls", b""),
                                            (b"cat readme.txt", b"cat: file not found\n"),
                                            (b"echo alive", b"alive\n")])

        image = bytearray(original)
        image[elf_lba * sector] ^= 1
        text = original[text_lba * sector:text_lba * sector + text_size]
        boot("independent file checksums", image, [(b"run hello.elf", b"run: file CRC mismatch\n"),
                                                   (b"cat readme.txt", text.replace(b"\r\n", b"\n"))])


if __name__ == "__main__":
    main()
