# SPDX-License-Identifier: MIT
"""Exercise the XTP2 wire contract and pacing with no sockets."""

import struct
import unittest
from unittest.mock import patch

import xtp2
from xtp2 import ProbePlan, decode_header, encode_datagram, payload, send_plan


class FakeClock:
    def __init__(self, oversleep_ns=0):
        self.now_ns = 9_000_000_000
        self.oversleep_ns = oversleep_ns
        self.sleeps = []

    def clock_ns(self):
        return self.now_ns

    def sleep(self, seconds):
        delay_ns = round(seconds * 1_000_000_000)
        if delay_ns <= 0:
            raise AssertionError("pacer asked for a nonpositive sleep")
        self.sleeps.append(delay_ns)
        self.now_ns += delay_ns + self.oversleep_ns
        self.oversleep_ns = 0


class ProtocolTest(unittest.TestCase):
    def test_golden_payloads_include_zero_state_fallback(self):
        cases = (
            (0, 0, "9a2ab7e749a957950313f748"),
            (0x89ABCDEF, 7, "d356cb2bca2339459eb17c5f"),
            (0xA341316C, 0, "1f19289dcce78c1742ceefbe"),
        )
        for seed, sequence, expected in cases:
            with self.subTest(seed=seed, sequence=sequence):
                self.assertEqual(payload(seed, sequence, 12), bytes.fromhex(expected))
                self.assertEqual(payload(seed, sequence, 512)[:12], bytes.fromhex(expected))

    def test_wire_sample_uses_network_byte_order(self):
        plan = ProbePlan(flow=0x12345678, seed=0x89ABCDEF, count=8, payload_size=12)
        expected_header = bytes.fromhex("58545032123456780000000700023456")
        packet = encode_datagram(plan, 7, 0x00023456)
        self.assertEqual(packet, expected_header + bytes.fromhex("d356cb2bca2339459eb17c5f"))
        self.assertEqual(decode_header(packet), (0x12345678, 7, 0x00023456))

    def test_encode_rejects_out_of_plan_sequence_and_excess_elapsed_time(self):
        plan = ProbePlan(123, count=4)
        for seq, stamp in ((-1, 0), (4, 0), (0, -1), (0, 12_000_001)):
            with self.subTest(seq=seq, stamp=stamp), self.assertRaises(ValueError):
                encode_datagram(plan, seq, stamp)

    def test_decode_rejects_truncated_headers_and_wrong_magic(self):
        header = struct.pack("!4sIII", b"XTP2", 1, 0, 0)
        for size in (0, 3, 4, 12, 15):
            with self.subTest(size=size), self.assertRaises(ValueError):
                decode_header(header[:size])
        with self.assertRaises(ValueError):
            decode_header(b"XTP1" + header[4:] + b"payload")


class PlanTest(unittest.TestCase):
    def test_plan_defaults_and_inclusive_bounds(self):
        plan = ProbePlan(1)
        self.assertEqual((plan.seed, plan.count, plan.payload_size, plan.pattern),
                         (0, 100, 160, "paced"))
        self.assertEqual(ProbePlan(1, count=1, payload_size=1).offsets_us(), (0,))
        maximum = ProbePlan(0xFFFFFFFF, seed=0xFFFFFFFF, count=500, payload_size=512)
        self.assertEqual(maximum.offsets_us()[-1], 499 * 20_000)

    def test_invalid_plan_ranges_and_short_burst(self):
        cases = (
            {"flow": 0}, {"flow": -1}, {"flow": 0x100000000},
            {"seed": -1}, {"seed": 0x100000000},
            {"count": 0}, {"count": 501},
            {"payload_size": 0}, {"payload_size": 513},
            {"pattern": "unspecified"},
            {"pattern": "burst5", "count": 4},
            {"pattern": "burst10", "count": 9},
            {"pattern": "burst20", "count": 19},
            {"pattern": "burst40", "count": 39},
            {"pattern": "burst50", "count": 49},
        )
        for invalid in cases:
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                ProbePlan(**({"flow": 1} | invalid))

    def test_bursts_have_one_initial_slot_then_paced_tail(self):
        for burst in (5, 10, 20, 40, 50):
            with self.subTest(burst=burst):
                at_minimum = ProbePlan(1, count=burst, pattern=f"burst{burst}")
                self.assertEqual(at_minimum.offsets_us(), (0,) * burst)
                with_tail = ProbePlan(1, count=burst + 3, pattern=f"burst{burst}")
                self.assertEqual(with_tail.offsets_us(), (0,) * burst + (20_000, 40_000, 60_000))

    def test_jitter_is_reproducible_strictly_ordered_and_bounded(self):
        plan = ProbePlan(1, seed=1234, count=30, pattern="jitter")
        offsets = plan.offsets_us()
        self.assertEqual(offsets, ProbePlan(1, seed=1234, count=30, pattern="jitter").offsets_us())
        self.assertNotEqual(offsets, ProbePlan(1, seed=1235, count=30, pattern="jitter").offsets_us())
        self.assertEqual(offsets[0], 0)
        self.assertEqual(len(offsets), 30)
        self.assertTrue(all(a < b for a, b in zip(offsets, offsets[1:])))
        for i, offset in enumerate(offsets):
            self.assertLessEqual(abs(offset - i * 20_000), 5000)


