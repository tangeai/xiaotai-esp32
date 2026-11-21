#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Summarize live C3 product requests by measured source, retaining gate failure."""

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
import re
import subprocess
import sys


ROWS = {
    "1": "main task",
    "2": "framework owner stack",
    "3": "framework objects and queues",
    "4": "RTC transport worker stack",
    "5": "platform worker and buffers",
    "6": "MQTT client stack and buffers",
    "7": "network, portal and DNS",
    "8": "KV worker and slots",
    "9": "identity worker and buffers",
    "10": "media worker stacks",
    "11": "media downlink objects and ring",
    "11a": "media folded send buffers",
    "12": "other media buffers",
    "13": "UI, themes and app view storage",
    "14": "interaction",
    "15": "feedback",
    "16": "VB6824 RX stack",
    "17": "AT task and buffers",
    "18": "RTC command pool",
    "19": "retired static storage (not charged to the new port)",
    "other": "other owned requests",
}
TASK_ROWS = {
    "main": "1", "xiaotai": "2", "xt_transport": "4", "xiaotai_platform": "5",
    "xiaotai_mqtt": "6", "xiaotai_net": "7", "xiaotai_dns": "7", "esp_http_server": "7",
    "xiaotai_kv": "8", "identity_reload": "9", "xt_capture": "10", "xt_playout": "10",
    "xt_uplink": "10", "xt_ui": "13", "xt_input": "14", "xt_feedback": "15",
    "vb6824_rx": "16", "at": "17",
}


@dataclass(frozen=True)
class Item:
    kind: str
    bytes: int
    site: int
    name: str
    fixture: bool
    comparable: bool


def resolve(binary, items):
    sites = sorted({item.site for item in items if item.site and item.comparable})
    if not sites:
        return {}
    # One fixed executable, no shell. The PCs are emitted by the allocator.
    output = subprocess.run(
        ["addr2line", "-f", "-e", binary], input="".join(f"{site - 1:x}\n" for site in sites),
        check=True, text=True, stdout=subprocess.PIPE, timeout=10,
    ).stdout.splitlines()
    if len(output) != 2 * len(sites):
        raise ValueError("incomplete source attribution")
    result = {}
    for site, function, location in zip(sites, output[::2], output[1::2]):
        if function == "__wrap_xiaotai_mem_alloc":
            raise ValueError(f"allocator wrapper lost the original caller for {site:x}")
        match = re.fullmatch(r"(.*):(\d+)(?:\s+\(discriminator \d+\))?", location)
        if not match:
            raise ValueError(f"bad source attribution for {site:x}: {location}")
        result[site] = (function, Path(match[1]), int(match[2]))
    return result


def classify(item, located, source_cache):
    if item.name != "-":
        return TASK_ROWS.get(item.name, "other")
    _, path, line = located
    parts = path.parts
    relative = path.as_posix()
    if "xiaotai_transport" in parts and path.name == "transport.c":
        if path not in source_cache:
            source_cache[path] = path.read_text(encoding="utf-8").splitlines()
        if "command_pool" in source_cache[path][line - 1]:
            return "18"
    if "xiaotai_media" in parts:
        if path.name == "downlink.c":
            return "11"
        if path.name == "audio_workers.c":
            if path not in source_cache:
                source_cache[path] = path.read_text(encoding="utf-8").splitlines()
            if re.search(r"fold_(producer|consumer)\s*=", source_cache[path][line - 1]):
                return "11a"
        return "12"
    if "xiaotai_framework" in parts:
        return "3"
    if "xiaotai_platform" in parts:
        return "5"
    if path.name == "mqtt.c":
        return "6"
    if "xiaotai_net" in parts:
        return "7"
    if path.name == "kv_worker.c":
        return "8"
    if "xiaotai_identity" in parts:
        return "9"
    if {"xiaotai_ui", "themes", "xiaotai_app_companion", "xiaotai_app_voice_terminal"} & set(parts):
        return "13"
    if "xiaotai_interaction" in parts:
        return "14"
    if "xiaotai_feedback" in parts:
        return "15"
    if "xiaotai_at" in parts or relative.endswith("/host/console.c"):
        return "17"
    return "other"


