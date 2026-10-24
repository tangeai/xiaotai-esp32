#!/usr/bin/env python3
"""Host-only safeguards for persistent-setting regression fixtures."""
import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import regress_at
import regress_onboarding


class Controls:
    def __init__(self, volume=73, gain=17, fail_save=False, prior_error=0):
        self.state = {"volume": volume, "gain": gain, "busy": 0, "settings_loaded": 1,
                      "settings_saved": 1, "settings_pending": 0, "settings_error": prior_error}
        self.commands, self.observations = [], []
        self.fail_save = fail_save
        self.selected = 0

    def command(self, text):
        self.commands.append(text)
        if text.startswith(("AT+VOL ", "AT+MIC ")):
            key = "volume" if text.startswith("AT+VOL ") else "gain"
            self.state.update({key: int(text.split()[-1]), "settings_saved": 0, "settings_pending": 1})
        return []

    def wait(self, predicate, timeout=3, allow_settings_error=False):
        self.assert_error_policy(allow_settings_error)
        for _ in range(4):
            state = self.state.copy()
            self.observations.append(state)
            if predicate(state):
                return state
            if not self.fail_save:
                self.state.update(settings_saved=1, settings_pending=0, settings_error=0)
        raise TimeoutError("settings not durably restored")

    def assert_error_policy(self, allow):
        if not allow:
            assert self.state["settings_error"] == 0

    def idle(self):
        return self.state.copy()

    def gesture(self, kind, page, selected):
        assert page == 4
        self.selected = selected
        if kind == "DOUBLE":
            field = "volume" if selected < 2 else "gain"
            step = (5, -5, 3, -3)[selected]
            maximum = 100 if selected < 2 else 36
            self.state[field] = max(0, min(maximum, self.state[field] + step))


class Port:
    is_open = True
    def close(self):
        self.is_open = False


