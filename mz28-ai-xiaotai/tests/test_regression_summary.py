import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location(
    "regression_summary", Path(__file__).parents[1] / "tools/regression_summary.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class RegressionSummaryTests(unittest.TestCase):
    def test_no_media_does_not_inherit_previous_audio(self):
        result = MODULE.summarize("""I (100) voip: DIALING: generation=1
I (101) audio: TOTAL | TX packets=500 bytes=30000 failed=0 backlog_drop=0 | RX packets=500 bytes=4000 bad_format=0
I (200) voip: SIGNAL | generation=1 cmd=0x2001 bytes=12 state=waiting-peer
I (201) voip: call resources ready: generation=1
I (202) voip: FLOW | generation=1 closing->idle internal_free=99999B
""")
        call = result["calls"][0]
        self.assertIsNone(call["audio_totals"])
        self.assertIsNone(call["media_start_ms"])
        self.assertEqual(call["last_state"], "idle")
        self.assertEqual(call["signals"][0]["command"], "0x2001")

    def test_media_and_memory(self):
        result = MODULE.summarize("""I (10) voip: INCOMING: generation=2
I (20) audio: START | codec=Opus
I (40) voip: state=active generation=2 heap=24000 largest=17000 stack_hwm=3200
I (60) voip: state=active generation=2 heap=22000 largest=18000 stack_hwm=3100
I (70) audio: RX | max_gap=35ms
I (80) audio: STOP | active_ms=60
I (81) audio: TOTAL | TX packets=3 bytes=180 failed=0 backlog_drop=0 | RX packets=2 bytes=16 bad_format=0
I (90) voip: state=idle generation=2 heap=96000 largest=86000 stack_hwm=3100
""")
        call = result["calls"][0]
        self.assertEqual(call["direction"], "incoming")
        self.assertEqual(call["active_ms"], 60)
        self.assertEqual(call["active_memory_min"],
                         {"free": 22000, "largest": 17000, "stack_hwm": 3100})
        self.assertEqual(call["last_idle_memory"]["free"], 96000)
        self.assertEqual(call["audio_totals"]["tx_packets"], 3)
        self.assertEqual(call["rx_max_gap_ms"], 35)
        self.assertIsNone(call["resources_ready_ms"])

    def test_host_marker_inside_uart_line(self):
        result = MODULE.summarize("I (1) voip: DIA\r\n[HOST UTC +1s] send status\r\n"
                                  "LING: generation=1\r\n")
        self.assertEqual(result["calls"][0]["direction"], "outgoing")

    def test_boots_do_not_merge(self):
        result = MODULE.summarize("I (10000) voip: DIALING: generation=1\n"
                                  "I (100) voip: DIALING: generation=1\n")
        self.assertEqual(len(result["calls"]), 2)
        self.assertEqual(result["calls"][1]["segment"], 1)

    def test_alerts_and_empty_evidence(self):
        result = MODULE.summarize("E (1) memory: allocation failed: total=9\n"
                                  "Guru Meditation Error\n")
        self.assertEqual(result["calls"], [])
        self.assertEqual(result["alert_count"], 2)
        self.assertIsNone(MODULE.summarize("I (1) voip: DIALING: generation=1\n")
                          ["calls"][0]["active_memory_min"])

    def test_control_failures_are_not_hidden(self):
        result = MODULE.summarize("W (10) voip: reject request busy; peer will time out\n"
                                  "W (20) voip: REJECT failed: ret=503 elapsed_ms=15\n"
                                  "E (30) voip: control queue full\n"
                                  "Task watchdog got triggered\n")
        self.assertEqual(result["alert_count"], 4)


if __name__ == "__main__":
    unittest.main()
