#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import contextlib
import io
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

import media_memory


class MediaMemoryTest(unittest.TestCase):
    def setUp(self):
        directory = TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.path = Path(directory.name)

    def sample(self, name, generation, free, largest, watermark=None, tasks=("xt_capture", "xt_uplink")):
        path = self.path / f"{name}.log"
        lines = [
            ">>> 00:00:00 AT+STATUS media",
            f"+MEDIA: ready=1 generation={generation} active={int(bool(generation))} draining=0",
            f"+MEDIA-MEM: internal_free={free} internal_largest={largest} bulk_free=1000 bulk_largest=800",
            "OK",
        ]
        for task in tasks:
            lines.append(f"[00:00:00] +TASK: name={task} stack_min_free=1408 priority=5 core=0")
        if watermark is not None:
            lines.append(f"+MEM: kind=internal min_free={watermark} free={free} min_largest=384 largest={largest}")
        path.write_text("\n".join(lines), encoding="utf-8")
        return path

    def test_topology_and_floors(self):
        idle = self.sample("idle", 0, 14000, 6000)
        active = self.sample("active", 9, 9000, 3000)
        after = self.sample("after", 0, 13500, 5900, watermark=8500)
        args = ["--idle", str(idle), "--active", str(active), "--after", str(after), "--topology", "folded"]
        for floor, expected in ((8000, 0), (9000, 1)):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(media_memory.main([*args, "--min-free", str(floor)]), expected)
            report = json.loads(out.getvalue())
            self.assertEqual(report["active"]["observed_min_internal_free"], 9000)
            self.assertIsNone(report["active"]["native_internal_min_free_since_boot"])
            self.assertEqual(report["observed_free_change_after"], -500)
            self.assertEqual(report["observed_largest_change_after"], -100)
            self.assertEqual(report["native_floor"]["passed"], expected == 0)

    def test_task_set_and_phase_must_match(self):
        path = self.sample("wrong", 0, 14000, 6000, tasks=("xt_capture", "xt_playout", "xt_uplink"))
        with self.assertRaisesRegex(ValueError, "does not match"):
            media_memory.phase([path], True, "folded")
        with self.assertRaisesRegex(ValueError, "unexpected media tasks"):
            media_memory.phase([path], False, "folded")
        self.assertEqual(media_memory.phase([path], False, "split")["observed_min_internal_largest"], 6000)

    def test_observed_min_is_not_native_floor(self):
        idle = self.sample("idle", 0, 14000, 6000)
        active = self.sample("active", 3, 9000, 3000)
        after = self.sample("after", 0, 13900, 5900)
        args = ["--idle", str(idle), "--active", str(active), "--after", str(after), "--min-free", "8000"]
        errors = io.StringIO()
        with contextlib.redirect_stderr(errors), self.assertRaises(SystemExit) as err:
            media_memory.main(args)
        self.assertEqual(err.exception.code, 2)
        self.assertIn("requires an internal AT+MEM watermark", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
