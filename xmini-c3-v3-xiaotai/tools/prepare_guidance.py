#!/usr/bin/env python3
"""Build bounded 8 kHz A-law prompts without modifying the UI font assets."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wav-dir", type=Path, required=True)
    parser.add_argument("--font", type=Path,
                        help="Legacy option, ignored; prepare_font.py owns the UI font assets")
    parser.add_argument("--ffmpeg", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    config = json.loads((root / "tools/guidance.json").read_text(encoding="utf-8"))
    directory = root / "components/xmini_audio/assets"
    directory.mkdir(exist_ok=True)
    bundle = bytearray()
    offsets = []
    records = []
    for clip in config["clips"]:
        source = args.wav_dir / (clip["name"] + ".wav")
        data = subprocess.check_output([args.ffmpeg, "-v", "error", "-i", str(source),
                                        "-ac", "1", "-ar", "8000", "-c:a", "pcm_alaw", "-f", "alaw", "pipe:1"])
        assert 0 < len(data) < 200000
        data += b"\xd5" * (-len(data) % 160)
        offsets.append((len(bundle), len(data)))
        bundle.extend(data)
        records.append({**clip, "offset": offsets[-1][0], "bytes": len(data),
                        "sha256": hashlib.sha256(data).hexdigest(),
                        "wav_sha256": hashlib.sha256(source.read_bytes()).hexdigest()})
    (directory / "guidance.alaw").write_bytes(bundle)
    table = "// Generated from tools/guidance.json; 8 kHz mono A-law, 160-byte aligned.\n"
    table += "static const struct { unsigned offset, bytes; } s_guidance[] = {\n"
    table += "".join(f"    {{{offset}, {length}}},\n" for offset, length in offsets)
    table += "};\n"
    (directory / "guidance_index.inc").write_text(table, encoding="ascii")
    (directory / "guidance_manifest.json").write_text(json.dumps({
        "voice": config["voice"], "rate": config["rate"], "sample_rate": 8000,
        "channels": 1, "codec": "pcm_alaw", "clips": records,
        "bundle_sha256": hashlib.sha256(bundle).hexdigest()}, ensure_ascii=False, indent=2), encoding="utf-8")

    print(json.dumps({"prompt_bytes": len(bundle), "clips": len(records)}))


if __name__ == "__main__":
    main()
