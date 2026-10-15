"""Encode a recording to 16 kHz mono Opus, 24 kbit/s CBR, 20 ms packets.

Requires FFmpeg with libopus and ffprobe. Runs offline, never on the device.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from opus_packets import pack_packets, packet_samples, to_ogg


def run(command):
    return subprocess.run(command, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE).stdout


def read_p3(source):
    """Read the VB6824 prompt container: big-endian type/reserved/length."""
    data = source.read_bytes()
    packets = []
    offset = 0
    while offset < len(data):
        if offset + 4 > len(data):
            raise ValueError("Truncated P3 packet header")
        packet_type, reserved, size = struct.unpack_from(">BBH", data, offset)
        offset += 4
        if packet_type != 0 or reserved != 0 or not size or offset + size > len(data):
            raise ValueError("Invalid P3 packet header")
        packet = data[offset:offset + size]
        if packet_samples(packet) != 2880:
            raise ValueError("Expected 60 ms mono Opus packets in P3 source")
        packets.append(packet)
        offset += size
    if not packets:
        raise ValueError("Empty P3 source")
    return packets


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--alaw-source", action="store_true", help="Source is raw 8 kHz mono A-law")
    parser.add_argument("--p3-source", action="store_true",
                        help="Source is a VB6824 P3 prompt with 60 ms Opus packets")
    parser.add_argument("--source-license", help="License identifier recorded in the manifest")
    parser.add_argument("--source-kind", help="Stable source description recorded in the manifest")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    args = parser.parse_args()
    if args.alaw_source and args.p3_source:
        parser.error("Choose at most one source format")
    manifest = args.output.with_suffix(args.output.suffix + ".json")
    if args.output.exists() or manifest.exists():
        parser.error("Output or manifest already exists; use a new path")
    with tempfile.TemporaryDirectory() as temp:
        source = args.source
        if args.p3_source:
            source = Path(temp) / "source.ogg"
            source.write_bytes(to_ogg(read_p3(args.source)))
        ogg = str(Path(temp) / "encoded.opus")
        command = [args.ffmpeg, "-v", "error", "-nostdin"]
        if args.alaw_source:
            command += ["-f", "alaw", "-ar", "8000", "-ac", "1"]
        command += ["-i", str(source), "-vn", "-ar", "16000", "-ac", "1",
                    "-c:a", "libopus", "-application", "voip", "-b:a", "24k", "-vbr", "off",
                    "-frame_duration", "20", ogg]
        run(command)
        metadata = json.loads(run([args.ffprobe, "-v", "error", "-select_streams", "a:0",
                                   "-show_packets", "-show_entries", "packet=size", "-of", "json", ogg]))
        raw = run([args.ffmpeg, "-v", "error", "-nostdin", "-i", ogg, "-map", "0:a:0",
                   "-c:a", "copy", "-f", "data", "pipe:1"])
        packets, offset = [], 0
        for item in metadata["packets"]:
            size = int(item["size"])
            packet = raw[offset:offset + size]
            if size != 60 or len(packet) != size or packet_samples(packet) != 960:
                raise ValueError("Unexpected encoding; expected 60-byte mono 20 ms Opus packets")
            packets.append(packet)
            offset += size
        if not packets or offset != len(raw):
            raise ValueError("Demuxed packet boundaries do not match")
        encoded = pack_packets(packets)
        manifest_data = {"codec": "opus", "sample_rate": 16000, "channels": 1,
                         "bitrate": 24000, "packet_ms": 20, "packet_count": len(packets),
                         "payload_bytes": 60, "storage": "uint16le length + raw Opus packet",
                         "source_sha256": hashlib.sha256(args.source.read_bytes()).hexdigest(),
                         "sha256": hashlib.sha256(encoded).hexdigest(),
                         "encoder": run([args.ffmpeg, "-version"]).decode().splitlines()[0]}
        if args.p3_source:
            manifest_data["source_format"] = "VB6824 P3 (60 ms Opus packets)"
        if args.source_license:
            manifest_data["source_license"] = args.source_license
        if args.source_kind:
            manifest_data["source_kind"] = args.source_kind
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(encoded)
        manifest.write_text(json.dumps(manifest_data, indent=2) + "\n", encoding="utf-8")
        print(f"Wrote {len(packets)} packets, {len(encoded)} bytes: {args.output}")


if __name__ == "__main__":
    main()
