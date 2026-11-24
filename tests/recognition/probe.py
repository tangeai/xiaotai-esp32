#!/usr/bin/env python3
"""Read device recognition config or exercise tasks using synthetic PCM only."""

import argparse
import http.client
import json
import os
import re
import secrets
import struct
import sys
import time
from urllib.parse import urlsplit

REQUESTS = "/v1/recognition/requests"
CONFIG = "/v1/recognition/config"
SAMPLE_RATE = 16000
MAX_REPLY = 128 * 1024
PENDING = {"queued", "processing"}
TERMINAL = {"succeeded", "partial_failed", "failed", "unknown", "cancelled"}


class ProbeError(Exception):
    pass


class AmbiguousResponse(ProbeError):
    """The server may have processed the request before the reply was lost."""


def http_url(value, paths, error):
    if (not isinstance(value, str) or not value
            or any(char.isspace() or ord(char) < 32 for char in value)
            or any(char in value for char in "?#\\")):
        raise ProbeError(error)
    try:
        url = urlsplit(value)
        port = url.port
    except ValueError:
        raise ProbeError(error) from None
    if (url.scheme != "http" or not url.hostname or url.username is not None
            or port == 0 or url.path not in paths):
        raise ProbeError(error)
    return url


def json_http(url, method, path, headers, body=None):
    connection = http.client.HTTPConnection(url.hostname, url.port, timeout=5)
    try:
        connection.request(method, path, body=body, headers=headers)
        with connection.getresponse() as response:
            http_status = response.status
            if 300 <= http_status < 400:
                raise ProbeError("redirect refused")
            raw = response.read(MAX_REPLY + 1)
            if len(raw) > MAX_REPLY:
                raise ProbeError("response exceeds size limit")
            if response.headers.get_content_type() != "application/json":
                raise ProbeError(f"expected JSON response (HTTP {http_status})")
    except (TimeoutError, ConnectionError, http.client.IncompleteRead) as error:
        raise AmbiguousResponse(type(error).__name__) from None
    except http.client.HTTPException as error:
        raise ProbeError(f"invalid HTTP response: {type(error).__name__}") from None
    finally:
        connection.close()
    try:
        return http_status, json.loads(raw)
    except (ValueError, UnicodeError):
        raise ProbeError(f"invalid JSON response (HTTP {http_status})") from None


def discover_endpoint(discovery_url):
    discovery = http_url(discovery_url, {"/services"}, "expected a configured HTTP /services URL")
    status, data = json_http(discovery, "GET", discovery.path, {"Accept": "application/json"})
    if status != 200 or not isinstance(data, dict):
        raise ProbeError(f"invalid service discovery (HTTP {status})")
    endpoint = data.get("recognition-srv")
    if not endpoint:
        raise ProbeError("service discovery does not advertise recognition-srv")
    http_url(endpoint, {"", "/"}, "discovery did not advertise a valid HTTP recognition origin")
    return endpoint


class RecognitionProbe:
    def __init__(self, endpoint, token):
        self.url = http_url(endpoint, {"", "/"}, "expected an explicit HTTP service origin")
        if not re.fullmatch(r"[A-Za-z0-9._~-]{1,4096}", token or ""):
            raise ProbeError("a test device token is required")
        self.token = token

    def request(self, method, path, expected, *, body=None, media=None):
        headers = {"Authorization": "Bearer " + self.token, "Accept": "application/json"}
        if media:
            headers["Content-Type"] = media
        http_status, envelope = json_http(self.url, method, path, headers, body)
        code = envelope.get("code") if isinstance(envelope, dict) else None
        if http_status not in expected or type(code) is not int or code != 200:
            raise ProbeError(f"{method} failed: HTTP {http_status}, code "
                             f"{code if type(code) is int else 'invalid'}")
        data = envelope.get("data")
        if not isinstance(data, dict):
            raise ProbeError("missing response data")
        return http_status, data


def read_config(client):
    _, config = client.request("GET", CONFIG, {200})
    policy = config.get("policy")
    effective = config.get("effective_enabled")
    if not isinstance(policy, dict) or type(effective) is not bool:
        raise ProbeError("missing policy or effective_enabled")
    enabled, allowed = policy.get("enabled"), policy.get("allowed")
    revision, monthly_limit = policy.get("revision"), policy.get("monthly_limit")
    schemes = policy.get("schemes")
    if (type(enabled) is not bool or type(allowed) is not bool or type(revision) is not int
            or revision < 0 or type(monthly_limit) is not int or monthly_limit < 0
            or not isinstance(schemes, list) or len(schemes) > 8):
        raise ProbeError("invalid recognition policy")
    for scheme in schemes:
        if (not isinstance(scheme, dict) or not isinstance(scheme.get("scheme_id"), str)
                or not scheme["scheme_id"] or type(scheme.get("scheme_version")) is not int
                or scheme["scheme_version"] < 1):
            raise ProbeError("invalid recognition scheme reference")
    limits = config.get("upload_limits")
    limit_keys = ("max_file_bytes", "max_audio_seconds")
    if effective and (not enabled or not allowed or not monthly_limit or not schemes):
        raise ProbeError("effective_enabled contradicts the recognition policy")
    if effective or limits is not None:
        minimum = 1 if effective else 0
        if (not isinstance(limits, dict) or any(
                type(limits.get(key)) is not int or limits[key] < minimum
                for key in limit_keys)):
            raise ProbeError("invalid recognition upload limits")
    return {
        "effective_enabled": effective,
        "policy_enabled": enabled,
        "policy_allowed": allowed,
        "policy_revision": revision,
        "monthly_limit": monthly_limit,
        "selected_scheme_count": len(schemes),
        "upload_limits": {key: limits[key] for key in limit_keys} if limits is not None else None,
    }


