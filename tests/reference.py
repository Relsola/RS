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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("qemu", type=Path)
    parser.add_argument("images", type=Path)
    args = parser.parse_args()
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(8)
        port = listener.getsockname()[1]
        # QEMU connects to the test's serial port. Send input only after the guest prompt.
        command = [str(args.qemu.resolve()), "-machine", "virt", "-cpu", "rva23s64,pmp=on",
                   "-smp", "1", "-m", "128M", "-nodefaults", "-display", "none",
                   "-monitor", "none", "-bios", "firmware.bin",
                   "-icount", "shift=6,align=off,sleep=off",
                   "-global", "virtio-mmio.force-legacy=false",
                   "-drive", "if=none,format=raw,file=disk.img,id=disk0,readonly=on",
                   "-device", "virtio-blk-device,drive=disk0",
                   "-chardev", f"socket,id=uart,host=127.0.0.1,port={port}",
                   "-serial", "chardev:uart"]
        proc = subprocess.Popen(command, cwd=args.images, stderr=subprocess.PIPE)
        serial = bytearray()
        try:
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(8)
                read_until(connection, serial, b"rs> ")
                connection.sendall(b"info\necho hello\n" + FILE_COMMANDS + TASK_COMMANDS + COMMANDS)
                read_until(connection, serial, b"System halted.")
            rc = proc.wait(timeout=5)
            expected = (b"kernel verified", b"MMU: Sv39", b"\r\nhello\r\n")
            if rc or not all(fragment in serial for fragment in expected):
                raise RuntimeError(f"unexpected reference result, exit {rc}")
            check_file_session(bytes(serial), args.images)
            check_user_session(bytes(serial))
            check_task_session(bytes(serial), args.images / "user.elf")
        except Exception as exc:
            proc.kill()
            _, error = proc.communicate()
            raise SystemExit(f"{exc}\nserial={serial!r}\nstderr={error!r}") from exc
        finally:
            proc.stderr.close()
        print(serial.decode("ascii").rstrip())


if __name__ == "__main__":
    main()
