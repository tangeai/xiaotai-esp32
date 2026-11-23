"""Validate the host probe against ThingConnect's read-only policy contract."""
# Replaces legacy: tests/test_recognition_probe.py
from contextlib import redirect_stderr, redirect_stdout
from email.message import Message
import importlib.util
import io
import json
from pathlib import Path
import unittest
from unittest.mock import patch


PROBE = Path(__file__).resolve().parent / "recognition/probe.py"
SPEC = importlib.util.spec_from_file_location("recognition_probe", PROBE)
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)

DISCOVERY = "http://discover.test/services"
ENDPOINT = "http://recognition.test"
TOKEN = "synthetic-test-token"


def policy(effective=True):
    return {
        "effective_enabled": effective,
        "policy": {
            "enabled": effective, "allowed": True, "revision": 4, "monthly_limit": 100,
            "schemes": [{"scheme_id": "audio-check", "scheme_version": 2}] if effective else [],
        },
        "upload_limits": {"max_file_bytes": 50_000, "max_audio_seconds": 15} if effective else None,
        "server_extension": "not echoed",
    }


def envelope(data):
    return {"code": 200, "msg": "any server wording", "data": data}


class HttpReply:
    def __init__(self, status, data, content="application/json"):
        self.status = status
        self.payload = json.dumps(data).encode("utf-8")
        self.headers = Message()
        self.headers["Content-Type"] = content
        self.headers["Location"] = "http://must-not-be-contacted.test"

    def __enter__(self):
        return self

    def __exit__(self, *_):
        return False

    def read(self, limit):
        return self.payload[:limit]


