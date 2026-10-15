"""Bounded serial capture; never auto-retry commands or erase/reset flash."""
import argparse
import codecs
from pathlib import Path
import sys
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=15)
    parser.add_argument("--reset-usb", action="store_true")
    parser.add_argument("--command", action="append", default=[])
    parser.add_argument("--command-delay", type=float, default=3)
    parser.add_argument("--command-interval", type=float, default=1)
    args = parser.parse_args()
    if args.seconds <= 0 or args.command_delay < 0 or args.command_interval <= 0:
        parser.error("duration/interval must be positive and command delay nonnegative")
    if args.command and args.command_delay + (len(args.command) - 1) * args.command_interval >= args.seconds:
        parser.error("duration must leave time for all commands and their responses")
    for command in args.command:
        if not command or any(c in command for c in "\r\n") or len(command.encode()) > 255:
            parser.error("each command must be one line of at most 255 UTF-8 bytes")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    decoder = codecs.getincrementaldecoder("utf-8")("replace")
    # Explicit idle lines avoid entering the ROM downloader on port open.
    port = serial.Serial(baudrate=115200, timeout=0.1, write_timeout=2)
    port.dtr = False
    port.rts = False
    port.port = args.port
    with args.output.open("xb") as log, port:
        if args.reset_usb:
            from esptool.reset import HardReset
            HardReset(port, uses_usb=True)()
        start = time.monotonic()
        sent = 0
        while time.monotonic() - start < args.seconds:
            elapsed = time.monotonic() - start
            if sent < len(args.command) and elapsed >= args.command_delay + sent * args.command_interval:
                command = args.command[sent]
                port.write((command + "\n").encode())
                port.flush()
                name = command.split(maxsplit=1)[0]
                print(f"[HOST +{elapsed:.1f}s] sent {name} (arguments omitted)", flush=True)
                sent += 1
            data = port.read(max(1, min(port.in_waiting, 4096)))
            if data:
                log.write(data)
                log.flush()
                print(decoder.decode(data), end="", flush=True)
        print(decoder.decode(b"", final=True), end="", flush=True)
    print(f"\n[HOST] capture complete: {args.output}")


if __name__ == "__main__":
    try:
        main()
    except (serial.SerialException, OSError) as exc:
        raise SystemExit(f"Serial capture failed: {exc}") from exc
