#!/usr/bin/env python3
"""Read-only platform checks and local audio tests on an already bound C3.

Does not flash, submit credentials, bind/unbind, or change cloud configuration.
Diagnostic guidance is audible, but never changes the actual binding code.
Volume/gain controls are exercised, then restored and verified saved.
"""
import argparse
import json
import re
import sys
import time
from pathlib import Path

from regress_at import Device, exercise_setting_steps, original_controls, restore_controls


def product(device):
    lines = device.command("AT+PRODUCT")
    phase = next(line for line in lines if line.startswith("+PRODUCT:"))
    identity = next(line for line in lines if line.startswith("+DEVICE:"))
    return {**dict(re.findall(r"(\w+)=([^ ]+)", phase)), **dict(re.findall(r"(\w+)=([^ ]+)", identity))}


def online(device, identity, timeout=45, expected_rtc=1):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        state = product(device)
        if state["phase"] == "6" and state["platform_online"] == "1" and state["rtc_ready"] == str(expected_rtc):
            assert state["id"] == identity and state["code"] == "-", state
            return state
        time.sleep(0.25)
    raise TimeoutError(str(state))


def contact_page_ready(device, page, timeout=20):
    expected_filter = {2: 2, 3: 1}[page]
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        lines = device.command("AT+CONTACTS")
        header = next(line for line in lines if line.startswith("+CONTACTS:"))
        state = dict(re.findall(r"(\w+)=([^ ]+)", header))
        if int(state["filter"]) == expected_filter and int(state["loading"]) == 0:
            assert int(state["error"]) == 0, lines
            ui = device.status()
            assert ui["page"] == page and ui["selected"] == 0 and not ui["key_active"], ui
            return lines
        time.sleep(0.05)
    raise TimeoutError("Contact page did not finish loading: " + str(state))


def ui_round_trip(device, args):
    initial = device.status()
    assert initial["page"] == initial["selected"] == initial["key_active"] == 0, {
        "required_home_precondition": initial,
        "interpretation": "No navigation or call intent is submitted from an unexpected initial page"}
    screens = {"home": device.screen(args.output, "home-before")}
    for page in range(1, 4):
        for selected in range(1, page):
            device.gesture("CLICK", 0, selected)
        device.gesture("DOUBLE", page, 0, overlap=page == 1)
        if page in (2, 3):
            contact_page_ready(device, page)
        screens[f"page-{page}"] = device.screen(args.output, f"page-{page}")
        device.gesture("HOLD", 0, 0)
        assert device.screen(args.output, f"home-after-{page}") == screens["home"]
    for selected in range(1, 4):
        device.gesture("CLICK", 0, selected)
    device.gesture("DOUBLE", 4, 0)
    screens["settings"] = device.screen(args.output, "settings")
    exercise_setting_steps(device)
    device.gesture("CLICK", 4, 4)
    device.gesture("DOUBLE", 5, 0)
    screens["audio"] = device.screen(args.output, "audio")
    assert len(set(screens.values())) == 6, screens
    device.gesture("DOUBLE", 5, 0)
    assert device.idle()["frames"] == 25
    device.gesture("CLICK", 5, 1)
    device.gesture("DOUBLE", 5, 1)
    assert device.idle()["recorded"] == 1
    device.gesture("CLICK", 5, 2)
    device.gesture("DOUBLE", 5, 2)
    assert device.idle()["frames"] == 50
    device.gesture("CLICK", 5, 3)
    device.command("AT+AUDIO RECORD")
    device.gesture("DOUBLE", 5, 3)
    assert device.idle()["recorded"] == 0
    device.gesture("HOLD", 4, 0)
    device.gesture("HOLD", 0, 0)
    return screens


