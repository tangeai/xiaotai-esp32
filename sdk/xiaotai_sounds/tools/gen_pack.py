# SPDX-License-Identifier: MIT
"""Verify one sound pack and emit a reproducible C table for host or ESP-IDF."""

import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path


NAME = re.compile(r"[a-z0-9_]+(?:\.[a-z0-9_]+)*")
PACK_NAME = re.compile(r"[a-z][a-z0-9_]*")
HASH = re.compile(r"[a-f0-9]{64}")
CODECS = {"pcm16", "g711a", "g711u", "opus"}
MAX_SOUND_BYTES = 10 * 1024 * 1024
SPOKEN_PACKS = {"guide_zh_alaw8k"}


def verify_hash(data, expected, label):
    if not isinstance(expected, str) or not HASH.fullmatch(expected):
        raise ValueError(f"{label}: invalid SHA-256 field")
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError(f"{label}: SHA-256 mismatch")


def integer(entry, key, low, high, default=None):
    value = entry.get(key, default)
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f"{key}: expected integer {low}..{high}")
    return value


def wave_frames(data, rate, channels):
    if len(data) < 44 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("invalid RIFF/WAVE header")
    if struct.unpack_from("<I", data, 4)[0] + 8 != len(data):
        raise ValueError("RIFF length does not match clip length")
    cursor = 12
    frame_bytes = frames = 0
    while cursor < len(data):
        if len(data) - cursor < 8:
            raise ValueError("truncated WAV chunk header")
        tag, length = struct.unpack_from("<4sI", data, cursor)
        cursor += 8
        if length + (length & 1) > len(data) - cursor:
            raise ValueError("truncated WAV chunk or padding")
        if tag == b"fmt ":
            if frame_bytes or length < 16:
                raise ValueError("duplicate or short WAV fmt chunk")
            fmt, ch, hz, byte_rate, align, bits = struct.unpack_from("<HHIIHH", data, cursor)
            if (fmt != 1 or bits != 16 or ch != channels or hz != rate or ch not in (1, 2)
                    or not 8000 <= hz <= 48000 or align != ch * 2 or byte_rate != hz * align):
                raise ValueError("WAV fmt does not match the documented PCM16 format")
            frame_bytes = align
        elif tag == b"data":
            if not frame_bytes or frames or not length or length % frame_bytes:
                raise ValueError("missing fmt, duplicate data or incomplete WAV frame")
            frames = length // frame_bytes
        cursor += length + (length & 1)
    if not frames:
        raise ValueError("WAV is missing data")
    return frames


def packet_count(data):
    cursor = count = 0
    while cursor < len(data):
        if len(data) - cursor < 2:
            raise ValueError("truncated packet length prefix")
        length = struct.unpack_from("<H", data, cursor)[0]
        cursor += 2
        if not 1 <= length <= 512 or length > len(data) - cursor:
            raise ValueError("packet length outside 1..512 or past clip end")
        cursor += length
        count += 1
    return count


def verify_clip(entry, files):
    file = entry["file"]
    if not isinstance(file, str) or file not in files:
        raise ValueError("clip does not refer to a declared asset")
    asset = files[file]
    offset = integer(entry, "offset", 0, len(asset), 0)
    length = integer(entry, "length", 1, MAX_SOUND_BYTES, len(asset) - offset)
    if length > len(asset) - offset:
        raise ValueError("clip offset/length extends past end of asset")
    data = memoryview(asset)[offset:offset + length]
    verify_hash(data, entry["sha256"], "clip")
    codec = entry["encoding"]
    if not isinstance(codec, str) or codec not in CODECS:
        raise ValueError("unknown audio codec")
    container = entry["container"]
    rate = integer(entry, "rate_hz", 1, 0xFFFFFFFF)
    channels = integer(entry, "channels", 1, 255)
    packet_ms = integer(entry, "packet_ms", 0, 255, 0)
    if container == "wav" and codec == "pcm16" and not packet_ms:
        frames = wave_frames(data, rate, channels)
    elif container == "raw" and codec != "opus" and not packet_ms:
        frame_bytes = (2 if codec == "pcm16" else 1) * channels
        if length % frame_bytes:
            raise ValueError("RAW is not a whole sample frame")
        if codec == "g711a" and (offset * 50 % (rate * channels) or length * 50 % (rate * channels)):
            raise ValueError("A-law offset and length must align to 20 ms (160 bytes at 8 kHz mono)")
        frames = length // frame_bytes
    elif container == "packets" and packet_ms:
        frames = packet_count(data)
        if frames != integer(entry, "packet_count", 1, 0xFFFFFFFF):
            raise ValueError("packet count does not match manifest")
    else:
        raise ValueError("unsupported container or inconsistent codec/packet duration")
    duration = frames * packet_ms if packet_ms else (frames * 1000 + rate - 1) // rate
    if duration != integer(entry, "duration_ms", 1, 0xFFFFFFFF):
        raise ValueError("duration does not match manifest")
    return file, offset, length, codec, container, rate, channels, packet_ms


