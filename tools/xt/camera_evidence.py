# SPDX-License-Identifier: MIT
"""Parse typed camera evidence from a single SDK console exchange."""

import re


_PHASES = {"off", "starting", "running", "stopping", "error", "unknown"}
_LUMA_STATES = {"valid", "invalid", "unavailable", "no_frame"}
_FORMATS = {"yuv422", "yuv420_paired", "jpeg", "rgb565", "unknown"}
_UINT32 = (1 << 32) - 1
_UINT64 = (1 << 64) - 1


def _rows(lines, *prefixes):
    rows = {prefix: [] for prefix in prefixes}
    for line in lines:
        tag, sep, rest = line.strip().partition(":")
        if sep and tag in rows:
            rows[tag].append(rest)
    return rows


def _fields(row, tag):
    fields = {}
    for word in row.split():
        key, sep, value = word.partition("=")
        if not sep or not key or not value or key in fields:
            raise ValueError(f"{tag}: malformed or repeated field")
        fields[key] = value
    return fields


def _single(rows, tag, *, optional=False):
    found = rows[tag]
    if not found and optional:
        return None
    if len(found) != 1:
        raise ValueError(f"{tag}: expected one row, got {len(found)}")
    return _fields(found[0], tag)


def _value(fields, key):
    if key not in fields:
        raise ValueError(f"missing camera evidence field {key}")
    return fields[key]


def _uint(fields, key, maximum=_UINT32):
    raw = _value(fields, key)
    if not re.fullmatch(r"[0-9]{1,20}", raw) or int(raw) > maximum:
        raise ValueError(f"invalid unsigned camera field {key}")
    return int(raw)


def _choice(fields, key, choices):
    value = _value(fields, key)
    if value not in choices:
        raise ValueError(f"invalid camera field {key}")
    return value


def _error(fields):
    error = _value(fields, "error")
    if not re.fullmatch(r"(?:[A-Z][A-Z0-9_]*|0x[0-9a-fA-F]{1,8})", error):
        raise ValueError("invalid camera error code")
    return error


def _luma(fields, state_key, mean_key, sample_key):
    state = _choice(fields, state_key, _LUMA_STATES)
    samples = _uint(fields, sample_key, 255)
    if state == "valid":
        y_mean = _uint(fields, mean_key, 255)
        if not samples:
            raise ValueError("valid luminance report needs samples")
    else:
        if _value(fields, mean_key) != "-1" or samples:
            raise ValueError("invalid absent luminance report")
        y_mean = None
    return {"state": state, "y_mean": y_mean, "samples": samples}


def video_snapshot(lines):
    """Return one complete AT+VIDEO snapshot; old optional rows may be absent."""
    video, up, down, recovery_tag = "+VIDEO", "+VIDEO-UP", "+VIDEO-DOWN", "+VIDEO-UP-RECOVERY"
    rows = _rows(lines, video, up, down, recovery_tag)
    core = _single(rows, video)
    uplink = _single(rows, up)
    downlink = _single(rows, down)
    recovery = _single(rows, recovery_tag, optional=True)
    luma_keys = ("up_y_state", "up_y_mean", "up_y_n")
    return {
        "requested": _uint(core, "requested"),
        "generation": _uint(core, "generation"),
        "up_phase": _choice(uplink, "phase", _PHASES),
        "down_phase": _choice(downlink, "phase", _PHASES),
        "error": _error(uplink),
        **{key: _uint(uplink, key) for key in
           ("captured", "sent", "capture_failed", "encode_failed", "send_failed")},
        "first_luma": _luma(core, *luma_keys) if any(key in core for key in luma_keys) else None,
        "recovery": {key: _uint(recovery, key) for key in ("reopens", "recovered", "exhausted")}
        if recovery is not None else None,
    }


_PROBE = "+VIDEO-PROBE"
_FRAME = "+VIDEO-PROBE-FRAME"
_LUMA = "+VIDEO-PROBE-LUMA"


def _first_probe_frame(rows):
    first = None
    for row in rows[_FRAME]:
        fields = _fields(row, _FRAME)
        if _uint(fields, "index") != 0:
            continue  # A later notification is not evidence of the first frame.
        if first is not None:
            raise ValueError(f"{_FRAME}: repeated first-frame report")
        crc = _value(fields, "crc32")
        if not re.fullmatch(r"[0-9a-fA-F]{8}", crc):
            raise ValueError(f"{_FRAME}: malformed CRC32")
        first = {
            "index": 0,
            "width": _uint(fields, "width", 65535),
            "height": _uint(fields, "height", 65535),
            "format": _choice(fields, "format", _FORMATS),
            "bytes": _uint(fields, "bytes", _UINT64),
            "crc32": crc.lower(),
            "ok": bool(_uint(fields, "ok", 1)),
        }
    return first


def probe_frame(lines):
    """Return the observed index-0 frame, even before the terminal record."""
    return _first_probe_frame(_rows(lines, _FRAME))


def probe_snapshot(lines):
    """Return the terminal probe and optional evidence, or None while pending."""
    rows = _rows(lines, _PROBE, _FRAME, _LUMA)
    terminal = _single(rows, _PROBE, optional=True)
    if terminal is None:
        return None
    result = _choice(terminal, "result", {"passed", "failed"})
    frames = _uint(terminal, "frames")
    error = _error(terminal)
    if (result == "passed") != (error == "NONE"):
        raise ValueError(f"{_PROBE}: result and error disagree")
    luma = _single(rows, _LUMA, optional=True)
    return {
        "result": result,
        "frames": frames,
        "error": error,
        "first_frame": _first_probe_frame(rows),
        "first_luma": _luma(luma, "state", "y_mean", "samples") if luma is not None else None,
    }
