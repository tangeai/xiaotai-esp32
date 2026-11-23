# SPDX-License-Identifier: MIT
"""Exercise XTP2 receiver accounting and endpoint admission without networking."""

from contextlib import redirect_stderr, redirect_stdout
import io
import json
import unittest
from unittest.mock import MagicMock, patch

import xtp2
from xtp2 import (Endpoint, ProbePlan, Receiver, encode_datagram, main, permission, receive_plan)


class ReceiverTest(unittest.TestCase):
    def test_reorder_duplicate_correction_and_expired_loss(self):
        plan = ProbePlan(7, 123, count=130, payload_size=4)
        rx = Receiver(plan)

        def record(seq, arrival, change=None):
            wire = encode_datagram(plan, seq, 17)
            rx.observe(change(wire) if change else wire, arrival)

        record(0, 0)
        record(2, 10, lambda wire: wire[:-1] + bytes([wire[-1] ^ 1]))
        record(1, 20)
        record(2, 30)
        record(3, 40, lambda wire: wire[:-1])
        record(3, 50)
        record(100, 60)
        record(4, 70)   # no retroactive revision of expired missing slot
        record(0, 80)   # an expired duplicate is late, not an in-window duplicate
        row = rx.finish(True)
        self.assertEqual(
            {key: row[key] for key in ("rx", "good", "loss", "max_gap", "ooo", "dup", "bad", "len", "late", "burst")},
            {"rx": 5, "good": 5, "loss": 125, "max_gap": 96, "ooo": 1, "dup": 2,
             "bad": 1, "len": 1, "late": 2, "burst": 9})
        self.assertTrue(row["complete"])
        self.assertEqual(rx.finish(False), row, "finish must be idempotent")

    def test_report_bounds_large_jump_and_does_not_finalise_partial_tail(self):
        plan = ProbePlan(7, count=500)
        data = encode_datagram(plan, 499, 1)
        for complete, missing in ((True, 499), (False, 436)):
            rx = Receiver(plan)
            rx.observe(data, 1)
            row = rx.finish(complete)
            self.assertEqual((row["rx"], row["good"], row["loss"], row["max_gap"]), (1, 1, missing, missing))

    def test_source_header_flow_ranges_and_clock_are_separate(self):
        plan = ProbePlan(432, count=2)
        packet = encode_datagram(plan, 0, 10)
        rx = Receiver(plan)
        rx.observe(b"untrusted", 1, from_peer=False)
        rx.observe(b"bad", 2)
        rx.observe(b"XTP1" + packet[4:], 3)
        rx.observe(encode_datagram(ProbePlan(433), 0, 10), 4)
        rx.observe(encode_datagram(ProbePlan(plan.flow), 2, 10), 5)
        rx.observe(xtp2.HEADER.pack(xtp2.MAGIC, plan.flow, 0, xtp2.LIMIT_US + 1) + packet[16:], 6)
        rx.observe(packet, 7)
        rx.observe(packet, 6)
        rx.observe(packet, xtp2.LIMIT_US + 1)
        row = rx.finish(True)
        self.assertEqual((row["peer"], row["malformed"], row["foreign"], row["range"], row["clock"]), (1, 3, 1, 1, 2))
        self.assertEqual((row["rx"], row["good"], row["loss"], row["burst"], row["bins"]), (1, 1, 1, 1, (0,) * 5))

    def test_timing_intervals_and_closed_burst_boundary(self):
        plan = ProbePlan(8, count=1)
        packet = encode_datagram(plan, 0, 0)
        rx = Receiver(plan)
        now = 0
        rx.observe(packet, now)
        for delta in (999, 1000, 4999, 5000, 14999, 15000, 24999, 25000):
            now += delta
            rx.observe(packet, now)
        self.assertEqual(rx.finish(True)["bins"], (1, 2, 2, 2, 1))

        rx = Receiver(plan)
        for stamp in (0, 5000, 5001):
            rx.observe(packet, stamp)
        self.assertEqual(rx.finish(True)["burst"], 2)
        rx = Receiver(plan)
        for _ in range(67):
            rx.observe(packet, 0)
        row = rx.finish(True)
        self.assertEqual((row["burst"], row["overflow"], row["dup"]), (65, True, 66))

    def test_fake_receive_complete_or_bounded_incomplete(self):
        plan = ProbePlan(1, count=3)
        expected = ("192.168.5.9", 49161)
        packet = encode_datagram(plan, 1, 1)
        for stop in (TimeoutError(), OSError()):
            rx = MagicMock(side_effect=[(packet, expected), (packet, ("192.168.5.10", 49161)), stop])
            result = receive_plan(plan, rx, expected_peer=expected, clock_ns=lambda: 1)
            self.assertEqual((result["rx"], result["good"], result["peer"]), (1, 1, 1))
            self.assertEqual(result["complete"], isinstance(stop, TimeoutError))
            self.assertEqual(result["loss"], 2 if isinstance(stop, TimeoutError) else 0)
        with patch.object(xtp2, "MAX_OBSERVATIONS", 2):
            rx = MagicMock(return_value=(packet, expected))
            result = receive_plan(plan, rx, expected_peer=expected, clock_ns=lambda: 1)
            self.assertEqual(result["status"], "observation_limit")
            self.assertEqual((result["complete"], rx.call_count, result["rx"], result["dup"], result["loss"]),
                             (False, 3, 1, 1, 0))

    def test_packet_copied_at_deadline_does_not_enter_run(self):
        plan = ProbePlan(1, count=1)
        peer = ("192.168.5.9", 49161)
        times = iter((0, 0, xtp2.LIMIT_US * 1000))
        result = receive_plan(plan, lambda timeout: (encode_datagram(plan, 0, 0), peer),
                              expected_peer=peer, clock_ns=lambda: next(times))
        self.assertEqual((result["complete"], result["rx"], result["loss"]), (True, 0, 1))


