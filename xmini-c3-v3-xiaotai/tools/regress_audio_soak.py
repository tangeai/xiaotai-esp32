#!/usr/bin/env python3
"""Bound/online product firmware: all guidance clips and local audio soak.

No flash, reset, network/identity changes, cloud calls or account actions.
Requires the RTC business owner to remain idle for the whole test.
"""
import argparse
import json
import re
import statistics
import time
from pathlib import Path

from regress_product import Peer, fields, number
from regress_at import original_controls, restore_controls


def wait_audio(peer, predicate, timeout=30):
    deadline = time.monotonic() + timeout
    last = {}
    while time.monotonic() < deadline:
        last = peer.audio()
        assert number(last["AUDIO"], "error") == 0, last
        if predicate(last):
            return last
        time.sleep(0.03)
    raise TimeoutError({"audio_predicate_timeout": last})


def idle_business(peer):
    state = peer.rtc()
    assert number(state["SESSION"], "owner") == 0 and number(state["SESSION"], "phase") == 0, state
    assert number(state["RTC"], "online") == 1 and number(state["RTC"], "connected") == 0, state
    assert number(state["RTC"], "connecting") == number(state["RTC"], "stopping") == 0, state
    assert number(state["RTC"], "closing") == number(state["RTC"], "sdk_pending") == 0, state
    assert number(state["MEDIA"], "events") == number(state["MEDIA"], "pending") == 0, state
    return state


