#!/usr/bin/env python3
"""Compare real AT+HELP records from a host fixture with its unit manifests.

The fixture runs AT+HELP through its console, then emits the bare records with
at_test_help_write(). Pass --exact when the manifests cover every loaded unit.
Without it, other units may contribute additional records to the capture.
"""
from argparse import ArgumentParser, REMAINDER
from collections import Counter
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
from sdk_selection import SelectionError, _load, vocabulary


def expected_records(manifests, option_off=(), with_probes=False):
    """Use the selector's spelling rules (including bare AT and shared verbs)."""
    units = [_load(path, path.stem) for path in manifests]
    disabled = set(option_off)
    if "XIAOTAI_AT_RESTART_COMMAND" in disabled:
        disabled.add("XIAOTAI_AT_RESTART_FORCE")
    return {
        " ".join(part for part in (row["command"], row["verb"], row["usage"]) if part)
        for row in vocabulary(units)
        if row["option"] not in disabled and (with_probes or row["command"] != "AT+PORT")
    }


def compare(records, expected, *, exact=False):
    """Return unsatisfied expectations; preserve spaces, case and duplicates."""
    counts = Counter(records)
    errors = [f"missing: {line!r}" for line in sorted(expected - counts.keys())]
    if exact:
        errors += [f"unexpected: {line!r}" for line in sorted(counts.keys() - expected)]
    errors += [f"duplicate ({count}): {line!r}" for line, count in sorted(counts.items()) if count != 1]
    return errors


def main(argv=None):
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, action="append", type=Path)
    parser.add_argument("--option-off", action="append", default=[])
    parser.add_argument("--with-probes", action="store_true")
    parser.add_argument("--exact", action="store_true")
    parser.add_argument("fixture", nargs=REMAINDER, help="-- executable [args ...]")
    args = parser.parse_args(argv)
    fixture = args.fixture[1:] if args.fixture[:1] == ["--"] else args.fixture
    if not fixture:
        parser.error("expected a fixture after --")
    try:
        expected = expected_records(args.manifest, args.option_off, args.with_probes)
        result = subprocess.run(fixture, check=True, stdout=subprocess.PIPE, encoding="ascii", timeout=30)
    except (SelectionError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"AT+HELP: {error}", file=sys.stderr)
        return 1
    errors = compare(result.stdout.splitlines(), expected, exact=args.exact)
    for error in errors:
        print(f"AT+HELP: {error}", file=sys.stderr)
    return bool(errors)


if __name__ == "__main__":
    sys.exit(main())
