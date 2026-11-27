# SPDX-License-Identifier: MIT
"""Check the actual ELF before emitting a firmware image with the idle stack policy."""

import hashlib
from pathlib import Path
import re
import subprocess
import sys

ARCHIVE_SHA256 = "328dfaaa3fb313216f30734cbfd1cdec676fd44cabfd8adcfa3b8a3462ef45f3"
INNER = "freertos_ThreadCreateWithStackSize"
KERNEL = "xTaskCreatePinnedToCore"
WRAP = "__wrap_" + KERNEL
SCOPE = "xt_tirtc_start_with_idle_stack"
ORIGIN = "__xt_idle_stack_origin_"
THREAD = "rtc_thread_create"
SESSION_WRAP = "__wrap_" + THREAD
SESSION_ORIGINS = {
    "inbound": "signal_tgtrp_listener_callback_wrapper",
    "outbound": "tgtrp_connection_whip_new",
}


def require(condition, detail):
    if not condition:
        raise ValueError(detail)


def run(tool, *args):
    return subprocess.check_output([tool, *map(str, args)], text=True, encoding="utf-8", errors="replace")


def calls(text, target, may_tail=False):
    ends = []
    for line in text.splitlines():
        step = re.fullmatch(r"\s*([0-9a-f]+):\s+([0-9a-f]{4}|[0-9a-f]{8})\s+(\w+)\s+(.+)", line)
        if step is None:
            continue
        address, raw, mnemonic, operands = step.groups()
        if not re.search(r"<" + re.escape(target) + r">\s*$", operands):
            continue
        if may_tail:
            require(mnemonic in ("jal", "jalr", "j", "jr"), f"invalid branch to {target}")
        else:
            require(mnemonic in ("jal", "jalr") and not re.match(r"\s*(zero|x0)\s*,", operands),
                    f"no normal return address from {target}")
        ends.append(int(address, 16) + len(raw) // 2)
    return ends


def verify(nm, objdump, archive, elf, map_path, session=False):
    require(hashlib.sha256(archive.read_bytes()).hexdigest() == ARCHIVE_SHA256, "Mini archive no longer matches")
    symbols = {}
    for line in run(nm, "-S", "--defined-only", elf).splitlines():
        parts = line.split()
        if len(parts) in (3, 4):
            symbols[parts[-1]] = int(parts[0], 16), int(parts[1], 16) if len(parts) == 4 else 0
    map_text = map_path.read_text(encoding="utf-8", errors="replace")

    def member(name, obj):
        addr, length = symbols[name]
        matches = re.findall(r"(?m)^\s*\.text\." + re.escape(name) + r"[ \t]*(?:\r?\n)?[ \t]+"
                             r"(0x[0-9a-f]+)[ \t]+(0x[0-9a-f]+)[ \t]+([^\r\n]+)"
                             + r"\(" + re.escape(obj) + r"\)[ \t]*$", map_text)
        require([Path(path.strip()).resolve() for start, size, path in matches
                 if int(start, 16) == addr and int(size, 16) == length] == [archive.resolve()],
                f"unproven linked SDK input: {name}/{obj}")
        return addr, length

    def code(name):
        return run(objdump, "-d", f"--disassemble={name}", elf)

    first, size = member(INNER, "platforms.o")
    member("TiRtcStart", "tiRTC.o")
    member("tgtrp_listen", "tgtrp.o")
    member("TaskQueueCreate", "taskq.o")
    member("rtc_thread_create", "rtc_thread.o")
    limit = first + 0xc0
    require(first < limit <= first + size, "guard extends past the audited caller")
    require(symbols[ORIGIN + "first"][0] == first and symbols[ORIGIN + "limit"][0] == limit,
            "runtime guard does not match the audited caller")
    ret = calls(code(INNER), WRAP)
    require(len(ret) == 1 and first < ret[0] < limit, "one wrapped SDK call expected in guard")
    require(calls(code(WRAP), KERNEL, may_tail=True), "kernel arguments must reach the real FreeRTOS call")
    require(len(calls(code("xiaotai_rtc_start"), SCOPE)) == 1 and calls(code(SCOPE), "TiRtcStart", may_tail=True),
            "the ESP transport startup scope is not in the linked path")
    creator = SESSION_WRAP if session else THREAD
    require(len(calls(code("TiRtcStart"), "tgtrp_listen")) == 1 and
            len(calls(code("TiRtcStart"), "TaskQueueCreate")) == 1 and
            len(calls(code("tgtrp_listen"), creator)) == 2 and
            len(calls(code(THREAD), INNER)) == 1 and
            len(calls(code("TaskQueueCreate"), INNER)) == 1, "startup thread creation path changed")
    if session:
        require(calls(code(SESSION_WRAP), THREAD, may_tail=True), "session policy must call the real creator")
        scopes = []
        for direction, name in SESSION_ORIGINS.items():
            start, length = member(name, "tgtrp.o")
            lower = symbols[f"_xt_session_{direction}_start"][0]
            upper = symbols[f"_xt_session_{direction}_end"][0]
            returns = calls(code(name), SESSION_WRAP)
            require(lower == start and upper == start + length,
                    f"{direction} policy must enclose only its audited function")
            require(len(returns) == 1 and lower < returns[0] < upper,
                    f"{direction} must have exactly one wrapped creator with a normal return")
            scopes.append((lower, upper))
            print(f"TiRTC session stack link verified ({direction}): {lower:#x}..{upper:#x}, return {returns[0]:#x}")
        scopes.sort()
        require(scopes[0][1] <= scopes[1][0], "independent session guards overlap")
    else:
        require(SESSION_WRAP not in symbols, "disabled session policy must not be linked")
    print(f"TiRTC idle stack link verified: {first:#x}..{limit:#x}, return {ret[0]:#x}")


if __name__ == "__main__":
    try:
        require(sys.argv[6:] in ([], ["--session"]), "unexpected session policy arguments")
        verify(sys.argv[1], sys.argv[2], *(Path(path) for path in sys.argv[3:6]),
               session=sys.argv[6:] == ["--session"])
    except (KeyError, ValueError, OSError, subprocess.CalledProcessError) as error:
        sys.exit(f"TiRTC idle stack link rejected: {error}")
