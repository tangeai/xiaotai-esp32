# SPDX-License-Identifier: MIT
import json
import subprocess
import sys
from pathlib import Path


path = Path(sys.argv[1]).resolve()
manifest = json.loads(path.read_text(encoding="utf-8"))
sys.path.insert(0, str(path.parents[2] / "scripts"))
import sdk_selection

legacy = next(item for item in manifest["commands"] if item["command"] == "AT+AUDIOTRACE?")
assert sdk_selection._commands(path, [legacy]) == [legacy]
for invalid in (
    dict(legacy, command="AT+AUDIOTRACE??"),
    dict(legacy, command="AT+?"),
    dict(legacy, access="action"),
    dict(legacy, verb="DUMP"),
    dict(legacy, usage="<name>"),
    {key: value for key, value in legacy.items() if key not in {"option", "symbol"}},
):
    try:
        sdk_selection._commands(path, [invalid])
    except sdk_selection.SelectionError:
        continue
    raise SystemExit(f"Query suffix must be restricted to optional bare reads: {invalid!r}")

for enabled, binary in enumerate(sys.argv[2:]):
    expected = {
        " ".join(filter(None, (item["command"], item.get("verb"), item.get("usage"))))
        for item in manifest["commands"]
        if not item.get("option") or (enabled and (enabled == 2 or item != legacy))
    }
    output = subprocess.check_output([binary, "help"], text=True)
    lines = [line.removeprefix("+HELP: ") for line in output.splitlines()
             if line.startswith(("+HELP: AT+AUDIO", "+HELP: AT+SOUND", "+HELP: AT+TRACE"))]
    if len(lines) != len(set(lines)) or set(lines) != expected:
        raise SystemExit(f"M11 HELP differs from the manifest ({enabled=}): {lines!r} != {expected!r}")
