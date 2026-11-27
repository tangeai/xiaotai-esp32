"""Bounded, sanitized diagnostic evidence for one identified SDK session."""

import re

import app_stacks
from console_sdk import ConsoleError, fields


# A bare AT+STATUS invokes every provider, including product-private sections.
READS = {
    "AT+NET": ("NET",),
    "AT+PLATFORM": ("PLATFORM", "PLATFORM-STATS"),
    "AT+MEM": ("MEM", "MEM-FAIL", "MEM-INFO"),
    "AT+STATUS state": ("STATE",),
    "AT+STATUS framework": ("FRAMEWORK", "FRAMEWORK-RESTART"),
    "AT+STATUS tasks": ("TASKS", "TASK"),
    "AT+STATUS port": ("PORT",),
    "AT+STATUS net": ("NET-DIAG",),
    "AT+STATUS media": ("MEDIA", "MEDIA-RX", "MEDIA-TX", "MEDIA-PROCESSOR", "MEDIA-LEVELS", "ECHO", "MEDIA-MEM"),
    "AT+STATUS link": ("LINK", "LINK-SLOT", "LINK-MEDIA"),
    "AT+LOG STATS": ("LOG-STATS",),
    "AT+LOG": ("LOG-END", "LOG"),
    "AT+CALL": ("CALL",),
    "AT+VIDEO": ("VIDEO", "VIDEO-UP", "VIDEO-DOWN"),
}
TEXT_FIELDS = {
    "NET": {"state", "kind", "rssi"}, "STATE": {"app", "activity"},
    "TASK": {"name"}, "PORT": {"tasks_scope", "task", "state", "unit"},
    "NET-DIAG": {"type", "loss", "rssi"}, "MEDIA-PROCESSOR": {"name"},
    "LINK": {"wait"}, "MEM": {"kind"}, "LOG": {"level", "src", "label", "kind"},
    "CALL": {"kind", "state"}, "VIDEO-UP": {"phase", "codec"}, "VIDEO-DOWN": {"phase", "codec"},
}
OMIT = re.compile(r"(?:^|_)(?:ssid|bssid|ip|gateway|mac|peer|key|secret|password|token|verification|device|client|user)(?:_|$)|_hex$")
ERROR_KEY = re.compile(r"(?:^|_)(?:error|errors|failed|failures|overflow|overflows|malformed|deadline_misses)$")
TOKEN = re.compile(r"[a-zA-Z0-9_.-]{1,63}")
NUMBER = re.compile(r"-?[0-9]{1,20}")
RESET = re.compile(r"^rst:(0x[0-9a-fA-F]{1,8}) \(([A-Z0-9_]{1,40})\),boot:0x[0-9a-fA-F]{1,8}(?:\s.*)?$")
ORDINARY_RESET = {"POWERON", "POWERON_RESET", "CHIP_POWER_ON", "SW_RESET", "SW_CPU_RESET", "SW_SYS_RESET",
                  "RTC_SW_CPU_RST", "RTC_SW_SYS_RST", "RTC_SW_SYS_RESET"}
MAX_RING = 64
LOG_TAIL = 32
STACK_MINIMUM = 1024
REQUIRED_ROWS = {"AT+STATUS tasks": {"TASKS"}, "AT+STATUS media": {"MEDIA", "MEDIA-RX", "MEDIA-TX"},
                 "AT+MEM": {"MEM", "MEM-FAIL", "MEM-INFO"}, "AT+VIDEO": {"VIDEO", "VIDEO-UP", "VIDEO-DOWN"}}


def validate_sha(sha):
    if not re.fullmatch(r"[0-9a-fA-F]{64}", sha):
        raise ConsoleError("snapshot requires a full running ELF SHA-256")


def safe_records(reply, prefixes):
    result = []
    for line in reply:
        name, _, body = line.partition(": ")
        name = name.removeprefix("+") if name.startswith("+") else ""
        if name not in prefixes:
            continue
        if len(line) > 512 or len(result) >= 128:
            raise ConsoleError("diagnostic snapshot exceeded its row limit")
        row = {}
        for key, value in fields([line], f"+{name}: ").items():
            if not re.fullmatch(r"[a-z][a-z0-9_]{0,47}", key) or OMIT.search(key):
                continue
            if NUMBER.fullmatch(value):
                row[key] = int(value)
            elif (key in TEXT_FIELDS.get(name, ()) or (ERROR_KEY.search(key) and value != "-")) and TOKEN.fullmatch(value):
                row[key] = value
        if row:
            result.append({"record": name, **row})
    if not result:
        raise ConsoleError("no recognized diagnostic records")
    return result


def one(rows, name):
    selected = [row for row in rows if row["record"] == name]
    return selected[0] if len(selected) == 1 else {}


def log_tail(rows):
    latest = one(rows, "LOG-END").get("latest")
    events = [row for row in rows if row["record"] == "LOG"]
    if not isinstance(latest, int) or not 0 <= latest <= 0xFFFFFFFF or len(events) > MAX_RING:
        raise ConsoleError("log boundary or length is not verifiable")
    sequences = [row.get("seq") for row in events]
    if (any(not isinstance(seq, int) or not 1 <= seq <= latest for seq in sequences)
            or sequences != sorted(set(sequences))):
        raise ConsoleError("log sequence is ambiguous or beyond the recorded boundary")
    return [*events[-LOG_TAIL:], {"record": "LOG-END", "latest": latest, "retained": min(LOG_TAIL, len(events))}]


