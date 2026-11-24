"""One explicitly authorized AI audio round on an already bound SDK session."""

import time

import sdk_bench
from console_sdk import ConsoleError, fields


START, STOP = "AT+AI START", "AT+AI STOP"
STATUS, LINK = "AT+STATUS ai_talk", "AT+LINK"
PLATFORM = "AT+PLATFORM"
MEDIA = "AT+STATUS media"
ACTIVE = {"listening", "thinking", "speaking"}
WAIT_READY, WAIT_ACTIVE, WAIT_MEDIA, WAIT_IDLE = 5, 15, 12, 12
UINT32_MAX = (1 << 32) - 1


class FailedCheck(ConsoleError):
    """The board answered, but the observed AI round did not meet the gate."""


def uint32(row, key):
    value = sdk_bench.unsigned(row, key)
    if value > UINT32_MAX:
        raise ConsoleError(f"invalid AI round {key} counter")
    return value


def snapshot(session):
    lines = session.send(STATUS)
    row = fields(lines, "+AI_TALK: ")
    stats = fields(lines, "+AI-STATS: ")
    phase, origin = row.get("phase"), row.get("origin")
    gen, link = uint32(row, "gen"), uint32(row, "link")
    if (phase not in ACTIVE | {"idle", "connecting", "ending"} or
            origin not in {"user", "wake", "other"} or
            (phase == "idle") != (gen == 0) or (phase == "idle" and link)):
        raise ConsoleError("invalid AI generation or phase")
    return phase, origin, gen, link, uint32(stats, "started"), row.get("end")


