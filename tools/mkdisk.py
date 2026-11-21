"""Build the RS read-only boot disk from the linked kernel payload."""
import argparse
import struct
import zlib
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    kernel = args.kernel.read_bytes()
    if not 0 < len(kernel) <= 8 * 1024 * 1024:
        parser.error("kernel size must be in the range 1 byte to 8 MiB")
    sector = 512
    header = struct.pack("<8sII", b"RSDISK01", len(kernel), zlib.crc32(kernel))
    image = header.ljust(sector, b"\0") + kernel + bytes(-len(kernel) % sector)
    args.output.write_bytes(image)


if __name__ == "__main__":
    main()
