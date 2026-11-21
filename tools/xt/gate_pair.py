"""A bounded two-board SDK call gate. Neither opens ports nor changes setup."""
import re
import time

from console_sdk import ConsoleError, fields
from sdk_bench import counter_delta, one, poll, records, state, unsigned


ACCEPT, REJECT, HANGUP = ("AT+CALL " + verb for verb in ("ACCEPT", "REJECT", "HANGUP"))
TERMINAL = {"idle", "ended", "error"}
RINGING = {"outgoing", "connecting"}
LIVE = RINGING | {"incoming", "active"}
CALL_TIMEOUT = 15
IDLE_TIMEOUT = 10
MEDIA_SECONDS = 2


class PairError(ConsoleError):
    """An error safe to include in the gate summary (no raw AT or IDs)."""


def _confirm(endpoint):
    if endpoint.confirm() is False:
        raise PairError("USB endpoint confirmation failed")


def _fence(endpoint, *, idle=False):
    _confirm(endpoint)
    endpoint.witness.checkpoint(idle=idle)


def _identity(endpoint):
    device = one(endpoint.session, "AT+BIND", "+BIND: ").get("device", "")
    if device == "-" or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", device):
        raise PairError("endpoint has no usable bound device identity")
    return device


def _calls(lines):
    rows = records(lines, "+CALL: ")
    device = [row for row in rows if row.get("kind") == "device"]
    if len(device) != 1:
        raise PairError("device call status is missing or ambiguous")
    if any(row.get("state") not in TERMINAL or unsigned(row, "gen") for row in rows if row is not device[0]):
        raise PairError("another call provider has a session")
    row = device[0]
    gen = unsigned(row, "gen")
    status = row.get("state")
    if not (status in TERMINAL and gen == 0 or status in LIVE and 0 < gen <= 0xffffffff):
        raise PairError("device call status and generation disagree")
    return status, gen, row.get("peer")


def _call(endpoint):
    return _calls(endpoint.session.send("AT+CALL"))


def _match(row, peer, generation=None, seen=(), retired=None):
    status, current, actual_peer = row
    if not current:
        if generation is not None and retired is not None:
            retired[0] = True
        return None
    if (actual_peer != peer or current in seen or generation is not None and current != generation
            or retired is not None and retired[0]):
        if retired is not None:
            retired[0] = True
        raise PairError("observed a different peer or call generation")
    return status, current


def _wait_call(endpoint, peer, phases, generation=None, seen=(), retired=None):
    return poll(lambda: _match(_call(endpoint), peer, generation, seen, retired),
                lambda current: current is not None and current[0] in phases, CALL_TIMEOUT)[1]


def _require(endpoint, peer, generation, phases, retired):
    _fence(endpoint)
    row = _match(_call(endpoint), peer, generation, retired=retired)
    if row is None or row[0] not in phases:
        raise PairError("expected call state no longer owns the session")
    return row[0]


def _playback(endpoint, link=None):
    try:
        lines = endpoint.session.send("AT+STATUS media")
        owner = fields(lines, "+MEDIA: ")
        row = fields(lines, "+MEDIA-RX: ")
        counts = {key: unsigned(row, field) for key, field in (
            ("media_received", "received"), ("media_decoded_packets", "played"),
            ("pcm_written", "pcm_written"), ("write_failed", "write_failed"),
        )}
        generation = unsigned(owner, "generation")
    except ConsoleError:
        raise PairError("media playback counters are missing or invalid") from None
    if link is not None and (generation != link or owner.get("active") != "1"):
        raise PairError("active media generation does not match the RTC slot")
    return counts


def _media(endpoint, peer, generation, retired):
    _fence(endpoint)
    lines = endpoint.session.send("AT+STATUS call")
    row = _match(_calls(lines), peer, generation, retired=retired)
    if row is None or row[0] != "active":
        raise PairError("call did not stay active while sampling audio")
    link = unsigned(fields(lines, "+CALL-DIAG: "), "link")
    slots = [slot for slot in records(endpoint.session.send("AT+LINK"), "+LINK-SLOT: ")
             if unsigned(slot, "gen") == link]
    if not link or len(slots) != 1 or slots[0].get("state") != "2":
        raise PairError("active call did not identify exactly one connected RTC slot")
    playback = _playback(endpoint, link)
    row = _match(_call(endpoint), peer, generation, retired=retired)
    if row is None or row[0] != "active":
        raise PairError("call changed while sampling audio")
    return link, {**{name: unsigned(slots[0], name) for name in ("audio_tx", "audio_rx")}, **playback}


def _wait_idle(endpoint, peer, generation, retired):
    def ready():
        row = _match(_call(endpoint), peer, generation, retired=retired)
        if row is not None:
            if generation is None:
                raise PairError("a new call appeared while waiting for idle")
            return False
        retired[0] = True  # Never act on this generation again after observing its end.
        row = state(endpoint.session)
        return (row.get("activity"), row.get("id")) == ("none", "0")

    poll(ready, bool, IDLE_TIMEOUT)
    _fence(endpoint, idle=True)
    if _call(endpoint)[1]:
        raise PairError("a call reappeared after idle")


