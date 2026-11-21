"""One serial conversation shared by the native VM and reference machine."""
import re


COMMANDS = b"ticks\nuser\nticks\nuser fault\necho shell resumed\nuser\nticks\nhalt\n"


def check_user_session(serial):
    output = serial.replace(b"\r\n", b"\n")
    ticks = [int(n) for n in re.findall(rb"^timer ticks: (\d+)$", output, re.MULTILINE)]
    if len(ticks) != 3 or not ticks[0] < ticks[1] < ticks[2]:
        raise AssertionError(f"timer did not advance across both user runs: {output!r}")
    faults = re.findall(rb"^user fault: cause=(\d+) epc=(0x[0-9a-f]+) tval=(0x[0-9a-f]+)$",
                       output, re.MULTILINE)
    if len(faults) != 1:
        raise AssertionError(f"expected one diagnosed user fault: {output!r}")
    cause, epc, tval = faults[0]
    if int(cause) != 13 or not 0x40000000 <= int(epc, 16) < 0x40001000 or int(tval, 16) != 0x10000000:
        raise AssertionError(f"expected a user load page fault at the UART: {faults[0]!r}")
    # Match complete output lines in order, independent of timing and command echo.
    fault_line = b"user fault: cause=" + cause + b" epc=" + epc + b" tval=" + tval
    expected = [b"hello from user mode", b"user exit: 0", fault_line, b"shell resumed",
                b"hello from user mode", b"user exit: 0", b"System halted."]
    lines = iter(output.splitlines())
    for line in expected:
        if line not in lines:
            raise AssertionError(f"missing or out-of-order output {line!r}: {output!r}")