class ProbeTest(unittest.TestCase):
    def cli(self, args, replies):
        calls = []
        responses = iter(replies)
        case = self

        class Connection:
            def __init__(self, host, port, *, timeout):
                case.assertEqual(timeout, 5)
                self.host = host

            def request(self, method, path, *, body, headers):
                calls.append((self.host, method, path, body, headers))

            def getresponse(self):
                response = next(responses)
                if isinstance(response, Exception):
                    raise response
                return HttpReply(*response)

            def close(self):
                pass

        stdout, stderr = io.StringIO(), io.StringIO()
        with (
            patch.object(probe.http.client, "HTTPConnection", Connection),
            patch.object(probe.http.client, "HTTPSConnection", side_effect=AssertionError("HTTP policy")),
            patch.object(probe.sys, "argv", [str(PROBE), *args]),
            patch.dict(probe.os.environ, {"XIAOTAI_TEST_DEVICE_TOKEN": TOKEN}),
            redirect_stdout(stdout),
            redirect_stderr(stderr),
        ):
            rc = probe.main()
        report = (stdout if rc == 0 else stderr).getvalue()
        self.assertNotIn(TOKEN, report)
        return rc, json.loads(report), calls

    def test_direct_read_no_task_id_or_upload(self):
        code, report, calls = self.cli(
            ["--endpoint", ENDPOINT, "--config-only"], [(200, envelope(policy()))],
        )
        self.assertEqual(code, 0)
        self.assertEqual(report["policy_revision"], 4)
        self.assertEqual(report["selected_scheme_count"], 1)
        self.assertNotIn("server_extension", report)
        self.assertEqual(calls, [
            ("recognition.test", "GET", probe.CONFIG, None,
             {"Authorization": f"Bearer {TOKEN}", "Accept": "application/json"}),
        ])

    def test_discovery_has_no_credentials_and_accepts_disabled_policy(self):
        code, report, calls = self.cli(
            ["--discovery-url", DISCOVERY, "--config-only"],
            [(200, {"recognition-srv": ENDPOINT}), (200, envelope(policy(False)))],
        )
        self.assertEqual(code, 0)
        self.assertIs(report["effective_enabled"], False)
        self.assertEqual(calls[0], ("discover.test", "GET", "/services", None, {"Accept": "application/json"}))
        self.assertEqual(calls[1][0:4], ("recognition.test", "GET", probe.CONFIG, None))
        self.assertEqual(calls[1][4]["Authorization"], f"Bearer {TOKEN}")

    def test_bad_discovery_never_sends_the_bearer(self):
        for status, payload in (
            (200, {}), (200, {"recognition-srv": "https://recognition.test"}),
            (200, {"recognition-srv": "http://someone@recognition.test/"}),
            (200, {"recognition-srv": "http://recognition.test:65536"}),
            (200, {"recognition-srv": "http://recognition.test:0"}),
            (200, {"recognition-srv": "http://recognition.test/?token=hidden"}),
            (302, {}),
        ):
            with self.subTest(status=status, payload=payload):
                code, _, calls = self.cli(
                    ["--discovery-url", DISCOVERY, "--config-only"], [(status, payload)],
                )
                self.assertEqual(code, 1)
                self.assertEqual(len(calls), 1)
                self.assertNotIn("Authorization", calls[0][4])

    def test_invalid_policy_cannot_be_treated_as_enabled(self):
        for field, value in (
            ("effective_enabled", 1), ("upload_limits", {"max_audio_seconds": True}),
            ("policy", {"enabled": True, "allowed": False, "revision": 4, "monthly_limit": 100,
                        "schemes": [{"scheme_id": "check", "scheme_version": 1}]}),
        ):
            with self.subTest(field=field):
                data = policy()
                data[field] = value
                code, _, calls = self.cli(
                    ["--endpoint", ENDPOINT, "--config-only"], [(200, envelope(data))],
                )
                self.assertEqual(code, 1)
                self.assertEqual(len(calls), 1)

    def test_errors_do_not_redirect_or_trigger_other_requests(self):
        for status, code in ((302, 200), (401, 401), (410, 6006)):
            with self.subTest(status=status):
                rc, report, calls = self.cli(
                    ["--endpoint", ENDPOINT, "--config-only"],
                    [(status, {"code": code, "msg": "should not be echoed"})],
                )
                self.assertEqual(rc, 1)
                self.assertEqual(len(calls), 1)
                self.assertNotIn("should not be echoed", report["error"])
                if status >= 400:
                    self.assertIn(str(code), report["error"])

    def test_submit_still_rechecks_policy_and_preserves_request_id(self):
        args = ["--endpoint", ENDPOINT, "--device-id", "test-board",
                "--request-id", "one-request", "--submit-synthetic"]
        for data in (policy(False), {**policy(), "upload_limits": {
                "max_file_bytes": 32_043, "max_audio_seconds": 1}}):
            rc, _, calls = self.cli(args, [(200, envelope(data))])
            self.assertEqual(rc, 1)
            self.assertEqual(len(calls), 1)
        task = {"device_id": "test-board", "request_id": "one-request",
                "status_revision": 2, "status": "succeeded"}
        rc, _, calls = self.cli(args, [(200, envelope(policy())), TimeoutError(), (200, envelope(task))])
        self.assertEqual(rc, 0)
        self.assertEqual([(call[1], call[2]) for call in calls], [
            ("GET", probe.CONFIG), ("POST", probe.REQUESTS), ("GET", probe.REQUESTS + "/one-request"),
        ])
        self.assertIn(b'one-request', calls[1][3])

    def test_other_schemes_are_not_rewritten_or_contacted(self):
        for url in ("https://recognition.test", "file:///etc/passwd", "http://host\\evil",
                    "http://user:pass@host", "http://host/path", "http://host/#fragment"):
            with self.subTest(url=url):
                rc, _, calls = self.cli(["--endpoint", url, "--config-only"], [])
                self.assertEqual(rc, 1)
                self.assertEqual(calls, [])

    def test_http_keeps_explicit_service_port(self):
        endpoint = probe.http_url("http://recognition.test:8080", {"", "/"}, "bad")
        self.assertEqual(endpoint.port, 8080)
        with patch.object(probe.http.client, "HTTPConnection") as connection:
            connection.return_value.getresponse.return_value = HttpReply(200, {})
            self.assertEqual(probe.json_http(endpoint, "GET", "/services", {}), (200, {}))
        connection.assert_called_once_with("recognition.test", 8080, timeout=5)


if __name__ == "__main__":
    unittest.main(verbosity=2)