def bounded(read, accept, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = read()
        if accept(value):
            return value
        time.sleep(min(0.2, max(0, deadline - time.monotonic())))
    return None


def owned(row, started, gen):
    if row[4] != started or row[2] != gen or row[1] != "user":
        raise ConsoleError("owned AI generation could not be reverified")
    return row


def audio(session, link):
    rows = sdk_bench.records(session.send(LINK), "+LINK-SLOT: ")
    matches = [row for row in rows if uint32(row, "gen") == link]
    if len(matches) > 1:
        raise ConsoleError("AI link generation matched more than one slot")
    if not matches:
        return None
    return {key: uint32(matches[0], key) for key in ("audio_tx", "audio_rx")}


def media_snapshot(session):
    row = sdk_bench.one(session, MEDIA, "+MEDIA: ")
    flags = ("ready", "active", "draining", "uplink_open")
    values = {key: uint32(row, key) for key in (*flags, "generation", "playing", "recording")}
    if any(values[key] > 1 for key in flags):
        raise ConsoleError("invalid AI media readiness flag")
    return values


def media_quiet(row):
    return row["ready"] == 1 and not any(value for key, value in row.items() if key != "ready")


def run_one(port, session, witness, confirm):
    """Use the caller's same-boot witness; do not open ports or settle the heap."""
    del port  # The witness owns the receive boundary and boot observations.
    facts = {"start_attempted": False, "start_acked": False,
             "stop_attempted": False, "stop_acked": False, "idle_verified": False,
             "ownership_lost": False}
    result = {"status": "not_verified", "detail": "AI round not completed", "facts": facts}
    gen, own_start, link = None, None, None

    def read_owned():
        try:
            current = snapshot(session)
            if current[4] == own_start and current[0] == "idle":
                return current
            owned(current, own_start, gen)
            if link is not None and current[3] != link:
                raise ConsoleError("owned AI audio link changed while being measured")
            return current
        except (ConsoleError, OSError):
            # A later matching reply does not repair a gap in ownership.
            facts["ownership_lost"] = True
            raise

    def early_end(row):
        try:
            witness.checkpoint(idle=True)
            final = snapshot(session)
            if final[0] != "idle" or final[4:] != row[4:]:
                raise ConsoleError("AI terminal evidence changed")
        except (ConsoleError, OSError):
            facts["ownership_lost"] = True
            raise
        facts.update(idle_verified=True, terminal_end=final[5] or "unknown")
        raise FailedCheck("AI ended before an owned STOP was sent")

    def read_live():
        row = read_owned()
        if row[0] == "idle":
            early_end(row)
        return row

    def read_media():
        row = media_snapshot(session)
        facts["media_before_start"] = row
        return row

    try:
        if any(session.check(command).access not in {"action", "guarded"} for command in (START, STOP)):
            raise ConsoleError("AI round requires explicit START and STOP permissions")
        if any(session.check(command).access != "read"
               for command in (STATUS, LINK, PLATFORM, MEDIA, "AT+VERSION", "AT+STATE")):
            raise ConsoleError("AI round requires read-only state and audio evidence")
        witness.checkpoint(idle=True)
        initial = snapshot(session)
        if initial[0] != "idle":
            raise ConsoleError("AI must be idle before this round")
        # The application can be idle while a local end cue still owns the audio parts.
        if not media_quiet(read_media()):
            if bounded(read_media, media_quiet, WAIT_READY) is None:
                raise ConsoleError("audio is not ready and idle; no AI START sent")
            witness.checkpoint(idle=True)
            if snapshot(session) != initial:
                raise ConsoleError("AI state changed while waiting for idle audio")
        if (sdk_bench.one(session, PLATFORM, "+PLATFORM: ").get("online") != "1" or
                sdk_bench.one(session, LINK, "+LINK: ").get("started") != "1"):
            raise ConsoleError("AI round requires the platform and RTC engine online")
        own_start = (initial[4] + 1) & UINT32_MAX
        confirm()
        facts["start_attempted"] = True
        session.send(START)
        facts["start_acked"] = True

        first = snapshot(session)
        if first[4] != own_start or first[1] != "user":
            raise ConsoleError("cannot attribute an AI generation to this START")
        if first[0] == "idle":
            early_end(first)
        gen = first[2]
        facts["generation"] = gen
        active = first if first[0] in ACTIVE and first[3] else bounded(
            read_live, lambda row: row[0] in ACTIVE and row[3] != 0, WAIT_ACTIVE)
        if active is None:
            raise FailedCheck("owned AI round did not reach an active audio link")
        link = active[3]
        facts["link_generation"] = link

        def sample():
            current = read_live()
            if current[0] not in ACTIVE:
                raise ConsoleError("owned AI audio link is no longer active")
            counts = audio(session, link)
            if counts is not None:
                previous = facts.get("audio", {})
                if any(counts[key] < previous[key] for key in previous):
                    raise ConsoleError("AI audio counters regressed")
                facts["audio"] = counts
            return counts

        observed = bounded(sample, lambda row: row and row["audio_tx"] > 0 and row["audio_rx"] > 0, WAIT_MEDIA)
        if observed is None:
            if "audio" not in facts:
                raise ConsoleError("no counters were captured for the owned AI link")
            raise FailedCheck("owned AI link did not show bidirectional audio")
        result.update(status="pass", detail="Owned AI link carried audio in both directions")
    except FailedCheck as error:
        result.update(status="fail", detail=str(error))
    except ConsoleError as error:
        result.update(status="not_verified", detail=str(error))
    except OSError:
        result.update(status="not_verified", detail="AI round serial or USB transport failed")
    finally:
        if gen is not None and facts["ownership_lost"]:
            facts["cleanup_unverified"] = True
        elif gen is not None and not facts["idle_verified"]:
            try:
                witness.checkpoint()
                confirm()
                read_live()
                facts["stop_attempted"] = True
                session.send(STOP)
                facts["stop_acked"] = True

                def stopped():
                    row = snapshot(session)
                    if row[4] != own_start or row[2] not in {0, gen}:
                        raise ConsoleError("another AI generation appeared after STOP")
                    return row[0] == "idle" and row[5] == "local"

                if not bounded(stopped, bool, WAIT_IDLE):
                    raise ConsoleError("AI did not report its local stop and return to idle")
                witness.checkpoint(idle=True)
                if not stopped():
                    raise ConsoleError("AI did not remain idle after STOP")
                facts["idle_verified"] = True
            except FailedCheck as error:
                result.update(status="fail", detail=str(error))
            except (ConsoleError, OSError):
                facts["cleanup_unverified"] = True
                if result["status"] != "fail":
                    result.update(status="not_verified", detail="Could not verify an owned AI STOP and idle state")
        if result["status"] == "pass":
            result["detail"] += "; STOP acknowledged and application idle (not a listening test)"
    return result
