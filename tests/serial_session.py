"""Extract the output of one command, including the return to the shell."""


def command_output(serial, command):
    marker = b"rs> " + command + b"\n"
    before, found, after = serial.partition(marker)
    if not found:
        raise AssertionError(f"missing {command!r} in {before!r}")
    body, prompt, _ = after.partition(b"rs> ")
    if not prompt:
        raise AssertionError(f"shell did not resume after {command!r}: {body!r}")
    return body
