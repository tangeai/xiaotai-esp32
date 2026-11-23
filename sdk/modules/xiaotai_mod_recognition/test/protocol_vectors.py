# SPDX-License-Identifier: MIT
"""Independent JSON and MIME fixtures for the real recognition protocol."""
import copy
from email import policy
from email.parser import BytesParser
import json
from pathlib import Path
import subprocess
import sys
import tempfile

INVALID = 0x108
NO_MEM = 0x101
NOT_FOUND = 0x105
TASK = "task_1"
DEVICE = "fixture-device"
MAX_REVISION = 2**53 - 1


def multipart(executable):
    message = BytesParser(policy=policy.default).parsebytes(subprocess.check_output([executable]))
    assert message.is_multipart() and not message.defects
    assert message.get_boundary() == "xiaotai-00010203040506070809abcdef000000"
    parts = list(message.iter_parts())
    assert len(parts) == 7 and all(not part.defects for part in parts)
    expected = [
        ("request_id", b"task_1"), ("config_revision", b"4"), ("media_type", b"audio"),
        ("audio_format", b"pcm_s16le"), ("sample_rate", b"8000"), ("channels", b"1"),
    ]
    for part, (name, data) in zip(parts, expected):
        assert part.get_param("name", header="content-disposition") == name
        assert part.get_payload(decode=True) == data and part.get_filename() is None
    assert parts[-1].get_param("name", header="content-disposition") == "files"
    assert parts[-1].get_filename() == "recording.pcm"
    assert parts[-1].get_content_type() == "application/octet-stream"
    assert parts[-1].get_payload(decode=True) == bytes(i % 251 for i in range(16000))


def encode(envelope):
    return json.dumps(envelope, ensure_ascii=False, separators=(",", ":")).encode()


