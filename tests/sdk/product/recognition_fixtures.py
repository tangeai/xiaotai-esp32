# SPDX-License-Identifier: MIT
"""Recognition query responses the platform sends, replayed by product_recognition_http.py.

Each case is (name, body, HTTP status, whether the device must accept it).
"""
import copy
import json


def fixtures():
    item = dict(item_id="item_1", order=1, scheme_id="fixture", scheme_version=1,
                output_mode="text", status="queued", result=None, error=None,
                speech=dict(status="skipped", error=None))
    basic = dict(code=200, data=dict(request_id="task_1", device_id="fixture-device",
                                   status_revision=1, status="queued", error=None, results=[item]))
    cases = [("queued", basic, 200, True)]

    def variant(name, modify, valid=False, status=200):
        value = copy.deepcopy(basic)
        modify(value)
        cases.append((name, value, status, valid))

    for state in ("processing", "failed", "unknown", "cancelled"):
        def modify(value, state=state):
            value["data"]["status"] = state
            value["data"]["results"][0]["status"] = state
            if state == "failed":
                value["data"]["error"] = dict(code=50200, msg="fixture failure")
                value["data"]["results"][0]["error"] = dict(code=50200, msg="fixture failure")
        variant(state, modify, True)
    for mode in ("text", "audio", "text_audio"):
        def modify(value, mode=mode):
            value["data"].update(status="succeeded", status_revision=3)
            value["data"]["results"][0].update(
                status="succeeded", output_mode=mode,
                result=None if mode == "audio" else dict(summary="result", alarm=False, labels=[], details={}),
                speech=dict(status="skipped", error=None) if mode == "text" else dict(
                    status="ready", url="/v1/recognition/requests/task_1/audio/item_1", format="wav", error=None))
        variant(mode, modify, True)
    large = copy.deepcopy(cases[-1][1])
    large["data"]["results"] = []
    for index in range(8):
        result = copy.deepcopy(cases[-1][1]["data"]["results"][0])
        result.update(item_id=f"item_{index+1}", order=index+1)
        result["speech"]["url"] = f"/v1/recognition/requests/task_1/audio/item_{index+1}"
        result["result"] = dict(summary="\u58f0" * 500, alarm=True, labels=["label"] * 32,
                                details={"long": "x" * 11000, "nested": [1, {"ok": True}]})
        large["data"]["results"].append(result)
    cases.append(("eight-large-results", large, 200, True))
    partial = copy.deepcopy(cases[-3][1])
    partial["data"].update(status="partial_failed", status_revision=4)
    partial["data"]["results"][0]["speech"] = dict(status="failed", error=dict(code=50200, msg="tts"))
    cases.append(("tts-failed", partial, 200, True))

    for name, key, value in (
        ("wrong-device", "device_id", "other"), ("wrong-id", "request_id", "other"),
        ("fractional-revision", "status_revision", 1.5), ("zero-revision", "status_revision", 0),
        ("unsafe-revision", "status_revision", 2**53), ("unknown-status", "status", "complete"),
        ("empty-results", "results", []), ("too-many-results", "results", [item] * 9),
        ("bad-error", "error", {"code": "50200", "msg": "bad"}),
    ):
        variant(name, lambda x, k=key, v=value: x["data"].update({k: v}))
    for name, key, value in (
        ("bad-item-id", "item_id", "bad/path"), ("bad-order", "order", 0),
        ("bad-version", "scheme_version", 0), ("bad-mode", "output_mode", "video"),
        ("missing-speech", "speech", None), ("premature-result", "result", {}),
    ):
        variant(name, lambda x, k=key, v=value: x["data"]["results"][0].update({k: v}))
    variant("duplicate-item", lambda x: x["data"]["results"].append(dict(item, order=2)))
    variant("business-error", lambda x: x.update(code=40300), status=403)
    variant("non-200-success", lambda x: None, status=202)
    variant("string-code", lambda x: x.update(code="200"))
    variant("missing-error", lambda x: x["data"].pop("error"))
    variant("unbounded-metadata", lambda x: x.update({f"key{i}": i for i in range(33)}))
    success = cases[7][1]  # text_audio
    for name, where, key, value in (
        ("external-audio", "speech", "url", "http://elsewhere/audio"),
        ("foreign-item-audio", "speech", "url", "/v1/recognition/requests/task_1/audio/item_2"),
        ("traversal-audio", "speech", "url", "/v1/recognition/requests/task_1/audio/../item_1"),
        ("wrong-format", "speech", "format", "mp3"),
        ("long-summary", "result", "summary", "x" * 501),
        ("empty-summary", "result", "summary", ""),
        ("too-many-labels", "result", "labels", ["x"] * 33),
        ("non-string-label", "result", "labels", [3]),
        ("array-details", "result", "details", []),
    ):
        value_json = copy.deepcopy(success)
        value_json["data"]["results"][0][where][key] = value
        cases.append((name, value_json, 200, False))
    encoded = [(name, json.dumps(value, ensure_ascii=False).encode(), status, valid)
               for name, value, status, valid in cases]
    simple = json.dumps(basic).encode()
    encoded += [
        ("duplicate-code", simple.replace(b'"code": 200', b'"code": 403, "code": 200'), 200, False),
        ("nul-identity", simple.replace(b'"task_1"', b'"task_1\\u0000hidden"'), 200, False),
        ("trailing", simple + b" garbage", 200, False),
        ("raw-nul", simple + b"\0", 200, False),
        ("too-deep", b"[" * 33 + b"0" + b"]" * 33, 200, False),
        ("broken-utf8", json.dumps(success).encode().replace(b'"result"', b'"\xc2"', 1), 200, False),
    ]
    for label, invalid in (("continuation", b"\x80"), ("overlong", b"\xc0\x80"),
                           ("surrogate", b"\xed\xa0\x80"), ("too-high", b"\xf4\x90\x80\x80"),
                           ("truncated", b"\xe4\xb8")):
        for field in (b'"labels": []', b'"details": {}'):
            replacement = b'"labels": ["' + invalid + b'"]' if b"labels" in field else (
                b'"details": {"nested": {"' + invalid + b'": "fixture"}}')
            encoded.append((label + field.decode(), json.dumps(success).encode().replace(field, replacement), 200, False))
    return encoded