def multipart(request_id, revision, pcm):
    boundary = secrets.token_hex(24)
    parts = []
    fields = (
        ("request_id", request_id), ("config_revision", str(revision)),
        ("media_type", "audio"), ("audio_format", "pcm_s16le"),
        ("sample_rate", str(SAMPLE_RATE)), ("channels", "1"),
    )
    for name, value in fields:
        parts.append(f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\""
                     f"\r\n\r\n{value}\r\n".encode("ascii"))
    parts.extend((
        f"--{boundary}\r\nContent-Disposition: form-data; name=\"files\"; filename=\"probe.pcm\""
        "\r\nContent-Type: application/octet-stream\r\n\r\n".encode("ascii"),
        pcm,
        f"\r\n--{boundary}--\r\n".encode("ascii"),
    ))
    return b"".join(parts), f"multipart/form-data; boundary={boundary}"


def task_snapshot(data, device_id, request_id):
    if data.get("device_id") != device_id or data.get("request_id") != request_id:
        raise ProbeError("response belongs to another device or request")
    revision, status = data.get("status_revision"), data.get("status")
    if type(revision) is not int or revision < 1:
        raise ProbeError("invalid task revision")
    if not isinstance(status, str) or status not in PENDING | TERMINAL:
        raise ProbeError("unknown task status")
    return revision, status


def prepare_submission(client, request_id):
    config = read_config(client)
    if not config["effective_enabled"]:
        raise ProbeError("recognition not enabled for this test device")
    limits = config["upload_limits"]

    # Exactly one second of signed PCM16, not a microphone or a file upload.
    pcm = struct.pack("<hhhh", -1200, -400, 400, 1200) * (SAMPLE_RATE // 4)
    if limits["max_file_bytes"] < len(pcm) + 44:
        raise ProbeError("policy does not allow the synthetic sample")
    return multipart(request_id, config["policy_revision"], pcm)


def run_probe(client, device_id, request_id, *, submit, query_count=5, query_interval=1.0):
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", request_id):
        raise ProbeError("invalid request identifier")
    http_status, latest_revision, latest_status = None, 0, None
    if submit:
        body, media = prepare_submission(client, request_id)
        try:
            http_status, task = client.request("POST", REQUESTS, {200, 202}, body=body, media=media)
            latest_revision, latest_status = task_snapshot(task, device_id, request_id)
        except AmbiguousResponse:
            # Never POST again automatically; a lost reply does not mean a lost task.
            pass
    for index in range(query_count):
        if latest_status in TERMINAL:
            break
        if index:
            time.sleep(query_interval)
        _, task = client.request("GET", f"{REQUESTS}/{request_id}", {200})
        revision, status = task_snapshot(task, device_id, request_id)
        if revision > latest_revision:
            latest_revision, latest_status = revision, status
    return {"submit_http": http_status, "status": latest_status, "status_revision": latest_revision}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--endpoint", help="explicit HTTP origin of the test service")
    source.add_argument("--discovery-url", help="configured HTTP /services URL; no bearer sent to it")
    parser.add_argument("--device-id", help="bound test device ID for task operations")
    parser.add_argument("--request-id", help="unique ID for this analysis attempt")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--submit-synthetic", action="store_true",
                      help="explicitly allow one synthetic audio submission")
    mode.add_argument("--query-only", action="store_true", help="read an existing task without uploading")
    mode.add_argument("--config-only", action="store_true", help="read policy without audio or task operations")
    args = parser.parse_args()
    if not args.config_only and (not args.device_id or not args.request_id):
        parser.error("task operations require --device-id and --request-id")
    try:
        endpoint = args.endpoint or discover_endpoint(args.discovery_url)
        client = RecognitionProbe(endpoint, os.environ.get("XIAOTAI_TEST_DEVICE_TOKEN"))
        if args.config_only:
            print(json.dumps(read_config(client)))
            return 0
        result = run_probe(client, args.device_id, args.request_id, submit=args.submit_synthetic)
    except (ProbeError, ValueError, OSError) as error:
        print(json.dumps({"passed": False, "error": str(error)}), file=sys.stderr)
        return 1
    print(json.dumps(result))
    return 0 if result["status"] == "succeeded" else 2


if __name__ == "__main__":
    raise SystemExit(main())
