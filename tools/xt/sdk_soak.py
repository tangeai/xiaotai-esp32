"""Same-boot and settled heap evidence for the SDK AI soak."""
import time

from console_sdk import ConsoleError, fields, identity
from sdk_bench import records, state, unsigned


SETTLE_SECONDS = 130
ACCEPTANCE_ROUNDS = 10
IDLE_FREE_MAX_LOSS_BYTES = 256
HEAPS = ("internal", "bulk", "dma")
COUNTERS = ("total", "free", "largest", "min_free", "min_largest", "samples")


class Evidence:
    def __init__(self, session, port, mark, boot_banner):
        self.session = session
        self.port = port
        self.mark = mark
        self.boot_banner = boot_banner
        self.previous = None

    def checkpoint(self, idle=False):
        try:
            build = identity(self.session.send("AT+VERSION"))
            if build != self.session.device:
                raise ConsoleError("SDK soak observed a different running identity")
            start = time.monotonic()
            current = state(self.session)
            end = time.monotonic()
            uptime = unsigned(current, "uptime_ms")
            if current.get("started") != "1":
                raise ConsoleError("SDK soak observed an application that was not started")
            mark = self.port.mark()
            if any(self.boot_banner.search(line) for line in self.port.since(self.mark)[:mark - self.mark]):
                raise ConsoleError("SDK soak observed a boot marker")
            if self.previous:
                previous_start, previous_end, previous_uptime = self.previous
                # If the board reset after the previous request, uptime cannot
                # exceed the entire elapsed host interval (with clock margin).
                interval = end - previous_start
                if interval < 0 or uptime < previous_uptime or uptime <= interval * 1010 + 1000:
                    raise ConsoleError("SDK soak cannot establish continuous device uptime")
                advance = uptime - previous_uptime
                margin = interval * 10 + 100
                if not (start - previous_end) * 1000 - margin <= advance <= interval * 1000 + margin:
                    raise ConsoleError("SDK soak device uptime does not track the host clock")
            self.previous = (start, end, uptime)
            self.mark = mark
        except ConsoleError:
            self.session.usable = False  # Never send cleanup on an unverified identity or boot.
            raise
        if idle and (current.get("activity"), current.get("id")) != ("none", "0"):
            raise ConsoleError("SDK soak requires an idle device")
        return {"elf_sha256": build["elf_sha256"], "uptime_ms": uptime}

    def settled_memory(self):
        quiet = self.checkpoint(idle=True)
        start = time.monotonic()
        time.sleep(SETTLE_SECONDS)
        waited = time.monotonic() - start
        if waited < SETTLE_SECONDS:
            raise ConsoleError("SDK soak did not complete the required idle window")
        before = self.checkpoint(idle=True)
        reply = self.session.send("AT+MEM")
        after = self.checkpoint(idle=True)
        external = fields(reply, "+MEM-INFO: ").get("external")
        rows = records(reply, "+MEM: ")
        if external not in {"0", "1"} or sorted(row.get("kind", "") for row in rows) != sorted(HEAPS):
            raise ConsoleError("SDK soak requires one AT+MEM row per heap and the external-memory flag")
        heaps = {row["kind"]: {key: unsigned(row, key) for key in COUNTERS} for row in rows}
        if any(not 0 <= row["largest"] <= row["free"] <= row["total"] for row in heaps.values()):
            raise ConsoleError("SDK soak received inconsistent AT+MEM geometry")
        return {"idle_seconds": round(waited, 3), "idle_started": quiet, "before": before, "after": after,
                "external": external == "1", "heaps": heaps}


def difference(first, last):
    if (first["external"] != last["external"] or
            any(first["heaps"][kind]["total"] != last["heaps"][kind]["total"] for kind in HEAPS)):
        raise ConsoleError("SDK soak heap capabilities or total changed between snapshots")
    return {key: {kind: last["heaps"][kind][key] - first["heaps"][kind][key] for kind in HEAPS}
            for key in ("free", "largest")}


def idle_free_gate(rounds, internal_delta):
    if rounds != ACCEPTANCE_ROUNDS:
        return {"status": "measured_only"}
    return {"status": "pass" if internal_delta >= -IDLE_FREE_MAX_LOSS_BYTES else "fail",
            "max_loss_bytes": IDLE_FREE_MAX_LOSS_BYTES}
