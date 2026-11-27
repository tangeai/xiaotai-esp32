# SPDX-License-Identifier: MIT
"""Pass the exact persisted fixture bytes into a fresh product process."""
import re
import subprocess
import sys


PREFIX = "XT-PARTIAL-WIFI: "


def run(binary, scenario, payload=None):
    result = subprocess.run([binary, scenario], input=payload, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=25)
    print("\n".join(line for line in result.stdout.splitlines()
                    if not line.startswith(PREFIX)), flush=True)
    result.check_returncode()
    return result.stdout


def main(binary):
    first = run(binary, "reprovision-partial-write")
    records = re.findall(rf"^{re.escape(PREFIX)}([0-9a-f]{{842}})$", first, re.MULTILINE)
    if len(records) != 1:
        raise RuntimeError("expected exactly one partial credential snapshot")
    run(binary, "reprovision-partial-cold", records[0] + "\n")


if __name__ == "__main__":
    main(sys.argv[1])
