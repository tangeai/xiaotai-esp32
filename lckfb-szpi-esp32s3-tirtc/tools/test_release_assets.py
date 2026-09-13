#!/usr/bin/env python3
"""Validate the small set of source-controlled product assets."""

from __future__ import annotations

import hashlib
from pathlib import Path
import wave


PROJECT = Path(__file__).resolve().parents[1]
REPO = PROJECT.parent

WAV_ASSETS = {
    "components/starter_product/assets/rings/kids_watch_incoming_tiny.wav": (8000, 1, 2),
    "components/starter_product/assets/rings/kids_watch_outgoing_tiny.wav": (8000, 1, 2),
    "components/starter_product/assets/prompts/ai_ack_female_8k.wav": (8000, 1, 2),
    "components/starter_product/assets/prompts/ai_ack_male_8k.wav": (8000, 1, 2),
}

FORBIDDEN = (
    REPO / "docs/rings",
    REPO / "docs/xiao-tai-esp32s3-multinet7",
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

    source_model = REPO / "common/models/nihaoxiaotai_v9.3_voice_tflite/nihaoxiaotai.tflite"
    build_model = PROJECT / "components/starter_voice/model/nihaoxiaotai.tflite"
    source_head = REPO / "common/models/nihaoxiaotai_v9.3_voice_tflite/head.h"
    build_head = PROJECT / "components/starter_voice/model/head.h"
    for path in (source_model, build_model, source_head, build_head):
        assert path.is_file(), f"missing wake asset: {path.relative_to(REPO)}"
    assert digest(source_model) == digest(build_model), "wake model source/build copies differ"
    assert digest(source_head) == digest(build_head), "wake head source/build copies differ"

    for path in FORBIDDEN:
        assert not path.exists(), f"obsolete or duplicate asset returned: {path.relative_to(REPO)}"

    cmake = (PROJECT / "components/starter_product/CMakeLists.txt").read_text()
    for relative in WAV_ASSETS:
        embedded = relative.removeprefix("components/starter_product/")
        assert f'"{embedded}"' in cmake, f"audio is not embedded: {embedded}"
    voice_cmake = (PROJECT / "components/starter_voice/CMakeLists.txt").read_text()
    assert 'EMBED_FILES "model/nihaoxiaotai.tflite"' in voice_cmake

    print("release asset policy: PASS")


if __name__ == "__main__":
    main()
