"""Check task isolation and the interrupted PC using the linked guest image."""
import re
import subprocess
from serial_session import command_output


TASK_COMMANDS = b"tasks yield\ntasks preempt\ntasks fault\ntasks memory\n"


def require_order(body, *expected):
    lines = iter(body.splitlines())
    for item in expected:
        if item not in lines:
            raise AssertionError(f"missing or out-of-order line {item!r}: {body!r}")


def compute_bounds(elf):
    symbols = subprocess.check_output(["llvm-nm", "--format=posix", "--defined-only", elf], text=True)
    bounds = {}
    for line in symbols.splitlines():
        fields = line.split()
        if fields and fields[0] in ("rs_compute_begin", "rs_compute_end"):
            bounds[fields[0]] = int(fields[2], 16)
    return bounds["rs_compute_begin"], bounds["rs_compute_end"]


def check_task_session(serial, elf):
    serial = serial.replace(b"\r\n", b"\n")
    cooperative = command_output(serial, b"tasks yield")
    switches = re.findall(rb"^switch ([AB] -> [AB]): yield epc=0x[0-9a-f]+$", cooperative, re.M)
    if switches != [b"A -> B", b"B -> A"]:
        raise AssertionError(f"unexpected cooperative switches: {cooperative!r}")
    require_order(cooperative, b"task A private value: 11", b"task A exit: 0",
                  b"task B private value: 22", b"task B exit: 0")

    preemptive = command_output(serial, b"tasks preempt")
    switch = re.search(rb"^switch A -> B: timer epc=(0x[0-9a-f]+)$", preemptive, re.M)
    begin, end = compute_bounds(elf)
    if not switch or not begin <= int(switch[1], 16) < end:
        raise AssertionError(f"expected timer switch in [{begin:#x},{end:#x}): {preemptive!r}")
    checksum = 0x31415926
    for i in range(1, 524289):
        checksum = (1664525 * checksum + i) & 0xffffffff
    require_order(preemptive, switch[0], b"task B ran", b"task B exit: 0",
                  f"task A checksum: 0x{checksum:08x}".encode(), b"task A exit: 0")

    faulted = command_output(serial, b"tasks fault")
    fault = re.search(rb"^task A fault: cause=13 epc=0x[0-9a-f]+ tval=0x0*10000000$", faulted, re.M)
    if not fault:
        raise AssertionError(f"missing task A load page fault on UART: {faulted!r}")
    require_order(faulted, fault[0], b"task B survived", b"task B exit: 0")

    memory = command_output(serial, b"tasks memory")
    require_order(memory, b"task A va=0x0000000040040000 fd=3 value=17 next=o", b"task A exit: 0",
                  b"task B va=0x0000000040040000 fd=3 value=29 next=_", b"task B exit: 0")
