"""Bounded interactive serial regression capture, without resets or automatic redial.

Enter a device command or :stop on stdin. Only one process owns the serial port.
"""
import argparse
import codecs
from datetime import datetime, timezone
from pathlib import Path
import queue
import sys
import threading
import time

import serial


def input_lines(commands):
    for line in sys.stdin:
        commands.put(line.strip())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=3600)
    parser.add_argument("--status-interval", type=float, default=60)
    args = parser.parse_args()
    if args.seconds <= 0 or args.status_interval < 0:
        parser.error("duration must be positive; status interval must be nonnegative")
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    commands = queue.Queue()
    threading.Thread(target=input_lines, args=(commands,), daemon=True).start()
    port = serial.Serial(baudrate=115200, timeout=0.1, write_timeout=2)
    port.dtr = False
    port.rts = False
    port.port = args.port
    decoder = codecs.getincrementaldecoder("utf-8")("replace")
    with args.output.open("xb") as log, port:
        start = time.monotonic()
        next_status = start + args.status_interval

        def marker(text):
            line = (f"\n[HOST {datetime.now(timezone.utc).isoformat(timespec='milliseconds')} "
                    f"+{time.monotonic() - start:.3f}s] {text}\n")
            log.write(line.encode())
            log.flush()
            print(line, end="", flush=True)

        def send(command):
            name = command.split(maxsplit=1)[0]
            allowed = {"status", "memory", "id", "contacts", "call", "answer", "reject",
                       "hangup", "cancel", "prompt", "rx-record", "rx-dump", "retry", "help"}
            if name not in allowed or len(command) > 255 or any(c in command for c in "\r\n"):
                marker("command rejected by host capture")
                return
            marker(f"send {command}")
            port.write((command + "\n").encode())
            port.flush()

        marker("capture started; :stop closes host capture, without hanging up or resetting")
        while time.monotonic() - start < args.seconds:
            try:
                command = commands.get_nowait()
            except queue.Empty:
                command = ""
            if command == ":stop":
                break
            if command:
                send(command)
            now = time.monotonic()
            if args.status_interval and now >= next_status:
                send("status")
                next_status = now + args.status_interval
            data = port.read(max(1, min(port.in_waiting, 4096)))
            if data:
                log.write(data)
                log.flush()
                print(decoder.decode(data), end="", flush=True)
        print(decoder.decode(b"", final=True), end="", flush=True)
        marker("capture stopped")


if __name__ == "__main__":
    try:
        main()
    except (serial.SerialException, OSError) as error:
        raise SystemExit(f"Serial capture failed: {error}") from error