def run(device, args, result):
    device.port.open()
    if args.reset:
        from esptool.reset import HardReset
        HardReset(device.port, uses_usb=True)()
    device.read(1)
    identity = device.command("AT+VERSION")
    assert any("mac=" + args.mac.lower() in line for line in identity), identity
    assert any("elf_sha256=" + args.elf_sha.lower() in line and "opus=0" in line and "video=0" in line for line in identity), identity
    result["initial_product"] = online(device, args.device_id, expected_rtc=args.rtc)
    device.wait(lambda state: not state["busy"], timeout=30)
    result["initial_audio"] = device.status()
    result["original_controls"] = original_controls(device)
    baseline_console = device.command("AT+CONSOLE")
    baseline_sof = next(int(match[1]) for line in baseline_console
        if (match := re.match(r"\+SOF: disconnects=(\d+) ", line)))
    result["console_baseline"] = baseline_console
    result["sof_baseline"] = baseline_sof
    result["ui_round_trips"] = []
    for cycle in range(args.ui_cycles):
        screens = ui_round_trip(device, args)
        result["ui_round_trips"].append({"cycle": cycle + 1, "screens": screens, "status": device.status()})
    result["screens"] = screens
    for text in ("AT+BIND REPLAY", "AT+RETRY", "AT+WIFI 5469,3132333435363738"):
        device.command(text, "ESP_ERR_INVALID_STATE")
    for text in ("AT+GUIDE 5", "AT+GUIDE 5 12345a", "AT+GUIDE 13", "AT+WIFI 00,"):
        device.command(text, "ESP_ERR_INVALID_ARG")
    result["prompts"] = []
    for prompt in range(13):
        device.command(f"AT+GUIDE {prompt}" + (" 012345" if prompt == 5 else ""))
        state = device.wait(lambda state: not state["busy"], timeout=30)
        assert state["prompt"] == prompt and state["owner"] == 0, state
        assert state["frames"] == state["expected"] > 0 and state["stopped"] == 0, state
        result["prompts"].append({"prompt": prompt, "frames": state["frames"], "heap_free": state["free"]})
        online(device, args.device_id, timeout=2, expected_rtc=args.rtc)
    device.command("AT+GUIDE 5 678969")
    result["digits_6_to_9"] = device.wait(lambda state: not state["busy"], timeout=30)
    assert result["digits_6_to_9"]["frames"] == result["digits_6_to_9"]["expected"] > 0
    assert result["digits_6_to_9"]["stopped"] == 0 and result["digits_6_to_9"]["owner"] == 0
    device.command("AT+GUIDE 5 000000")
    device.wait(lambda state: state["busy"] and state["frames"] >= 5)
    device.command("AT+AUDIO STOP")
    state = device.idle()
    assert state["stopped"] == 1 and 0 < state["frames"] < state["expected"], state
    result["diagnostic_cancel"] = state
    result["cycles"] = []
    for i in range(args.cycles):
        capture = device.operation("RECORD", 50)
        assert capture["recorded"] == 1
        playback = device.operation("PLAY", 50)
        result["cycles"].append({"cycle": i + 1, "capture_peak": capture["peak"], "heap_free": playback["free"], "largest": playback["largest"]})
    device.command("AT+AUDIO TONE")
    device.command("AT+AUDIO RECORD", "ESP_ERR_INVALID_STATE")
    assert device.idle()["frames"] == 25
    result["final_product"] = online(device, args.device_id, expected_rtc=args.rtc)
    result["final_audio"] = device.status()
    result["home_frame_sha256"] = device.screen(args.output, "home")
    result["console"] = device.command("AT+CONSOLE")
    assert any(line.startswith(f"+SOF: disconnects={baseline_sof} ") for line in result["console"]), result["console"]
    result["errors"] = [line for line in device.lines if re.match(r"E \(\d+\)", line)]
    result["crashes"] = [line for line in device.lines if any(word in line for word in ("Guru Meditation", "assert failed", "CORRUPT HEAP", "Stack protection", "abort()"))]
    result["resets"] = sum("rst:0x" in line for line in device.lines)
    assert not result["errors"] and not result["crashes"] and result["resets"] == int(args.reset), result
    result["passed"] = True


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("port")
    p.add_argument("--mac", required=True)
    p.add_argument("--elf-sha", required=True)
    p.add_argument("--device-id", required=True)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--reset", action="store_true")
    p.add_argument("--cycles", type=int, default=20)
    p.add_argument("--ui-cycles", type=int, default=1)
    p.add_argument("--rtc", type=int, choices=(0, 1), default=1)
    args = p.parse_args()
    assert 1 <= args.cycles <= 100
    assert 1 <= args.ui_cycles <= 20
    args.output.mkdir(parents=True, exist_ok=False)
    device = Device(args.port, args.output)
    result = {"passed": False, "port": args.port, "mac": args.mac, "elf_sha256": args.elf_sha,
              "device_id": args.device_id, "acoustic_listening_tested": False, "rtc_tested": False}
    start = time.monotonic()
    run_error = None
    try:
        run(device, args, result)
    except Exception as error:
        run_error = error
        result["passed"] = False
        result["failure"] = repr(error)
        try:
            result["console_diagnostic_after_failure"] = device.command("AT+CONSOLE")
        except Exception as diagnostic_error:
            result["console_diagnostic_error"] = repr(diagnostic_error)
        raise
    finally:
        restoration_error = None
        if device.port.is_open and "original_controls" in result:
            try:
                result["settings_restore"] = restore_controls(device, result["original_controls"])
                if "sof_baseline" in result:
                    result["settings_restore_console"] = device.command("AT+CONSOLE")
                    assert any(line.startswith(f"+SOF: disconnects={result['sof_baseline']} ")
                        for line in result["settings_restore_console"]), result["settings_restore_console"]
            except Exception as error:
                restoration_error = error
                result["settings_restore_failure"] = repr(error)
                result["passed"] = False
        close_error = None
        result["close_errors"] = []
        for resource, handle in (("port", device.port), ("log", device.log)):
            try:
                handle.close()
            except Exception as error:
                if close_error is None:
                    close_error = error
                result["close_errors"].append({"resource": resource, "error": repr(error)})
                result["passed"] = False
        result["seconds"] = round(time.monotonic() - start, 2)
        result_write_error = None
        try:
            (args.output / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
        except Exception as error:
            result_write_error = error
            result["passed"] = False
            result["result_write_failure"] = repr(error)
            try:
                print("Could not save regression result: " + repr(error), file=sys.stderr, flush=True)
            except Exception:
                pass
        print(json.dumps({key: result.get(key) for key in
            ("passed", "port", "seconds", "failure", "settings_restore_failure", "close_errors", "result_write_failure")}), flush=True)
        if restoration_error is not None and run_error is None:
            raise RuntimeError("Settings/USB cleanup verification failed") from restoration_error
        if close_error is not None and run_error is None:
            raise RuntimeError("Port/log close failed") from close_error
        if result_write_error is not None and run_error is None:
            raise RuntimeError("Regression result could not be saved") from result_write_error


if __name__ == "__main__":
    main()