def _recover(endpoint, peer, generation, retired):
    """At most one local cleanup, only for a proven, still-owned session."""
    try:
        _fence(endpoint)
        row = _match(_call(endpoint), peer, generation, retired=retired)
        if row is not None:
            if generation is None:
                return False
            _confirm(endpoint)
            endpoint.session.send(REJECT if row[0] == "incoming" else HANGUP)
        _wait_idle(endpoint, peer, generation, retired)
        return True
    except (ConsoleError, OSError):
        return False


def _round(caller, callee, ids, seen, progress):
    peers = ids[::-1]
    endpoints = caller, callee
    for endpoint, expected in zip(endpoints, ids):
        _fence(endpoint, idle=True)
        if _call(endpoint)[1] or _identity(endpoint) != expected:
            raise PairError("endpoint binding or idle state changed before dialing")
    _confirm(caller)
    progress["attempted"] = True
    generations = progress["generations"]
    retired = progress["retired"]
    caller.session.send(f"AT+CALL DEVICE {peers[0]}")
    generations[0] = _wait_call(caller, peers[0], RINGING, seen=seen[0])
    generations[1] = _wait_call(callee, peers[1], {"incoming"}, seen=seen[1])
    _require(caller, peers[0], generations[0], RINGING, retired[0])
    _require(callee, peers[1], generations[1], {"incoming"}, retired[1])
    _confirm(callee)
    callee.session.send(ACCEPT)
    for endpoint, peer, gen, ended in zip(endpoints, peers, generations, retired):
        _wait_call(endpoint, peer, {"active"}, gen, retired=ended)
    samples = list(zip(endpoints, peers, generations, retired))
    before = [_media(*sample) for sample in samples]
    time.sleep(MEDIA_SECONDS)
    after = [_media(*sample) for sample in samples]
    counts = []
    for (old_link, old), (link, new) in zip(before, after):
        if old_link != link:
            raise PairError("active RTC slot changed during audio collection")
        counts.append(counter_delta(new, old))
    if not all(row["audio_tx"] > 0 and row["audio_rx"] > 0 for row in counts):
        raise PairError("audio counters did not advance in both directions")
    if not all(row["media_received"] > 0 and row["media_decoded_packets"] > 0 and row["pcm_written"] > 0
               for row in counts):
        raise PairError("downlink receive, decode, or successful PCM output did not advance on both endpoints")
    _require(callee, peers[1], generations[1], {"active"}, retired[1])
    _require(caller, peers[0], generations[0], {"active"}, retired[0])
    _confirm(caller)
    caller.session.send(HANGUP)
    for endpoint, peer, gen, ended in zip(endpoints, peers, generations, retired):
        _wait_idle(endpoint, peer, gen, ended)
    for history, gen in zip(seen, generations):
        history.add(gen)
    return {"caller_generation": generations[0], "callee_generation": generations[1],
            "caller_audio": counts[0], "callee_audio": counts[1], "both_idle": True}


def run_pair(left, right):
    """Check left->right then right->left; caller owns endpoints and port lifetime."""
    facts = {}
    try:
        if left.port is right.port or left.session is right.session:
            raise PairError("two separately confirmed endpoints are required")
        for endpoint in (left, right):
            _fence(endpoint, idle=True)
            for command in ("AT+BIND", "AT+CALL", "AT+LINK", "AT+STATUS call", ACCEPT, REJECT, HANGUP):
                endpoint.session.check(command)
            _playback(endpoint)
        ids = _identity(left), _identity(right)
        if ids[0] == ids[1]:
            raise PairError("endpoints share a device identity")
        for endpoint, peer in zip((left, right), ids[::-1]):
            endpoint.session.check(f"AT+CALL DEVICE {peer}")
    except (ConsoleError, OSError) as error:
        detail = str(error) if isinstance(error, PairError) else "pair identity, idle state, or grants are unverified"
        return {"status": "not_verified", "detail": detail, "facts": facts}

    seen = set(), set()
    for direction, endpoints, pair_ids, history in (
            ("left_to_right", (left, right), ids, seen),
            ("right_to_left", (right, left), ids[::-1], seen[::-1])):
        progress = {"attempted": False, "generations": [None, None], "retired": [[False], [False]]}
        try:
            facts[direction] = _round(*endpoints, pair_ids, history, progress)
        except (ConsoleError, OSError) as error:
            if progress["attempted"]:
                facts[direction] = {
                    "caller_idle_verified": _recover(endpoints[0], pair_ids[1],
                                                      progress["generations"][0], progress["retired"][0]),
                    "callee_idle_verified": _recover(endpoints[1], pair_ids[0],
                                                      progress["generations"][1], progress["retired"][1]),
                }
            detail = str(error) if isinstance(error, PairError) else "pair call or proof failed"
            status = "fail" if progress["attempted"] or facts else "not_verified"
            return {"status": status, "detail": f"{direction}: {detail}", "facts": facts}
    return {"status": "pass", "detail": "both call directions carried audio through PCM output and returned idle",
            "facts": facts}