class SettingsRegressionTest(unittest.TestCase):
    def test_onboarding_waits_for_authoritative_contact_page_before_hold(self):
        for page, expected_filter in ((2, 2), (3, 1)):
            with self.subTest(page=page):
                class Contacts:
                    def __init__(self):
                        self.commands = []
                    def command(self, text):
                        self.commands.append(text)
                        loading = int(len(self.commands) == 1)
                        return [f"+CONTACTS: filter={expected_filter} loading={loading} error=0"]
                    def status(self):
                        assert len(self.commands) == 2
                        return {"page": page, "selected": 0, "key_active": 0, "epoch": 8}
                device = Contacts()
                with patch.object(regress_onboarding.time, "sleep"):
                    result = regress_onboarding.contact_page_ready(device, page)
                self.assertEqual(device.commands, ["AT+CONTACTS", "AT+CONTACTS"])
                self.assertIn("loading=0", result[0])

    def test_onboarding_never_navigates_an_unexpected_initial_page(self):
        class UnexpectedPage:
            def status(self):
                return {"page": 2, "selected": 0, "key_active": 0}
            def screen(self, *args):
                raise RuntimeError("Unexpected page must be rejected before screenshots or gestures")
            def gesture(self, *args, **kwargs):
                raise RuntimeError("Unexpected page must not submit a call intent")
        with self.assertRaisesRegex(AssertionError, "required_home_precondition"):
            regress_onboarding.ui_round_trip(UnexpectedPage(), None)

    def test_onboarding_contact_error_is_not_an_empty_page_success(self):
        class FailedPage:
            def command(self, text):
                assert text == "AT+CONTACTS"
                return ["+CONTACTS: filter=2 loading=0 error=264"]
        with self.assertRaises(AssertionError):
            regress_onboarding.contact_page_ready(FailedPage(), 2)

    def test_current_settings_and_relative_ui_steps(self):
        device = Controls()
        self.assertEqual(regress_at.original_controls(device), {"volume": 73, "gain": 17})
        self.assertEqual(regress_at.exercise_setting_steps(device), {"volume": 73, "gain": 17})
        device = Controls(100, 36)
        self.assertEqual(regress_at.exercise_setting_steps(device), {"volume": 95, "gain": 33})
        device = Controls(0, 0)
        self.assertEqual(regress_at.exercise_setting_steps(device), {"volume": 0, "gain": 0})

    def test_restore_waits_for_saved_and_clears_prior_error(self):
        device = Controls(40, 24, prior_error=-1)
        state = regress_at.restore_controls(device, {"volume": 73, "gain": 17})
        self.assertEqual(device.commands, ["AT+AUDIO STOP", "AT+VOL 73", "AT+MIC 17"])
        self.assertTrue(any(item["settings_pending"] for item in device.observations))
        self.assertEqual((state["volume"], state["gain"], state["settings_saved"],
                          state["settings_pending"], state["settings_error"]), (73, 17, 1, 0, 0))

    def test_restore_never_accepts_values_without_persistence(self):
        device = Controls(fail_save=True)
        with self.assertRaises(TimeoutError):
            regress_at.restore_controls(device, {"volume": 73, "gain": 17})

    def test_status_keeps_settings_error_separate(self):
        device = object.__new__(regress_at.Device)
        lines = ["+UI: page=0 selected=0 display_fault=0", "+AUDIO: ready=1 busy=0 error=0",
                 "+HEAP: integrity=1 free=12000", "+SETTINGS: loaded=1 saved=0 pending=1 error=-1"]
        device.command = lambda text: lines
        with self.assertRaises(AssertionError):
            device.status()
        state = device.status(allow_settings_error=True)
        self.assertEqual(state["error"], 0)
        self.assertEqual(state["settings_error"], -1)

    def test_both_main_finalizers_fail_when_restore_fails(self):
        for module, body, success in ((regress_at, "regress", "pass_result"),
                                      (regress_onboarding, "run", "passed")):
            for primary_failure in (False, True):
                with self.subTest(module=module.__name__, primary_failure=primary_failure), tempfile.TemporaryDirectory() as temp:
                    output = Path(temp) / "evidence"
                    device = Controls()
                    device.port = Port()
                    device.log = io.BytesIO()
                    def execute(_device, _args, result):
                        result["original_controls"] = {"volume": 73, "gain": 17}
                        if primary_failure:
                            raise ValueError("original test failure")
                        result[success] = True
                    arguments = [module.__file__, "TEST_PORT", "--mac", "00:00:00:00:00:01",
                                 "--elf-sha", "a" * 64, "--output", str(output)]
                    if module is regress_onboarding:
                        arguments += ["--device-id", "TEST_ID"]
                    with patch.object(sys, "argv", arguments), patch.object(module, "Device", return_value=device), \
                            patch.object(module, body, side_effect=execute), \
                            patch.object(module, "restore_controls", side_effect=TimeoutError("save failed")), \
                            contextlib.redirect_stdout(io.StringIO()):
                        expected = ValueError if primary_failure else RuntimeError
                        with self.assertRaises(expected):
                            module.main()
                    result = json.loads((output / "result.json").read_text(encoding="utf-8"))
                    self.assertFalse(result[success])
                    self.assertIn("save failed", result["settings_restore_failure"])
                    self.assertFalse(device.port.is_open)
                    self.assertTrue(device.log.closed)
                    if primary_failure:
                        self.assertIn("original test failure", result["failure"])

    def test_onboarding_checks_usb_after_persisted_restoration(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "evidence"
            device = Controls()
            device.port, device.log = Port(), io.BytesIO()
            device.command = lambda text: ["+SOF: disconnects=3 history=ffffffff"]
            def execute(_device, _args, result):
                result.update(passed=True, original_controls={"volume": 73, "gain": 17}, sof_baseline=2)
            arguments = [regress_onboarding.__file__, "TEST_PORT", "--mac", "00:00:00:00:00:01",
                         "--elf-sha", "a" * 64, "--output", str(output), "--device-id", "TEST_ID"]
            with patch.object(sys, "argv", arguments), patch.object(regress_onboarding, "Device", return_value=device), \
                    patch.object(regress_onboarding, "run", side_effect=execute), \
                    patch.object(regress_onboarding, "restore_controls", return_value={"settings_saved": 1}), \
                    contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(RuntimeError):
                    regress_onboarding.main()
            result = json.loads((output / "result.json").read_text(encoding="utf-8"))
            self.assertFalse(result["passed"])
            self.assertIn("disconnects=3", result["settings_restore_failure"])
            self.assertEqual(result["settings_restore"]["settings_saved"], 1)


if __name__ == "__main__":
    unittest.main()
