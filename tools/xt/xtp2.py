# SPDX-License-Identifier: MIT
"""Bounded, fixed-peer UDP timing probe for an independently armed lab receiver."""

import argparse
from collections import deque
from dataclasses import asdict, dataclass
import ipaddress
import json
import re
import socket
import struct
import time

HEADER = struct.Struct("!4sIII")
MAGIC = b"XTP2"
U32 = (1 << 32) - 1
FRAME_US = 20000
LIMIT_US = 12_000_000
LATE_US = 5000
BURST_LIMIT_US = 15000
WINDOW = 64
MAX_DATAGRAM = HEADER.size + 512
IPV4_UDP_BUFFER = 65535
MAX_OBSERVATIONS = 4000
BURSTS = {"paced": 1, "burst5": 5, "burst10": 10, "burst20": 20, "burst40": 40, "burst50": 50, "jitter": 1}
LAB_NETWORKS = tuple(ipaddress.IPv4Network(net) for net in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"))


def _bounded(name, value, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f"{name} must be an integer in [{low}, {high}]")
    return value


def _step(state):
    state ^= (state << 13) & U32
    state ^= state >> 17
    return state ^ ((state << 5) & U32)


def payload(seed, seq, size):
    """Byte-for-byte protocol pattern, independent of the schedule."""
    _bounded("seed", seed, 0, U32)
    _bounded("sequence", seq, 0, U32)
    _bounded("payload size", size, 1, 512)
    state = seed ^ ((seq * 0x9E3779B9) & U32) ^ 0xA341316C
    state = state or 0x6D2B79F5
    data = bytearray(size)
    for i in range(size):
        state = _step(state)
        data[i] = state & 0xFF
    return bytes(data)


@dataclass(frozen=True)
class ProbePlan:
    flow: int
    seed: int = 0
    count: int = 100
    payload_size: int = 160
    pattern: str = "paced"

    def __post_init__(self):
        _bounded("flow", self.flow, 1, U32)
        _bounded("seed", self.seed, 0, U32)
        _bounded("count", self.count, 1, 500)
        _bounded("payload size", self.payload_size, 1, 512)
        if not isinstance(self.pattern, str) or self.pattern not in BURSTS:
            raise ValueError("unknown pace pattern")
        if self.count < BURSTS[self.pattern]:
            raise ValueError("count is less than the initial burst")

    def offsets_us(self):
        if self.pattern != "jitter":
            burst = BURSTS[self.pattern]
            return tuple(max(0, i - burst + 1) * FRAME_US for i in range(self.count))
        state = (self.seed ^ 0xD1B54A35) or 0x6D2B79F5
        offsets = [0]
        for i in range(1, self.count):
            state = _step(state)
            offsets.append(i * FRAME_US + state % 10001 - 5000)
        return tuple(offsets)


def encode_datagram(plan, seq, sent_us):
    _bounded("sequence", seq, 0, plan.count - 1)
    _bounded("sender timestamp", sent_us, 0, LIMIT_US)
    return HEADER.pack(MAGIC, plan.flow, seq, sent_us) + payload(plan.seed, seq, plan.payload_size)


def decode_header(data):
    if len(data) < HEADER.size:
        raise ValueError("short XTP2 header")
    magic, flow, seq, sent_us = HEADER.unpack_from(data)
    if magic != MAGIC:
        raise ValueError("invalid XTP2 magic")
    return flow, seq, sent_us


def send_plan(plan, send, *, clock_ns=time.monotonic_ns, sleep=time.sleep):
    """Send using an injected datagram transport. Never catch up lost time in a new burst."""
    offsets = plan.offsets_us()
    bodies = [payload(plan.seed, seq, plan.payload_size) for seq in range(plan.count)]
    start = clock_ns()
    sent = max_late = last_sent = first_slot_span = 0
    first_stamp = None
    status = "complete"
    prior_slot = None
    for seq, (offset, body) in enumerate(zip(offsets, bodies)):
        due_ns = start + offset * 1000
        now = clock_ns()
        while now < due_ns:
            sleep((due_ns - now) / 1_000_000_000)
            now = clock_ns()
        stamp = (now - start) // 1000
        if prior_slot != offset:
            max_late = max(max_late, stamp - offset)
            if stamp - offset > LATE_US:
                status = "schedule_late"
                break
        elif stamp - offset > BURST_LIMIT_US:
            status = "schedule_late"
            break
        packet = HEADER.pack(MAGIC, plan.flow, seq, stamp) + body
        try:
            if send(packet) != len(packet):
                status = "io_error"
                break
        except OSError:
            status = "io_error"
            break
        sent += 1
        prior_slot = offset
        last_sent = stamp
        if first_stamp is None:
            first_stamp = stamp
        if offset == 0:
            first_slot_span = stamp - first_stamp
    return {"sent": sent, "completed": sent == plan.count, "status": status,
            "max_late_us": max_late, "last_sent_us": last_sent if sent else None,
            "first_slot_span_us": first_slot_span, "stop_elapsed_us": stamp if status != "complete" else None}


class Receiver:
    """Device-compatible counters with fixed bounds and a 64-sequence retention window."""

    def __init__(self, plan):
        self.plan = plan
        self._base = 0
        self._high = -1
        self._seen = bytearray(plan.count)
        self._last_arrival = None
        self._times = deque(maxlen=WINDOW)
        self._run = 0
        self._summary = None
        self.stats = dict.fromkeys(("rx", "good", "loss", "ooo", "dup", "bad", "len", "late",
                                    "max_gap", "burst", "malformed", "foreign", "range", "clock", "peer"), 0)
        self.stats["overflow"] = False
        self.stats["bins"] = [0] * 5

    def _retire(self, until):
        for i in range(self._base, until):
            if self._seen[i]:
                self._run = 0
            else:
                self._run += 1
                self.stats["loss"] += 1
                self.stats["max_gap"] = max(self.stats["max_gap"], self._run)
        self._base = until

    def observe(self, data, arrival_us, *, from_peer=True):
        if self._summary is not None:
            raise ValueError("capture is already finished")
        if not from_peer:
            self.stats["peer"] += 1
            return
        try:
            flow, seq, sent_us = decode_header(data)
        except ValueError:
            self.stats["malformed"] += 1
            return
        if flow != self.plan.flow:
            self.stats["foreign"] += 1
            return
        if seq >= self.plan.count:
            self.stats["range"] += 1
            return
        if sent_us > LIMIT_US:
            self.stats["malformed"] += 1
            return
        last = self._last_arrival
        if not 0 <= arrival_us <= LIMIT_US or (last is not None and arrival_us < last):
            self.stats["clock"] += 1
            return

        thresholds = (1000, 5000, 15000, 25000)
        if last is not None:
            self.stats["bins"][sum(arrival_us - last >= boundary for boundary in thresholds)] += 1
        self._last_arrival = arrival_us
        while self._times and arrival_us - self._times[0] > 5000:
            self._times.popleft()
        peak = len(self._times) + 1
        self._times.append(arrival_us)
        self.stats["burst"] = max(self.stats["burst"], peak)
        self.stats["overflow"] |= peak > WINDOW

        if seq < self._base:
            self.stats["late"] += 1
            return
        if seq >= self._base + WINDOW:
            self._retire(seq - WINDOW + 1)
        if self._seen[seq]:
            self.stats["dup"] += 1
        else:
            self._seen[seq] = 1
            self.stats["rx"] += 1
            if seq < self._high:
                self.stats["ooo"] += 1
            self._high = max(self._high, seq)

        if len(data) != HEADER.size + self.plan.payload_size:
            self.stats["len"] += 1
        elif data[HEADER.size:] != payload(self.plan.seed, seq, self.plan.payload_size):
            self.stats["bad"] += 1
        elif self._seen[seq] == 1:
            self._seen[seq] = 2
            self.stats["good"] += 1

    def finish(self, complete):
        if self._summary is None:
            if complete:
                self._retire(self.plan.count)
            self._summary = {**self.stats, "complete": bool(complete), "bins": tuple(self.stats["bins"])}
        return dict(self._summary)


def receive_plan(plan, receive, *, expected_peer, clock_ns=time.monotonic_ns):
    """Measure until the fixed deadline (including duplicates after the last unique sequence)."""
    rx = Receiver(plan)
    start = clock_ns()
    for attempt in range(MAX_OBSERVATIONS + 1):
        remaining = LIMIT_US * 1000 - (clock_ns() - start)
        if remaining <= 0:
            return {**rx.finish(True), "status": "complete"}
        try:
            data, peer = receive(remaining / 1_000_000_000)
        except TimeoutError:
            return {**rx.finish(True), "status": "complete"}
        except OSError:
            return {**rx.finish(False), "status": "io_error"}
        arrival = (clock_ns() - start) // 1000
        if arrival >= LIMIT_US:
            return {**rx.finish(True), "status": "complete"}
        if attempt == MAX_OBSERVATIONS:
            break
        rx.observe(data, arrival, from_peer=peer == expected_peer)
    return {**rx.finish(False), "status": "observation_limit"}


@dataclass(frozen=True)
class Endpoint:
    address: str
    port: int

    def __post_init__(self):
        try:
            addr = ipaddress.IPv4Address(self.address)
        except ipaddress.AddressValueError as error:
            raise ValueError("use a numeric RFC1918 lab IPv4 address") from error
        if self.address != str(addr) or not any(addr in net for net in LAB_NETWORKS):
            raise ValueError("use a numeric RFC1918 lab IPv4 address")
        if addr.packed[-1] == 255:
            raise ValueError("do not use a possible broadcast address")
        _bounded("port", self.port, 1024, 65535)

    @classmethod
    def parse(cls, text):
        address, sep, port = text.partition(":")
        if not sep or not port.isascii() or not port.isdecimal() or len(port) > 5:
            raise ValueError("endpoint must be lab IPv4:port")
        return cls(address, int(port))

    def __str__(self):
        return f"{self.address}:{self.port}"

    def pair(self):
        return self.address, self.port


def permission(mode, local, peer):
    if mode == "send":
        return f"XTP2 SEND {local} -> {peer}"
    if mode == "receive":
        return f"XTP2 RECEIVE {peer} -> {local}"
    raise ValueError("unknown network operation")


def _network(mode, plan, local, peer, allow, *, socket_factory=None):
    if local == peer or allow != permission(mode, local, peer):
        raise ValueError("provide the exact --allow for this direction and distinct endpoints")
    socket_factory = socket_factory or socket.socket
    try:
        with socket_factory(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(local.pair())
            if mode == "send":
                sock.settimeout(0.05)
                sock.connect(peer.pair())
                return send_plan(plan, sock.send)

            def receive(timeout):
                sock.settimeout(timeout)
                # Winsock may raise WSAEMSGSIZE on truncation; retain the
                # header of an oversize datagram so it is counted consistently.
                return sock.recvfrom(IPV4_UDP_BUFFER)

            return receive_plan(plan, receive, expected_peer=peer.pair())
    except OSError as error:
        raise ValueError(f"could not open fixed UDP endpoint ({type(error).__name__})") from error


def _parse_int(text):
    if text.isascii() and text.isdecimal() and len(text) <= 10:
        return int(text)
    raise argparse.ArgumentTypeError("expected an unsigned decimal integer")


def board_notices(since, start, flow, *, clock=time.monotonic, sleep=time.sleep):
    """Read the fresh AT boundary until this exact flow finishes, without re-arming."""
    ready = f"+NET-PROBE2: ready flow={flow} port=49161 limit_ms=12000"
    done = re.compile(fr"\+NET-PROBE2: done flow={flow} rc=(-?\d+) complete=([01]) .+")
    deadline = clock() + 14
    armed = False
    lines = 0
    while clock() < deadline:
        batch = since(start)
        start += len(batch)
        lines += len(batch)
        if lines > 4096 or any(line.startswith("+READY:") for line in batch):
            raise ValueError("UDP probe lost its serial boundary")
        for line in batch:
            if line == ready:
                if armed:
                    raise ValueError("UDP probe returned duplicate readiness")
                armed = True
                yield line
            elif match := done.fullmatch(line):
                yield line
                if not armed or match.groups() != ("0", "1"):
                    raise ValueError("UDP probe did not complete its receive window")
                return
        sleep(0.025)
    raise TimeoutError("UDP probe did not return a complete result")


def add_arguments(parser):
    modes = parser.add_subparsers(dest="mode", required=True)
    for name in ("plan", "send", "receive"):
        mode = modes.add_parser(name)
        mode.add_argument("--flow", type=_parse_int, required=True)
        mode.add_argument("--seed", type=_parse_int, default=0)
        mode.add_argument("--count", type=_parse_int, default=100)
        mode.add_argument("--payload-size", type=_parse_int, default=160)
        mode.add_argument("--pattern", choices=BURSTS, default="paced")
        if name != "plan":
            mode.add_argument("--local", required=True, metavar="IP:PORT")
            mode.add_argument("--peer", required=True, metavar="IP:PORT")
            mode.add_argument("--allow", required=True, help="exact XTP2 SEND/RECEIVE tuple; authorize one lab run")


def run_args(args):
    plan = ProbePlan(args.flow, args.seed, args.count, args.payload_size, args.pattern)
    if args.mode == "plan":
        result = {"offsets_us": plan.offsets_us(), "datagram_size": HEADER.size + plan.payload_size}
    else:
        result = _network(args.mode, plan, Endpoint.parse(args.local), Endpoint.parse(args.peer), args.allow)
    print(json.dumps({"mode": args.mode, **asdict(plan), **result}))
    return 0 if result.get("status", "complete") == "complete" else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    add_arguments(parser)
    args = parser.parse_args(argv)
    try:
        return run_args(args)
    except ValueError as error:
        parser.error(str(error))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