def run(peer, args, result):
    peer.device.port.open()
    peer.device.read(0.5)
    version = peer.command("AT+VERSION")
    assert any("board=Xmini-C3-V3" in line and "mac=" + peer.mac in line for line in version), version
    assert any("elf_sha256=" + peer.elf in line and "opus=0" in line and "video=0" in line and "rtc=1" in line for line in version), version
    product = fields(peer.command("AT+PRODUCT"))
    assert product["DEVICE"]["id"] == peer.identity and product["DEVICE"]["platform_online"] == "1", product
    peer.verified = True
    preflight = {"version": version, "product": product, "rtc": peer.rtc(),
                 "memtrace": peer.command("AT+MEMTRACE"), "console": peer.command("AT+CONSOLE")}
    result["preflight"] = preflight
    baseline_sof = number(fields(preflight["console"])["SOF"], "disconnects")
    result["sof_baseline"] = baseline_sof
    (args.output / "preflight.json").write_text(json.dumps(preflight, indent=2), encoding="utf-8")
    if number(preflight["rtc"]["SESSION"], "phase") == 8:
        assert args.dismiss_retired_error, "Retired business error needs explicit --dismiss-retired-error"
        assert number(preflight["rtc"]["SESSION"], "owner") == 0, preflight
        assert all(number(preflight["rtc"]["RTC"], key) == 0 for key in
                   ("sdk_pending", "closing", "connecting", "connected", "stopping")), preflight
        result["explicit_dismiss"] = peer.command("AT+CALL DISMISS")
    start_line = len(peer.device.lines)
    initial_rtc = idle_business(peer)
    initial = wait_audio(peer, lambda state: number(state["AUDIO"], "busy") == 0 and number(state["AUDIO"], "device_open") == 0)
    result["original_controls"] = original_controls(peer.device)
    console = peer.command("AT+CONSOLE")
    assert number(fields(console)["SOF"], "disconnects") == baseline_sof, console
    result.update(version=version, initial_product=product, initial_rtc=initial_rtc,
                  initial_audio=initial, initial_console=console, prompts=[], cycles=[])

    for prompt in range(13):
        peer.command(f"AT+GUIDE {prompt}" + (" 012345" if prompt == 5 else ""))
        finished = wait_audio(peer, lambda state: number(state["AUDIO"], "busy") == 0 and
            number(state["AUDIO"], "prompt") == prompt and number(state["AUDIO"], "device_open") == 0)
        audio = finished["AUDIO"]
        assert number(audio, "owner") == 0 and number(audio, "frames") == number(audio, "expected") > 0 and number(audio, "stopped") == 0, finished
        idle_business(peer)
        result["prompts"].append({"prompt": prompt, "state": finished})
        (args.output / "result.partial.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    peer.command("AT+GUIDE 5 678969")
    extra = wait_audio(peer, lambda state: number(state["AUDIO"], "busy") == 0 and number(state["AUDIO"], "device_open") == 0)
    assert number(extra["AUDIO"], "frames") == number(extra["AUDIO"], "expected") > 0 and number(extra["AUDIO"], "stopped") == 0, extra
    result["digits_6_to_9"] = extra

    for cycle in range(args.cycles):
        idle_business(peer)
        peer.command("AT+AUDIO RECORD")
        capture = wait_audio(peer, lambda state: number(state["AUDIO"], "busy") == 0 and number(state["AUDIO"], "device_open") == 0)
        audio = capture["AUDIO"]
        assert number(audio, "recorded") == 1 and number(audio, "frames") == 50 and number(audio, "stopped") == 0, capture
        assert number(capture["STREAM"], "source") == 0, capture
        peer.command("AT+AUDIO PLAY")
        playback = wait_audio(peer, lambda state: number(state["AUDIO"], "busy") == 0 and number(state["AUDIO"], "device_open") == 0)
        assert number(playback["AUDIO"], "frames") == 50 and number(playback["AUDIO"], "stopped") == 0, playback
        result["cycles"].append({"cycle": cycle + 1, "capture": capture, "playback": playback, "rtc": idle_business(peer)})
        (args.output / "result.partial.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
        print(f"{args.port}: cycle {cycle + 1}/{args.cycles}, free={playback['HEAP']['free']}", flush=True)

    result["idle_observations"] = []
    end = time.monotonic() + args.idle_seconds
    while time.monotonic() < end:
        result["idle_observations"].append({"audio": peer.audio(), "rtc": idle_business(peer)})
        time.sleep(1)
    result["final_audio"] = peer.audio()
    result["final_rtc"] = idle_business(peer)
    result["final_memtrace"] = peer.command("AT+MEMTRACE")
    initial_trace = fields(preflight["memtrace"])["MEMTRACE"]
    final_trace = fields(result["final_memtrace"])["MEMTRACE"]
    assert number(final_trace, "failures") == number(initial_trace, "failures"), final_trace
    result["final_product"] = fields(peer.command("AT+PRODUCT"))
    assert result["final_product"]["DEVICE"]["id"] == peer.identity and result["final_product"]["DEVICE"]["platform_online"] == "1", result["final_product"]
    result["console"] = peer.command("AT+CONSOLE")
    assert number(fields(result["console"])["SOF"], "disconnects") == baseline_sof, result["console"]
    lines = peer.device.lines[start_line:]
    result["errors"] = [line for line in lines if re.match(r"E \(\d+\)", line)]
    result["crashes_or_resets"] = [line for line in lines if any(word in line for word in
        ("Guru Meditation", "assert failed", "CORRUPT HEAP", "Stack protection", "abort()", "rst:0x"))]
    assert not result["errors"] and not result["crashes_or_resets"], {"errors": result["errors"], "crashes": result["crashes_or_resets"]}
    values = [number(item["playback"]["HEAP"], "free") for item in result["cycles"]]
    result["heap_trend"] = {"first_five_median": statistics.median(values[:5]),
        "last_five_median": statistics.median(values[-5:]), "lowest": min(values), "highest": max(values),
        "interpretation": "RTC owner/connection idle was checked each round. MQTT/TCP/TLS temporary allocations can vary; exact equality is not a leak criterion."}
    result["passed"] = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("--mac", required=True)
    parser.add_argument("--device-id", required=True)
    parser.add_argument("--elf-sha", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cycles", type=int, default=50)
    parser.add_argument("--idle-seconds", type=int, default=30)
    parser.add_argument("--dismiss-retired-error", action="store_true",
                        help="After saved identity/RTC/MEMTRACE evidence, explicitly dismiss only owner=0 SDK-pending=0 non-closing ERROR")
    args = parser.parse_args()
    if not 1 <= args.cycles <= 100 or not 0 <= args.idle_seconds <= 3600:
        parser.error("cycles 1..100; idle-seconds 0..3600")
    if not re.fullmatch(r"[0-9a-fA-F]{64}", args.elf_sha) or "PRIVATE" not in args.output.resolve().parts:
        parser.error("Full ELF SHA-256 and PRIVATE output directory required")
    args.output.mkdir(parents=True, exist_ok=False)
    peer = Peer("device", args.port, args.mac, args.device_id, args.elf_sha, args.output / "device")
    result = {"passed": False, "port": args.port, "mac": args.mac, "device_id": args.device_id,
              "elf_sha256": args.elf_sha, "acoustic_listening_tested": False, "rtc_calls_tested": False}
    started = time.monotonic()
    try:
        run(peer, args, result)
    except Exception as error:
        result["failure"] = f"{type(error).__name__}: {str(error)[:240]}"
        result["failure_details"] = repr(error)
        raise
    finally:
        restoration_error = None
        if peer.device.port.is_open and "original_controls" in result:
            try:
                result["settings_restore"] = restore_controls(peer.device, result["original_controls"])
                result["restore_rtc"] = idle_business(peer)
                result["restore_console"] = peer.command("AT+CONSOLE")
                assert number(fields(result["restore_console"])["SOF"], "disconnects") == result["sof_baseline"], result["restore_console"]
            except Exception as error:
                restoration_error = error
                result["settings_restore_failure"] = repr(error)
                result["passed"] = False
        peer.device.port.close()
        peer.device.log.close()
        result["seconds"] = round(time.monotonic() - started, 3)
        (args.output / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
        print(json.dumps({key: result.get(key) for key in ("passed", "port", "seconds", "failure", "settings_restore_failure")}), flush=True)
        if restoration_error is not None and "failure" not in result:
            raise RuntimeError("Settings/USB cleanup verification failed") from restoration_error


if __name__ == "__main__":
    main()
