"""The same disk file commands run on the interpreter and the reference board."""
from pathlib import Path

from serial_session import command_output


FILE_COMMANDS = b"ls\ncat readme.txt\nrun hello.elf\nrun readme.txt\ncat missing\n"


def check_file_session(serial, images):
    serial = serial.replace(b"\r\n", b"\n")
    readme = (Path(__file__).resolve().parents[1] / "OS/files/readme.txt").read_bytes()
    elf_size = (images / "hello.elf").stat().st_size
    listing = f"readme.txt  {len(readme)} bytes\nhello.elf  {elf_size} bytes\n".encode()
    expected = {
        b"ls": listing,
        b"cat readme.txt": readme.replace(b"\r\n", b"\n"),
        b"run hello.elf": b"hello from disk ELF\nuser exit: 0\n",
        b"run readme.txt": b"run: expected a static RISC-V ELF64 executable\n",
        b"cat missing": b"cat: file not found\n",
    }
    for command, body in expected.items():
        actual = command_output(serial, command)
        if actual != body:
            raise AssertionError(f"{command!r}: expected {body!r}, got {actual!r}")
