#!/usr/bin/env python3
"""Validate the small set of source-controlled product assets."""

from __future__ import annotations

import hashlib
from pathlib import Path
import wave


PROJECT = Path(__file__).resolve().parents[1]

WAV_ASSETS = {
    "components/starter_product/assets/rings/kids_watch_incoming_tiny.wav": (8000, 1, 2),
    "components/starter_product/assets/rings/kids_watch_outgoing_tiny.wav": (8000, 1, 2),
    "components/starter_product/assets/prompts/ai_ack_female_8k.wav": (8000, 1, 2),
    "components/starter_product/assets/prompts/ai_ack_male_8k.wav": (8000, 1, 2),
}

FORBIDDEN = (
    PROJECT / "docs/rings",
    PROJECT / "docs/xiao-tai-esp32s3-multinet7",
    PROJECT / "components/starter_voice/assets/multinet7",
)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    for relative, expected in WAV_ASSETS.items():
        path = PROJECT / relative
        assert path.is_file(), f"missing product audio: {relative}"
        with wave.open(str(path), "rb") as wav:
            actual = (wav.getframerate(), wav.getnchannels(), wav.getsampwidth())
        assert actual == expected, f"unexpected WAV format for {relative}: {actual}"
        assert path.read_bytes()[36:40] == b"data", f"non-canonical WAV header: {relative}"

    expected_models = {
        "head.h": "cc0364fa603a43c90d9de0b0fc39587c7c4d3350c3d1537b7018cf3a923d1a1e",
        "nihaoxiaotai.tflite": "1dcfe29a10733ec272854bfbafb8a231f10bf3b0399cf6192cc08b38006fac78",
    }
    for name, expected in expected_models.items():
        path = PROJECT / "components/starter_voice/model" / name
        assert path.is_file(), f"missing wake asset: {name}"
        assert digest(path) == expected, f"wake asset differs from pinned v9.3-20260915-nhwc: {name}"

    for path in FORBIDDEN:
        assert not path.exists(), f"obsolete or duplicate asset returned: {path.relative_to(PROJECT)}"

    cmake = (PROJECT / "components/starter_product/CMakeLists.txt").read_text()
    for relative in WAV_ASSETS:
        embedded = relative.removeprefix("components/starter_product/")
        assert f'"{embedded}"' in cmake, f"audio is not embedded: {embedded}"
    voice_cmake = (PROJECT / "components/starter_voice/CMakeLists.txt").read_text()
    assert 'EMBED_FILES "model/nihaoxiaotai.tflite"' in voice_cmake

    print("release asset policy: PASS")


if __name__ == "__main__":
    main()
