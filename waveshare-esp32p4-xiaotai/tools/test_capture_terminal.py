#!/usr/bin/env python3
"""Console query may lack newline; raw readline never answers it."""
from capture_uplink import Terminal, until

class Serial:
    def __init__(self):
        self.pending = bytearray(b"\x1b[6n")
        self.writes = []
    @property
    def in_waiting(self):
        return len(self.pending)
    def read(self, count):
        value = bytes(self.pending[:count]); del self.pending[:count]
        return value
    def write(self, value):
        self.writes.append(value)
        if value == b"\x1b[1;80R":
            self.pending.extend(b"Firmware: version=test\r\n")

serial = Serial()
assert until(Terminal(serial), "Firmware:", timeout=1) == "Firmware: version=test"
assert serial.writes == [b"\x1b[1;80R"]
class Fragmented(Serial):
    def read(self, count):
        return super().read(1)

assert until(Terminal(Fragmented()), "Firmware:", timeout=1) == "Firmware: version=test"
serial = Serial()
serial.pending = bytearray()
terminal = Terminal(serial)
terminal.command("version")
assert serial.writes == [b"\x15version\r"]
serial.pending = bytearray(b"Guru Meditation Error\n")
terminal.readline()
assert terminal.signals == ["Guru Meditation"]
class Disconnected(Serial):
    def read(self, count):
        raise OSError("disconnected")
try:
    Terminal(Disconnected()).readline()
except RuntimeError as error:
    assert "不能排除重启" in str(error)
else:
    raise AssertionError("disconnect not reported")
print("PASS: newline-free ANSI query answered before firmware response")
