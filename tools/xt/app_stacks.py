"""Score a read-only SDK task snapshot without inferring task ownership."""

from collections import Counter
import re

import sdk_bench
from console_sdk import ConsoleError, END, complete, fields, platform_stats


COMMAND = "AT+STATUS tasks"
PORT_COMMAND = "AT+STATUS port"
TASK_NAME = re.compile(r"[A-Za-z0-9_.:-]{1,15}")
UINT32_MAX = 0xFFFFFFFF


def validate_request(expected_sha, expected, minimum):
    if not re.fullmatch(r"[0-9a-fA-F]{64}", expected_sha):
        raise ConsoleError("expected a full 64-digit ELF SHA-256")
    if len(expected) > 48 or len(expected) != len(set(expected)) or any(
            name == "-" or not TASK_NAME.fullmatch(name) for name in expected):
        raise ConsoleError("use distinct --expect-task names as printed by AT+STATUS tasks")
    if not 1 <= minimum <= UINT32_MAX:
        raise ConsoleError("--min-free must be a positive byte count no larger than uint32")


def unsigned(row, key):
    value = sdk_bench.unsigned(row, key)
    if value > UINT32_MAX:
        raise ConsoleError(f"invalid {key}: exceeds uint32")
    return value


def framed(lines):
    if any(END.fullmatch(line) for line in lines[:-1]):
        raise ConsoleError("more than one task snapshot terminator")
    complete(lines, "task snapshot")


def parse_snapshot(lines):
    framed(lines)
    summary = fields(lines, "+TASKS: ")
    supported = summary.get("supported")
    count = unsigned(summary, "count")
    if supported not in ("0", "1"):
        raise ConsoleError("task snapshot missing a valid supported flag")
    rows = []
    for row in sdk_bench.records(lines, "+TASK: "):
        name = row.get("name", "")
        core = row.get("core", "")
        if not TASK_NAME.fullmatch(name) or not re.fullmatch(r"(?:-1|[0-9]{1,2})", core):
            raise ConsoleError("task snapshot has an invalid task name or core")
        rows.append({"name": name, "stack_min_free_bytes": unsigned(row, "stack_min_free"),
                     "priority": unsigned(row, "priority"), "core": int(core)})
    if supported == "0" and (count or rows or "error" in summary):
        raise ConsoleError("unsupported task snapshot unexpectedly reports task data")
    if supported == "0":
        status = "unsupported"
    elif "error" in summary or not count:
        status = "provider_error"
    elif count != len(rows):
        status = "incomplete"
    else:
        status = "complete"
    return status, count, rows


def parse_application_subset(lines):
    framed(lines)
    found_scope, rows = False, []
    for row in sdk_bench.records(lines, "+PORT: "):
        if "tasks_scope" in row:
            if found_scope or row != {"tasks_scope": "application_subset", "unit": "bytes"}:
                raise ConsoleError("unknown or repeated application task scope")
            found_scope = True
        elif "task" in row:
            name, state = row["task"], row.get("state")
            if not found_scope or not TASK_NAME.fullmatch(name):
                raise ConsoleError("application task row lacks scope or a valid name")
            if state == "unavailable" and row.get("stack_min_free") == "-":
                value = None
            elif state == "sampled":
                value = unsigned(row, "stack_min_free")
            else:
                raise ConsoleError("unknown application task sample state")
            rows.append({"name": name, "stack_min_free_bytes": value})
    return rows if found_scope else None


def report(status, count, rows, expected, minimum, source="freertos_tasks"):
    matches = Counter(row["name"] for row in rows)
    selected = [row for row in rows if row["name"] in expected and matches[row["name"]] == 1]
    missing = [name for name in expected if not matches[name]]
    ambiguous = [name for name in expected if matches[name] > 1]
    unsampled = [row["name"] for row in selected if row["stack_min_free_bytes"] is None]
    sampled = [row for row in selected if row["stack_min_free_bytes"] is not None]
    usable = status in {"complete", "application_subset"}
    low = [row["name"] for row in sampled if row["stack_min_free_bytes"] < minimum] if usable else []
    coverage = bool(expected) and usable and not (missing or ambiguous or unsampled)
    return {
        "schema": 1, "verdict": "fail" if low else "pass" if coverage else "not_measured",
        "scope": "explicit_tasks" if expected else "live_task_inventory",
        "all_application_tasks": "not_measured", "selected_coverage_complete": coverage,
        "threshold_bytes": minimum,
        "source": source, "table_status": status, "declared_count": count, "received_count": len(rows),
        "requested": list(expected), "missing": missing, "ambiguous": ambiguous, "unsampled": unsampled,
        "below_threshold": low,
        "observed_min_free_bytes": min((row["stack_min_free_bytes"] for row in rows
                                        if row["stack_min_free_bytes"] is not None), default=None),
        "selected_min_free_bytes": min((row["stack_min_free_bytes"] for row in sampled), default=None),
        "tasks": rows,
    }


def application_subset(session, expected, minimum):
    source = "port_application_subset"
    try:
        if session.check(PORT_COMMAND).access != "read":
            return None
        lines = session.send(PORT_COMMAND)
    except ConsoleError:
        return None  # Some builds do not provide any port diagnostics.
    try:
        rows = parse_application_subset(lines)
    except ConsoleError:
        return report("malformed_reply", None, [], expected, minimum, source)
    if rows is None:
        return None
    return report("application_subset", None, rows, expected, minimum, source)


def read(session, expected, minimum):
    """Read one fresh task snapshot from an already identified SDK session."""
    try:
        allowed_read = session.check(COMMAND).access == "read"
    except ConsoleError:
        allowed_read = False
    if not allowed_read:
        return report("command_unavailable", None, [], expected, minimum)
    try:
        lines = session.send(COMMAND)
    except ConsoleError:
        return report("reply_failed", None, [], expected, minimum)
    try:
        status, count, rows = parse_snapshot(lines)
    except ConsoleError:
        return report("malformed_reply", None, [], expected, minimum)
    if status == "unsupported":
        subset = application_subset(session, expected, minimum)
        if subset is not None:
            return subset
    return report(status, count, rows, expected, minimum)


def read_workers(session, minimum):
    """Read subsystem-owned watermarks separately from the task inventory."""
    probes = (("transport", "AT+LINK"), ("platform", "AT+PLATFORM"))
    result = {}
    for worker, command in probes:
        sample = {"command": command, "status": "not_measured", "stack_min_free_bytes": None}
        result[worker] = sample
        try:
            if session.check(command).access != "read":
                continue
            lines = session.send(command)
            framed(lines)
            if worker == "transport":
                free = unsigned(fields(lines, "+LINK: "), "worker_stack_free")
            else:
                free = platform_stats(lines).get("worker_stack_free")
        except ConsoleError:
            continue
        # Zero is also the provider's value before sampling or after worker exit.
        if free:
            sample.update(status="below_threshold" if free < minimum else "sampled",
                          stack_min_free_bytes=free)
    return result
