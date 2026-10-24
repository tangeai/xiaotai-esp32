#!/usr/bin/env python3
"""Exercise Xmini AT; temporarily test volume/gain and restore saved originals.

Does not flash or change network/identity. Settings restoration is verified.
"""
import argparse
import hashlib
import json
import re
import time
from pathlib import Path

class Device:
    def __init__(self, port, output):
        import serial
        self.port = serial.Serial()
        self.port.port = port
        self.port.baudrate = 115200
        self.port.timeout = 0.03
        self.port.write_timeout = 2
        self.port.dtr = False
        self.port.rts = False
        self.log = (output / "serial.log").open("xb")
        self.buffer = bytearray()
        self.lines = []
        self.commands = 0

    def read(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            chunk = self.port.read(4096)
            self.log.write(chunk)
            self.log.flush()
            self.buffer.extend(chunk)
            while b"\n" in self.buffer:
                line, _, rest = self.buffer.partition(b"\n")
                self.buffer = bytearray(rest)
                self.lines.append(line.decode("utf-8", errors="replace").strip())

    def send(self, data):
        self.log.write(b"\n>>> " + repr(data).encode("ascii") + b"\n")
        self.log.flush()
        self.port.write(data)

    def response(self, start, error=None, timeout=3):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.read(0.03)
            for line in self.lines[start:]:
                if line == "OK" or line.startswith("ERROR:"):
                    expected = "OK" if error is None else "ERROR: " + error
                    assert line == expected, (line, expected)
                    return self.lines[start:]
        raise TimeoutError("No terminal AT response")

    def command(self, text, error=None, ending=b"\n"):
        self.commands += 1
        start = len(self.lines)
        self.send(text.encode("ascii") + ending)
        return self.response(start, error)

    def status(self, allow_settings_error=False):
        lines = self.command("AT+STATUS")
        state = {}
        for line in lines:
            if line.startswith(("+UI:", "+AUDIO:", "+HEAP:", "+INPUT:")):
                state.update({k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)", line)})
            elif line.startswith("+SETTINGS:"):
                state.update({"settings_" + k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)", line)})
        assert {"page", "selected", "busy", "integrity", "free"} <= state.keys(), lines
        assert state["ready"] == state["integrity"] == 1, state
        assert state["display_fault"] == state["error"] == 0, state
        if not allow_settings_error:
            assert state.get("settings_error", 0) == 0, state
        return state

    def wait(self, predicate, timeout=3, allow_settings_error=False):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = self.status(allow_settings_error=allow_settings_error)
            if predicate(state):
                return state
            time.sleep(0.05)
        raise TimeoutError("State predicate did not become true: " + str(state))

    def idle(self):
        return self.wait(lambda state: not state["busy"])

    def gesture(self, kind, page, selected, overlap=False):
        start = len(self.lines)
        self.command("AT+KEY " + kind)
        if overlap:
            self.command("AT+KEY CLICK", "ESP_ERR_INVALID_STATE")
        state = self.wait(lambda state: not state["key_active"])
        assert (state["page"], state["selected"]) == (page, selected), state
        events = [line for line in self.lines[start:] if "key source=AT event=" in line]
        expected = {"CLICK": 1, "DOUBLE": 2, "HOLD": 3}[kind]
        assert len(events) == 1 and f"event={expected} " in events[0], events
        return state

    def operation(self, name, frames):
        self.command("AT+AUDIO " + name)
        state = self.idle()
        assert state["frames"] == frames, state
        return state

    def screen(self, output, name):
        lines = self.command("AT+SCREEN")
        data = bytearray()
        for line in lines:
            match = re.fullmatch(r"\+SCREEN: offset=(\d+) hex=([0-9a-f]{64})", line)
            if match:
                assert int(match[1]) == len(data), line
                data.extend(bytes.fromhex(match[2]))
        assert len(data) == 1024 and any(data), (len(data), lines)
        (output / (name + ".ssd1306")).write_bytes(data)
        # PBM is an exact, lossless rendering of the last successfully sent frame.
        pixels = bytearray(1024)
        for y in range(64):
            for x in range(128):
                if data[(y // 8) * 128 + x] & (1 << (y % 8)):
                    pixels[y * 16 + x // 8] |= 1 << (7 - x % 8)
        (output / (name + ".pbm")).write_bytes(b"P4\n128 64\n" + pixels)
        return hashlib.sha256(data).hexdigest()


def original_controls(device):
    state = device.wait(lambda state: not state["busy"] and state.get("settings_loaded") == 1 and
        state.get("settings_saved") == 1 and state.get("settings_pending") == 0, timeout=10)
    return {"volume": state["volume"], "gain": state["gain"]}


def restore_controls(device, original):
    device.command("AT+AUDIO STOP")
    device.wait(lambda state: not state["busy"], timeout=10, allow_settings_error=True)
    device.command(f"AT+VOL {original['volume']}")
    device.command(f"AT+MIC {original['gain']}")
    return device.wait(lambda state: state["volume"] == original["volume"] and state["gain"] == original["gain"] and
        state.get("settings_loaded") == 1 and state.get("settings_saved") == 1 and
        not state["busy"] and state.get("settings_pending") == 0 and state.get("settings_error") == 0,
        timeout=10, allow_settings_error=True)


def exercise_setting_steps(device):
    values = device.idle()
    expected = {"volume": values["volume"], "gain": values["gain"]}
    for selected, field, step, maximum in ((0, "volume", 5, 100), (1, "volume", -5, 100),
                                           (2, "gain", 3, 36), (3, "gain", -3, 36)):
        if selected:
            device.gesture("CLICK", 4, selected)
        device.gesture("DOUBLE", 4, selected)
        expected[field] = max(0, min(maximum, expected[field] + step))
        assert device.idle()[field] == expected[field]
    return expected


def regress(device, args, result):
    device.port.open()
    if args.reset:
        from esptool.reset import HardReset
        HardReset(device.port, uses_usb=True)()
    device.read(2)
    device.command("AT")
    identity = device.command("AT+VERSION")
    assert any("board=Xmini-C3-V3" in line and "mac=" + args.mac.lower() in line for line in identity), identity
    assert any("elf_sha256=" + args.elf_sha.lower() in line for line in identity), identity
    device.command("AT+HELP")
    for ending in (b"\r", b"\r\n", b"\n"):
        start = len(device.lines)
        device.command("AT", ending=ending)
        device.read(0.1)
        assert device.lines[start:].count("OK") == 1
    device.command("AT+STATUX\bS")
    start = len(device.lines)
    device.send(b"AT+ST")
    device.read(0.1)
    assert "OK" not in device.lines[start:]
    device.send(b"ATUS\n")
    device.response(start)
    start = len(device.lines)
    device.send(b"A" * 130 + b"\nAT\n")
    device.response(start, "LINE_TOO_LONG")
    device.read(0.3)
    assert device.lines[start:].count("OK") == 1
    start = len(device.lines)
    device.send(b"AT\n" * 32)
    device.read(1)
    assert device.lines[start:].count("OK") == 32
    initial = device.idle()
    assert (initial["page"], initial["selected"], initial["recorded"]) == (0, 0, 0), initial
    result["initial"] = initial
    result["original_controls"] = original_controls(device)
    screens = {"home": device.screen(args.output, "home")}
    device.command("AT+AUDIO PLAY", "ESP_ERR_INVALID_STATE")
    device.command("AT+AUDIO STOP")
    for text in ("AT+VOL 101", "AT+VOL -1", "AT+MIC 37", "AT+MIC 12x", "AT+RESET", "AT+KEY BAD"):
        device.command(text, "ESP_ERR_INVALID_ARG")
    for name, field, values in (("VOL", "volume", (0, 100, 40)), ("MIC", "gain", (0, 36, 24))):
        for value in values:
            device.command(f"AT+{name} {value}")
            assert device.idle()[field] == value

    for page in range(1, 4):
        for selected in range(1, page):
            device.gesture("CLICK", 0, selected)
        device.gesture("DOUBLE", page, 0, overlap=page == 1)
        screens[f"page-{page}"] = device.screen(args.output, f"page-{page}")
        device.gesture("HOLD", 0, 0)
        assert device.screen(args.output, f"home-after-{page}") == screens["home"]
    for selected in range(1, 4):
        device.gesture("CLICK", 0, selected)
    device.gesture("DOUBLE", 4, 0)
    screens["settings"] = device.screen(args.output, "settings")
    tested_controls = exercise_setting_steps(device)
    device.gesture("CLICK", 4, 4)
    device.gesture("DOUBLE", 5, 0)
    screens["audio"] = device.screen(args.output, "audio")
    assert len(set(screens.values())) == len(screens), screens
    device.command("AT+DISPLAY")
    before = device.status()["display_frames"]
    device.command("AT+DISPLAY")
    assert device.wait(lambda state: state["display_frames"] > before)["display_fault"] == 0

    device.gesture("DOUBLE", 5, 0)
    assert device.idle()["frames"] == 25
    device.gesture("CLICK", 5, 1)
    device.gesture("DOUBLE", 5, 1)
    state = device.idle()
    assert state["frames"] == 50 and state["recorded"] == 1
    device.gesture("CLICK", 5, 2)
    device.gesture("DOUBLE", 5, 2)
    assert device.idle()["frames"] == 50
    device.gesture("CLICK", 5, 3)
    device.command("AT+AUDIO RECORD")
    device.gesture("DOUBLE", 5, 3)
    stopped = device.idle()
    assert 0 < stopped["frames"] < 50 and stopped["recorded"] == 0, stopped
    device.gesture("HOLD", 4, 0)
    device.gesture("HOLD", 0, 0)

    cycles = []
    baseline = device.idle()
    for cycle in range(args.cycles):
        capture = device.operation("RECORD", 50)
        assert capture["recorded"] == 1
        playback = device.operation("PLAY", 50)
        assert playback["free"] == baseline["free"], playback
        cycles.append({"cycle": cycle + 1, "record_peak": capture["peak"], "free": playback["free"], "largest": playback["largest"]})
        print(f"{args.port}: cycle {cycle + 1}/{args.cycles}, free={playback['free']}", flush=True)
    device.command("AT+AUDIO TONE")
    device.command("AT+AUDIO RECORD", "ESP_ERR_INVALID_STATE")
    assert device.idle()["frames"] == 25
    cancellations = {}
    for name in ("RECORD", "PLAY", "TONE"):
        if name == "PLAY":
            device.command("AT+AUDIO PLAY", "ESP_ERR_INVALID_STATE")
            device.operation("RECORD", 50)
        device.command("AT+AUDIO " + name)
        device.wait(lambda state: state["busy"] and state["frames"] >= 5)
        device.command("AT+AUDIO STOP")
        state = device.idle()
        assert 0 < state["frames"] < (25 if name == "TONE" else 50), state
        cancellations[name] = state["frames"]
        if name == "RECORD":
            assert state["recorded"] == 0
    # A bounded soak detects resets and heap drift; it is not a long-run test.
    for _ in range(args.idle_seconds):
        time.sleep(1)
        state = device.status()
        assert not state["busy"] and state["free"] == baseline["free"], state
    final = device.status()
    assert (final["page"], final["selected"]) == (0, 0), final
    assert final["volume"] == tested_controls["volume"] and final["gain"] == tested_controls["gain"], final
    assert final["free"] == initial["free"], (initial, final)
    assert device.screen(args.output, "home-final") == screens["home"]
    device.read(0.1)
    errors = [line for line in device.lines if re.match(r"E \(\d+\)", line)]
    crashes = [line for line in device.lines if any(word in line for word in ("Guru Meditation", "assert failed", "CORRUPT HEAP", "Stack protection", "abort()"))]
    resets = [line for line in device.lines if "rst:0x" in line]
    assert not errors and not crashes and len(resets) == int(args.reset), (errors, crashes, resets)
    result.update(pass_result=True, final=final, heap_delta=final["free"] - initial["free"],
                  cycles=cycles, cancellations=cancellations, screens=screens,
                  errors=errors, resets=len(resets), commands=device.commands,
                  gpio_contact_tested=False, acoustic_listening_tested=False, network_tested=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("--mac", required=True)
    parser.add_argument("--elf-sha", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cycles", type=int, default=20)
    parser.add_argument("--idle-seconds", type=int, default=30)
    parser.add_argument("--reset", action="store_true")
    args = parser.parse_args()
    assert 1 <= args.cycles <= 100 and 0 <= args.idle_seconds <= 3600
    args.output.mkdir(parents=True, exist_ok=False)
    result = {"port": args.port, "mac": args.mac, "elf_sha256": args.elf_sha, "pass_result": False}
    started = time.monotonic()
    device = Device(args.port, args.output)
    try:
        regress(device, args, result)
    except Exception as error:
        result["failure"] = repr(error)
        raise
    finally:
        restoration_error = None
        if device.port.is_open and "original_controls" in result:
            try:
                result["settings_restore"] = restore_controls(device, result["original_controls"])
            except Exception as error:
                restoration_error = error
                result["settings_restore_failure"] = repr(error)
                result["pass_result"] = False
        if device.port.is_open:
            device.port.close()
        device.log.close()
        result["duration_seconds"] = round(time.monotonic() - started, 2)
        (args.output / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
        print(json.dumps({key: result.get(key) for key in
            ("pass_result", "port", "duration_seconds", "failure", "settings_restore_failure")}), flush=True)
        if restoration_error is not None and "failure" not in result:
            raise RuntimeError("Original settings were not restored and persisted") from restoration_error


if __name__ == "__main__":
    main()
