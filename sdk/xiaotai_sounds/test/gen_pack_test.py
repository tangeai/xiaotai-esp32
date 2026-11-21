# SPDX-License-Identifier: MIT
"""Check deterministic output and clear failures before an asset reaches C."""

import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(sys.argv[1]).resolve()
TOOL = ROOT / "tools" / "gen_pack.py"


def invoke(manifest, output, mode="host", expect=None):
    result = subprocess.run([sys.executable, "-B", str(TOOL), "--mode", mode,
                             str(manifest), str(output)], capture_output=True, text=True)
    if expect is None:
        assert result.returncode == 0, result.stderr
        return output.read_bytes()
    assert result.returncode == 1 and expect in result.stderr, result.stderr
    assert not output.exists(), "a rejected pack created a C table"


def malformed_pack(work):
    directory = work / "fixture"
    directory.mkdir()
    manifest = directory / "pack.json"
    output = directory / "out.c"
    raw = directory / "xt_sound_fixture_law.alaw"

    def emit(data, offset=0, length=None, sha=None, clip_patch=None):
        raw.write_bytes(data)
        clip = data[offset:] if length is None else data[offset:offset + length]
        h = lambda value: hashlib.sha256(value).hexdigest()
        entry = {"name": "guide.test", "file": raw.name, "encoding": "g711a", "container": "raw",
                 "sha256": h(clip), "offset": offset, "length": len(clip) if length is None else length,
                 "rate_hz": 8000, "channels": 1, "duration_ms": 20}
        if clip_patch:
            entry.update(clip_patch)
        manifest.write_text(json.dumps({"name": "fixture", "files": {raw.name: sha or h(data)},
                                        "sounds": [entry]}), encoding="utf-8")

    emit(bytes(160), sha="0" * 64)
    invoke(manifest, output, expect="SHA-256 mismatch")
    emit(bytes(160), clip_patch={"sha256": "0" * 64})
    invoke(manifest, output, expect="clip: SHA-256 mismatch")
    emit(bytes(160), offset=200, length=160)
    invoke(manifest, output, expect="offset: expected integer")
    emit(bytes(160), offset=0, length=320)
    invoke(manifest, output, expect="extends past end")
    emit(bytes(159))
    invoke(manifest, output, expect="align to 20 ms")
    emit(bytes(320), offset=1, length=160)
    invoke(manifest, output, expect="align to 20 ms")
    emit(bytes(160), clip_patch={"duration_ms": 21})
    invoke(manifest, output, expect="duration does not match")
    emit(b"\x03\x00xy", clip_patch={"container": "packets", "encoding": "opus", "packet_count": 1,
                                      "packet_ms": 20})
    invoke(manifest, output, expect="past clip end")
    emit(bytes(44), clip_patch={"container": "wav", "encoding": "pcm16"})
    invoke(manifest, output, expect="invalid RIFF/WAVE header")
    emit(bytes(160))
    extra = directory / "xt_sound_fixture_extra.alaw"
    extra.write_bytes(bytes(160))
    invoke(manifest, output, expect="do not match the files selected for embedding")
    extra.unlink()
    embedded = invoke(manifest, output, mode="idf")
    assert b".data = _binary_xt_sound_fixture_law_alaw_start + 0, .size = 160" in embedded


def real_packs(work):
    for directory in sorted((ROOT / "packs").iterdir()):
        manifest = directory / "pack.json"
        info = json.loads(manifest.read_text(encoding="utf-8"))
        host = invoke(manifest, work / "first.c")
        assert invoke(manifest, work / "second.c") == host
        idf = invoke(manifest, work / "idf.c", mode="idf")
        assert idf == invoke(manifest, work / "idf_again.c", mode="idf")
        assert b"0x" in host and b"0x" not in idf
        prefix = f"xiaotai_sound_pack_{info['name']}".encode("ascii")
        assert prefix in host and prefix in idf
        for entry in info["sounds"]:
            symbol = "_binary_" + entry["file"].replace(".", "_") + "_start"
            binary = directory / entry["file"]
            location = f"{symbol} + {entry.get('offset', 0)}, .size = {entry.get('length', binary.stat().st_size)}"
            assert location.encode("ascii") in idf, entry["name"]


with tempfile.TemporaryDirectory(prefix="xiaotai-sounds-test-") as temp:
    work = Path(temp)
    malformed_pack(work)
    real_packs(work)
