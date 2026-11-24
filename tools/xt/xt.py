"""XiaoTai multi-device test kit.

  xt.py scan                         identify boards on all serial ports -> devices.json
  xt.py at DEV CMD [CMD ...] [-s N]  send AT commands (no reset), print replies
  xt.py log DEV [-s N]               listen only
  xt.py gate DEV                     SDK bring-up gate with a device-bound allow-file
  xt.py snapshot DEV                 read one verified, sanitized SDK diagnostic snapshot
  xt.py boot-check PORT              explicitly authorize and verify native USB chip resets
  xt.py app-stacks DEV               read stack watermarks for explicitly selected tasks
  xt.py partition-plan              plan from a saved table or an authorized ROM read
  xt.py udp-pattern                  plan or run a fixed-peer diagnostic UDP test
  xt.py flash DEV BIN [--offset X]   esptool write-flash (APP only by default)
  xt.py screen DEV OUT.png           Xmini OLED snapshot
  xt.py ai DEV [-s N]                AI session: start, sample, stop, verdict
  xt.py call A B [-s N]              A dials B, B answers over serial, hang up, verdict
  xt.py phone shot OUT.png | tap X Y | text STR | key CODE | ui OUT.xml
  xt.py watch DEV [DEV ...] [--hours H] [--every S]   long run: drop-outs, reboots, heap trend

DEV is a name from devices.json (e.g. xmini, m5) or a COM port.
Runs are saved under runs/<time>-<test>/ (raw logs + summary.json).
Ordinary serial commands open ports without requesting reset. boot-check has its own explicit reset gate.
"""
import argparse
from contextlib import ExitStack, closing
import datetime
import json
import math
import os
import re
import struct
import subprocess
import sys
import threading
import time
import uuid
import zlib
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = list_ports = None

KIT = Path(__file__).resolve().parent
ROOT = KIT.parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(KIT))
from console_sdk import (ConsoleError, DATA, END, LEGACY_DIAGNOSTICS, LIMIT, READY, RESTARTED, Session,
                         command_line, fields, help_records, hex_text, identity, legacy_preflight, load_vocabulary)
from decode_screen import decode_lines, png as screen_png
import phone_ui
import sdk_bench
import wx_phone
import xtp2

STATE = Path(os.environ["XT_STATE_DIR"]) if "XT_STATE_DIR" in os.environ else KIT
if not STATE.is_absolute() or STATE.drive.startswith("\\") or not STATE.is_dir():
    raise SystemExit("xt: XT_STATE_DIR must be an existing absolute local directory")
DEVICES = STATE / "devices.json"
RUNS = STATE / "runs"
PYTHON = os.environ.get("XT_PYTHON", sys.executable)  # needs pyserial + esptool

# Per-board AT dialects. {id} = callee device id, {index} = callee contact index.
DIALECT = {
    "xmini": {"chip": "esp32c3", "dial": "AT+CALL DEVICE {id}", "answer": "AT+CALL ACCEPT",
              "reject": "AT+CALL REJECT", "hangup": "AT+CALL HANGUP", "dismiss": "AT+CALL DISMISS",
              "ai_start": "AT+AI START", "ai_stop": "AT+AI STOP", "state": "AT+RTC", "status": "AT+STATUS"},
    "cores3": {"chip": "esp32s3", "dial": "AT+CALL {index}", "answer": "AT+ANSWER",
               "reject": "AT+REJECT", "hangup": "AT+HANGUP", "dismiss": None,
               "ai_start": "AT+AI START", "ai_stop": "AT+AI STOP", "state": "AT+STATUS", "status": "AT+STATUS"},
    # LCKFB SZPI S3: same runtime status text as CoreS3, Xmini-style call verbs.
    "szpi": {"chip": "esp32s3", "dial": "AT+CALL DIAL {id}", "answer": "AT+CALL ACCEPT",
             "reject": "AT+CALL REJECT", "hangup": "AT+CALL HANGUP", "dismiss": None,
             "ai_start": "AT+AI START", "ai_stop": "AT+AI STOP", "state": "AT+STATUS", "status": "AT+STATUS"},
}


# ---------------------------------------------------------------- serial
class Port:
    """Background reader; keeps every line with a timestamp."""

    def __init__(self, name, log_path=None, *, exclusive=False):
        self.name = name
        p = serial.Serial(port=None, baudrate=115200, timeout=0.05, write_timeout=2)
        p.port = name
        p.dtr = False
        p.rts = False
        if exclusive and os.name == "posix":
            p.exclusive = True
        p.open()
        self.serial = p
        self.lines = []
        self.lock = threading.Lock()
        self.rx_lock = threading.Lock()
        self.sdk_lock = threading.Lock()
        self.rx_resume = threading.Event()
        self.rx_resume.set()
        self.sdk_start = None
        self.log = open(log_path, "ab") if log_path else None
        self.stop = False
        self.partial = b""
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self):
        while not self.stop:
            if not self.rx_resume.wait(0.05):
                continue
            with self.rx_lock:
                if not self.rx_resume.is_set():
                    continue
                try:
                    self._capture(self.serial.read(4096))
                except serial.SerialException:
                    break

    def _capture(self, data):
        if not data:
            return
        if self.log:
            self.log.write(data)
            self.log.flush()
        *done, self.partial = (self.partial + data).split(b"\n")
        now = time.monotonic()
        with self.lock:
            for raw in done:
                self.lines.append((now, raw.rstrip(b"\r").decode("utf-8", "replace")))

    def mark(self):
        with self.lock:
            return len(self.lines)

    def since(self, mark):
        with self.lock:
            return [l for _, l in self.lines[mark:]]

    def send(self, command, wait=1.5, until=r"^(OK|ERROR.*)$"):
        start = self.mark()
        if self.log:
            self.log.write(f"\n>>> {time.strftime('%H:%M:%S')} {command}\n".encode())
        self.serial.write((command + "\r\n").encode("utf-8"))
        self.serial.flush()
        deadline = time.monotonic() + wait
        pattern = re.compile(until)
        while time.monotonic() < deadline:
            if any(pattern.match(l) for l in self.since(start)):
                time.sleep(0.15)
                break
            time.sleep(0.05)
        return self.since(start)

    def sdk_exchange(self, command, wait):
        data = (command_line(command) + "\r\n").encode("ascii")
        with self.sdk_lock:
            return self._sdk_exchange(data, wait)

    def _sdk_send(self, data):
        # Complete the reader's in-flight read and drain old bytes before
        # taking the mark; a mark in the line list alone misses driver backlog.
        boundary_error = "could not establish a fresh reply boundary before writing"
        self.rx_resume.clear()
        try:
            with self.rx_lock:
                for _ in range(64):
                    pending = self.serial.in_waiting
                    if not pending and not self.partial:
                        break
                    if len(self.partial) > 4096:
                        raise ConsoleError(boundary_error)
                    chunk = self.serial.read(max(1, min(pending, 65536)))
                    if not chunk:
                        raise ConsoleError(boundary_error)
                    self._capture(chunk)
                else:
                    if self.serial.in_waiting or self.partial:
                        raise ConsoleError(boundary_error)
                self.sdk_start = self.mark()
                if self.serial.write(data) != len(data):
                    raise ConsoleError("serial command write was incomplete")
        except serial.SerialException as error:
            raise ConsoleError("serial transport failed while preparing or writing the command") from error
        finally:
            self.rx_resume.set()

    def _sdk_exchange(self, data, wait):
        # No legacy fallback after a timeout. Long transfers renew only on
        # framed progress, never on ordinary boot or logging chatter.
        self._sdk_send(data)
        start = self.sdk_start
        try:
            self.serial.flush()
        except serial.SerialException as error:
            raise ConsoleError("serial command did not flush") from error
        deadline = time.monotonic() + wait
        result = []
        seen = 0
        while time.monotonic() < deadline:
            batch = self.since(start)
            start += len(batch)
            if not batch:
                time.sleep(0.025)
                continue
            terminal = False
            for line in batch:
                seen += len(line) + 1
                if seen > LIMIT or len(line) > 4096:
                    raise ConsoleError("serial reply exceeded the bounded capture size")
                if line.startswith(READY):
                    raise ConsoleError(RESTARTED)
                if terminal:
                    if END.fullmatch(line):
                        raise ConsoleError("multiple serial command terminators")
                    continue
                result.append(line)
                terminal = bool(END.fullmatch(line))
                if DATA.match(line):
                    deadline = time.monotonic() + wait
            if terminal:
                return result
        raise ConsoleError("timed out before the final serial reply")

    def wait_for(self, regex, timeout, start=None):
        start = self.mark() if start is None else start
        pattern = re.compile(regex)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for line in self.since(start):
                m = pattern.search(line)
                if m:
                    return m
            time.sleep(0.05)
        return None

    def close(self):
        self.stop = True
        self.thread.join(timeout=1)
        self.serial.close()
        if self.log:
            self.log.close()


# ---------------------------------------------------------------- registry
def load_devices():
    return json.loads(DEVICES.read_text(encoding="utf-8")) if DEVICES.exists() else {}


def resolve(dev):
    devices = load_devices()
    if dev in devices:
        return dev, devices[dev]
    for name, info in devices.items():
        if info.get("port", "").upper() == dev.upper():
            return name, info
    return dev, {"port": dev, "kind": "unknown"}


def identify(port_name):
    port = Port(port_name)
    try:
        reply = port.send("AT+VERSION", wait=2)
        text = "\n".join(reply)
        info = {"port": port_name}
        m = re.search(r"board=(\S+).*", text)
        # Legacy send may collect asynchronous lines after a command's terminator.
        terminal = next((i + 1 for i, line in enumerate(reply) if END.fullmatch(line)), len(reply))
        sdk = identity(reply[:terminal])
        if sdk:
            info.update(kind="sdk", board=sdk["board"], chip=sdk["chip"], elf=sdk["elf_sha256"][:8],
                        elf_sha256=sdk["elf_sha256"], device_id=None, online=None)
            # A locked console still yields useful identity. Do not unlock it.
            try:
                help_lines = port.sdk_exchange("AT+HELP", 3)
                if help_lines[-1] != "OK":
                    return info
                names = help_records(help_lines)
                if "AT+BIND" in names:
                    binding = fields(port.sdk_exchange("AT+BIND", 3), "+BIND: ")
                    if binding.get("device") not in (None, "-"):
                        info["device_id"] = binding["device"]
                if "AT+PLATFORM" in names:
                    platform = fields(port.sdk_exchange("AT+PLATFORM", 3), "+PLATFORM: ")
                    if platform.get("online") in ("0", "1"):
                        info["online"] = platform["online"] == "1"
            except ConsoleError:
                pass
        elif m and "Xmini" in m.group(1):
            info["kind"] = "xmini"
            info["board"] = m.group(1)
            elf = re.search(r"elf_sha256=([0-9a-f]{8})", text)
            info["elf"] = elf.group(1) if elf else None
            product = "\n".join(port.send("AT+PRODUCT", wait=2))
            d = re.search(r"\+DEVICE: id=(\S+) platform_online=(\d) rtc_ready=(\d)", product)
            if d:
                info.update(device_id=d.group(1), online=d.group(2) == "1" and d.group(3) == "1")
        elif m and "szpi" in m.group(1):
            info["kind"] = "szpi"
            info["board"] = m.group(1)
            elf = re.search(r"elf_sha256=([0-9a-f]{8})", text)
            info["elf"] = elf.group(1) if elf else None
            status = "\n".join(port.send("AT+STATUS", wait=3))
            info["online"] = "mqtt=connected" in status
        elif "Firmware:" in text:
            info["kind"] = "cores3"
            elf = re.search(r"elf-sha256=([0-9a-f]{8})", text)
            info["elf"] = elf.group(1) if elf else None
            status = "\n".join(port.send("AT+STATUS", wait=3))
            info["online"] = "mqtt=connected" in status
        else:
            info["kind"] = "unknown"
            info["reply"] = text[:200]
        return info
    finally:
        port.close()


def cmd_scan(args):
    devices = load_devices()
    known_ids = {v.get("device_id"): k for k, v in devices.items() if v.get("device_id")}
    for p in sorted(list_ports.comports(), key=lambda p: p.device):
        try:
            info = identify(p.device)
        except Exception as e:  # port busy, no reply
            print(f"{p.device}: {e}")
            continue
        info["usb"] = p.serial_number
        name = next((k for k, v in devices.items() if v.get("usb") and v.get("usb") == p.serial_number), None)
        name = name or next((k for k, v in devices.items() if not v.get("usb") and v.get("port") == p.device), None)
        name = name or known_ids.get(info.get("device_id")) or info["kind"]
        merged = {**devices.get(name, {}), **info}
        devices[name] = merged
        print(f"{p.device}: {name} kind={info['kind']} elf={info.get('elf')} "
              f"id={merged.get('device_id', '?')} online={info.get('online')}")
    DEVICES.write_text(json.dumps(devices, indent=2, ensure_ascii=False), encoding="utf-8")