def report(phase, metrics, items, locations, source_cache):
    by_row = defaultdict(Counter)
    by_source = Counter()
    kinds = Counter()
    for item in items:
        located = locations.get(item.site)
        if item.fixture:
            kinds["fixture"] += item.bytes
        elif not item.comparable:
            kinds["excluded"] += item.bytes
        else:
            if located and located[1].name == "?":
                raise ValueError(f"unresolved counted allocation: 0x{item.site:x}")
            kind = "stack" if item.name != "-" else item.kind
            row = classify(item, located, source_cache)
            by_row[row][kind] += item.bytes
            kinds[kind] += item.bytes
            if located:
                function, path, line = located
                at = path.as_posix().split("/xiaotai-deps/", 1)[-1]
                label = f"{at}:{line} ({function})"
            else:
                label = f"configured:{item.name}"
            by_source[(row, kind, label)] += item.bytes
    for kind in ("stack", "bulk", "internal", "dma", "fixture", "excluded"):
        if kinds[kind] != metrics[kind]:
            raise ValueError(f"{phase}: inconsistent {kind} live sum")
    total = sum(kinds[kind] for kind in ("stack", "bulk", "internal", "dma"))
    if total != metrics["total"] or total - metrics["baseline"] != metrics["delta"]:
        raise ValueError(f"{phase}: inconsistent measured total")
    print(f"\n{phase}: requested resident RAM (bytes)")
    print(" row  item                                      stack internal     dma    bulk   total")
    for number, label in ROWS.items():
        values = by_row[number]
        cols = "".join(f" {values[key]:7d}" for key in ("stack", "internal", "dma", "bulk"))
        print(f"{number:>4}  {label:<39}{cols} {sum(values.values()):7d}")
    print(f"total={total} baseline={metrics['baseline']} delta={metrics['delta']:+d} "
          f"budget={metrics['budget']} {'OVER' if total > metrics['budget'] else 'WITHIN'}; "
          f"excluded MQTT stack={kinds['excluded']} fixture-only={kinds['fixture']}")
    for (row, kind, label), size in sorted(by_source.items()):
        print(f"  {row:>5} {kind:>8} {size:7d}  {label}")
    return total > metrics["budget"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary")
    parser.add_argument("scenario", choices=("ledger-session", "ledger-provision"))
    args = parser.parse_args()
    run = subprocess.run([args.binary, args.scenario], capture_output=True, text=True, timeout=50)
    sys.stderr.write(run.stderr)
    phases = defaultdict(list)
    totals = {}
    for line in run.stdout.splitlines():
        if line.startswith("ledger-item "):
            _, phase, kind, size, site, task, fixture, compared = line.split()
            phases[phase].append(Item(kind, int(size), int(site, 16), task, fixture == "1", compared == "1"))
        elif line.startswith("ledger-total "):
            _, phase, *pairs, verdict = line.split()
            metrics = {k: int(v) for k, v in (pair.split("=") for pair in pairs)}
            over = metrics["total"] > metrics["budget"]
            if phase in totals or verdict != ("over-budget" if over else "within-budget"):
                raise ValueError("bad ledger phase record")
            totals[phase] = metrics
        else:
            print(line)
    expected = ("online-idle", "media-active") if args.scenario == "ledger-session" else ("provision-portal",)
    completed = [phase for phase in expected if phase in phases and phase in totals]
    locations = resolve(args.binary, (item for phase in completed for item in phases[phase]))
    source_cache = {}
    # Do not short-circuit when the first phase exceeds the cap.
    caps = [report(phase, totals[phase], phases[phase], locations, source_cache) for phase in completed]
    if tuple(phases) != expected or tuple(totals) != expected:
        print(f"FAIL: product exit={run.returncode}; missing or invalid ledger phases", file=sys.stderr)
        return 1
    if run.returncode not in (0, 1) or bool(run.returncode) != any(caps):
        print(f"FAIL: product exit={run.returncode} does not match the measured budget result", file=sys.stderr)
        return 1
    return run.returncode


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (KeyError, ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
