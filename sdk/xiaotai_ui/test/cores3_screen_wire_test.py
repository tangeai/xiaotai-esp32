"""Decode the actual SDK sender's 320x240 LVGL capture without hardware."""
# Replaces legacy: tests/test_cores3_screen_wire.py
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from decode_screen import decode  # noqa: E402


def main(executable):
    frame = subprocess.check_output([executable, "--dump"], timeout=10)
    width, height, rgb = decode(frame, sdk_only=True)
    assert (width, height, len(rgb)) == (320, 240, 320 * 240 * 3)
    assert rgb[:9] == bytes.fromhex("ff000000ff000000ff")
    for before, after in (
        (b"seq=1 ", b"seq=0 "),
        (b"hex=00F8E0071F00", b"hex=01F8E0071F00"),
        (b"+SCREEN-END:", b"truncated:"),
        (b"OK\r\n", b"OK\r\n+SCREEN-DATA: seq=1200 hex=0000\r\n"),
    ):
        assert before in frame
        try:
            decode(frame.replace(before, after, 1), sdk_only=True)
        except ValueError:
            continue
        raise AssertionError(f"Accepted a corrupted CoreS3 frame: {before!r}")
    print("PASS: 320x240 CoreS3 colours, malformed transfers, short capture rejection")


if __name__ == "__main__":
    main(sys.argv[1])