def boot_seen(lines):
    return any(line.startswith(("ESP-ROM:", "+READY: at=1 ")) or RESET.fullmatch(line) for line in lines)


def scan(session, restarted=lambda: False):
    if not session.expected_sha or session.device.get("elf_sha256") != session.expected_sha:
        raise ConsoleError("snapshot requires a verified running ELF before reading diagnostics")
    evidence, raw = {}, {}
    for cmd, prefixes in READS.items():
        step = evidence[cmd] = {"status": "unknown"}
        if restarted():
            session.usable = False
        if not session.usable:
            step["reason"] = "reply boundary lost; no further commands sent"
            continue
        if cmd == "AT+LOG":
            capacity = one(evidence["AT+LOG STATS"].get("rows", []), "LOG-STATS").get("capacity")
            if not isinstance(capacity, int) or not 1 <= capacity <= MAX_RING:
                step["reason"] = "event ring unavailable or larger than bounded capture"
                continue
        try:
            if session.check(cmd).access != "read":
                raise ConsoleError("not a declared read")
        except ConsoleError:
            step["reason"] = "not available as a product-declared read"
            continue
        try:
            reply = session.send(cmd)
            rows = safe_records(reply, prefixes)
            if cmd == "AT+LOG":
                rows = log_tail(rows)
            if cmd in ("AT+STATUS tasks", "AT+STATUS port"):
                raw[cmd] = reply
            if REQUIRED_ROWS.get(cmd, set()) - {row["record"] for row in rows}:
                step.update(reason="diagnostic records are incomplete", rows=rows)
            else:
                evidence[cmd] = {"status": "collected", "rows": rows}
        except ConsoleError:
            step["reason"] = "command rejected, incomplete or malformed"
        except OSError:
            step["reason"] = "serial read failed; no further commands sent"
            session.usable = False
    return evidence, raw


def stack_samples(raw):
    try:
        port_rows = app_stacks.parse_application_subset(raw.get("AT+STATUS port", []))
        if port_rows is not None:
            return "application_subset", port_rows
    except ConsoleError:
        pass
    try:
        status, _, rows = app_stacks.parse_snapshot(raw.get("AT+STATUS tasks", []))
    except ConsoleError:
        status, rows = "unknown", []
    owner = [row for row in rows if row["name"] == "xiaotai"]  # Framework's dedicated owner, not all app tasks.
    if status == "complete" and len(owner) == 1:
        return "framework_owner_only", owner
    return "unknown", []


def reports_error(value):
    """An error field is clean when it is zero (decimal or 0x hex, as the video rows report it), OK, NONE or -."""
    if isinstance(value, int):
        return value != 0
    if not isinstance(value, str) or value in {"OK", "NONE", "-"}:
        return False
    try:
        return int(value, 0) != 0
    except ValueError:
        return True


def summarize(evidence, raw, boot_lines, *, restarted=False):
    issues = []
    unknown = [f"{cmd}: {step['reason']}" for cmd, step in evidence.items() if step["status"] != "collected"]

    def rows(cmd):
        return evidence[cmd].get("rows", ())

    platform = one(rows("AT+PLATFORM"), "PLATFORM").get("online")
    if platform == 0:
        issues.append("platform is offline")
    elif platform != 1:
        unknown.append("platform online state")
    network = one(rows("AT+NET"), "NET").get("connected")
    if network == 0:
        issues.append("network is disconnected")
    elif network != 1:
        unknown.append("network link state")
    mem = one(rows("AT+MEM"), "MEM-FAIL")
    if not isinstance(mem.get("failures"), int) or mem["failures"] < 0 or mem.get("installed") != 1:
        unknown.append("allocation failure monitoring")
    stack_scope, stacks = stack_samples(raw)
    unknown.append("full application task stack coverage is not established by this snapshot")
    for row in stacks:
        free = row["stack_min_free_bytes"]
        if free is not None and free < STACK_MINIMUM:
            issues.append(f"application stack {row['name']} minimum {free} B (<{STACK_MINIMUM} B)")
    resets = [{"code": match[1], "reason": match[2]} for line in boot_lines if (match := RESET.fullmatch(line))]
    if restarted:
        issues.append("device restarted during the identified serial session")
    if not resets:
        unknown.append("reset cause: no ROM reset line observed in this capture")
    for row in resets:
        if row["reason"] not in ORDINARY_RESET:
            issues.append(f"unexpected ROM reset: {row['reason']} ({row['code']})")
    for step in evidence.values():
        for row in step.get("rows", ()):
            for key, value in row.items():
                if ERROR_KEY.search(key) and reports_error(value):
                    issues.append(f"{row['record']}.{key}={value}")
    return {"issues": list(dict.fromkeys(issues)), "unknown": list(dict.fromkeys(unknown)),
            "stack_scope": stack_scope, "stack_minimum_bytes": STACK_MINIMUM, "observed_resets": resets}


def report(session, evidence, raw, boot_lines, *, restarted=False):
    summary = summarize(evidence, raw, boot_lines, restarted=restarted)
    visible = ("board", "revision", "version", "platform", "chip", "cores", "elf_sha256")
    return {"summary": summary,
            "firmware": {key: session.device[key] for key in visible if key in session.device},
            "evidence": evidence}
