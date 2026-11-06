"""Derive board tuning with a hardware-valid, luma-independent gamma X grid."""
import argparse
import copy
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "vendor/wt99/esp_cam_sensor/sensors/sc2336/cfg/sc2336_default_p4_eco5.json"
OUTPUT = ROOT / "main/boards/wt99p461-s1/drivers/wt99p461_s1/sc2336_isp.json"
CHANNELS = ("red", "green", "blue")


def validate_grid(x):
    if len(x) != 16 or x[-1] != 255:
        raise ValueError("P4 gamma requires 16 points ending at 255")
    previous = 0
    for index, point in enumerate(x):
        value = 256 if index == 15 else point
        delta = value - previous
        if delta <= 0 or delta & (delta - 1):
            raise ValueError(f"P4 gamma point {index}: non-power-of-two span {delta}")
        previous = value


def interpolate(x, y, target):
    points = [(0, 0)] + list(zip(x, y))
    for (x0, y0), (x1, y1) in zip(points, points[1:]):
        if x0 <= target <= x1:
            return y0 + (y1 - y0) * (target - x0) / (x1 - x0)
    raise ValueError("Gamma sample outside curve")


def derive(source):
    result = copy.deepcopy(source)
    gamma = result["SC2336"]["aen"]["gamma"]
    if gamma["use_gamma_param"]:
        raise ValueError("Expected explicit upstream gamma curves")
    grid = list(gamma["table"][0]["x"])
    validate_grid(grid)
    for unit in gamma["table"]:
        for channel in CHANNELS:
            curve = unit.get(channel, unit)
            x, y = curve["x"], curve["y"]
            if len(x) != len(y) or x[-1] != 255 or any(b <= a for a, b in zip([0] + x, x)):
                raise ValueError("Invalid source gamma curve")
            unit[channel] = {
                "x": list(grid),
                "y": [int(interpolate(x, y, point) + 0.5) for point in grid],
            }
        # A common grid keeps IPA interpolation from generating illegal spans.
        unit.pop("x", None)
        unit.pop("y", None)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    source = json.loads(SOURCE.read_text(encoding="utf-8"))
    result = derive(source)
    if args.check:
        if json.loads(OUTPUT.read_text(encoding="utf-8")) != result:
            raise SystemExit("SC2336 board tuning differs from its reviewed derivation")
        print("SC2336 board tuning verified: gamma grid only; other ISP settings unchanged")
    else:
        OUTPUT.write_text(json.dumps(result, indent=4) + "\n", encoding="utf-8")
        print(OUTPUT)


if __name__ == "__main__":
    main()
