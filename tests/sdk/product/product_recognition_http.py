# SPDX-License-Identifier: MIT
"""Replay the deployed query vectors through each generated product's HTTP port."""
import json
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from recognition_fixtures import fixtures  # noqa: E402

STATES = ("queued", "processing", "succeeded", "partial_failed", "failed", "unknown", "cancelled")


def main(executable):
    cases = [(name, body, status, valid, 0) for name, body, status, valid in fixtures()]
    valid_body = next(body for name, body, _, _, _ in cases if name == "failed")
    cases.extend((name, valid_body, status, valid, mode) for name, status, valid, mode in (
        ("queued-behind-exchange", 200, True, 1),
        ("connect-failure", 0, False, 2),
        ("receive-reset-mid-body", 200, False, 3),
        ("declared-body-too-large", 200, False, 4),
        ("chunked-without-length", 200, True, 5),
        ("cancelled-while-queued", 200, True, 6),
        ("queued-21s-within-owner-deadline", 200, True, 7),
        ("queued-26s-past-owner-deadline", 200, True, 8),
        ("response-head-failure", 0, False, 9),
        ("eof-before-first-body-byte", 200, False, 10),
        ("response-head-stall", 0, False, 11),
        ("body-read-stall", 200, False, 12),
        ("whole-wire-timeout", 200, False, 13),
        ("notice-oom-first-view", 200, True, 14),
        ("notice-oom-replacing-view", 200, True, 15),
        ("notice-oom-first-document", 200, True, 16),
        ("notice-oom-replacing-document", 200, True, 17),
        ("eof-after-23-body-bytes", 200, False, 18),
    ))
    failed = []
    for name, body, status, valid, mode in cases:
        body = body.replace(b'"fixture-device"', b'"host-device"')
        expected = json.loads(body)["data"] if valid else None
        fields = (expected["status_revision"], len(expected["results"]), STATES.index(expected["status"])) if expected else (0, 0, 0)
        wire = (" ".join(str(item) for item in (status, mode, len(body), *fields)) + "\n").encode() + body
        run = subprocess.run([executable, "recognition-query-wire"], input=wire, capture_output=True, timeout=15)
        if run.returncode:
            failed.append(name)
            print(f"{name}: FAIL\n{run.stdout.decode(errors='replace')}{run.stderr.decode(errors='replace')}", flush=True)
            continue
        evidence = [line for line in run.stdout.decode(errors="replace").splitlines() if line.startswith("query wire:")]
        assert len(evidence) == 1, (name, evidence)
        print(f"{name}: {evidence[0]}", flush=True)
    assert not failed, failed
    print(f"{len(cases)} deployed payload and transport vectors replayed through the generated product", flush=True)


if __name__ == "__main__":
    main(sys.argv[1])
