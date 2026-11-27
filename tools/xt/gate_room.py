"""Exercise one existing room without changing its membership."""

import re

from boot_check import platform
from console_sdk import ConsoleError, fields
from sdk_bench import one, poll, records, state, unsigned


OPEN, PRESS, RELEASE, CLOSE = ("AT+ROOM " + command for command in ("OPEN", "PTT 1", "PTT 0", "CLOSE"))
READS = ("AT+VERSION", "AT+STATE", "AT+STATUS room", "AT+PLATFORM", "AT+LINK")
ACTIONS = (OPEN, PRESS, RELEASE, CLOSE)
WAITING, LISTENING, SPEAKING, BLOCKED = 3, 5, 6, 8


def result(status, detail, **facts):
    return {"status": status, "detail": detail, "facts": facts}


def _flag(row, key):
    value = row.get(key)
    if value not in ("0", "1"):
        raise ConsoleError(f"room status has no valid {key} flag")
    return value == "1"


def _room(session):
    lines = session.send("AT+STATUS room")
    row = fields(lines, "+ROOM: ")
    info = fields(lines, "+ROOM-SESSION: ")
    phase = unsigned(row, "state")
    if phase > BLOCKED or not re.fullmatch(r"-?[0-9]{1,10}", row.get("error", "")):
        raise ConsoleError("unexpected room phase or error")
    code = row.get("code", "")
    if not re.fullmatch(r"-|[0-9]{6}", code):
        raise ConsoleError("invalid room assignment status")
    flags = {key: _flag(row, key) for key in ("known", "assigned", "open", "busy", "transmitting")}
    if flags["known"] and flags["assigned"] != (code != "-"):
        raise ConsoleError("inconsistent room assignment status")
    return dict(flags, phase=phase, code=code, gen=unsigned(row, "gen"), revision=unsigned(row, "revision"),
                error=int(row["error"]), settled=_flag(info, "settled"))


def _activity(session):
    row = state(session)
    active = row.get("activity", "")
    ident = unsigned(row, "id")
    if row.get("started") != "1" or not active or ((ident == 0) != (active == "none")):
        raise ConsoleError("framework activity is not measurable")
    return active, ident


def _closed(room, activity):
    return (not any(room[key] for key in ("open", "busy", "transmitting", "gen")) and room["settled"]
            and activity == ("none", 0))


class _Room:
    def __init__(self, session, witness, confirm, original):
        self.session, self.witness, self.confirm = session, witness, confirm
        self.code, self.revision = original["code"], original["revision"]
        self.generation = self.activity_id = 0
        self.open_issued = self.ownership_lost = False

    def lose(self, reason):
        self.ownership_lost = True
        raise ConsoleError(reason)

    def observe(self):
        room, activity = _room(self.session), _activity(self.session)
        if room["revision"] < self.revision or room["busy"]:
            self.lose("room status moved backwards or a membership operation intervened")
        self.revision = room["revision"]
        if room["code"] not in (self.code, "-"):
            self.lose("room assignment changed; no cleanup command was sent to that room")
        if activity[0] not in ("none", "room") or (self.activity_id and activity[1] not in (0, self.activity_id)):
            self.lose("another framework activity intervened")
        if room["gen"] and (not room["open"] or activity[0] != "room"):
            self.lose("room link and framework activity do not agree")
        if self.generation and room["gen"] not in (0, self.generation):
            self.lose("room link generation changed; no command sent to the new link")
        if activity[0] == "room":
            self.activity_id = activity[1]
        if room["gen"]:
            self.generation = room["gen"]
        return room, activity

    def send(self, command):
        self.confirm()  # No serial operation between the USB check and the action.
        if command == OPEN:
            self.open_issued = True
        self.session.send(command)

    def phase(self, phase):
        def read():
            room, activity = self.observe()
            if room["phase"] == BLOCKED:
                raise ConsoleError("room connection attempts were blocked")
            return (room["phase"] == phase and room["open"] and room["known"] and room["assigned"]
                    and room["code"] == self.code and room["error"] == 0 and room["gen"] != 0
                    and room["transmitting"] == (phase == SPEAKING) and activity[0] == "room")

        poll(read, bool, 20 if not self.generation else 2)

    def uplink(self):
        lines = self.session.send("AT+LINK")
        root = fields(lines, "+LINK: ")
        slots = [row for row in records(lines, "+LINK-SLOT: ") if unsigned(row, "gen") == self.generation]
        if root.get("started") != "1" or root.get("connected") != "1" or len(slots) != 1 or slots[0].get("state") != "2":
            raise ConsoleError("could not measure the test room's connected link slot")
        return unsigned(slots[0], "audio_tx")

    def close(self):
        if self.ownership_lost:
            raise ConsoleError("cannot clean up after room ownership was lost")
        self.witness.checkpoint()
        room, activity = self.observe()
        if _closed(room, activity):
            return room
        if not room["open"]:
            raise ConsoleError("room attendance ownership could not be verified for cleanup")
        if room["transmitting"]:
            if room["gen"] != self.generation or not room["gen"]:
                raise ConsoleError("cannot release a room transmission without its matched link")
            self.send(RELEASE)
            self.phase(LISTENING)
            self.witness.checkpoint()
            room, activity = self.observe()
            if _closed(room, activity):
                return room
            if not room["open"] or room["transmitting"]:
                raise ConsoleError("room changed while checking its release")
        self.send(CLOSE)
        room, _ = poll(self.observe, lambda pair: _closed(*pair), 12)
        self.witness.checkpoint(idle=True)
        return room


