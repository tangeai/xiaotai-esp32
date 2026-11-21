# SPDX-License-Identifier: MIT
import unittest

from camera_evidence import probe_frame, probe_snapshot, video_snapshot


STATUS = (
    "+VIDEO: started=1 requested=7 generation=7 ignored=0 uplink_codecs=2 downlink_codecs=2 "
    "camera_width=320 camera_height=240 camera_rotation=0 camera_full_range=0 "
    "up_y_state=valid up_y_mean=0 up_y_n=64",
    "+VIDEO-UP: phase=running error=0x0 codec=jpeg camera_enabled=1 width=320 height=240 quality=45 "
    "bitrate_bps=0 fps_x10=77 captured=34 sent=32 key_frames=32 key_requests=0 stale=0 congested=0 "
    "late=2 oversize=0 capture_failed=0 encode_failed=0 send_failed=0 backpressure=0 "
    "encode_max_us=9084 send_max_us=4120 age_max_ms=74",
    "+VIDEO-DOWN: phase=off error=0x0 codec=none received=0",
    "OK",
)
FRAME = "+VIDEO-PROBE-FRAME: index=0 width=320 height=240 format=yuv422 bytes=153600 crc32=0000ff1e ok=1"
LUMA = "+VIDEO-PROBE-LUMA: state=valid y_mean=78 samples=64"
TERMINAL = "+VIDEO-PROBE: result=passed frames=3 error=NONE"


class CameraEvidenceTest(unittest.TestCase):
    def test_video_counters_and_real_dark_frame(self):
        self.assertEqual(video_snapshot(iter(STATUS)), {
            "requested": 7, "generation": 7, "up_phase": "running", "down_phase": "off", "error": "0x0",
            "captured": 34, "sent": 32, "capture_failed": 0, "encode_failed": 0, "send_failed": 0,
            "first_luma": {"state": "valid", "y_mean": 0, "samples": 64}, "recovery": None,
        })

    def test_video_missing_and_reported_luma_distinct(self):
        old = STATUS[0].split(" up_y_state=")[0]
        self.assertIsNone(video_snapshot((old, *STATUS[1:]))["first_luma"])
        for state in ("invalid", "unavailable", "no_frame"):
            with self.subTest(state=state):
                actual = video_snapshot((f"{old} up_y_state={state} up_y_mean=-1 up_y_n=0 future=42", *STATUS[1:]))
                self.assertEqual(actual["first_luma"], {"state": state, "y_mean": None, "samples": 0})

    def test_video_uplink_error_and_optional_recovery(self):
        up = STATUS[1].replace("phase=running error=0x0", "phase=error error=TIMEOUT")
        up = up.replace("capture_failed=0 encode_failed=0 send_failed=0",
                        "capture_failed=1 encode_failed=2 send_failed=3")
        recovery = "+VIDEO-UP-RECOVERY: reopens=4 recovered=3 exhausted=1"
        actual = video_snapshot((STATUS[0], up, recovery, STATUS[2], "OK"))
        self.assertEqual((actual["up_phase"], actual["error"]), ("error", "TIMEOUT"))
        self.assertEqual((actual["capture_failed"], actual["encode_failed"], actual["send_failed"]), (1, 2, 3))
        self.assertEqual(actual["recovery"], {"reopens": 4, "recovered": 3, "exhausted": 1})

    def test_malformed_status_is_not_a_zero_snapshot(self):
        for lines in (
            STATUS[:2], (*STATUS, STATUS[1]),
            (STATUS[0].replace(" up_y_n=64", ""), *STATUS[1:]),
            (STATUS[0].replace("up_y_n=64", "up_y_n=0"), *STATUS[1:]),
            (STATUS[0].replace("up_y_mean=0", "up_y_mean=256"), *STATUS[1:]),
            (STATUS[0], STATUS[1].replace("captured=34", "captured=-1"), *STATUS[2:]),
            (STATUS[0], STATUS[1].replace("sent=32", "sent=4294967296"), *STATUS[2:]),
            (STATUS[0], STATUS[1].replace(" captured=34", ""), *STATUS[2:]),
            (*STATUS, "+VIDEO-UP-RECOVERY: reopens=0 recovered=2"),
        ):
            with self.subTest(lines=lines), self.assertRaises(ValueError):
                video_snapshot(lines)

    def test_probe_first_frame_is_available_before_terminal(self):
        lines = ("OK", FRAME, FRAME.replace("index=0", "index=1"), LUMA)
        first = {
            "index": 0, "width": 320, "height": 240, "format": "yuv422",
            "bytes": 153600, "crc32": "0000ff1e", "ok": True,
        }
        self.assertEqual(probe_frame(iter(lines)), first)
        self.assertIsNone(probe_snapshot(lines))
        self.assertEqual(probe_snapshot(iter((*lines, TERMINAL))), {
            "result": "passed", "frames": 3, "error": "NONE", "first_frame": first,
            "first_luma": {"state": "valid", "y_mean": 78, "samples": 64},
        })

    def test_probe_missing_async_reports_are_not_fabricated(self):
        later = FRAME.replace("index=0", "index=2")
        self.assertIsNone(probe_frame([later]))
        self.assertEqual(probe_snapshot([later, TERMINAL]), {
            "result": "passed", "frames": 3, "error": "NONE", "first_frame": None, "first_luma": None,
        })
        self.assertIsNone(probe_snapshot(["OK", "+VIDEO-PROBE-NOTICE: result=pending"]))

    def test_probe_busy_and_timeout_keep_distinct_luma_evidence(self):
        busy = "+VIDEO-PROBE: result=failed frames=0 error=BUSY"
        self.assertEqual(probe_snapshot([busy]), {
            "result": "failed", "frames": 0, "error": "BUSY", "first_frame": None, "first_luma": None,
        })
        timeout = busy.replace("BUSY", "TIMEOUT")
        luma = "+VIDEO-PROBE-LUMA: state=no_frame y_mean=-1 samples=0"
        data = probe_snapshot([luma, timeout])
        self.assertEqual(data["error"], "TIMEOUT")
        self.assertIsNone(data["first_frame"])
        self.assertEqual(data["first_luma"], {"state": "no_frame", "y_mean": None, "samples": 0})

    def test_probe_invalid_returned_descriptor_is_still_observed(self):
        frame = "+VIDEO-PROBE-FRAME: index=0 width=0 height=0 format=unknown bytes=0 crc32=00000000 ok=0"
        luma = "+VIDEO-PROBE-LUMA: state=invalid y_mean=-1 samples=0"
        terminal = "+VIDEO-PROBE: result=failed frames=1 error=INVALID_SIZE"
        data = probe_snapshot([frame, luma, terminal])
        self.assertFalse(data["first_frame"]["ok"])
        self.assertEqual(data["first_frame"]["width"], 0)
        self.assertEqual(data["first_luma"], {"state": "invalid", "y_mean": None, "samples": 0})

    def test_probe_malformed_typed_data_fails_closed(self):
        for lines in (
            [TERMINAL.replace(" frames=3", "")], [TERMINAL, TERMINAL],
            [TERMINAL.replace("error=NONE", "error=TIMEOUT")], [FRAME, FRAME, TERMINAL],
            [FRAME.replace("ok=1", "ok=2"), TERMINAL],
            [FRAME.replace("crc32=0000ff1e", "crc32=ff1e"), TERMINAL],
            [LUMA.replace("samples=64", "samples=unknown"), TERMINAL],
        ):
            with self.subTest(lines=lines), self.assertRaises(ValueError):
                probe_snapshot(lines)


if __name__ == "__main__":
    unittest.main()
