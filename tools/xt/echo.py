"""Single-talk echo measurement on one SDK device (xt.py echo).

The device's own speaker is the only source: a looped feedback effect (by
default the incoming-call ring) plays while nobody talks nearby, and
+MEDIA-LEVELS from AT+STATUS media is read about once a second. For each
volume the report compares the quiet floor with the ring:

  echo_reduction_db = (mic ring over quiet) - (processed ring over quiet)

which does not depend on any fixed gain inside the processor. Every action
(volume, effect on and off, the final volume restore) must be allowed
explicitly; all of them are checked before the first one is sent.
"""
import math
import re
import statistics
import time

from console_sdk import ConsoleError, fields

LEVEL_KEYS = ("mic_rms", "mic_peak", "ref_rms", "ref_peak", "monitor_rms", "monitor_peak",
              "processed_rms", "processed_peak", "clean_rms", "clean_peak")
VOLUME = re.compile(r"\+SET: name=audio\.volume kind=int value=(\d+)")


def levels(reply):
    for line in reply:
        if line.startswith("+MEDIA-LEVELS: "):
            row = {k: int(v) for k, v in fields([line], "+MEDIA-LEVELS: ").items() if re.fullmatch(r"-?\d+", v)}
            if "window" in row and all(k in row for k in LEVEL_KEYS):
                return row
    return None


def current_volume(session):
    for line in session.send("AT+SET"):
        match = VOLUME.match(line)
        if match:
            return int(match[1])
    raise ConsoleError("audio.volume is not readable on this firmware")


def plan(session, volumes, effect):
    """Return (original volume, ordered volumes); refuse before any action if a step is not allowed."""
    if levels(session.send("AT+STATUS media")) is None:
        raise ConsoleError("this firmware does not report +MEDIA-LEVELS")
    original = current_volume(session)
    order = volumes or [original]
    needed = [f"AT+FEEDBACK SET {effect} 1", f"AT+FEEDBACK SET {effect} 0"]
    if volumes:
        needed += [f"AT+SET audio.volume {v}" for v in dict.fromkeys([*volumes, original])]
    for command in needed:
        session.check(command)
    return original, order


def read_phase(session, volume, phase, seconds, sleep, clock):
    rows, last, end = [], None, clock() + seconds
    while clock() < end:
        row = levels(session.send("AT+STATUS media"))
        if row and row != last:
            rows.append({"volume": volume, "phase": phase, **row})
            last = row
        sleep(1.05)
    return rows


def measure(session, volumes=None, effect="companion.incoming", quiet_s=3.0, ring_s=9.0,
            sleep=time.sleep, clock=time.monotonic):
    original, order = plan(session, volumes, effect)
    samples, restored = [], None
    try:
        for volume in order:
            if volumes:
                session.send(f"AT+SET audio.volume {volume}")
            sleep(1.2)
            samples += read_phase(session, volume, "quiet", quiet_s, sleep, clock)
            session.send(f"AT+FEEDBACK SET {effect} 1")
            sleep(1.2)
            samples += read_phase(session, volume, "ring", ring_s, sleep, clock)
            session.send(f"AT+FEEDBACK SET {effect} 0")
            sleep(1.5)
    finally:
        if session.usable:
            session.send(f"AT+FEEDBACK SET {effect} 0")
            if volumes:
                session.send(f"AT+SET audio.volume {original}")
            restored = original
    return {"effect": effect, "volume_before": original, "volume_restored": restored,
            "samples": samples, "summary": summarize(samples)}


def db(a, b):
    return round(20 * math.log10(a / b), 1) if a > 0 and b > 0 else None


def summarize(samples):
    result = []
    for volume in dict.fromkeys(s["volume"] for s in samples):
        quiet = [s for s in samples if s["volume"] == volume and s["phase"] == "quiet"]
        ring = [s for s in samples if s["volume"] == volume and s["phase"] == "ring"]
        floor = statistics.median([s["ref_rms"] for s in quiet]) if quiet else 0
        ring = [s for s in ring if s["ref_rms"] >= 4 * max(1, floor)]  # windows that carried the effect
        if not quiet or not ring:
            result.append({"volume": volume, "error": "no quiet or no ring windows"})
            continue
        q = {k: statistics.median(s[k] for s in quiet) for k in ("mic_rms", "processed_rms")}
        r = {k: statistics.median(s[k] for s in ring) for k in ("mic_rms", "ref_rms", "processed_rms", "clean_rms")}
        mic_lift, processed_lift = db(r["mic_rms"], q["mic_rms"]), db(r["processed_rms"], q["processed_rms"])
        result.append({
            "volume": volume, "windows": len(ring), "window_means": all(s["window"] == 1 for s in quiet + ring),
            "ring": r, "quiet": q, "mic_peak_max": max(s["mic_peak"] for s in ring),
            "clipped": max(s["mic_peak"] for s in ring) >= 32767,
            "mic_over_ref_db": db(r["mic_rms"], r["ref_rms"]),
            "echo_reduction_db": (round(mic_lift - processed_lift, 1)
                                  if mic_lift is not None and processed_lift is not None else None),
            "residual_over_floor_db": processed_lift,
        })
    return result


def table(summary):
    lines = [f"{'volume':>6} {'mic':>6} {'peak':>6} {'ref':>6} {'processed':>9} {'reduction_dB':>12} "
             f"{'residual_dB':>11} {'mic/ref_dB':>10} clip"]
    for row in summary:
        if "error" in row:
            lines.append(f"{row['volume']:>6} {row['error']}")
            continue
        lines.append(f"{row['volume']:>6} {row['ring']['mic_rms']:>6.0f} {row['mic_peak_max']:>6} "
                     f"{row['ring']['ref_rms']:>6.0f} {row['ring']['processed_rms']:>9.0f} "
                     f"{row['echo_reduction_db']!s:>12} {row['residual_over_floor_db']!s:>11} "
                     f"{row['mic_over_ref_db']!s:>10} {'YES' if row['clipped'] else ''}")
    return "\n".join(lines)