class EndpointTest(unittest.TestCase):
    def setUp(self):
        self.local = Endpoint.parse("192.168.5.8:55161")
        self.peer = Endpoint.parse("192.168.5.9:49161")

    def test_rejects_nonlab_or_nonliteral_addresses_and_unbound_ports(self):
        for value in ("example.test:1234", "8.8.8.8:49161", "127.0.0.1:49161", "0.0.0.0:49161",
                      "224.0.0.1:49161", "192.168.5.255:49161", "192.168.5.8:1023",
                      "192.168.5.8:65536", "192.168.5.8:+1024", "::1:49161"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                Endpoint.parse(value)
        self.assertEqual(Endpoint.parse("10.8.7.6:1024").pair(), ("10.8.7.6", 1024))
        self.assertEqual(Endpoint.parse("172.31.2.3:65535").pair(), ("172.31.2.3", 65535))

    def test_board_notices_require_fresh_flow_and_complete_window(self):
        ready = "+NET-PROBE2: ready flow=7 port=49161 limit_ms=12000"
        done = "+NET-PROBE2: done flow=7 rc=0 complete=1 rx=1"
        capture = [ready, done, "+NET-PROBE2: ready flow=6 port=49161 limit_ms=12000", ready, done]
        self.assertEqual(list(xtp2.board_notices(lambda start: capture[start:], 2, 7)), [ready, done])
        for bad in ([done], [ready, ready, done], [ready, "+READY: reset=1", done],
                    [ready, done.replace("rc=0 complete=1", "rc=264 complete=0")]):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                list(xtp2.board_notices(lambda start: bad[start:], 0, 7))

    def test_no_socket_for_offline_plan_or_wrong_permission(self):
        with patch.object(xtp2.socket, "socket") as socket_factory:
            with redirect_stdout(io.StringIO()) as out:
                self.assertEqual(main(["plan", "--flow", "17", "--count", "1"]), 0)
            result = json.loads(out.getvalue())
            self.assertEqual((result["offsets_us"], result["datagram_size"]), ([0], 176))
            with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
                main(["send", "--flow", "17", "--local", str(self.local), "--peer", str(self.peer),
                      "--allow", permission("receive", self.local, self.peer)])
            self.assertEqual(raised.exception.code, 2)
            socket_factory.assert_not_called()

    def test_socket_adapters_keep_fixed_local_and_peer(self):
        for mode in ("send", "receive"):
            with self.subTest(mode=mode):
                factory = MagicMock()
                sock = factory.return_value.__enter__.return_value
                sock.recvfrom.return_value = (b"test", self.peer.pair())

                def receive(_plan, read, *, expected_peer):
                    self.assertEqual(expected_peer, self.peer.pair())
                    self.assertEqual(read(1.25), (b"test", self.peer.pair()))
                    return {"status": "complete"}

                with (patch.object(xtp2, "send_plan", return_value={"status": "complete"}) as tx,
                      patch.object(xtp2, "receive_plan", side_effect=receive) as rx):
                    result = xtp2._network(mode, ProbePlan(17), self.local, self.peer,
                                           permission(mode, self.local, self.peer), socket_factory=factory)
                self.assertEqual(result["status"], "complete")
                sock.bind.assert_called_once_with(self.local.pair())
                if mode == "send":
                    sock.connect.assert_called_once_with(self.peer.pair())
                    tx.assert_called_once_with(ProbePlan(17), sock.send)
                    rx.assert_not_called()
                else:
                    sock.connect.assert_not_called()
                    sock.settimeout.assert_called_once_with(1.25)
                    sock.recvfrom.assert_called_once_with(xtp2.IPV4_UDP_BUFFER)
                    tx.assert_not_called()


if __name__ == "__main__":
    unittest.main()
