"""The same disk file commands run on the interpreter and the reference board."""
from pathlib import Path

from serial_session import command_output


FILE_COMMANDS = (b"ls\nls /proc\ncat readme.txt\ncat /proc/..//./readme.txt\n"
                 b"cat /readme.txt/..\nrun hello.elf\nrun readme.txt\ncat missing\n"
                 b"run /memory.elf\nrun /family.elf\n")


def check_file_session(serial, images):
    serial = serial.replace(b"\r\n", b"\n")
    readme = (Path(__file__).resolve().parents[1] / "OS/files/readme.txt").read_bytes()
    listing = f"readme.txt  {len(readme)} bytes\n"
    for name in ("hello.elf", "memory.elf", "family.elf"):
        listing += f"{name}  {(images / name).stat().st_size} bytes\n"
    contents = readme.replace(b"\r\n", b"\n")
    expected = {
        b"ls": listing.encode() + b"proc/\n",
        b"ls /proc": b"meminfo\n",
        b"cat readme.txt": contents,
        b"cat /proc/..//./readme.txt": contents,
        b"cat /readme.txt/..": b"cat: not a directory\n",
        b"run hello.elf": b"hello from disk ELF\nuser exit: 0\n",
        b"run readme.txt": b"run: expected a static RISC-V ELF64 executable\n",
        b"cat missing": b"cat: file not found\n",
        b"run /memory.elf": contents + b"memory.elf: file copied, pages released\nuser exit: 0\n",
        b"run /family.elf": (b"child: page=C file=_\nchild exit: 37\n"
                             b"parent: page=P file=p wait=37\nuser exit: 0\n"),
    }
    for command, body in expected.items():
        actual = command_output(serial, command)
        if actual != body:
            raise AssertionError(f"{command!r}: expected {body!r}, got {actual!r}")