# ---------------------------------------------------------------- run dirs
def run_dir(test):
    d = RUNS / f"{datetime.datetime.now():%Y%m%d-%H%M%S}-{test}"
    d.mkdir(parents=True)
    return d


def finish(d, summary):
    (d / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    print(f"run: {d}")


# ---------------------------------------------------------------- simple commands
def sdk_requested(args, info, peer=False):
    option = "peer_commands" if peer else "commands_file"
    return bool(getattr(args, option, None) or info.get("kind") == "sdk")


def sdk_vocabulary(args, info, peer=False):
    vocabulary = getattr(args, "peer_commands" if peer else "commands_file", None) or info.get("commands_file")
    if not vocabulary:
        raise ConsoleError("SDK bench commands require --commands for the selected build (and --peer-commands if needed)")
    return load_vocabulary(vocabulary)


def sdk_session(args, info, port, peer=False):
    vocabulary = sdk_vocabulary(args, info, peer)
    sha = getattr(args, "peer_elf_sha256" if peer else "elf_sha256", None)
    return Session(port.sdk_exchange, vocabulary, getattr(args, "allow", ()), sha)


def require_legacy(port, *, read_only=False, confirmed=False):
    legacy_preflight(port.send("AT+VERSION", wait=2), read_only=read_only, confirmed=confirmed)


def cmd_at(args):
    if not 0 < args.wait <= 120 or not 0 <= args.secs <= 600:
        raise ConsoleError("reply idle timeout must be in (0, 120] and capture duration in [0, 600]")
    name, info = resolve(args.dev)
    port = Port(info["port"])
    try:
        if sdk_requested(args, info):
            if getattr(args, "legacy", False):
                raise ConsoleError("--legacy cannot be used with SDK firmware or --commands")
            from console_sdk_aliases import translate
            session = sdk_session(args, info, port)
            plan = []
            for line in args.commands:
                line = line.strip(" ")
                translated = translate(line, session.device["board"])
                extra = None
                if not translated:
                    translated, extra = sdk_bench.workflow(session, line)
                for command in translated:
                    session.check(command)
                plan.append((translated, extra))
            for commands, extra in plan:
                for command in commands:
                    replies = session.send(command, wait=args.wait)
                    pattern_probe = command.startswith("AT+NET PROBE2 ")
                    for line in replies:
                        if pattern_probe and line.startswith("+NET-PROBE2:"):
                            continue
                        print(line)
                    if pattern_probe:
                        flow = int(command.split()[4])
                        for line in xtp2.board_notices(port.since, port.sdk_start, flow):
                            print(line, flush=True)
                    if command == "AT+VIDEO PROBE":
                        probe = port.wait_for(r"^\+VIDEO-PROBE: result=(passed|failed) frames=([0-9]+) error=([A-Z0-9_]+)$",
                                              15, port.sdk_start)
                        if not probe:
                            raise ConsoleError("video probe did not return a complete typed result")
                        print(probe[0])
                        if probe[1] != "passed" or probe[2] != "3" or probe[3] != "NONE":
                            raise ConsoleError("video probe failed")
                if extra:
                    print(extra)
        else:
            if any(not c or any(not char.isprintable() for char in c) for c in args.commands):
                raise ConsoleError("each legacy command must be one printable line")
            require_legacy(port, read_only=all(c in LEGACY_DIAGNOSTICS for c in args.commands),
                           confirmed=getattr(args, "legacy", False))
            for c in args.commands:
                for line in port.send(c, wait=args.wait):
                    print(line)
        if args.secs:
            start = port.mark()
            time.sleep(args.secs)
            print("\n".join(port.since(start)))
    finally:
        port.close()


def cmd_log(args):
    name, info = resolve(args.dev)
    port = Port(info["port"])
    try:
        start = port.mark()
        time.sleep(args.secs)
        print("\n".join(port.since(start)))
    finally:
        port.close()


def cmd_flash(args):
    from flash_baud import (BRIDGE_BAUD, FALLBACK_BAUD, LEGACY_BAUD, image_fingerprint, prewrite_rate_failure,
                            usb_bridge)

    name, info = resolve(args.dev)
    chip = DIALECT.get(info.get("kind"), {}).get("chip") or args.chip
    if not chip:
        sys.exit("unknown chip; pass --chip")
    if subprocess.run([PYTHON, "-c", "import esptool"], capture_output=True).returncode:
        sys.exit(f"esptool is not installed for {PYTHON}; set XT_PYTHON to a Python that has it "
                 r"(e.g. an ESP-IDF environment: C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe)")
    port = info["port"]
    image = args.bin
    baud = args.baud
    bridge, identity_before, image_before = False, None, None
    if baud is None:
        bridge, serial_number, identity_before = usb_bridge(list_ports, port)
        baud = BRIDGE_BAUD if bridge else LEGACY_BAUD
        known_usb = info.get("usb")
        if bridge and known_usb and (not serial_number or known_usb.casefold() != serial_number.casefold()):
            sys.exit("USB-UART bridge does not match the selected device's recorded USB serial; recheck the port")
        if identity_before is not None:
            image_before = image_fingerprint(image)
    # Keep the selected chip, port, reset policy, offset and local image identical for any repeat.
    prefix = [PYTHON, "-m", "esptool", "--chip", chip, "--port", port, "-b"]
    suffix = ["--before", "default_reset", "--after", "hard_reset", "write_flash", args.offset, image]
    # Decode as UTF-8 explicitly: with the locale codec (GBK on Chinese Windows) an
    # undecodable byte kills the reader thread and leaves stdout/stderr as None.
    env = {**os.environ, "PYTHONIOENCODING": "utf-8", "PYTHONUTF8": "1"}

    def attempt(rate):
        cmd = [*prefix, str(rate), *suffix]
        print(" ".join(cmd))
        result = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", env=env)
        print("\n".join(line for line in result.stdout.splitlines() if "Writing at" not in line))
        if result.returncode:
            print(result.stderr.strip(), file=sys.stderr)
            print(f"FAIL: esptool exited {result.returncode}", file=sys.stderr)
        return result

    out = attempt(baud)
    if bridge and prewrite_rate_failure(out):
        _, _, identity_after = usb_bridge(list_ports, port)
        if (identity_before is not None and identity_before == identity_after and image_before is not None and
                image_before == image_fingerprint(image)):
            print("921600 serial rate was rejected before writing. Retrying this same flash once at 115200.",
                  file=sys.stderr)
            out = attempt(FALLBACK_BAUD)
        else:
            print("No retry: the same USB bridge and unchanged local image could not be confirmed.", file=sys.stderr)
    sys.exit(out.returncode)


# ---------------------------------------------------------------- screen
def xmini_frame(lines):
    """Tolerant decode: collects offset/hex chunks even with log lines interleaved."""
    data = {}
    for line in lines:
        m = re.search(r"\+SCREEN: offset=(\d+) hex=([0-9a-f]{64})", line)
        if m:
            data[int(m.group(1))] = bytes.fromhex(m.group(2))
    if sorted(data) != list(range(0, 1024, 32)):
        raise ValueError(f"screen incomplete: {len(data)}/32 chunks")
    return b"".join(data[o] for o in range(0, 1024, 32))


def png(frames, path, scale=3, cols=3):
    pad = 12
    w, h = 128 * scale, 64 * scale
    rows = (len(frames) + cols - 1) // cols
    cols = min(cols, len(frames))
    tw, th = cols * (w + pad) + pad, rows * (h + pad) + pad
    px = [bytearray(b"\x28" * (tw * 3)) for _ in range(th)]
    for i, f in enumerate(frames):
        ox, oy = pad + (i % cols) * (w + pad), pad + (i // cols) * (h + pad)
        for y in range(64):
            for x in range(128):
                on = f[(y // 8) * 128 + x] >> (y % 8) & 1
                c = b"\x78\xc8\xff" if on else b"\0\0\0"
                for dy in range(scale):
                    row = px[oy + y * scale + dy]
                    for dx in range(scale):
                        o = (ox + x * scale + dx) * 3
                        row[o:o + 3] = c
    raw = b"".join(b"\0" + bytes(r) for r in px)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", tw, th, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def grab_screen(port):
    return xmini_frame(port.send("AT+SCREEN", wait=3))


def cmd_screen(args):
    name, info = resolve(args.dev)
    port = Port(info["port"])
    try:
        sdk = sdk_requested(args, info)
        if sdk:
            lines = sdk_session(args, info, port).send("AT+SCREEN", wait=5)
        else:
            require_legacy(port)
            lines = port.send("AT+SCREEN", wait=10, until=r"^(OK|ERROR.*)$")
        width, height, rgb = decode_lines(lines, sdk_only=sdk)
        Path(args.out).write_bytes(screen_png(width, height, rgb))
        print(args.out)
    finally:
        port.close()


# ---------------------------------------------------------------- metrics
def xmini_media(port):
    text = "\n".join(port.send("AT+RTC", wait=2))
    m = re.search(r"\+MEDIA: tx=(\d+) rx=(\d+) tx_dropped=(\d+) rx_dropped=(\d+)", text)
    s = re.search(r"\+SESSION: owner=(\d+) phase=(\d+) generation=(\d+) media_active=(\d)", text)
    return {"tx": int(m[1]), "rx": int(m[2]), "tx_dropped": int(m[3]), "rx_dropped": int(m[4])} if m else {}, \
           {"owner": int(s[1]), "phase": int(s[2]), "generation": int(s[3]), "media": s[4] == "1"} if s else {}


def xmini_echo(port):
    text = "\n".join(port.send("AT+STATUS", wait=2))
    m = re.search(r"\+ECHO: enabled=(\d) closed=(\d+) dt=(\d+) open=(\d+) gain_q15=(\d+) far_floor=(\d+) noise=(\d+)", text)
    if not m:
        return {}
    keys = ("enabled", "closed", "dt", "open", "gain_q15", "far_floor", "noise")
    return dict(zip(keys, map(int, m.groups())))


def xmini_heap(port):
    text = "\n".join(port.send("AT+STATUS", wait=2))
    m = re.search(r"\+HEAP: free=(\d+) largest=(\d+) minimum=(\d+)", text)
    return {"free": int(m[1]), "largest": int(m[2]), "minimum": int(m[3])} if m else {}


def cores3_media(port):
    text = "\n".join(port.send("AT+STATUS", wait=3))
    m = re.search(r"Media: active=(\w+) audio-tx=(\d+) audio-rx=(\d+) dropped=(\d+)", text)
    s = re.search(r"Session: (\S+)", text)
    d = re.search(r"Downlink: decoded=(\d+) played=(\d+) decode-fail=(\d+)", text)
    media = {"active": m[1] == "yes", "tx": int(m[2]), "rx": int(m[3]), "dropped": int(m[4])} if m else {}
    if d:
        media.update(decoded=int(d[1]), played=int(d[2]), decode_fail=int(d[3]))
    return media, (s[1] if s else None)


def delta(after, before):
    return {k: after[k] - before.get(k, 0) for k in after
            if isinstance(after[k], int) and not isinstance(after[k], bool)}


# ---------------------------------------------------------------- AI test
def cmd_ai(args):
    name, info = resolve(args.dev)
    if sdk_requested(args, info):
        return sdk_ai_run(args, name, info)
    if info.get("kind") != "xmini":
        sys.exit("ai test currently drives Xmini boards")
    d = run_dir(f"ai-{name}")
    port = Port(info["port"], d / f"{name}.log")
    dialect = DIALECT["xmini"]
    summary = {"test": "ai", "device": name, "seconds": args.secs}
    try:
        require_legacy(port)
        port.send(dialect["dismiss"])
        before, _ = xmini_media(port)
        start = port.mark()
        port.send(dialect["ai_start"])
        summary["active"] = bool(port.wait_for(r"xmini_business: phase=6 owner=1", 15, start))
        summary["profile"] = (port.wait_for(r"AI profile verified (.*) gen=", 15, start) or [None, None])[1]
        frames, samples = [], []
        t0 = time.monotonic()
        while time.monotonic() - t0 < args.secs:
            time.sleep(min(10, args.secs))
            media, session = xmini_media(port)
            samples.append({"t": round(time.monotonic() - t0), **media, **session})
            if args.screens:
                try:
                    frames.append(grab_screen(port))
                except ValueError:
                    pass
        port.send(dialect["ai_stop"])
        summary["returned_idle"] = bool(port.wait_for(r"xmini_business: phase=0 owner=0", 10, start))
        after, session = xmini_media(port)
        summary["media"] = delta(after, before)
        summary["heap"] = xmini_heap(port)
        summary["samples"] = samples
        lost = summary["media"].get("rx_dropped", 0)
        rx = summary["media"].get("rx", 0)
        summary["rx_drop_pct"] = round(100 * lost / max(1, rx + lost), 2)
        summary["pass"] = bool(summary["active"] and summary["returned_idle"] and rx > 0
                               and summary["media"].get("tx", 0) > 0)
        if frames:
            png(frames, d / "screens.png")
    finally:
        port.close()
    finish(d, summary)


# ---------------------------------------------------------------- call test
def contact_index(port, device_id):
    text = "\n".join(port.send("AT+CONTACTS", wait=3))
    for line in text.splitlines():
        if device_id in line:
            m = re.search(r"(?:index=|\+CONTACT:\s*)(\d+)", line)
            if m:
                return int(m.group(1))
    raise SystemExit(f"{device_id} not in caller contacts")


PATTERNS = {
    "xmini": {"ringing": r"phase=3 owner=[23]", "active": r"phase=6 owner=[23]", "idle": r"phase=0 owner=0"},
    "cores3": {"ringing": r"state=call-incoming", "active": r"state=call-active", "idle": r"state=waiting"},
    "szpi": {"ringing": r"state=call-incoming", "active": r"state=call-active", "idle": r"state=waiting"},
}


def media(port, kind):
    return xmini_media(port)[0] if kind == "xmini" else cores3_media(port)[0]


def cmd_call(args):
    a_name, a = resolve(args.caller)
    b_name, b = resolve(args.callee)
    if sdk_requested(args, a) or sdk_requested(args, b, peer=True):
        return sdk_pair(args, a_name, a, b_name, b)
    if not b.get("device_id"):
        sys.exit(f"{b_name} has no device_id in devices.json")
    d = run_dir(f"call-{a_name}-{b_name}")
    pa = Port(a["port"], d / f"{a_name}.log")
    pb = Port(b["port"], d / f"{b_name}.log")
    da, db = DIALECT[a["kind"]], DIALECT[b["kind"]]
    sa, sb = PATTERNS[a["kind"]], PATTERNS[b["kind"]]
    summary = {"test": "call", "caller": a_name, "callee": b_name, "seconds": args.secs}
    shots = {a_name: [], b_name: []}

    def shot(name, port, kind):
        if args.screens and kind == "xmini":
            try:
                shots[name].append(grab_screen(port))
            except ValueError:
                pass
    try:
        require_legacy(pa)
        require_legacy(pb)
        for port, dl in ((pa, da), (pb, db)):
            if dl["dismiss"]:
                port.send(dl["dismiss"])
        a_before, b_before = media(pa, a["kind"]), media(pb, b["kind"])
        mark_a, mark_b = pa.mark(), pb.mark()
        if a["kind"] == "xmini":  # dial needs the device contacts in the cache
            pa.send("AT+CONTACTS DEV", wait=2)
            pa.wait_for(r"\+CONTACTS: .*loading=0", 1)
            time.sleep(2)
        index = contact_index(pa, b["device_id"]) if "{index}" in da["dial"] else 0
        t_dial = time.monotonic()
        pa.send(da["dial"].format(id=b["device_id"], index=index))
        ring = pb.wait_for(sb["ringing"], 15, mark_b)
        summary["ringing_s"] = round(time.monotonic() - t_dial, 2) if ring else None
        if not ring:
            summary["pass"] = False
            summary["failure"] = "callee never rang"
            code = pa.wait_for(r"code=(\d{5})", 1, mark_a)
            if code:
                summary["server_code"] = code[1]
            return
        shot(a_name, pa, a["kind"])
        shot(b_name, pb, b["kind"])
        time.sleep(args.ring)
        pb.send(db["answer"])
        t_answer = time.monotonic()
        active = pa.wait_for(sa["active"], 10, mark_a) and pb.wait_for(sb["active"], 10, mark_b)
        summary["connected_s"] = round(time.monotonic() - t_answer, 2) if active else None
        time.sleep(2)
        shot(a_name, pa, a["kind"])
        shot(b_name, pb, b["kind"])
        time.sleep(args.secs)
        a_mid, b_mid = media(pa, a["kind"]), media(pb, b["kind"])
        for role, port, kind in (("caller", pa, a["kind"]), ("callee", pb, b["kind"])):
            if kind == "xmini":
                summary[f"{role}_echo"] = xmini_echo(port)
        mark_a, mark_b = pa.mark(), pb.mark()
        pa.send(da["hangup"])
        summary["callee_saw_hangup"] = bool(pb.wait_for(sb["idle"] + "|对方已挂断", 10, mark_b))
        summary["caller_idle"] = bool(pa.wait_for(sa["idle"], 10, mark_a))
        time.sleep(1)
        shot(a_name, pa, a["kind"])
        shot(b_name, pb, b["kind"])
        for name, frames in shots.items():
            if frames:
                png(frames, d / f"{name}-screens.png")
        # Xmini counters are cumulative; CoreS3 counters restart with each call.
        summary["caller_media"] = delta(a_mid, a_before) if a["kind"] == "xmini" else a_mid
        summary["callee_media"] = delta(b_mid, b_before) if b["kind"] == "xmini" else b_mid
        for role, port, kind in (("caller", pa, a["kind"]), ("callee", pb, b["kind"])):
            m = summary[f"{role}_media"]
            lost = m.get("rx_dropped", m.get("dropped", 0))
            summary[f"{role}_rx_drop_pct"] = round(100 * lost / max(1, m.get("rx", 0) + lost), 2)
            if kind == "xmini":
                summary[f"{role}_heap"] = xmini_heap(port)
        ok_media = all(summary[f"{r}_media"].get(k, 0) > 0 for r in ("caller", "callee") for k in ("tx", "rx"))
        summary["pass"] = bool(summary["connected_s"] is not None and ok_media and summary["callee_saw_hangup"]
                               and summary["caller_idle"] and summary["caller_rx_drop_pct"] < 2
                               and summary["callee_rx_drop_pct"] < 2)
    finally:
        pa.close()
        pb.close()
        finish(d, summary)


# ---------------------------------------------------------------- AI through the real key flow
def cmd_ai_keys(args):
    """Home -> double-click starts AI -> click interrupts the answer -> hold ends."""
    name, info = resolve(args.dev)
    if sdk_requested(args, info):
        return sdk_ai_run(args, name, info, keys=True)
    d = run_dir(f"ai-keys-{name}")
    p = Port(info["port"], d / f"{name}.log")
    frames, summary = [], {"test": "ai-keys", "device": name}

    def shot():
        try:
            frames.append(grab_screen(p))
        except ValueError:
            pass
    try:
        require_legacy(p)
        p.send("AT+CALL DISMISS")
        ui = "\n".join(p.send("AT+UI"))
        summary["started_on_home"] = "page=0 " in ui
        shot()
        m = p.mark()
        p.send("AT+KEY DOUBLE")
        summary["ai_active"] = bool(p.wait_for(r"phase=6 owner=1", 12, m))
        summary["answer_started"] = bool(p.wait_for(r"AI profile verified", 1, m))
        time.sleep(1.5)
        shot()
        m = p.mark()
        p.send("AT+KEY CLICK")
        hit = p.wait_for(r"AI interrupt sent gen=\d+ error=(-?\d+)", 3, m)
        summary["interrupt_sent"] = bool(hit) and hit[1] == "0"
        time.sleep(1)
        shot()
        time.sleep(args.secs)
        shot()
        m = p.mark()
        p.send("AT+KEY HOLD")
        summary["ended"] = bool(p.wait_for(r"phase=0 owner=0", 10, m))
        time.sleep(1)
        shot()
        summary["back_home"] = "page=0 " in "\n".join(p.send("AT+UI"))
        summary["heap"] = xmini_heap(p)
        errors = [l for l in p.since(0) if re.search(r"xmini_business: phase=\d+ owner=\d+ gen=\d+ error=-|MQTT error", l)]
        summary["errors"] = errors[:5]
        summary["pass"] = all(summary[k] for k in ("ai_active", "interrupt_sent", "ended", "back_home")) and not errors
    finally:
        p.close()
        if frames:
            png(frames, d / "screens.png")
        finish(d, summary)


# ---------------------------------------------------------------- memory soak (leak trend)
def idle_heap(port, settle=3.0):
    time.sleep(settle)
    return xmini_heap(port)


def cmd_soak(args):
    """Repeat AI and (optionally) a device call; sample the idle heap after every round.

    A leak shows as a steady fall of idle free/largest across rounds; the minimum
    (low-water mark) only tells the worst moment. Fit a line to judge, not one sample.
    """
    name, info = resolve(args.dev)
    if sdk_requested(args, info):
        if args.peer:
            raise ConsoleError("use a separate SDK call run with both build identities confirmed")
        if not 0 < args.secs <= 600:
            raise ConsoleError("SDK soak session duration must be in (0, 600] seconds")
        if not sdk_ai_run(args, name, info, rounds=args.rounds, soak=True):
            raise ConsoleError("SDK soak failed; see the run summary")
        return
    peer = resolve(args.peer) if args.peer else None
    d = run_dir(f"soak-{name}")
    p = Port(info["port"], d / f"{name}.log")
    pp = Port(peer[1]["port"], d / f"{peer[0]}.log") if peer else None
    rows = []
    try:
        require_legacy(p)
        if pp:
            require_legacy(pp)
        p.send("AT+CALL DISMISS")
        rows.append({"round": 0, **idle_heap(p)})
        print(rows[-1], flush=True)
        for r in range(1, args.rounds + 1):
            m = p.mark()
            p.send("AT+AI START")
            ai_ok = bool(p.wait_for(r"phase=6 owner=1", 15, m))
            time.sleep(args.secs)
            p.send("AT+AI STOP")
            p.wait_for(r"phase=0 owner=0", 10, m)
            call_ok = None
            if pp:
                p.send("AT+CONTACTS DEV", wait=2)
                time.sleep(1.5)
                m, mp = p.mark(), pp.mark()
                p.send(f"AT+CALL DEVICE {peer[1]['device_id']}")
                if pp.wait_for(PATTERNS[peer[1]["kind"]]["ringing"], 15, mp):
                    pp.send(DIALECT[peer[1]["kind"]]["answer"])
                    call_ok = bool(p.wait_for(r"phase=6 owner=2", 10, m))
                    time.sleep(args.secs)
                p.send("AT+CALL HANGUP")
                p.wait_for(r"phase=0 owner=0", 10, m)
                p.send("AT+CALL DISMISS")
            row = {"round": r, "ai": ai_ok, "call": call_ok, **idle_heap(p)}
            rows.append(row)
            print(row, flush=True)
    finally:
        p.close()
        if pp:
            pp.close()
    xs = [r["round"] for r in rows]

    def slope(key):
        ys = [r.get(key, 0) for r in rows]
        n = len(xs)
        if n < 3:
            return None
        mx, my = sum(xs) / n, sum(ys) / n
        den = sum((x - mx) ** 2 for x in xs)
        return round(sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den, 1) if den else None
    summary = {"test": "soak", "device": name, "peer": peer[0] if peer else None, "rounds": args.rounds,
               "free_bytes_per_round": slope("free"), "largest_bytes_per_round": slope("largest"),
               "first": rows[0], "last": rows[-1], "rows": rows}
    # Judge on the idle free trend: more than 256 B lost per round on average is a leak suspect.
    s = summary["free_bytes_per_round"]
    summary["leak_suspect"] = s is not None and s < -256
    summary["pass"] = not summary["leak_suspect"]
    (d / "rows.json").write_text(json.dumps(rows, indent=1), encoding="utf-8")
    finish(d, summary)


def sdk_ai_run(args, name, info, keys=False, rounds=1, soak=False):
    if not 1 <= rounds <= 100:
        raise ConsoleError("AI soak rounds must be in 1..100")
    test = "soak" if soak else "ai-keys" if keys else "ai"
    d = run_dir(f"{test}-{name}")
    summary = {"test": test, "device": name, "pass": False, "rounds": []}
    if soak:
        summary.update(requested_rounds=rounds, same_boot_verified=False, idle_free_gate={"status": "not_verified"})
    port = Port(info["port"], d / f"{name}.log")
    try:
        mark = port.mark() if soak else None
        session = sdk_session(args, info, port)
        start, stop = ("AT+KEY DOUBLE", "AT+KEY HOLD") if keys else ("AT+AI START", "AT+AI STOP")
        click = "AT+KEY CLICK"
        for command in (start, stop, click) if keys else (start, stop):
            session.check(command)
        witness = None
        if soak:
            from sdk_soak import Evidence, difference, idle_free_gate
            for command in ("AT+VERSION", "AT+STATE", "AT+AI", "AT+LINK", "AT+MEM"):
                session.check(command)
            witness = Evidence(session, port, mark, BOOT_BANNER)
            summary["first"] = witness.settled_memory()
        for index in range(rounds):
            before_round = witness.checkpoint(idle=True) if witness else None
            if not witness and sdk_bench.state(session).get("activity") != "none":
                raise ConsoleError("device must be idle before the AI run")
            before = sdk_bench.link_media(session)
            phase = lambda: sdk_bench.one(session, "AT+AI", "+AI: ")
            view = (sdk_bench.one(session, "AT+UI", "+UI: ")["view"] if keys else None)
            live = False
            generation = None
            running = {"listening", "thinking", "speaking"}
            try:
                session.send(start)
                live = True
                active = sdk_bench.poll(phase, lambda row: row.get("phase") in running, 15)
                if witness:
                    generation = sdk_bench.unsigned(active, "gen")
                    if not generation:
                        raise ConsoleError("SDK soak could not identify the active AI generation")
                if keys:
                    sdk_bench.poll(phase, lambda row: row.get("phase") == "speaking", 10)
                    session.send(click)
                    sdk_bench.poll(phase, lambda row: row.get("phase") == "listening", 6)
                time.sleep(args.secs)
                current = sdk_bench.link_media(session)
                if witness:
                    witness.checkpoint()
                    observed = phase()
                    if observed.get("phase") not in running or sdk_bench.unsigned(observed, "gen") != generation:
                        raise ConsoleError("SDK soak lost the active AI generation before stop")
                session.send(stop)
                live = False
                sdk_bench.poll(phase, lambda row: row.get("phase") == "idle", 12)
                sdk_bench.poll(lambda: sdk_bench.state(session), lambda row: row.get("activity") == "none", 12)
                if keys:
                    sdk_bench.poll(lambda: sdk_bench.one(session, "AT+UI", "+UI: "), lambda row: row.get("view") == view, 5)
                if getattr(args, "screens", False):
                    width, height, rgb = decode_lines(session.send("AT+SCREEN", wait=5), sdk_only=True)
                    (d / f"{name}-{index + 1}.png").write_bytes(screen_png(width, height, rgb))
                row = {"media": sdk_bench.counter_delta(current, before)}
                if witness:
                    row.update(round=index + 1, generation=generation, before=before_round,
                               after=witness.checkpoint(idle=True))
                else:
                    row.update(sdk_bench.health(session))
                summary["rounds"].append(row)
            finally:
                if live and session.usable:
                    try:
                        if not witness:
                            session.send(stop)
                        else:
                            witness.checkpoint()
                            observed = phase()
                            if observed.get("phase") != "idle":
                                if not generation or sdk_bench.unsigned(observed, "gen") != generation:
                                    raise ConsoleError("cannot identify an owned AI generation for cleanup")
                                session.send(stop)
                    except ConsoleError as error:
                        summary["cleanup_failure"] = str(error)
                elif live and witness:
                    summary["cleanup_failure"] = "device identity or command outcome could not be reverified"
        if witness:
            summary["last"] = witness.settled_memory()
            delta = difference(summary["first"], summary["last"])
            summary.update(same_boot_verified=True, free_delta_bytes=delta["free"], largest_delta_bytes=delta["largest"],
                           idle_free_gate=idle_free_gate(rounds, delta["free"]["internal"]))
        memory_ok = summary.get("idle_free_gate", {}).get("status") != "fail"
        summary["pass"] = memory_ok and len(summary["rounds"]) == rounds and all(
            row["media"]["tx"] > 0 and row["media"]["rx"] > 0 for row in summary["rounds"])
    except ConsoleError as error:
        summary["failure"] = str(error)
    finally:
        port.close()
        finish(d, summary)
    if soak:
        return summary["pass"]


# ---------------------------------------------------------------- per-frame trace (checksum compare)
TRACE_LINE = re.compile(r"\+TRACE:(\d),(\d+),(\d+),(\d+),([0-9a-f]{8}),(\d+),(\d+),(\d+)")
SDK_TRACE_LINE = re.compile(TRACE_LINE.pattern + r"(?:,(\d+),(\d+))?\Z")
STATE = {0: "closed", 1: "attenuated", 2: "open", 3: "n/a"}


def dump_trace(port, sdk=False):
    lines = (port.send("AT+TRACE DUMP", wait=5) if sdk else
             port.send("AT+TRACE DUMP", wait=40, until=r"^(OK|ERROR.*)$"))
    rings = {0: [], 1: []}
    for line in lines:
        m = SDK_TRACE_LINE.fullmatch(line) if sdk else TRACE_LINE.search(line)
        if m:
            d, i, t, ln, fnv, db, pre, flags = m.groups()[:8]
            if sdk and (d not in {"0", "1"} or (d == "1") != (m[9] is not None)):
                raise ConsoleError("unexpected SDK trace frame shape")
            rings[int(d)].append({"i": int(i), "t": int(t), "len": int(ln), "fnv": fnv, "db": int(db),
                                  "pre": int(pre), "flags": int(flags)})
            if sdk and d == "1":
                rings[1][-1].update(sender_ms=int(m[9]), duration_ms=int(m[10]))
    if sdk:
        header = fields(lines, "+TRACE: ")
        if any(len(rings[direction]) != sdk_bench.unsigned(header, key) or
               [row["i"] for row in rings[direction]] != list(range(len(rings[direction])))
               for direction, key in enumerate(("tx", "rx"))):
            raise ConsoleError("SDK trace indexes or frame counts do not match its header")
    return rings


def sdk_pair(args, a_name, a, b_name, b, trace=False):
    test = "trace" if trace else "call"
    if a["port"] == b["port"]:
        raise ConsoleError("two different local serial ports are required")
    d = run_dir(f"{test}-{a_name}-{b_name}")
    summary = {"test": test, "caller": a_name, "callee": b_name, "pass": False}
    with ExitStack() as exits:
        pa = Port(a["port"], d / f"{a_name}.log")
        exits.callback(pa.close)
        pb = Port(b["port"], d / f"{b_name}.log")
        exits.callback(pb.close)
        rings = None
        try:
            sa, sb = sdk_session(args, a, pa), sdk_session(args, b, pb, peer=True)
            dev = sdk_bench.one(sb, "AT+BIND", "+BIND: ").get("device")
            if not dev or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", dev) or dev == "-":
                raise ConsoleError("callee has no confirmed device id")
            if any(sdk_bench.state(s).get("activity") != "none" for s in (sa, sb)):
                raise ConsoleError("both devices must be idle before the call")
            dial, answer, hangup = f"AT+CALL DEVICE {dev}", "AT+CALL ACCEPT", "AT+CALL HANGUP"
            for session, commands in ((sa, (dial, hangup)), (sb, (answer,))):
                for command in commands:
                    session.check(command)
            follow_cmd = "AT+LOG FOLLOW 1"
            off = "AT+LOG FOLLOW 0"
            follow = all(s.has(follow_cmd) and s.has(off) and {follow_cmd, off} <= s.permits for s in (sa, sb))
            arm = "AT+TRACE ARM" + (f" {args.frames}" if trace and args.frames is not None else "")
            armed = []
            live_follow = []
            live_call = False
            for session in (sa, sb):
                if follow:
                    session.check(follow_cmd)
                    session.check(off)
                if trace:
                    for cmd in (arm, "AT+TRACE OFF", "AT+TRACE DUMP"):
                        session.check(cmd)
            try:
                for session in (sa, sb):
                    if follow:
                        session.send(follow_cmd)
                        live_follow.append(session)
                    if trace:
                        session.send(arm)
                        armed.append(session)
                before = [sdk_bench.link_media(s) for s in (sa, sb)]
                ma, mb = pa.mark(), pb.mark()
                sa.send(dial)
                live_call = True
                current_a = sdk_bench.wait_call(pa, sa, "outgoing", ma, follow=follow)
                current_b = sdk_bench.wait_call(pb, sb, "incoming", mb, follow=follow)
                gens = [sdk_bench.unsigned(row, "gen") for row in (current_a, current_b)]
                cams = [(sa, a_name), (sb, b_name)] if getattr(args, "screens", False) else []
                # Screenshot support is optional and actual firmware may refuse it during an activity.
                time.sleep(getattr(args, "ring", 0))
                sb.send(answer)
                for port, session, mark, gen in ((pa, sa, ma, gens[0]), (pb, sb, mb, gens[1])):
                    sdk_bench.wait_call(port, session, "connected", mark, gen, follow=follow)
                time.sleep(args.secs)
                after = [sdk_bench.link_media(s) for s in (sa, sb)]
                marks = (pa.mark(), pb.mark())
                sa.send(hangup)
                live_call = False
                for port, session, mark, gen in zip((pa, pb), (sa, sb), marks, gens):
                    sdk_bench.wait_call(port, session, "ended", mark, gen, follow=follow)
                    sdk_bench.poll(lambda: sdk_bench.state(session), lambda row: row.get("activity") == "none", 10)
                for session, name in cams:
                    width, height, rgb = decode_lines(session.send("AT+SCREEN", wait=5), sdk_only=True)
                    (d / f"{name}.png").write_bytes(screen_png(width, height, rgb))
                counts = [sdk_bench.counter_delta(post, pre) for post, pre in zip(after, before)]
                summary.update(caller_media=counts[0], callee_media=counts[1])
                summary["pass"] = all(row["tx"] > 0 and row["rx"] > 0 and
                                      100 * row["rx_dropped"] / row["rx"] < 2 for row in counts)
                if trace:
                    rings = [dump_trace(s, sdk=True) for s in (sa, sb)]
                    (d / "trace.json").write_text(json.dumps(dict(zip((a_name, b_name), rings))), encoding="utf-8")
                    for name, tx, rx in ((f"{a_name}->{b_name}", rings[0][0], rings[1][1]),
                                         (f"{b_name}->{a_name}", rings[1][0], rings[0][1])):
                        summary[name], _ = direction_report(name, tx, rx)
                    summary["pass"] &= all(ring[0] and ring[1] for ring in rings)
            finally:
                cleanup = [(sa, hangup)] if live_call else []
                cleanup += [(s, "AT+TRACE OFF") for s in reversed(armed)]
                cleanup += [(s, off) for s in reversed(live_follow)]
                for session, command in cleanup:
                    try:
                        session.send(command)
                    except ConsoleError as error:
                        summary.setdefault("cleanup_errors", []).append(str(error))
                        summary["pass"] = False
        except ConsoleError as error:
            summary["failure"] = str(error)
            summary["pass"] = False
        finally:
            finish(d, summary)


def match_frames(tx, rx, window=200):
    """Greedy in-order match of sender checksums against receiver checksums."""
    j, matched, reordered = 0, [], 0
    rx_valid = [r for r in rx if r["len"]]
    used = set()
    for k, f in enumerate(tx):
        hit = None
        for c in range(j, min(len(rx_valid), j + window)):
            if c not in used and rx_valid[c]["fnv"] == f["fnv"]:
                hit = c
                break
        if hit is None:
            for c in range(max(0, j - window), j):  # late/out-of-order
                if c not in used and rx_valid[c]["fnv"] == f["fnv"]:
                    hit, reordered = c, reordered + 1
                    break
        if hit is not None:
            used.add(hit)
            matched.append((k, hit))
            j = max(j, hit + 1)
    return matched, rx_valid


def direction_report(name, tx, rx):
    matched, rx_valid = match_frames(tx, rx)
    hits = {k for k, _ in matched}
    silent = [f for f in tx if f["db"] <= 3]
    lost = [f for k, f in enumerate(tx) if k not in hits]
    lost_audible = [f for f in lost if f["db"] > 30]
    first_rx = rx_valid[0]["fnv"] if rx_valid else None
    rep = {
        "sent": len(tx), "received_played": len(rx_valid), "underruns": sum(1 for r in rx if not r["len"]),
        "matched": len(matched), "unmatched_sent": len(lost), "unmatched_audible": len(lost_audible),
        "corrupt_or_lost_pct": round(100 * len(lost) / max(1, len(tx)), 2),
        "silent_sent": len(silent),
        "sender_state": {STATE[s]: sum(1 for f in tx if f["flags"] & 3 == s) for s in STATE},
        "level_db_sent_median": sorted(f["db"] for f in tx)[len(tx) // 2] if tx else None,
        "level_db_mic_median": sorted(f["pre"] for f in tx)[len(tx) // 2] if tx else None,
    }
    rep["_first_rx_matched"] = first_rx is not None
    return rep, matched


def cmd_trace(args):
    a_name, a = resolve(args.caller)
    b_name, b = resolve(args.callee)
    if sdk_requested(args, a) or sdk_requested(args, b, peer=True):
        return sdk_pair(args, a_name, a, b_name, b, trace=True)
    d = run_dir(f"trace-{a_name}-{b_name}")
    pa, pb = Port(a["port"], d / f"{a_name}.log"), Port(b["port"], d / f"{b_name}.log")
    da, db_ = DIALECT[a["kind"]], DIALECT[b["kind"]]
    sa, sb = PATTERNS[a["kind"]], PATTERNS[b["kind"]]
    summary = {"test": "trace", "caller": a_name, "callee": b_name, "seconds": args.secs}
    try:
        require_legacy(pa)
        require_legacy(pb)
        for port, dl, kind in ((pa, da, a["kind"]), (pb, db_, b["kind"])):
            if dl["dismiss"]:
                port.send(dl["dismiss"])
            frames = args.frames if args.frames is not None else 1500
            frames = min(frames, 3000) if kind == "xmini" else frames
            reply = port.send(f"AT+TRACE ARM {frames}")
            if not any(l.strip() == "OK" for l in reply):
                summary["failure"] = f"trace arm failed: {reply[-1:]}"
                return
        if a["kind"] == "xmini":
            pa.send("AT+CONTACTS DEV", wait=2)
            time.sleep(2)
        ma, mb = pa.mark(), pb.mark()
        index = contact_index(pa, b["device_id"]) if "{index}" in da["dial"] else 0
        pa.send(da["dial"].format(id=b["device_id"], index=index))
        if not pb.wait_for(sb["ringing"], 15, mb):
            summary["failure"] = "callee never rang"
            return
        time.sleep(1)
        pb.send(db_["answer"])
        if not pa.wait_for(sa["active"], 10, ma):
            summary["failure"] = "not connected"
            return
        print(f"call up: talk now for {args.secs:.0f} s", flush=True)
        time.sleep(args.secs)
        pa.send(da["hangup"])
        pa.wait_for(sa["idle"], 10, ma)
        time.sleep(1)
        ra, rb = dump_trace(pa), dump_trace(pb)
        for port in (pa, pb):
            port.send("AT+TRACE OFF")
        (d / "trace.json").write_text(json.dumps({a_name: ra, b_name: rb}), encoding="utf-8")
        ab, mab = direction_report(f"{a_name}->{b_name}", ra[0], rb[1])
        ba, mba = direction_report(f"{b_name}->{a_name}", rb[0], ra[1])
        summary[f"{a_name}->{b_name}"] = ab
        summary[f"{b_name}->{a_name}"] = ba
        # When the Xmini attenuated/closed its own speech, what was the peer sending at that time?
        x_name, x_ring, p_ring = (a_name, ra, rb) if a["kind"] == "xmini" else (b_name, rb, ra)
        if x_ring[0]:
            attenuated = [f for f in x_ring[0] if f["flags"] & 3 == 1 and f["pre"] > 50]
            summary[f"{x_name}_speech_frames"] = sum(1 for f in x_ring[0] if f["pre"] > 50)
            summary[f"{x_name}_speech_attenuated"] = len(attenuated)
            summary[f"{x_name}_speech_closed"] = sum(1 for f in x_ring[0] if f["flags"] & 3 == 0 and f["pre"] > 50)
            peer_open = [f for f in p_ring[0] if f["flags"] & 3 == 2]
            summary["peer_gate_open_frames"] = len(peer_open)
            summary["peer_sent_median_db_when_open"] = (sorted(f["db"] for f in peer_open)[len(peer_open) // 2]
                                                        if peer_open else None)
        summary["pass"] = ab["corrupt_or_lost_pct"] < 2 and ba["corrupt_or_lost_pct"] < 2
    finally:
        pa.close()
        pb.close()
        finish(d, summary)


# ---------------------------------------------------------------- WeChat (phone) tests
def phone_profile():
    return load_devices().get("phone", {})


def tap(xy):
    subprocess.run(["adb", "shell", "input", "tap", str(xy[0]), str(xy[1])], capture_output=True)


def phone_shot(path):
    path.write_bytes(subprocess.run(["adb", "exec-out", "screencap", "-p"], capture_output=True).stdout)


def screen_pixels():
    raw = subprocess.run(["adb", "exec-out", "screencap"], capture_output=True).stdout
    w, h = struct.unpack("<II", raw[:8])
    header = len(raw) - w * h * 4

    def px(x, y):
        o = header + (y * w + x) * 4
        return tuple(raw[o:o + 3])
    return px


def near(c, ref, tol=30):
    return all(abs(a - b) <= tol for a, b in zip(c, ref))


GREEN, GREY_BG, CALL_BG = (88, 190, 106), (246, 246, 246), (44, 44, 44)


def phone_state(prof=None, px=None):
    """'list' (device list), 'call' (dark call/ended page), 'banner' (incoming answer button) or 'other'."""
    prof = phone_profile() if prof is None else prof
    px = screen_pixels() if px is None else px
    if near(px(*prof["answer_banner"]), GREEN, 40):
        return "banner"
    if (near(px(100, 1600), GREY_BG, 12) and near(px(*prof["list_probe"]), GREEN)
            and near(px(*prof["add_button_probe"]), GREEN)):
        return "list"
    if near(px(100, 1600), CALL_BG, 20):
        return "call"
    return "other"


def read_phone_ui(serial=None, timeout=6):
    """Read a transient device-local dump without saving any user UI to runs/."""
    path = f"/data/local/tmp/xt-ui-{uuid.uuid4().hex}.xml"
    script = (f'umask 077; trap "rm -f {path}" EXIT; '
              f'uiautomator dump {path} >/dev/null && cat {path}')
    try:
        target = ["-s", serial] if serial else []
        result = subprocess.run(["adb", *target, "shell", script], capture_output=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return result.stdout if result.returncode == 0 else None


def reopen_miniprogram():
    """Return a preflight outcome; never navigate a known auth/empty page."""
    prof = phone_profile()
    for attempt in range(2):
        state = phone_state()
        if state == "list":
            return phone_ui.Page.READY
        blocker = phone_ui.blocked_page(read_phone_ui())
        if blocker != phone_ui.Page.UNKNOWN:
            return blocker
        if attempt:
            return phone_ui.Page.UNKNOWN
        if state == "call":  # live page: hang up; ended pages: 关闭 (two layouts)
            for xy in prof["close_buttons"]:
                tap(xy)
                time.sleep(1.5)
                if phone_state() != "call":
                    break
            continue
        subprocess.run(["adb", "shell", "am", "start", "-f", "0x14000000", "-n", "com.tencent.mm/.ui.LauncherUI"],
                       capture_output=True)  # NEW_TASK|CLEAR_TOP: WeChat main tab
        time.sleep(2)
        subprocess.run(["adb", "shell", "input", "swipe", "720", "500", "720", "2200", "400"], capture_output=True)
        time.sleep(1.5)
        tap(prof["drawer_xiaotai"])
        time.sleep(3)


def outgoing_phone(info, *, expected_serial=None):
    prof = phone_profile()
    if expected_serial is not None and prof.get("adb_serial") != expected_serial:
        raise ConsoleError("configured phone does not match the authorized gate phone")
    phone = wx_phone.VerifiedPhone(prof, info.get("phone_wx_out"), KIT)
    image = phone.frame()
    width, height = image.size
    pixels = lambda x, y: image.getpixel((x, y)) if 0 <= x < width and 0 <= y < height else (-999,) * 3
    if phone.matches(image):
        raise ConsoleError("selected phone already shows the expected call banner; no new call started")
    if phone_state(prof, pixels) != "list":
        page = phone_ui.blocked_page(read_phone_ui(phone.serial))
        raise ConsoleError(phone_ui.FAILURE[page])
    return phone


def outgoing_phone_hangup(phone, *, confirm=None):
    try:
        point = wx_phone.coordinate(phone_profile().get("hangup"), 2)
    except ValueError as error:
        raise ConsoleError("configure the phone hangup point before requesting --phone-hangup") from error
    if not wx_phone.live_hangup_at(read_phone_ui(phone.serial), phone.caller, point):
        raise ConsoleError("phone hangup control was not verified; no phone action sent")
    if confirm is not None:
        confirm("PHONE_WX_OUT_HANGUP")
    phone.tap(point)


def outgoing_phone_receipt(phone):
    deadline = time.monotonic() + 10
    while (remaining := deadline - time.monotonic()) > 0:
        value = wx_phone.ended_seconds(read_phone_ui(phone.serial, timeout=min(6, remaining)), phone.caller)
        if value is not None:
            return value
        time.sleep(min(0.4, max(0, deadline - time.monotonic())))
    raise ConsoleError("phone did not show a verified ended receipt for the caller with nonzero duration")


def hangup_on_phone(timeout=8):
    """Hang up from the call page; close floating windows covering it first."""
    prof = phone_profile()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        px = screen_pixels()
        if near(px(*prof["hangup"]), (234, 92, 85), 40):  # red hang-up button visible
            tap(prof["hangup"])
            return True
        tap(prof["popup_close"])  # e.g. 服务通知 floating window
        time.sleep(1)
    return False


def first_wechat_contact(port, kind):
    if kind == "xmini":
        port.send("AT+CONTACTS ALL", wait=2)
        time.sleep(2)
        for line in port.send("AT+CONTACTS", wait=3):
            m = re.search(r"\+CONTACT: index=\d+ wechat=1 id=(\S+)", line)
            if m:
                return m.group(1)
    else:
        for line in port.send("AT+CONTACTS", wait=3):
            m = re.match(r"\+CONTACT:(\d+),1,", line)
            if m:
                return int(m.group(1))
    raise SystemExit("no WeChat contact on device")


def wx_run(args, incoming):
    name, info = resolve(args.dev)
    if sdk_requested(args, info):
        return sdk_wechat_run(args, name, info, incoming)
    kind = info["kind"]
    prof = phone_profile()
    d = run_dir(f"wx-{'in' if incoming else 'out'}-{name}")
    port = Port(info["port"], d / f"{name}.log")
    pat, dl = PATTERNS[kind], DIALECT[kind]
    s = {"test": "wx-in" if incoming else "wx-out", "device": name}
    frames = []
    phone_ready = False

    def shot():
        if kind == "xmini":
            try:
                frames.append(grab_screen(port))
            except ValueError:
                pass
    try:
        require_legacy(port)
        if dl["dismiss"]:
            port.send(dl["dismiss"])
        before = media(port, kind)
        m = port.mark()
        t = time.monotonic()
        if incoming:
            page = reopen_miniprogram()
            phone_ready = page == phone_ui.Page.READY
            if not phone_ready:
                s["pass"] = False
                s["failure"] = phone_ui.FAILURE[page]
                return
        else:
            phone = outgoing_phone(info)
        if incoming:  # phone calls the device; the device answers over serial
            tap(info["phone_wx_button"])
            ring = port.wait_for(pat["ringing"], 20, m)
            s["device_ringing_s"] = round(time.monotonic() - t, 2) if ring else None
            if not ring:
                s["pass"] = False
                return
            time.sleep(1)
            shot()
            port.send(dl["answer"])
        else:  # the device calls the phone; the phone answers from the banner
            target = first_wechat_contact(port, kind)
            port.send(f"AT+CALL WX {target}" if kind == "xmini" else f"AT+CALL {target}")
            try:
                answered = phone.answer()
            except ConsoleError:
                port.send(dl["hangup"])
                raise
            shot()
            if not answered:
                s["pass"] = False
                s["failure"] = "no verified incoming banner for the expected caller"
                port.send(dl["hangup"])
                return
            s["phone_banner_verified"] = s["phone_answer_tapped"] = True
        t = time.monotonic()
        active = port.wait_for(pat["active"], 15, m)
        s["connected_s"] = round(time.monotonic() - t, 2) if active else None
        if not incoming:
            s["device_connected"] = bool(active)
        time.sleep(3)
        if incoming:
            phone_shot(d / "phone-active.png")
        shot()
        time.sleep(args.secs)
        mid = media(port, kind)
        s["media"] = delta(mid, before) if kind == "xmini" else mid
        m2 = port.mark()
        if args.phone_hangup:
            if incoming:
                s["phone_hangup_tapped"] = hangup_on_phone()
            else:
                try:
                    outgoing_phone_hangup(phone)
                except ConsoleError:
                    port.send(dl["hangup"])
                    raise
                s["phone_hangup_tapped"] = True
        else:
            port.send(dl["hangup"])
        s["hangup_by"] = "phone" if args.phone_hangup else "device"
        s["device_idle"] = bool(port.wait_for(pat["idle"], 10, m2))
        s["errors"] = [l for l in port.since(m)
                       # -40008 is "remote side closed": how a call normally ends, and the
                       # status line keeps it as the last error until the next one.
                       if re.search(r"error=-(?!40008\b)|invalid (hangup|WX|command)|MQTT error", l)][:5]
        time.sleep(1.5)
        if incoming:
            phone_shot(d / "phone-after.png")
        else:
            s["phone_connected_duration_s"] = outgoing_phone_receipt(phone)
        shot()
        mm = s["media"]
        lost = mm.get("rx_dropped", mm.get("dropped", 0))
        s["rx_drop_pct"] = round(100 * lost / max(1, mm.get("rx", 0) + lost), 2)
        s["pass"] = bool(s["connected_s"] is not None and mm.get("tx", 0) > 0 and mm.get("rx", 0) > 0
                         and s["device_idle"] and not s["errors"] and s["rx_drop_pct"] < 2)
    except ConsoleError as error:
        s["pass"] = False
        s["failure"] = str(error)
    finally:
        port.close()
        if frames:
            png(frames, d / f"{name}-screens.png")
        if phone_ready:
            reopen_miniprogram()
        finish(d, s)


def wechat_cleanup(session, generation, *, incoming):
    if not generation:
        raise ConsoleError("WeChat generation not confirmed; no cleanup action sent")
    rows = sdk_bench.records(session.send("AT+CALL"), "+CALL: ")
    wechat = [row for row in rows if row.get("kind") == "wechat"]
    if len(wechat) != 1:
        raise ConsoleError("WeChat call status is not unique; no cleanup action sent")
    target = wechat[0]
    state = target.get("state")
    current = sdk_bench.unsigned(target, "gen")
    inactive = {"idle", "ended", "error"}
    if state in inactive and not current:
        return None
    if current != generation:
        raise ConsoleError("WeChat call generation changed; no cleanup action sent")
    # Local AT controls are shared by both providers and have no generation argument.
    if any(row.get("kind") != "wechat" and
           (row.get("state") not in inactive or sdk_bench.unsigned(row, "gen")) for row in rows):
        raise ConsoleError("another call provider owns a session; no cleanup action sent")
    if incoming and state == "incoming":
        return "AT+CALL REJECT"
    if state in {"connecting", "active"} or not incoming and state == "outgoing":
        return "AT+CALL HANGUP"
    raise ConsoleError("unexpected WeChat call state; no cleanup action sent")


def require_incoming_binding(session, expected):
    if not isinstance(expected, str) or expected.strip() in {"", "-"}:
        raise ConsoleError("wx-in unverified: configure the exact board binding ID")
    binding = sdk_bench.one(session, "AT+BIND", "+BIND: ")
    # Phase 1 is BOUND; signed describes the separate signed-rebinding path.
    if binding.get("device") != expected or binding.get("phase") != "1":
        raise ConsoleError("wx-in unverified: running board does not have the reviewed binding")
    platform = sdk_bench.one(session, "AT+PLATFORM", "+PLATFORM: ")
    if platform.get("online") != "1" or platform.get("signaling") != "1":
        raise ConsoleError("wx-in unverified: board signaling is not online")
    if platform.get("authorized") != "1":
        raise ConsoleError("wx-in unverified: board platform is not authorized")


def sdk_wechat_run(args, name, info, incoming):
    kind = "wechat"
    test = "wx-in" if incoming else "wx-out"
    run = run_dir(f"{test}-{name}")
    port = Port(info["port"], run / f"{name}.log")
    summary = {"test": test, "device": name, "pass": False}
    gate_confirm = getattr(args, "gate_confirm", None)
    gate_serial = getattr(args, "gate_phone_serial", None)
    gated = gate_confirm is not None or gate_serial is not None
    outgoing_end_attempted = False

    def note_cleanup_error(error):
        summary.setdefault("cleanup_errors", []).append(str(error))
        summary["pass"] = False

    def confirm(action):
        nonlocal outgoing_end_attempted
        if gated:
            if identity(session.send("AT+VERSION")) != session.device:
                raise ConsoleError("running device identity changed; no gated action sent")
            if gate_confirm(action) is not True:
                raise ConsoleError(f"gate authorization no longer permits {action}")
        try:
            if action == "PHONE_WX_OUT_ANSWER":
                current = sdk_bench.call_state(session, kind)
                if (not gen or sdk_bench.unsigned(current, "gen") != gen or
                        current.get("state") not in {"outgoing", "connecting"}):
                    raise ConsoleError("outgoing WeChat generation changed; no phone answer sent")
            elif action == "PHONE_WX_OUT_HANGUP" and wechat_cleanup(session, gen, incoming=False) != hangup:
                raise ConsoleError("owned outgoing WeChat call ended; no phone hangup sent")
        except ConsoleError as error:
            outgoing_end_attempted = True
            note_cleanup_error(error)
            raise

    def end_outgoing():
        nonlocal outgoing_end_attempted
        # An observed ownership loss or uncertain write must not be retried in finally.
        outgoing_end_attempted = True
        try:
            confirm(hangup)
            if wechat_cleanup(session, gen, incoming=False) is None:
                return False
            session.send(hangup)
            return True
        except ConsoleError as error:
            note_cleanup_error(error)
            raise

    try:
        if gated and (incoming or not callable(gate_confirm) or not isinstance(gate_serial, str) or not gate_serial):
            raise ConsoleError("gate WeChat requires an authorized outbound phone and action guard")
        session = sdk_session(args, info, port)
        if sdk_bench.state(session).get("activity") != "none":
            raise ConsoleError("device must be idle before the WeChat run")
        command = "AT+CALL ACCEPT" if incoming else f"AT+CALL INDEX {sdk_bench.wechat_contact(session)}"
        hangup = "AT+CALL HANGUP"
        session.check(command)
        session.check(hangup)
        if incoming:
            session.check("AT+CALL REJECT")
            if args.phone_hangup:
                raise ConsoleError("SDK wx-in does not verify phone-side hangup; use device hangup")
            expected = info.get("device_id")
            require_incoming_binding(session, expected)
            phone = wx_phone.VerifiedIncomingPhone(phone_profile(), info.get("phone_wx_in"), KIT, expected)
        else:
            expected = info.get("device_id")
            if not expected or sdk_bench.one(session, "AT+BIND", "+BIND: ").get("device") != expected:
                raise ConsoleError("running device does not match the reviewed wx-out reference")
            phone = outgoing_phone(info, expected_serial=gate_serial) if gated else outgoing_phone(info)
        follow_on, follow_off = "AT+LOG FOLLOW 1", "AT+LOG FOLLOW 0"
        follow = {follow_on, follow_off} <= session.permits and session.has(follow_on) and session.has(follow_off)
        live = watching = incoming_attempted = False
        gen = None
        try:
            if follow:
                confirm(follow_on)
                session.send(follow_on)
                watching = True
            before = sdk_bench.link_media(session)
            mark = port.mark()
            if incoming:
                def confirm_incoming():
                    if identity(session.send("AT+VERSION")) != session.device:
                        raise ConsoleError("wx-in unverified: running board identity changed")
                    if sdk_bench.state(session).get("activity") != "none":
                        raise ConsoleError("wx-in unverified: running board is no longer idle")
                    require_incoming_binding(session, expected)

                phone.dial(confirm_incoming, read_ui=read_phone_ui)
                incoming_attempted = True
                summary["phone_target_verified"] = summary["phone_call_tapped"] = True
                row = sdk_bench.wait_call(port, session, "incoming", mark, kind=kind, follow=follow)
                gen = sdk_bench.unsigned(row, "gen")
                if not gen:
                    raise ConsoleError("incoming WeChat event did not identify a session")
                live = True
                time.sleep(args.ring)
                if wechat_cleanup(session, gen, incoming=True) != "AT+CALL REJECT":
                    raise ConsoleError("owned incoming WeChat call is no longer ringing")
                session.send(command)
            else:
                confirm(command)
                if gated:
                    summary["gate_started"] = True
                session.send(command)
                live = True
                row = sdk_bench.wait_call(port, session, "outgoing", mark, kind=kind, follow=follow)
                gen = sdk_bench.unsigned(row, "gen")
                if not gen:
                    raise ConsoleError("outgoing WeChat event did not identify a session")
                answered = phone.answer(confirm=confirm)
                if not answered:
                    raise ConsoleError("no verified incoming banner for the expected caller")
                summary["phone_banner_verified"] = summary["phone_answer_tapped"] = True
            sdk_bench.wait_call(port, session, "connected", mark, gen, kind=kind, follow=follow)
            if not incoming:
                summary["device_connected"] = True
            time.sleep(args.secs)
            media = sdk_bench.counter_delta(sdk_bench.link_media(session), before)
            mark = port.mark()
            if args.phone_hangup:
                outgoing_phone_hangup(phone, confirm=confirm)
            elif not incoming:
                if not end_outgoing():
                    raise ConsoleError("owned outgoing WeChat call ended before hangup")
            else:
                if wechat_cleanup(session, gen, incoming=True) != hangup:
                    raise ConsoleError("owned incoming WeChat call ended before hangup")
                session.send(hangup)
            sdk_bench.wait_call(port, session, "ended", mark, gen, kind=kind, follow=follow)
            live = False
            sdk_bench.poll(lambda: sdk_bench.state(session), lambda row: row.get("activity") == "none", 10)
            summary["media"] = media
            if not incoming:
                summary["phone_connected_duration_s"] = outgoing_phone_receipt(phone)
            summary["pass"] = media["tx"] > 0 and media["rx"] > 0
        finally:
            cleanup = []
            if incoming and (live or (incoming_attempted and not gen)):
                try:
                    action = wechat_cleanup(session, gen, incoming=True)
                    if action:
                        cleanup.append(action)
                except ConsoleError as error:
                    note_cleanup_error(error)
            elif not incoming and live and not outgoing_end_attempted:
                try:
                    end_outgoing()
                except ConsoleError:
                    pass  # end_outgoing records why it did not send a cleanup action.
            if watching:
                cleanup.append(follow_off)
            for command in cleanup:
                try:
                    confirm(command)
                    session.send(command)
                except ConsoleError as error:
                    note_cleanup_error(error)
    except ConsoleError as error:
        summary["failure"] = str(error)
    finally:
        port.close()
        finish(run, summary)
    return run, summary


def cmd_wx_in(args):
    wx_run(args, True)


def cmd_wx_out(args):
    wx_run(args, False)


# ---------------------------------------------------------------- phone
def adb(*a, binary=False):
    out = subprocess.run(["adb", *a], capture_output=True)
    if out.returncode:
        sys.exit(out.stderr.decode(errors="replace"))
    return out.stdout if binary else out.stdout.decode("utf-8", "replace")


def cmd_phone(args):
    if args.action == "shot":
        Path(args.arg[0]).write_bytes(adb("exec-out", "screencap", "-p", binary=True))
        print(args.arg[0])
    elif args.action == "tap":
        adb("shell", "input", "tap", args.arg[0], args.arg[1])
    elif args.action == "text":
        adb("shell", "input", "text", args.arg[0])
    elif args.action == "key":
        adb("shell", "input", "keyevent", args.arg[0])
    elif args.action == "ui":
        adb("shell", "uiautomator", "dump", "/sdcard/xt_ui.xml")
        Path(args.arg[0]).write_text(adb("shell", "cat", "/sdcard/xt_ui.xml"), encoding="utf-8")
        print(args.arg[0])


# ---------------------------------------------------------------- regression suite
def regress_steps(a, b, kinds, full, phone):
    """The standard two-device regression: calls both ways, AI keys, WeChat, then audio and memory."""
    steps = [["call", a, b, "-s", "8"], ["call", b, a, "-s", "8"]]
    steps += [["ai-keys", dev] for dev in (a, b) if kinds[dev] == "xmini"]
    if phone:
        for dev in (a, b):
            steps += [["wx-in", dev], ["wx-out", dev, "--phone-hangup"]]
    if full:
        steps += [["trace", a, b, "-s", "10", "--frames", "600"],
                  ["soak", a, "--peer", b, "--rounds", "10"]]
    return steps


# ---------------------------------------------------------------- watch
BOOT_BANNER = re.compile(r"ESP-ROM:|rst:0x[0-9a-f]+ \(|^\+READY: at=1 source=")


def health(port, kind):
    """One status sample, the same fields for every board kind."""
    text = "\n".join(port.send("AT+STATUS", wait=3))
    row = {}
    if kind == "xmini":
        text += "\n" + "\n".join(port.send("AT+PRODUCT", wait=2)) + "\n" + "\n".join(port.send("AT+NETWORK", wait=2))
        m = re.search(r"\+HEAP: free=(\d+) largest=(\d+) minimum=(\d+).*?uptime_ms=(\d+)", text)
        if m:
            row.update(free=int(m[1]), largest=int(m[2]), minimum=int(m[3]), uptime_s=int(m[4]) // 1000)
        m = re.search(r"platform_online=(\d) rtc_ready=(\d)", text)
        if m:
            row.update(online=m[1] == "1", rtc=m[2] == "1")
    else:
        m = re.search(r"Heap: internal-free=(\d+) internal-largest=(\d+) psram-free=(\d+) psram-largest=(\d+)", text)
        if m:
            row.update(free=int(m[1]), largest=int(m[2]), psram_free=int(m[3]), psram_largest=int(m[4]))
        m = re.search(r"Platform: api=(\S+) mqtt=(\S+)", text)
        if m:
            row.update(online=m[1] == "ready" and m[2] == "connected", api=m[1], mqtt=m[2])
    m = re.search(r"rssi=(-?\d+)", text)
    if m:
        row["rssi"] = int(m[1])
    return row


def slope_per_hour(points):
    """Least-squares slope of (seconds, value) points, in value units per hour."""
    if len(points) < 3:
        return None
    n = len(points)
    mx = sum(x for x, _ in points) / n
    my = sum(y for _, y in points) / n
    var = sum((x - mx) ** 2 for x, _ in points)
    return round(3600 * sum((x - mx) * (y - my) for x, y in points) / var, 1) if var else None


def watch_booted(port, device):
    before = device["mark"]
    after = port.mark()
    device["mark"] = after
    return any(BOOT_BANNER.search(line) for line in port.since(before)[:after - before])


def cmd_watch(args):
    """Watch boards for hours: platform drop-outs, reboots and heap trend, one row per sample.

    Leave the boards idle (or let other tools drive them on other ports). A drop-out is
    any sample where the platform is not online; a reboot is a boot banner on the serial
    stream or a USB serial port that disappears and comes back.
    """
    duration = args.hours * 3600
    if not math.isfinite(duration) or duration <= 0 or not math.isfinite(args.every) or args.every <= 0:
        raise ConsoleError("watch --hours and --every must be positive and finite")
    permits = getattr(args, "allow", ())
    sha = getattr(args, "elf_sha256", None)
    devices = {}
    for dev in args.devs:
        name, info = resolve(dev)
        vocabulary = sdk_vocabulary(args, info) if sdk_requested(args, info) else None
        devices[name] = {"info": info, "kind": info.get("kind"), "vocabulary": vocabulary,
                         "port": None, "mark": 0, "session": None,
                         "samples": [], "sample_errors": 0, "offline_events": 0, "offline_s": 0.0, "reboots": 0,
                         "port_drops": 0, "was_online": None, "offline_since": None}
    d = run_dir("watch-" + "-".join(args.devs))
    rows = open(d / "rows.jsonl", "a", encoding="utf-8")
    start = time.monotonic()
    end = start + duration
    completed = interrupted = False
    try:
        while True:
            final_sample = time.monotonic() >= end
            for name, s in devices.items():
                now = time.monotonic() - start
                if s["port"] is None:
                    try:
                        s["port"] = Port(s["info"]["port"], d / f"{name}.log")
                        s["mark"] = s["port"].mark()
                    except serial.SerialException:
                        s["sample_errors"] += 1
                        print(f"{now / 60:7.1f} min {name}: serial port unavailable", flush=True)
                        continue
                port = s["port"]
                booted = watch_booted(port, s)
                if booted:
                    s["reboots"] += 1
                    s["session"] = None
                try:
                    if s["vocabulary"] is not None:
                        if s["session"] is None:
                            s["session"] = Session(port.sdk_exchange, s["vocabulary"], permits, sha)
                        row = sdk_bench.health(s["session"])
                    else:
                        row = health(port, s["kind"])
                except ConsoleError as error:
                    s["session"] = None
                    s["sample_errors"] += 1
                    print(f"{now / 60:7.1f} min {name}: no valid sample: {error}", flush=True)
                    continue
                except serial.SerialException:
                    port.close()
                    s["port"] = None
                    s["session"] = None
                    s["port_drops"] += 1
                    print(f"{now / 60:7.1f} min {name}: serial port dropped", flush=True)
                    continue
                if watch_booted(port, s):
                    if not booted:
                        s["reboots"] += 1
                    booted = True
                    s["session"] = None
                online = row.get("online")
                if online is False and s["was_online"] is not False:
                    s["offline_events"] += 1
                    s["offline_since"] = now
                if online and s["offline_since"] is not None:
                    s["offline_s"] += now - s["offline_since"]
                    s["offline_since"] = None
                if online is not None:
                    s["was_online"] = online
                row = {"t_s": round(now), "dev": name, "rebooted": booted, **row}
                s["samples"].append(row)
                rows.write(json.dumps(row) + "\n")
                rows.flush()
                if booted or online is False:
                    print(f"{now / 60:7.1f} min {name}: {'REBOOT ' if booted else ''}{row}", flush=True)
            if final_sample:
                completed = True
                break
            time.sleep(min(args.every, max(0.0, end - time.monotonic())))
    except KeyboardInterrupt:
        interrupted = True
    finally:
        rows.close()
        for s in devices.values():
            if s["port"]:
                s["port"].close()
    elapsed = time.monotonic() - start
    summary = {"test": "watch", "hours_planned": args.hours, "every_s": args.every,
               "elapsed_h": round(elapsed / 3600, 2), "elapsed_s": round(elapsed, 3),
               "window_completed": completed, "interrupted": interrupted, "devices": {}}
    for name, s in devices.items():
        if s["offline_since"] is not None:
            s["offline_s"] += elapsed - s["offline_since"]
        samples = [r for r in s["samples"] if "free" in r]
        is_sdk = sdk_requested(args, s["info"])
        summary["devices"][name] = {
            "samples": len(s["samples"]), "offline_events": s["offline_events"],
            "offline_minutes": round(s["offline_s"] / 60, 1), "reboots": s["reboots"], "port_drops": s["port_drops"],
            "sample_errors": s["sample_errors"],
            "owner_stack_last": s["samples"][-1].get("owner_stack_min_free") if s["samples"] else None,
            "owner_stack_1024": (all(r.get("owner_stack_1024") is True for r in s["samples"])
                                 if is_sdk and s["samples"] else None),
            "sdk_evidence": (bool(s["samples"]) and all("online" in r and r.get("owner_stack_1024") is True
                             for r in s["samples"])) if is_sdk else None,
            "free_first": samples[0]["free"] if samples else None, "free_last": samples[-1]["free"] if samples else None,
            "free_min": min((r["free"] for r in samples), default=None),
            "free_bytes_per_hour": slope_per_hour([(r["t_s"], r["free"]) for r in samples]),
            "minimum_last": samples[-1].get("minimum") if samples else None,
        }
    summary["pass"] = completed and not interrupted and all(
        v["samples"] >= 2 and v["sample_errors"] == 0 and v["offline_events"] == 0 and
        v["reboots"] == 0 and v["port_drops"] == 0 and v["sdk_evidence"] is not False
        for v in summary["devices"].values())
    finish(d, summary)
    if not summary["pass"]:
        raise ConsoleError("watch did not verify the requested healthy window; see the run summary")


def cmd_regress(args):
    kinds = {dev: resolve(dev)[1].get("kind") for dev in (args.a, args.b)}
    if "sdk" in kinds.values():
        raise ConsoleError("SDK runs use separate ai/call/trace/soak commands with exact per-run permissions")
    d = run_dir(f"regress-{args.a}-{args.b}")
    results = []
    for step in regress_steps(args.a, args.b, kinds, args.full, args.phone):
        out = subprocess.run([PYTHON, str(Path(__file__).resolve()), *step],
                             capture_output=True, text=True, encoding="utf-8", errors="replace")
        (d / f"{len(results):02d}-{step[0]}.log").write_text(out.stdout + out.stderr, encoding="utf-8")
        run = re.search(r"^run: (.+)$", out.stdout, re.M)
        passed = None
        if run:
            try:
                passed = json.loads((Path(run[1].strip()) / "summary.json").read_text(encoding="utf-8")).get("pass")
            except (OSError, ValueError):
                pass
        results.append({"step": " ".join(step), "pass": bool(passed), "run": run[1].strip() if run else None})
        print(f"{'PASS' if passed else 'FAIL'}  {' '.join(step)}", flush=True)
    finish(d, {"test": "regress", "devices": [args.a, args.b], "full": args.full, "phone": args.phone,
               "steps": results, "pass": all(r["pass"] for r in results)})


def cmd_partition_plan(args):
    from partition_plan import plan

    try:
        identity = (args.expect_board, args.expect_chip, args.usb_vid_pid, args.usb_serial,
                    args.rom_mac, args.revision)
        if args.read_table:
            from partition_capture import live_plan, request

            target = request(args.allow, args.port, *identity)
            if serial is None:
                raise ValueError("live partition read requires pyserial in the selected Python")
            report = live_plan(args.build_dir, target, list_usb=list_ports.comports,
                               serial_factory=serial.Serial, python=PYTHON)
        else:
            if args.allow or any(value is not None for value in identity):
                raise ValueError("ROM identity and authorization flags require --read-table")
            report = plan(args.build_dir, args.board_table, port=args.port, python=PYTHON)
    except OSError as error:
        raise ValueError(f"cannot read partition plan inputs: {error}") from error
    print(json.dumps(report, indent=2))
    if report["mode"] == "manual-full-review":
        raise SystemExit(1)


def cmd_boot_check(args):
    import boot_check

    boot_check.validate_request(args.allow, args.usb_serial, args.elf_sha256,
                                args.expect_app, args.rounds, args.timeout)
    vocabulary = load_vocabulary(args.commands_file)
    try:
        from esptool.reset import HardReset
    except ImportError as error:
        raise ConsoleError("boot-check requires esptool with native USB HardReset support") from error
    if not callable(getattr(HardReset, "reset", None)):
        raise ConsoleError("selected esptool does not expose native USB HardReset.reset")

    def confirm():
        return boot_check.bind_usb(list_ports.comports(), args.port, args.usb_serial)

    report = boot_check.run(lambda: closing(Port(confirm(), exclusive=True)),
                            lambda port: boot_check.chip_reset(port, HardReset, confirm),
                            confirm, vocabulary, args.elf_sha256, args.expect_app, args.rounds, args.timeout)
    print(json.dumps(report, indent=2))
    if not report["pass"]:
        raise SystemExit(1)


def cmd_app_stacks(args):
    import app_stacks

    app_stacks.validate_request(args.elf_sha256, args.expect_task, args.min_free)
    vocabulary = load_vocabulary(args.commands_file)
    if not vocabulary.board:
        raise ConsoleError("app-stacks requires a vocabulary with exact board identity")
    _, device = resolve(args.dev)
    try:
        with closing(Port(device["port"], exclusive=True)) as port:
            session = Session(port.sdk_exchange, vocabulary, (), args.elf_sha256)
            report = app_stacks.read(session, args.expect_task, args.min_free)
            if args.workers:
                report["worker_watermarks"] = app_stacks.read_workers(session, args.min_free)
    except OSError as error:
        raise ConsoleError("could not open or read the specified serial port") from error
    report.update(board=vocabulary.board, elf_sha256=session.device["elf_sha256"])
    print(json.dumps(report, indent=2))
    if report["verdict"] != "pass":
        raise SystemExit(1)


def gate_wx_out(target, phone_serial, confirm):
    from types import SimpleNamespace

    args = SimpleNamespace(commands_file=target.words, elf_sha256=target.sha,
                           allow=tuple(action for action in target.permits if action.startswith("AT+")),
                           gate_confirm=confirm, gate_phone_serial=phone_serial, phone_hangup=False,
                           ring=0, secs=8)
    name = re.sub(r"[^A-Za-z0-9_.-]", "_", target.name)
    return sdk_wechat_run(args, name, target.info, incoming=False)


def cmd_gate(args):
    import gate

    try:
        primary, peer, phone, tasks, isolated = gate.load(args.allow_file, ROOT, args, resolve)
    except OSError as error:
        raise ConsoleError(f"could not read gate inputs: {error}") from error
    out = run_dir(f"gate-{primary.product}")
    report = gate.Gate(sys.modules[__name__], primary, peer, phone, tasks, isolated, out).run()
    print(f"gate: {report['verdict']}\nrun: {out}")
    if report["verdict"] != "pass":
        raise SystemExit(1 if report["verdict"] == "fail" else 2)


def cmd_snapshot(args):
    import snapshot

    snapshot.validate_sha(args.elf_sha256)
    vocabulary = load_vocabulary(args.commands_file)
    if not vocabulary.board:
        raise ConsoleError("snapshot requires a vocabulary with exact board identity")
    if args.output.exists() or not args.output.parent.is_dir():
        raise ConsoleError("snapshot requires a new output file in an existing directory")
    _, device = resolve(args.dev)
    try:
        with closing(Port(device["port"], exclusive=True)) as port:
            session = Session(port.sdk_exchange, vocabulary, (), args.elf_sha256)
            mark = port.mark()

            def restarted():
                return snapshot.boot_seen(port.since(mark))

            evidence, task_rows = snapshot.scan(session, restarted)
            report = snapshot.report(session, evidence, task_rows, port.since(0), restarted=restarted())
        with args.output.open("x", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2)
            stream.write("\n")
    except OSError as error:
        raise ConsoleError("could not use the specified serial port or snapshot output file") from error


def cmd_echo(args):
    import echo
    import snapshot

    snapshot.validate_sha(args.elf_sha256)
    vocabulary = load_vocabulary(args.commands_file)
    if not vocabulary.board:
        raise ConsoleError("echo requires a vocabulary with exact board identity")
    if args.output.exists() or not args.output.parent.is_dir():
        raise ConsoleError("echo requires a new output file in an existing directory")
    volumes = [int(v) for v in args.volumes.split(",")] if args.volumes else None
    if volumes and any(not 0 <= v <= 100 for v in volumes):
        raise ConsoleError("volumes are 0 to 100")
    _, device = resolve(args.dev)
    try:
        with closing(Port(device["port"], exclusive=True)) as port:
            session = Session(port.sdk_exchange, vocabulary, args.allow, args.elf_sha256)
            report = echo.measure(session, volumes, args.effect, args.quiet, args.ring)
        report["firmware"] = {"board": session.device["board"], "elf_sha256": session.device["elf_sha256"]}
        with args.output.open("x", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2)
            stream.write("\n")
    except OSError as error:
        raise ConsoleError("could not use the specified serial port or echo output file") from error
    print(echo.table(report["summary"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    def sdk_options(parser, peer=False):
        parser.add_argument("--commands", dest="commands_file", type=Path, help="generated SDK product vocabulary")
        parser.add_argument("--allow", action="append", default=[], help="exact local action, one flag per command")
        parser.add_argument("--elf-sha256", help="full running SDK firmware hash; required for actions")
        if peer:
            parser.add_argument("--peer-commands", type=Path)
            parser.add_argument("--peer-elf-sha256")

    sub.add_parser("scan").set_defaults(fn=cmd_scan)
    p = sub.add_parser("hex", help="encode UTF-8 text as one AT parameter without opening a port")
    p.add_argument("text"); p.set_defaults(fn=lambda args: print(hex_text(args.text)))
    p = sub.add_parser("at"); p.add_argument("dev"); p.add_argument("commands", nargs="+")
    p.add_argument("-s", "--secs", type=float, default=0); p.add_argument("-w", "--wait", type=float, default=2)
    p.add_argument("--legacy", action="store_true", help="confirm known older firmware that cannot answer AT+VERSION")
    sdk_options(p)
    p.set_defaults(fn=cmd_at)
    p = sub.add_parser("log"); p.add_argument("dev"); p.add_argument("-s", "--secs", type=float, default=10)
    p.set_defaults(fn=cmd_log)
    p = sub.add_parser("partition-plan", help="compare a verified build with an offline table or authorized ROM read")
    p.add_argument("--build-dir", type=Path, required=True, help="firmware output with build-record.json")
    table = p.add_mutually_exclusive_group(required=True)
    table.add_argument("--board-table", type=Path, help="saved 3072 bytes from flash offset 0x8000; no device I/O")
    table.add_argument("--read-table", action="store_true", help="enter ROM and read only 3072 bytes at 0x8000, then reset")
    p.add_argument("--port", help="explicit local USB port (only printed with --board-table)")
    p.add_argument("--expect-board", help="physically confirmed board model, exactly as in the build record")
    p.add_argument("--expect-chip", help="expected chip target matching the verified build")
    p.add_argument("--usb-vid-pid", help="confirmed USB VID:PID")
    p.add_argument("--usb-serial", help="confirmed USB descriptor serial")
    p.add_argument("--rom-mac", help="confirmed MCU base MAC, independent of a USB bridge serial")
    p.add_argument("--revision", help="exact ROM chip revision, major.minor")
    p.add_argument("--allow", action="append", default=[], help="live read authorization: PARTITION_TABLE_ROM_RESET")
    p.set_defaults(fn=cmd_partition_plan)
    p = sub.add_parser("boot-check", help="verify each explicitly authorized native USB chip reset")
    p.add_argument("port", help="explicit enumerated native USB serial port, not a device alias")
    p.add_argument("--usb-serial", required=True, help="confirmed USB descriptor serial number for this device")
    p.add_argument("--commands", dest="commands_file", type=Path, required=True)
    p.add_argument("--elf-sha256", required=True, help="full expected running ELF SHA-256")
    p.add_argument("--expect-app", required=True, help="expected idle foreground app from the selected build")
    p.add_argument("--allow", action="append", default=[], help="exact authorization: USB_CHIP_RESET")
    p.add_argument("--rounds", type=int, default=3); p.add_argument("--timeout", type=int, default=45)
    p.set_defaults(fn=cmd_boot_check)
    p = sub.add_parser("app-stacks", help="read one verified SDK task-stack snapshot without changing state")
    p.add_argument("dev")
    p.add_argument("--commands", dest="commands_file", type=Path, required=True)
    p.add_argument("--elf-sha256", required=True, help="full expected running ELF SHA-256")
    p.add_argument("--expect-task", action="append", default=[], help="exact task name, one flag per required task")
    p.add_argument("--workers", action="store_true", help="also read transport and platform worker watermarks")
    p.add_argument("--min-free", type=int, default=1024, help="minimum task stack watermark in bytes")
    p.set_defaults(fn=cmd_app_stacks)
    p = sub.add_parser("gate", help="run the SDK bring-up gate with a bound exact action manifest")
    p.add_argument("dev")
    p.add_argument("--product", required=True)
    p.add_argument("--elf-sha256", required=True, help="full expected running ELF SHA-256")
    p.add_argument("--peer", help="explicit serial device selected by its own allow-file entry")
    p.add_argument("--phone", action="store_true", help="permit only phone workflows named in the allow-file")
    p.add_argument("--allow-file", type=Path, required=True)
    p.set_defaults(fn=cmd_gate)
    p = sub.add_parser("snapshot", help="read one SDK snapshot without actions or raw serial logs")
    p.add_argument("dev")
    p.add_argument("--commands", dest="commands_file", type=Path, required=True)
    p.add_argument("--elf-sha256", required=True, help="full expected running ELF SHA-256")
    p.add_argument("--output", type=Path, required=True, help="new sanitized JSON report")
    p.set_defaults(fn=cmd_snapshot)
    p = sub.add_parser("echo", help="single-talk echo measurement: the device's own speaker plays a looped effect")
    p.add_argument("dev")
    p.add_argument("--commands", dest="commands_file", type=Path, required=True)
    p.add_argument("--elf-sha256", required=True, help="full expected running ELF SHA-256")
    p.add_argument("--allow", action="append", default=[], help="exact action: effect on/off, each volume and the restore")
    p.add_argument("--volumes", help="comma-separated volumes 0-100; omit to keep the current volume")
    p.add_argument("--effect", default="companion.incoming", help="looped feedback state to play")
    p.add_argument("--quiet", type=float, default=3.0, help="seconds of quiet floor per volume")
    p.add_argument("--ring", type=float, default=9.0, help="seconds of playback per volume")
    p.add_argument("--output", type=Path, required=True, help="new JSON report")
    p.set_defaults(fn=cmd_echo)
    p = sub.add_parser("flash"); p.add_argument("dev"); p.add_argument("bin")
    p.add_argument("--offset", default="0x10000"); p.add_argument("--chip")
    p.add_argument("--baud", type=int, help="explicit baud rate (one attempt); otherwise select by USB port")
    p.set_defaults(fn=cmd_flash)
    p = sub.add_parser("screen"); p.add_argument("dev"); p.add_argument("out"); sdk_options(p); p.set_defaults(fn=cmd_screen)
    p = sub.add_parser("ai"); p.add_argument("dev"); p.add_argument("-s", "--secs", type=float, default=30)
    sdk_options(p)
    p.add_argument("--screens", action="store_true"); p.set_defaults(fn=cmd_ai)
    p = sub.add_parser("ai-keys"); p.add_argument("dev"); p.add_argument("-s", "--secs", type=float, default=4)
    sdk_options(p)
    p.set_defaults(fn=cmd_ai_keys)
    for wx_name, wx_fn in (("wx-in", cmd_wx_in), ("wx-out", cmd_wx_out)):
        p = sub.add_parser(wx_name); p.add_argument("dev"); p.add_argument("-s", "--secs", type=float, default=15)
        p.add_argument("--ring", type=float, default=5); p.add_argument("--phone-hangup", action="store_true")
        sdk_options(p)
        p.set_defaults(fn=wx_fn)
    p = sub.add_parser("watch"); p.add_argument("devs", nargs="+")
    p.add_argument("--hours", type=float, default=8); p.add_argument("--every", type=float, default=60)
    sdk_options(p)
    p.set_defaults(fn=cmd_watch)
    p = sub.add_parser("soak"); p.add_argument("dev"); p.add_argument("--peer")
    p.add_argument("--rounds", type=int, default=10); p.add_argument("-s", "--secs", type=float, default=8)
    sdk_options(p, peer=True)
    p.set_defaults(fn=cmd_soak)
    p = sub.add_parser("trace"); p.add_argument("caller"); p.add_argument("callee")
    p.add_argument("-s", "--secs", type=float, default=20); p.add_argument("--frames", type=int)
    sdk_options(p, peer=True)
    p.set_defaults(fn=cmd_trace)
    p = sub.add_parser("call"); p.add_argument("caller"); p.add_argument("callee")
    p.add_argument("-s", "--secs", type=float, default=20); p.add_argument("--ring", type=float, default=3)
    sdk_options(p, peer=True)
    p.add_argument("--screens", action="store_true"); p.set_defaults(fn=cmd_call)
    p = sub.add_parser("phone"); p.add_argument("action", choices=["shot", "tap", "text", "key", "ui"])
    p.add_argument("arg", nargs="*"); p.set_defaults(fn=cmd_phone)
    p = sub.add_parser("regress"); p.add_argument("a"); p.add_argument("b")
    p.add_argument("--full", action="store_true", help="also run the frame trace and a 10-round memory soak")
    p.add_argument("--phone", action="store_true", help="also run WeChat calls through the USB phone")
    p.set_defaults(fn=cmd_regress)
    p = sub.add_parser("udp-pattern", help="plan or run an explicitly authorized private-LAN probe")
    xtp2.add_arguments(p)
    p.set_defaults(fn=lambda args: sys.exit(xtp2.run_args(args)))
    args = ap.parse_args()
    if serial is None and args.cmd not in {"hex", "phone", "partition-plan", "udp-pattern"}:
        ap.exit(2, "xt: serial commands require pyserial in the selected Python environment\n")
    try:
        args.fn(args)
    except (ValueError, TimeoutError) as error:
        ap.exit(1, f"xt: {error}\n")


if __name__ == "__main__":
    main()
