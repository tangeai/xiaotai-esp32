"""Generate the original short interaction sounds used by the MZ28 demo."""

from __future__ import annotations

import argparse
import math
from pathlib import Path
import struct
import wave


SAMPLE_RATE = 16_000


def envelope(index: int, count: int, attack_ms: float, release_ms: float) -> float:
    attack = max(1, round(SAMPLE_RATE * attack_ms / 1000.0))
    release = max(1, round(SAMPLE_RATE * release_ms / 1000.0))
    return min(1.0, index / attack, (count - 1 - index) / release)


def tone(frequencies: tuple[float, ...], duration_ms: int, gain: float,
         *, decay: float = 0.0) -> list[float]:
    count = round(SAMPLE_RATE * duration_ms / 1000.0)
    output = []
    for index in range(count):
        position = index / max(1, count - 1)
        shape = envelope(index, count, 5.0, 42.0)
        if decay:
            shape *= math.exp(-decay * position)
        value = 0.0
        for frequency in frequencies:
            phase = 2.0 * math.pi * frequency * index / SAMPLE_RATE
            value += (math.sin(phase) + 0.22 * math.sin(phase * 2.0) +
                      0.07 * math.sin(phase * 3.0)) / 1.29
        output.append(gain * shape * value / len(frequencies))
    return output


def silence(duration_ms: int) -> list[float]:
    return [0.0] * round(SAMPLE_RATE * duration_ms / 1000.0)


def write_wav(path: Path, samples: list[float]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    pcm = bytearray()
    for sample in samples:
        value = round(max(-1.0, min(1.0, sample)) * 32767.0)
        pcm.extend(struct.pack("<h", value))
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(SAMPLE_RATE)
        output.writeframes(pcm)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_dir", type=Path)
    args = parser.parse_args()

    sounds = {
        # A bright two-part motif remains recognizable on the small speaker
        # without imitating a proprietary phone ringtone.
        "ring_incoming": (
            tone((880.0,), 130, 0.52, decay=0.8) + silence(30) +
            tone((1174.7,), 130, 0.52, decay=0.8) + silence(30) +
            tone((1568.0,), 220, 0.55, decay=1.1) + silence(180) +
            tone((1174.7,), 110, 0.48, decay=0.7) + silence(30) +
            tone((1568.0,), 110, 0.50, decay=0.7) + silence(30) +
            tone((2093.0,), 240, 0.54, decay=1.2) + silence(80)
        ),
        # One short telephone-like beep; the product audio loop supplies the
        # quiet interval, producing an unambiguous beep-beep-beep cadence.
        "ring_outgoing": (
            tone((620.0, 760.0), 220, 0.52) + silence(20)
        ),
        "popup": (
            tone((1318.5,), 60, 0.34, decay=0.8) + silence(20) +
            tone((1760.0,), 80, 0.37, decay=1.2) + silence(40)
        ),
        "success": (
            tone((880.0,), 80, 0.36, decay=0.5) + silence(20) +
            tone((1174.7,), 80, 0.39, decay=0.5) + silence(20) +
            tone((1568.0,), 160, 0.43, decay=1.0) + silence(40)
        ),
        "exclamation": (
            tone((760.0,), 160, 0.43) + silence(60) +
            tone((540.0,), 240, 0.46) + silence(40)
        ),
        "call_ended": (
            tone((700.0,), 100, 0.38) + silence(40) +
            tone((460.0,), 180, 0.42) + silence(40)
        ),
    }
    for name, samples in sounds.items():
        write_wav(args.output_dir / f"{name}.wav", samples)


if __name__ == "__main__":
    main()
