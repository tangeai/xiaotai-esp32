#!/usr/bin/env python3
"""Observe current MultiNet prompt events without resetting the board.

A trigger proves one acoustic detection, not a measured recognition rate.
Runtime AI readiness is reported separately from the local detection.
"""

from __future__ import annotations

import argparse
import re
import sys
import time

import serial


PROMPT = re.compile(
    r"MultiNet prompt detected id=([1-3]) phrase=(你好小钛|小钛小钛|小钛同学) "
    r"probability=([0-9.]+); starting remote AI"
)
PHRASES = {1: "你好小钛", 2: "小钛小钛", 3: "小钛同学"}
ACCEPTED = re.compile(r"AI wake request accepted token=(\d+) session=(\d+)")
ACTIVE = re.compile(r"state=ai-active session=(\d+) ")
FAILURES = (
    "Guru Meditation Error",
    "abort() was called",
    "stack overflow",
    "Rebooting...",
    "offline local voice unavailable",
)


def parse_prompt(line: str) -> tuple[int, str, float] | None:
    match = PROMPT.search(line)
    if match is None:
        return None
    command_id = int(match.group(1))
    phrase = match.group(2)
    if PHRASES[command_id] != phrase:
        return None
    return command_id, phrase, float(match.group(3))


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--seconds", type=float, default=20.0)
    parser.add_argument("--expected-phrase", choices=tuple(PHRASES.values()))
    parser.add_argument("--post-trigger-seconds", type=float, default=8.0)
    parser.add_argument("--require-ai-ready", action="store_true")
    parser.add_argument("--negative", action="store_true", help="quiet/no-wake-word interval; any detection fails")
    args = parser.parse_args()
    if args.seconds <= 0 or args.post_trigger_seconds < 0:
        parser.error("seconds must be positive and post-trigger-seconds nonnegative")
    if args.negative and (args.require_ai_ready or args.expected_phrase):
        parser.error("negative interval cannot require a phrase or AI readiness")

    counts = {phrase: 0 for phrase in PHRASES.values()}
    max_probability = 0.0
    failed = False
    ai_ready = False
    first_trigger = True
    accepted_session = None
    queued = False
    listening_observed = False
    paused = False
    deadline = time.monotonic() + args.seconds
    with serial.Serial(args.port, 115200, timeout=0.2) as uart:
        while time.monotonic() < deadline:
            line = uart.readline().decode("utf-8", errors="replace").strip()
            detected = parse_prompt(line)
            if "local-pcm fed" in line:
                if "result=ESP_OK listening=1" in line:
                    listening_observed = True
                else:
                    paused = True
            if "local-pcm blocked" in line:
                paused = True
            if detected is not None:
                _, phrase, probability = detected
                counts[phrase] += 1
                max_probability = max(max_probability, probability)
                print(f"DETECTED phrase={phrase} probability={probability:.3f}")
                if first_trigger and not args.negative:
                    deadline = max(deadline, time.monotonic() + args.post_trigger_seconds)
                    first_trigger = False
            # Do not print arbitrary UART data: it can contain device credentials.
            for marker in FAILURES:
                if marker in line:
                    failed = True
                    print(f"FAIL: {marker}")
            if "AI start_session sent" in line:
                print("INFO: AI start_session sent; acceptance still pending")
            if sum(counts.values()) and "wake request queued token=" in line:
                queued = True
            accepted = ACCEPTED.search(line)
            if sum(counts.values()) and accepted:
                accepted_session = accepted.group(2)
            active = ACTIVE.search(line)
            if active and accepted_session == active.group(1):
                ai_ready = True
            if "AI start ignored:" in line:
                print("INFO: runtime rejected AI start")

    matches = counts[args.expected_phrase] if args.expected_phrase else sum(counts.values())
    print(f"SUMMARY detections={counts} max_prob={max_probability:.3f} "
          f"product_queued={queued} runtime_accepted={accepted_session is not None} ai_ready={ai_ready}")
    if failed:
        return 2
    if args.negative:
        if not listening_observed or paused:
            print("INCONCLUSIVE: detector was not confirmed listening throughout the negative interval")
            return 2
        print(f"NEGATIVE detections={sum(counts.values())} observation_seconds={args.seconds:.1f}")
        return 1 if sum(counts.values()) else 0
    if matches == 0:
        print("FAIL: no matching MultiNet detection; this log alone cannot distinguish a miss from paused audio")
        return 1
    if args.require_ai_ready and not ai_ready:
        print("FAIL: detected wake did not reach its accepted AI session")
        return 1
    print("PASS: MultiNet acoustic detection observed; recognition rate and AI connection are separate checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