def load(manifest):
    pack = json.loads(manifest.read_text(encoding="utf-8"))
    if not isinstance(pack, dict):
        raise ValueError("pack manifest must be an object")
    name = pack["name"]
    if not isinstance(name, str) or not PACK_NAME.fullmatch(name) or name != manifest.parent.name:
        raise ValueError("pack name must be a C identifier and match the directory name")
    pattern = re.compile(rf"xt_sound_{name}_[a-z0-9_]+\.[a-z0-9]+")
    declared = pack["files"]
    if not isinstance(declared, dict) or not declared or any(not pattern.fullmatch(file) for file in declared):
        raise ValueError(f"assets must use file names beginning with xt_sound_{name}_")
    actual = {asset.name for asset in manifest.parent.glob(f"xt_sound_{name}_*")}
    if actual != declared.keys():
        raise ValueError("declared assets do not match the files selected for embedding")
    files = {}
    for file in sorted(declared):
        data = (manifest.parent / file).read_bytes()
        verify_hash(data, declared[file], file)
        files[file] = data

    sounds = pack["sounds"]
    if not isinstance(sounds, list) or not sounds:
        raise ValueError("sound list must be nonempty")
    clips = {}
    for entry in sounds:
        if not isinstance(entry, dict):
            raise ValueError("invalid sound entry")
        symbol = entry["name"]
        if not isinstance(symbol, str) or not NAME.fullmatch(symbol) or len(symbol) > 31 or symbol in clips:
            raise ValueError(f"invalid or repeated sound name: {symbol!r}")
        try:
            clips[symbol] = verify_clip(entry, files)
        except (ValueError, KeyError) as error:
            raise ValueError(f"{symbol}: {error}") from error
    if {clip[0] for clip in clips.values()} != files.keys():
        raise ValueError("unreferenced pack asset")
    if name in SPOKEN_PACKS:
        mandatory = {"guide.binding"} | {f"digit.{d}" for d in range(10)}
        if missing := sorted(mandatory - clips.keys()):
            raise ValueError(f"spoken binding is missing: {', '.join(missing)}")
    return name, files, clips


def generate(name, files, clips, embedded):
    lines = ["/* Generated by gen_pack.py. Do not edit. */", '#include "xiaotai/sounds.h"', ""]
    symbols = {}
    for i, (file, data) in enumerate(files.items()):
        if embedded:
            # ESP-IDF's data_file_embed_asm.cmake uses MAKE_C_IDENTIFIER on the basename.
            symbols[file] = f"_binary_{file.replace('.', '_')}_start"
            lines.append(f"extern const uint8_t {symbols[file]}[];")
        else:
            symbols[file] = f"asset_{i}"
            lines.append(f"static const uint8_t asset_{i}[] = {{")
            lines.extend("    " + ", ".join(f"0x{byte:02x}" for byte in data[pos:pos + 16]) + ","
                         for pos in range(0, len(data), 16))
            lines.append("};")
    lines += ["", "static const xiaotai_sound_t sounds[] = {"]
    for sound, (file, offset, length, codec, container, rate, channels, packet_ms) in clips.items():
        lines += [f'    {{.name = "{sound}", .encoding = XIAOTAI_CODEC_{codec.upper()},',
                  f"     .container = XIAOTAI_SOUND_{container.upper()}, .rate_hz = {0 if container == 'wav' else rate},",
                  f"     .channels = {0 if container == 'wav' else channels}, .packet_ms = {packet_ms},",
                  f"     .data = {symbols[file]} + {offset}, .size = {length}}},"]
    lines += ["};", "", f"const xiaotai_sound_pack_t xiaotai_sound_pack_{name} = {{",
              f'    .name = "{name}", .sounds = sounds, .count = sizeof(sounds) / sizeof(sounds[0]),', "};", ""]
    return "\n".join(lines).encode("ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", required=True, choices=("host", "idf"))
    parser.add_argument("manifest", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        args.output.write_bytes(generate(*load(args.manifest), embedded=args.mode == "idf"))
    except (KeyError, OSError, ValueError) as error:
        parser.exit(1, f"{args.manifest}: {error}\n")


if __name__ == "__main__":
    main()
