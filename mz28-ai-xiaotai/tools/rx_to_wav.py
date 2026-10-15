"""Export a complete RX_OPUS dump to Ogg Opus or decode to PCM WAV using FFmpeg."""
import argparse
from pathlib import Path
import re
import subprocess
import wave
from opus_packets import unpack_packets, packet_samples, to_ogg


def extract(text):
    matches = list(re.finditer(
        r"RX_OPUS_BEGIN (\d+) rate=16000 channels=1\r?\n(.*?)RX_OPUS_END", text, re.S))
    if not matches:
        raise ValueError("No complete 16 kHz mono Opus dump found")
    match = matches[-1]
    if "RX_OPUS_BEGIN" in match[2]:
        raise ValueError("Interrupted/nested RX dump")
    # A log prefix may contain spaces, but must never consume the previous RX line.
    chunks = re.findall(r"^(?:[^\r\n]*?[ \t])?RX ([0-9a-fA-F]+)\r?$", match[2], re.M)
    data = bytes.fromhex("".join(chunks))
    if len(data) != int(match[1]) or not data:
        raise ValueError("Incomplete or empty RX dump")
    unpack_packets(data)
    return data


def decode_wav(packets, output, ffmpeg="ffmpeg"):
    result = subprocess.run(
        [ffmpeg, "-v", "error", "-xerror", "-nostdin", "-err_detect", "explode",
         "-f", "ogg", "-i", "pipe:0", "-map", "0:a:0", "-ar", "16000", "-ac", "1",
         "-c:a", "pcm_s16le", "-f", "s16le", "pipe:1"],
        input=to_ogg(packets), stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    if not result.stdout or len(result.stdout) % 2:
        raise ValueError("Decoder produced no complete PCM samples")
    with wave.open(str(output), "wb") as target:
        target.setnchannels(1)
        target.setsampwidth(2)
        target.setframerate(16000)
        target.writeframes(result.stdout)
    return len(result.stdout) // 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("output", type=Path, help=".wav requires FFmpeg; .opus exports Ogg directly")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="FFmpeg executable path")
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Output already exists; choose a new path")
    if args.output.suffix.lower() not in (".wav", ".opus"):
        parser.error("Output must end in .wav or .opus")
    try:
        packets = unpack_packets(extract(args.log.read_text(encoding="utf-8")))
        if args.output.suffix.lower() == ".opus":
            args.output.write_bytes(to_ogg(packets))
        else:
            decode_wav(packets, args.output, args.ffmpeg)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        detail = error.stderr.decode(errors="replace") if isinstance(error, subprocess.CalledProcessError) else str(error)
        parser.exit(1, f"Export failed: {detail}\n")
    duration = sum(packet_samples(p) for p in packets) / 48000
    print(f"Wrote {len(packets)} Opus packets ({duration:.3f}s) to {args.output}")


if __name__ == "__main__":
    main()
