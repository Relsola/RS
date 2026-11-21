"""Boot the same firmware and disk with QEMU's RVA23S64 reference CPU model."""
import argparse
import socket
import subprocess
from pathlib import Path

from user_session import COMMANDS, check_user_session
from task_session import TASK_COMMANDS, check_task_session
from file_session import FILE_COMMANDS, check_file_session


def read_until(connection, serial, marker):
    while marker not in serial:
        chunk = connection.recv(4096)
        if not chunk:
            raise RuntimeError(f"serial closed before {marker!r}")
        serial.extend(chunk)


def run_reference(qemu, firmware, disk, commands):
    """Send a serial session ending in halt and return the raw UART bytes."""
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(8)
        port = listener.getsockname()[1]
        # QEMU connects to the test's serial port. Send input only after the guest prompt.
        # -drive treats commas as option separators. The child runs in the disk's directory.
        disk_name = disk.name.replace(",", ",,")
        command = [str(qemu.resolve()), "-machine", "virt", "-cpu", "rva23s64,pmp=on",
                   "-smp", "1", "-m", "128M", "-nodefaults", "-display", "none",
                   "-monitor", "none", "-bios", str(firmware.resolve()),
                   "-icount", "shift=6,align=off,sleep=off",
                   "-global", "virtio-mmio.force-legacy=false",
                   "-drive", f"if=none,format=raw,file={disk_name},id=disk0,readonly=on",
                   "-device", "virtio-blk-device,drive=disk0",
                   "-chardev", f"socket,id=uart,host=127.0.0.1,port={port}",
                   "-serial", "chardev:uart"]
        proc = subprocess.Popen(command, cwd=disk.parent, stderr=subprocess.PIPE)
        serial = bytearray()
        try:
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(8)
                read_until(connection, serial, b"rs> ")
                connection.sendall(commands)
                read_until(connection, serial, b"System halted.")
            rc = proc.wait(timeout=5)
            if rc:
                raise RuntimeError(f"unexpected reference result, exit {rc}")
        except Exception as exc:
            proc.kill()
            _, error = proc.communicate()
            raise SystemExit(f"{exc}\nserial={serial!r}\nstderr={error!r}") from exc
        finally:
            proc.stderr.close()
        return bytes(serial)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("qemu", type=Path)
    parser.add_argument("images", type=Path)
    args = parser.parse_args()
    commands = b"info\necho hello\n" + FILE_COMMANDS + TASK_COMMANDS + COMMANDS
    serial = run_reference(args.qemu, args.images / "firmware.bin", args.images / "disk.img", commands)
    expected = (b"kernel verified", b"MMU: Sv39", b"\r\nhello\r\n")
    if not all(fragment in serial for fragment in expected):
        raise AssertionError(f"unexpected reference output: {serial!r}")
    check_file_session(serial, args.images)
    check_user_session(serial)
    check_task_session(serial, args.images / "user.elf")
    print(serial.decode("ascii").rstrip())


if __name__ == "__main__":
    main()
