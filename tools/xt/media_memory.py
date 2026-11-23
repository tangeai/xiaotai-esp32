#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Summarize media memory from saved AT transcripts, without opening a port."""

import argparse
import json
from pathlib import Path
import re


RECORD = re.compile(r"^\s*(?:\[[^\]]+\]\s*)?\+(MEDIA|MEDIA-MEM|MEM|TASK):\s*(.*)$")
TASKS = {"xt_capture", "xt_clean", "xt_playout", "xt_uplink"}
TOPOLOGIES = {
    "folded": {"xt_capture", "xt_uplink"},
    "encoded": {"xt_playout", "xt_uplink"},
    "split": {"xt_capture", "xt_playout", "xt_uplink"},
}


def number(fields, name):
    value = fields.get(name, "")
    if not value.isascii() or not value.isdecimal():
        raise ValueError(f"missing or invalid {name}")
    return int(value)


def read_snapshot(path, active):
    """One phase may contain repeated status samples, all in the same state."""
    states, heap, watermarks, tasks = [], [], [], {}
    for line in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        match = RECORD.match(line)
        if not match:
            continue
        kind, row = match.groups()
        fields = dict(token.split("=", 1) for token in row.split() if "=" in token)
        if kind == "MEDIA":
            generation = number(fields, "generation")
            if (number(fields, "ready") != 1 or number(fields, "active") != int(active)
                    or number(fields, "draining") or bool(generation) != active):
                raise ValueError("media status does not match the claimed phase")
            states.append(generation)
        elif kind == "MEDIA-MEM":
            free, largest = number(fields, "internal_free"), number(fields, "internal_largest")
            if largest > free:
                raise ValueError("internal_largest exceeds internal_free")
            heap.append((free, largest))
        elif kind == "MEM" and fields.get("kind") == "internal":
            free, largest = number(fields, "min_free"), number(fields, "min_largest")
            if free > number(fields, "free") or largest > number(fields, "largest"):
                raise ValueError("internal watermark exceeds current free memory")
            watermarks.append((free, largest))
        elif kind == "TASK" and fields.get("name") in TASKS:
            name = fields["name"]
            free = number(fields, "stack_min_free")
            tasks[name] = min(tasks.get(name, free), free)
    if not states or not heap or not tasks:
        raise ValueError("collect complete AT+STATUS media and AT+STATUS tasks replies")
    return states, heap, watermarks, tasks


def phase(paths, active, topology):
    generations, heap, watermarks, stack = [], [], [], {}
    native_sources = 0
    for path in paths:
        try:
            states, samples, native, tasks = read_snapshot(path, active)
            if topology:
                seen = set(tasks)
                expected = TOPOLOGIES[topology]
                if seen != expected and not (topology == "split" and seen == expected | {"xt_clean"}):
                    raise ValueError(f"unexpected media tasks for {topology}: {', '.join(sorted(seen))}")
        except ValueError as error:
            raise ValueError(f"{path}: {error}") from error
        generations.extend(states)
        heap.extend(samples)
        watermarks.extend(native)
        native_sources += bool(native)
        for name, free in tasks.items():
            stack[name] = min(stack.get(name, free), free)
    return {
        "snapshots": len(heap),
        "generations": sorted(set(generations)),
        "observed_min_internal_free": min(free for free, _ in heap),
        "observed_min_internal_largest": min(largest for _, largest in heap),
        "native_watermark_files": native_sources,
        "native_internal_min_free_since_boot": min((free for free, _ in watermarks), default=None),
        "native_internal_min_sampled_largest": min((largest for _, largest in watermarks), default=None),
        "task_stack_min_free": stack,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("idle", "active", "after"):
        parser.add_argument(f"--{name}", type=Path, nargs="+", required=True, metavar="LOG")
    parser.add_argument("--topology", choices=TOPOLOGIES, help="expected task set on a normal boot")
    parser.add_argument("--min-free", type=int, help="require this native internal minimum free (bytes)")
    args = parser.parse_args(argv)
    if args.min_free is not None and args.min_free <= 0:
        parser.error("--min-free must be positive")
    try:
        report = {"source": "saved_at_transcripts", "topology_checked": args.topology}
        for name in ("idle", "active", "after"):
            report[name] = phase(getattr(args, name), name == "active", args.topology)
        report["observed_free_change_after"] = (
            report["after"]["observed_min_internal_free"] - report["idle"]["observed_min_internal_free"]
        )
        report["observed_largest_change_after"] = (
            report["after"]["observed_min_internal_largest"] - report["idle"]["observed_min_internal_largest"]
        )
    except (ValueError, OSError) as error:
        parser.error(str(error))
    passed = True
    if args.min_free is not None:
        if report["after"]["native_watermark_files"] != len(args.after):
            parser.error("--min-free requires an internal AT+MEM watermark in every --after log")
        measured = report["after"]["native_internal_min_free_since_boot"]
        passed = measured >= args.min_free
        report["native_floor"] = {"bytes": args.min_free, "passed": passed}
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