def run_one(port, session, witness, confirm):
    """Open an assigned room, measure its own audio uplink, and leave attendance only."""
    del port  # The caller's witness owns serial history and reset checks.
    for commands, access in ((READS, "read"), (ACTIONS, "action")):
        for command in commands:
            if not session.has(command):
                return result("not_verified", "room status or one of its scoped controls is unavailable")
            if session.check(command).access != access:
                raise ConsoleError("room command access does not match the test contract")
    baseline, activity = _room(session), _activity(session)
    if not _closed(baseline, activity):
        return result("not_verified", "room or another activity is already in use")
    if not baseline["known"] or not baseline["assigned"]:
        return result("not_verified", "pre-existing room membership is not confirmed")
    if baseline["phase"] != WAITING or baseline["error"] or not platform(session):
        return result("not_verified", "assigned room or platform is not ready at baseline")
    link = one(session, "AT+LINK", "+LINK: ")
    if (link.get("started"), link.get("connected")) != ("1", "0"):
        return result("not_verified", "transport is unavailable or another link is already open")

    flow = _Room(session, witness, confirm, baseline)
    try:
        witness.checkpoint(idle=True)
        current, activity = flow.observe()
        if current != baseline or not _closed(current, activity):
            return result("not_verified", "room baseline changed before any command was sent")
        flow.send(OPEN)
        flow.phase(LISTENING)
        before = flow.uplink()
        witness.checkpoint()
        flow.phase(LISTENING)
        flow.send(PRESS)
        flow.phase(SPEAKING)
        # PTT can expire automatically on touch products; do not renew it.
        witness.checkpoint()
        current, _ = flow.observe()
        if not current["open"] or current["gen"] != flow.generation or not flow.generation:
            raise ConsoleError("room link changed before push-to-talk release")
        flow.send(RELEASE)
        flow.phase(LISTENING)
        after = flow.uplink()
        flow.phase(LISTENING)
    except ConsoleError as error:
        if flow.open_issued:
            try:
                flow.close()
            except ConsoleError:
                raise ConsoleError(f"{error}; room cleanup could not be verified") from error
        raise

    final = flow.close()
    if not final["known"] or not final["assigned"] or final["code"] != baseline["code"]:
        return result("fail", "original room assignment could not be verified after closing")
    if after < before:
        raise ConsoleError("room audio counter moved backwards in the same link generation")
    if after == before:
        return result("fail", "room link registered no audio uplink", audio_tx_delta=0, closed=True)
    return result("pass", "board joined, transmitted, released PTT and closed attendance; remote listening not verified",
                  audio_tx_delta=after - before, closed=True, membership_unchanged=True)