def task_vectors(executable):
    with tempfile.TemporaryDirectory(prefix="xiaotai-recognition-protocol-") as tmp:
        path = Path(tmp) / "service.json"

        def run(body, error=0, code=200, http=200, fail=0, kind="query", request=TASK):
            path.write_bytes(encode(body) if isinstance(body, dict) else body)
            cmd = [executable, str(path), str(http), str(error), str(code), str(fail), kind, request]
            result = subprocess.run(cmd, capture_output=True, check=False, timeout=15)
            assert result.returncode == 0, (body if isinstance(body, bytes) and len(body) < 512 else cmd,
                                            result.stderr.decode("utf-8", errors="replace"))
            return result.stdout.decode()

        item = dict(item_id="item_1", order=1, scheme_id="fixture", scheme_version=1,
                    output_mode="text", status="queued", result=None, error=None,
                    speech=dict(status="skipped", error=None))
        simple = dict(code=200, data=dict(request_id=TASK, device_id=DEVICE, status_revision=1,
                                         status="queued", error=None, results=[item]))
        pristine = encode(simple)
        assert run(simple) == "1 1 0\nitem_1 0 0 5 0 0\n"
        for i, state in enumerate(("processing", "succeeded", "partial_failed", "failed", "unknown", "cancelled"), 1):
            sample = copy.deepcopy(simple)
            sample["data"]["status"] = state
            assert run(sample).splitlines()[0] == f"1 1 {i}"
        success = copy.deepcopy(simple)
        success["data"].update(status="succeeded", status_revision=MAX_REVISION)
        text = dict(summary="\u58f0\U0001f4ac", alarm=True, labels=["label"], details={})
        voice = dict(status="ready", error=None, format="wav", url=f"/v1/recognition/requests/{TASK}/audio/item_1")
        result = success["data"]["results"][0]
        result.update(status="succeeded", result=text, output_mode="text_audio", speech=voice)
        rendered = f"{MAX_REVISION} 1 2\nitem_1 2 2 2 0 0\n7:{text['summary']}\n"
        assert run(success) == rendered
        assert run(json.dumps(success, ensure_ascii=True).encode()) == rendered
        text_only = copy.deepcopy(success)
        text_only["data"]["results"][0].update(output_mode="text", speech=dict(status="skipped", error=None))
        assert "item_1 2 0 5 0 0" in run(text_only)
        audio_only = copy.deepcopy(success)
        audio_only["data"]["results"][0].update(output_mode="audio", result=None)
        assert "item_1 2 1 2 0 0" in run(audio_only)
        partial = copy.deepcopy(success)
        partial["data"]["results"][0]["speech"] = dict(status="failed", error=dict(code=50200, msg="tts"))
        assert "item_1 2 2 3 0 50200" in run(partial)
        large = copy.deepcopy(success)
        large["data"]["results"] = [
            dict(result, item_id=f"id_{i}", order=i,
                 speech=dict(voice, url=f"/v1/recognition/requests/{TASK}/audio/id_{i}"),
                 result=dict(summary="\u58f0" * 500, alarm=True, labels=["label"] * 32, details={"long": "x" * 11000}))
            for i in range(1, 9)
        ]
        assert run(large).count("1500:" + "\u58f0" * 500) == 8
        for depth in (16, 17, 32, 33):
            sample = copy.deepcopy(success)
            details = {}
            highest = details
            # Root, data, results, item, result, details occupy six levels.
            for _ in range(depth - 6):
                highest["nested"] = {}
                highest = highest["nested"]
            sample["data"]["results"][0]["result"]["details"] = details
            run(sample, error=0 if depth <= 32 else INVALID, code=200 if depth <= 32 else -1)

        for key, value in (
            ("device_id", "other"), ("request_id", "other"), ("status", "complete"), ("status_revision", 1.5),
            ("status_revision", 0), ("status_revision", 2**53), ("results", []), ("results", [item] * 9),
            ("results", [item, dict(item, order=2)]), ("error", {"code": "3", "msg": "x"}),
        ):
            sample = copy.deepcopy(simple)
            sample["data"][key] = value
            run(sample, INVALID)
        for key, value in (
            ("item_id", "bad/path"), ("order", 0), ("scheme_version", 0), ("scheme_id", ""),
            ("output_mode", "video"), ("speech", None), ("result", {}),
            ("speech", dict(status="pending", error=None)),
        ):
            sample = copy.deepcopy(simple)
            sample["data"]["results"][0][key] = value
            run(sample, INVALID)
        for section, key, value in (
            ("speech", "url", "http://elsewhere/audio"),
            ("speech", "url", "/v1/recognition/requests/task_1/audio/item_2"),
            ("speech", "url", "/v1/recognition/requests/task_1/audio/../item_1"),
            ("speech", "format", "mp3"), ("result", "summary", "x" * 501),
            ("result", "summary", ""), ("result", "labels", ["label"] * 33),
            ("result", "labels", [3]), ("result", "details", []),
        ):
            sample = copy.deepcopy(success)
            sample["data"]["results"][0][section][key] = value
            run(sample, INVALID)
        run(pristine.replace(b'"code":200', b'"code":40901,"co\\u0064e":200'), INVALID, code=-1)
        run(pristine.replace(b'"order":1', b'"ord\\u0065r":1,"order":1'), INVALID)
        for bad in (b"\x80", b"\xc0\x80", b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\xe4\xb8"):
            run(encode(success).replace(b'"details":{}', b'"details":{"' + bad + b'":0}'), INVALID, code=-1)
            run(encode(success).replace(b'"labels":["label"]', b'"labels":["' + bad + b'"]'), INVALID, code=-1)
        for bad in (pristine + b"\0", pristine + b" trailing",
                    pristine.replace(b'"task_1"', b'"task_1\\u0000hidden"'),
                    pristine.replace(b'"task_1"', b'"task_1\\ud800"')):
            run(bad, INVALID, code=-1)
        run(dict(simple, **{f"key{i}": i for i in range(33)}), INVALID, code=-1)
        run(simple, INVALID, http=202)
        run({"code": 6006}, INVALID, code=6006, http=410)
        run(b" " * (1024 * 1024 + 1), 0x104, code=-1)
        for fail in (1, 2, 3):  # token table, task index, retained body; all clean up
            run(simple, NO_MEM, code=-1 if fail == 1 else 200, fail=fail)
        run(simple, NO_MEM, code=-1, fail=-1)  # heap headroom
        run(simple, NO_MEM, code=-1, fail=-2)  # largest continuous block
        run(simple, fail=-3)  # synthetic heap watermarks sufficient
        sample = copy.deepcopy(success)
        sample["data"]["results"][0]["result"]["summary"] = "z" * 200
        run(sample, NO_MEM, code=200, fail=3)  # summary decode scratch

        notice = dict(type="recognition_result", channel="recognition", event_id="event-1", payload=success)
        assert run(notice, code=-1, kind="notification") == rendered
        conf = dict(notice, type="recognition_config_changed",
                    payload=dict(code=200, data=dict(device_id=DEVICE, revision=MAX_REVISION)))
        assert run(conf, code=-1, kind="notification", request="") == f"revision={MAX_REVISION}\n"
        for bad in (dict(notice, channel="other"), dict(notice, event_id=""), dict(notice, type="other"),
                    dict(notice, payload=dict(success, code=403)),
                    dict(conf, payload=dict(code=200, data=dict(device_id=DEVICE, revision=0))),
                    encode(notice).replace(b'"event_id":', b'"event\\u005fid":"other","event_id":')):
            run(bad, INVALID, code=-1, kind="notification")
        other = dict(conf, payload=dict(code=200, data=dict(device_id="other", revision=1)))
        run(other, NOT_FOUND, code=-1, kind="notification")
        run(notice, NOT_FOUND, code=-1, kind="notification", request="other")


if __name__ == "__main__":
    {"multipart": multipart, "task": task_vectors}[sys.argv[1]](sys.argv[2])
