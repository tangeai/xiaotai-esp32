"""Bench workflows over the unified AT command and event vocabulary."""
import json
import re
import time

from console_sdk import ConsoleError, fields, hex_text


def state(session):
    return fields(session.send("AT+STATE"), "+STATE: ")


def one(session, command, prefix):
    return fields(session.send(command), prefix)


def records(lines, prefix):
    return [fields([line], prefix) for line in lines if line.startswith(prefix)]


def unsigned(record, key):
    value = record.get(key, "")
    if not re.fullmatch(r"[0-9]{1,20}", value):
        raise ConsoleError(f"missing or invalid {key} counter")
    return int(value)


def link_media(session):
    row = one(session, "AT+LINK", "+LINK-MEDIA: ")
    return {key: unsigned(row, key) for key in ("tx", "rx", "tx_dropped", "rx_dropped", "stale")}


def counter_delta(after, before):
    if any(after[key] < value for key, value in before.items()):
        raise ConsoleError("counters reset during this run; the samples do not describe one session")
    return {key: after[key] - value for key, value in before.items()}


def owner_stack(lines):
    summary = fields(lines, "+TASKS: ")
    owner = [row for row in records(lines, "+TASK: ") if row.get("name") == "xiaotai"]
    if summary.get("supported") != "1" or len(owner) != 1:
        return None
    return unsigned(owner[0], "stack_min_free")


def downlink_topics(lines):
    for row in records(lines, "+TOPIC: "):
        if row.get("src") != "link" or row.get("name") not in {"downlink.accepted", "downlink.rejected"}:
            continue
        names = ("kind", "gen", "stream", "codec", "rate", "len", "n")
        if row["name"] == "downlink.rejected":
            names += ("why",)
        yield {"name": row["name"], **{key: unsigned(row, key) for key in names}}


def health(session):
    row = {"uptime_s": unsigned(state(session), "uptime_ms") // 1000}
    if session.has("AT+NET"):
        net = one(session, "AT+NET", "+NET: ")
        if re.fullmatch(r"-?[0-9]{1,5}", net.get("rssi", "")):
            row["rssi"] = int(net["rssi"])
    if session.has("AT+PLATFORM"):
        row["online"] = one(session, "AT+PLATFORM", "+PLATFORM: ").get("online") == "1"
    if session.has("AT+LINK"):
        row["rtc"] = any(link.get("connected") == "1" for link in records(session.send("AT+LINK"), "+LINK: "))
    if session.has("AT+MEM"):
        for heap in records(session.send("AT+MEM"), "+MEM: "):
            if heap.get("kind") == "internal":
                row.update(free=unsigned(heap, "free"), largest=unsigned(heap, "largest"),
                           minimum=unsigned(heap, "min_free"))
    if session.has("AT+STATUS tasks"):
        free = owner_stack(session.send("AT+STATUS tasks"))
        row["owner_stack_min_free"] = free
        row["owner_stack_1024"] = free >= 1024 if free is not None else None
    return row


def poll(read, accept, timeout, *, clock=time.monotonic, sleep=time.sleep):
    end = clock() + timeout
    while clock() < end:
        value = read()
        if accept(value):
            return value
        sleep(min(0.15, max(0, end - clock())))
    raise ConsoleError("timed out waiting for the expected state")


def call_state(session, kind="device"):
    rows = [row for row in records(session.send("AT+CALL"), "+CALL: ") if row.get("kind") == kind]
    if len(rows) != 1:
        raise ConsoleError(f"call status for {kind} is unavailable")
    return rows[0]


def call_event(lines, kind, label, generation=None, after=None):
    src = "call" if kind == "device" else "wechat"
    for row in records(lines, "+LOG: "):
        if row.get("src") == src and row.get("label") == label:
            gen = unsigned(row, "gen")
            if gen and (generation is None or gen == generation) and (after is None or unsigned(row, "seq") > after):
                return row
    return None


def wait_call(port, session, label, start, generation=None, kind="device", timeout=15, *, follow=False):
    if follow:
        return poll(lambda: call_event(port.since(start), kind, label, generation), bool, timeout)
    states = {"outgoing": {"outgoing", "connecting"}, "incoming": {"incoming"},
              "connected": {"active"}, "ended": {"idle", "ended", "error"}}
    def same_call(row):
        # A released call keeps its ended state for the record but clears its generation to 0.
        released = label == "ended" and unsigned(row, "gen") == 0
        return (row.get("state") in states[label] and
                (generation is None or row.get("state") == "idle" or released or unsigned(row, "gen") == generation))
    return poll(lambda: call_state(session, kind), same_call, timeout)


def wechat_contact(session):
    matches = [row for row in records(session.send("AT+CONTACTS"), "+CONTACT: ") if row.get("source") == "wechat"]
    if not matches:
        raise ConsoleError("no cached WeChat contact; query the selected contact page first")
    return unsigned(matches[0], "index")


def workflow(session, original):
    """Plan all conditional steps before allowing a change to persist."""
    net = one(session, "AT+NET", "+NET: ")
    connected = net.get("connected")
    if connected not in ("0", "1"):
        raise ConsoleError("network state is missing connected=0/1")
    if original == "AT+RETRY":
        return (["AT+NET RETRY"] if connected == "0" else []) + ["AT+BIND RETRY"], None
    if not original.startswith("wifi "):
        raise ConsoleError("unknown compatibility workflow")
    try:
        credentials = json.loads(original[5:])
        if not isinstance(credentials, dict) or set(credentials) != {"ssid", "password"}:
            raise ValueError
        ssid, password = credentials["ssid"], credentials["password"]
        if not isinstance(ssid, str) or not isinstance(password, str) or not ssid or "\x00" in ssid + password:
            raise ValueError
        args = f"{hex_text(ssid)} {hex_text(password)}"
    except (ValueError, TypeError) as error:
        raise ConsoleError('expected wifi {"ssid":"...","password":"..."}') from error
    if net.get("provisioning") == "1":
        return [f"AT+NET WIFI {args}"], None
    if net.get("provisioning") != "0":
        raise ConsoleError("network state is missing provisioning=0/1")
    if connected == "0":
        return [f"AT+NET STORE {args}", "AT+NET RETRY"], None
    if session.has("AT+RESTART"):
        return [f"AT+NET STORE {args}", "AT+RESTART"], None
    return [f"AT+NET STORE {args}"], "Saved. This product has no AT+RESTART; power cycle it locally."
