#!/usr/bin/env python3
from capture_uplink import stop_for_export

class Port:
    def __init__(self, size):
        self.size = size
        self.commands = []
    def command(self, text):
        self.commands.append(text)
    def readline(self):
        return (f"VCAP STATUS active=0 bytes={self.size} frames=1 width=1280 height=960 reason=manual-stop\n").encode()

port = Port(100)
assert stop_for_export(port)["bytes"] == 100
assert port.commands == ["video-capture stop"]  # preserve nonempty media
port = Port(0)
try:
    stop_for_export(port)
except RuntimeError as error:
    assert "0字节" in str(error)
else:
    raise AssertionError("empty capture admitted to dump")
assert port.commands == ["video-capture stop", "video-capture clear"]
print("PASS: export stops first, retains nonempty capture, releases only empty buffer")
