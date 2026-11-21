"""Pack a bootable RS disk with an optional flat, read-only file directory."""
import argparse
import re
import struct
import zlib
from pathlib import Path


SECTOR = 512
NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,18}")
DIRECTORY_HEADER = 32
RECORD = struct.Struct("<20sIII")
FILE_LIMIT = (SECTOR - DIRECTORY_HEADER) // RECORD.size
FILE_SIZE_LIMIT = 64 * 1024
FS_MAGIC = b"RSFS0001"


def sector_padded(data):
    return data + bytes(-len(data) % SECTOR)


def pack_disk(kernel, file_specs):
    if not 0 < len(kernel) <= 8 * 1024 * 1024:
        raise ValueError("kernel size must be in the range 1 byte to 8 MiB")
    header = bytearray(SECTOR)
    struct.pack_into("<8sII", header, 0, b"RSDISK01", len(kernel), zlib.crc32(kernel))
    payload = sector_padded(kernel)
    if not file_specs:
        return header + payload
    if len(file_specs) > FILE_LIMIT:
        raise ValueError(f"too many files (maximum {FILE_LIMIT})")

    header[16:24] = FS_MAGIC
    directory = bytearray(SECTOR)
    directory[:8] = FS_MAGIC
    struct.pack_into("<I", directory, 12, len(file_specs))
    next_sector = 2 + len(payload) // SECTOR
    contents = bytearray()
    names = set()
    for index, spec in enumerate(file_specs):
        name, separator, path = spec.partition("=")
        if not separator or not path or not NAME.fullmatch(name) or name in names:
            raise ValueError(f"invalid or duplicate file specification {spec!r}")
        names.add(name)
        with Path(path).open("rb") as source:
            data = source.read(FILE_SIZE_LIMIT + 1)
        if len(data) > FILE_SIZE_LIMIT:
            raise ValueError(f"{name} exceeds {FILE_SIZE_LIMIT} bytes")
        RECORD.pack_into(directory, DIRECTORY_HEADER + index * RECORD.size,
                         name.encode("ascii"), next_sector, len(data), zlib.crc32(data))
        padded = sector_padded(data)
        contents.extend(padded)
        next_sector += len(padded) // SECTOR
    struct.pack_into("<I", directory, 8, zlib.crc32(directory[12:]))
    return header + payload + directory + contents


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--file", metavar="NAME=PATH", action="append", default=[],
                        help="store a file in the RSFS directory (repeatable)")
    args = parser.parse_args()

    try:
        image = pack_disk(args.kernel.read_bytes(), args.file)
        args.output.write_bytes(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