class SendTest(unittest.TestCase):
    def run_plan(self, plan, clock, fail_seq=None):
        sent = []
        attempted = []

        def send(packet):
            header = decode_header(packet)
            attempted.append(header)
            if header[1] == fail_seq:
                raise OSError("fixture send failed")
            sent.append(packet)
            return len(packet)

        outcome = send_plan(plan, send, clock_ns=clock.clock_ns, sleep=clock.sleep)
        return outcome, sent, attempted

    def test_paced_sender_uses_absolute_slots_and_actual_elapsed_timestamp(self):
        clock = FakeClock(oversleep_ns=2_500_000)
        plan = ProbePlan(123, seed=99, count=4, payload_size=12)
        outcome, sent, headers = self.run_plan(plan, clock)
        expected_times = [0, 22_500, 40_000, 60_000]
        self.assertEqual(headers, [(plan.flow, i, time) for i, time in enumerate(expected_times)])
        self.assertEqual(clock.sleeps, [20_000_000, 17_500_000, 20_000_000])
        for i, packet in enumerate(sent):
            self.assertEqual(packet[16:], payload(plan.seed, i, plan.payload_size))
        self.assertEqual(outcome["status"], "complete")
        self.assertTrue(outcome["completed"])
        self.assertEqual(outcome["sent"], 4)
        self.assertEqual(outcome["max_late_us"], 2500)
        self.assertEqual(outcome["last_sent_us"], 60_000)

    def test_burst_sender_does_not_build_payloads_between_sends(self):
        clock = FakeClock()
        plan = ProbePlan(123, count=13, pattern="burst10", payload_size=12)
        built = []
        sent = []

        def watched_payload(seed, seq, size):
            self.assertFalse(sent, "payload generation entered the pacing loop")
            built.append((seed, seq, size))
            return payload(seed, seq, size)

        def send(packet):
            self.assertEqual(len(built), plan.count)
            sent.append(decode_header(packet))
            return len(packet)

        with patch.object(xtp2, "payload", side_effect=watched_payload):
            outcome = send_plan(plan, send, clock_ns=clock.clock_ns, sleep=clock.sleep)
        self.assertTrue(outcome["completed"])
        self.assertEqual(sent, [(plan.flow, i, at) for i, at in enumerate(plan.offsets_us())])
        self.assertEqual(clock.sleeps, [20_000_000] * 3)

    def test_sender_stops_on_first_late_slot_without_catchup(self):
        clock = FakeClock(oversleep_ns=5_001_000)
        outcome, _, headers = self.run_plan(ProbePlan(12, count=4), clock)
        self.assertEqual(headers, [(12, 0, 0)])
        self.assertEqual(outcome["status"], "schedule_late")
        self.assertFalse(outcome["completed"])
        self.assertEqual(outcome["sent"], 1)
        self.assertEqual(outcome["max_late_us"], 5001)
        self.assertEqual(outcome["last_sent_us"], 0)

    def test_sender_accepts_the_exact_lateness_limit(self):
        clock = FakeClock(oversleep_ns=5_000_000)
        outcome, _, headers = self.run_plan(ProbePlan(12, count=3), clock)
        self.assertEqual(headers, [(12, 0, 0), (12, 1, 25_000), (12, 2, 40_000)])
        self.assertTrue(outcome["completed"])
        self.assertEqual(outcome["max_late_us"], 5000)

    def test_burst_may_use_15_ms_but_stops_before_sending_any_later(self):
        for gap_us, expected_status in ((15_000, "complete"), (15_001, "schedule_late")):
            with self.subTest(gap_us=gap_us):
                clock = FakeClock()
                plan = ProbePlan(12, count=11, pattern="burst10")
                sent = []

                def send(packet):
                    sent.append(decode_header(packet))
                    if len(sent) == 1:
                        clock.now_ns += gap_us * 1000
                    return len(packet)

                outcome = send_plan(plan, send, clock_ns=clock.clock_ns, sleep=clock.sleep)
                self.assertEqual(outcome["status"], expected_status)
                if gap_us == 15_000:
                    self.assertEqual(sent, [(12, 0, 0)] + [(12, i, gap_us) for i in range(1, 10)] +
                                     [(12, 10, 20_000)])
                    self.assertTrue(outcome["completed"])
                else:
                    self.assertEqual(sent, [(12, 0, 0)])
                    self.assertFalse(outcome["completed"])

    def test_send_error_reports_only_the_packets_already_sent(self):
        clock = FakeClock()
        outcome, sent, attempted = self.run_plan(ProbePlan(12, count=4), clock, fail_seq=2)
        self.assertEqual(attempted, [(12, 0, 0), (12, 1, 20_000), (12, 2, 40_000)])
        self.assertEqual(len(sent), 2)
        self.assertFalse(outcome["completed"])
        self.assertEqual(outcome["status"], "io_error")
        self.assertEqual(outcome["sent"], 2)
        self.assertEqual(outcome["last_sent_us"], 20_000)

    def test_short_send_is_not_counted_or_retried(self):
        clock = FakeClock()
        attempted = []

        def short_send(packet):
            attempted.append(decode_header(packet))
            return len(packet) - 1

        outcome = send_plan(ProbePlan(123, count=3), short_send, clock_ns=clock.clock_ns, sleep=clock.sleep)
        self.assertEqual(attempted, [(123, 0, 0)])
        self.assertEqual(outcome["status"], "io_error")
        self.assertFalse(outcome["completed"])
        self.assertEqual(outcome["sent"], 0)
        self.assertIsNone(outcome["last_sent_us"])


if __name__ == "__main__":
    unittest.main()
