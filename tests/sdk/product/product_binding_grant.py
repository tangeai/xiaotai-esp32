# SPDX-License-Identifier: MIT
"""Pass the stored fixture credentials between two fresh product host processes."""
import base64
import hashlib
import hmac
import re
import subprocess
import sys


def run(binary, action, payload=None):
    result = subprocess.run([binary, action], input=payload, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
    public = (line for line in result.stdout.splitlines()
              if not line.startswith(("XT-IDENTITY: ", "XT-LOGIN: ")))
    print("\n".join(public), flush=True)
    result.check_returncode()
    return result.stdout


def main(binary):
    first = run(binary, "binding-grant-save")
    records = re.findall(r"^XT-IDENTITY: ([0-9a-f]{644})$", first, re.MULTILINE)
    if len(records) != 1:
        raise RuntimeError("expected exactly one normalized KV export")
    second = run(binary, "binding-grant-reload", payload=records[0] + "\n")
    requests = re.findall(r"^XT-LOGIN: (host-device) (\d+) ([0-9a-f]{16}) ([A-Za-z0-9+/]{43}=)$",
                          second, re.MULTILINE)
    if len(requests) != 1:
        raise RuntimeError("expected exactly one signed login from the new process")
    device, timestamp, nonce, signature = requests[0]
    digest = hmac.new(b"host-device-key", (device + timestamp + nonce).encode("ascii"), hashlib.sha256).digest()
    if not hmac.compare_digest(base64.b64encode(digest).decode("ascii"), signature):
        raise RuntimeError("cold login did not sign with the written identity")
    print("Verified formal login signature from the persisted synthetic identity")


if __name__ == "__main__":
    main(sys.argv[1])
