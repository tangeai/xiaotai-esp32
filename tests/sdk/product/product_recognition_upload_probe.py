# SPDX-License-Identifier: MIT
# Replaces legacy: tests/regression/test_recognition_upload.py
"""Verify received multipart with a parser independent of the firmware writer."""
import email.policy
from email.parser import BytesParser
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run(executable):
    for case in ("success", "dns", "connect", "send", "eof", "body-reset", "body-stall", "empty-eof", "head-reset"):
        with tempfile.TemporaryDirectory() as tmp:
            capture = Path(tmp) / "upload.mime"
            result = subprocess.run(
                [executable, "recognition-upload-" + case],
                env=dict(os.environ, PRODUCT_UPLOAD_CAPTURE_PATH=str(capture)),
                capture_output=True, text=True, timeout=18,
            )
            assert result.returncode == 0, result.stdout + result.stderr
            evidence, = (line for line in result.stdout.splitlines() if line.startswith("UPLOAD_PROBE "))
            print(evidence, flush=True)
            if case in ("dns", "connect", "send"):
                assert not capture.exists(), case
                continue
            parts = list(BytesParser(policy=email.policy.default).parsebytes(capture.read_bytes()).iter_parts())
            names = [part.get_param("name", header="Content-Disposition") for part in parts]
            assert names == ["request_id", "config_revision", "media_type", "audio_format", "sample_rate",
                             "channels", "files"], names
            values = [part.get_payload(decode=True) for part in parts]
            request_id = evidence.partition("request_id=")[2].encode("ascii")
            assert values[:6] == [request_id, b"4", b"audio", b"pcm_s16le", b"16000", b"1"], values[:6]
            assert parts[-1].get_filename() == "recording.pcm", parts[-1].get_filename()
            assert parts[-1].get_content_type() == "application/octet-stream"
            assert values[-1] == bytes(i % 251 for i in range(32000))
            print(f"MIME {case} fields={','.join(names)} revision=4 pcm_s16le/16000/mono bytes=32000 "
                  f"sha256={hashlib.sha256(values[-1]).hexdigest()} match=all")


if __name__ == "__main__":
    run(sys.argv[1])
