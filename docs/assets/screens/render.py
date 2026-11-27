#!/usr/bin/env python3
"""Pick the screenshots the documents use out of a tools/ui_preview.py run.

    python3 tools/ui_preview.py --out build/ui-preview
    python3 docs/assets/screens/render.py build/ui-preview

Frames are looked up by name, not by number, so a scenario that gains or
loses a step still maps to the same picture. A frame that is missing is
reported and skipped; the existing picture stays.
"""
import pathlib
import re
import sys

from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent

# short name -> product directory in the preview output
PRODUCTS = {
    "s3": "xiaotai-szpi-s3",
    "cores3": "xiaotai-cores3-lite",
    "p4": "xiaotai-waveshare-p4-rev3",
    "wt99": "xiaotai-wt99p461-s1-1v1",
}

# output name -> (product, scenario directory, frame name without its number)
FRAMES = {
    "s3-setup": ("s3", "setup", "setup"),
    "s3-home": ("s3", "home", "ai-ended"),   # the same page; the first frame may catch a blink
    "s3-ai-connecting": ("s3", "home", "ai-connecting"),
    "s3-ai-listening": ("s3", "home", "ai-listening"),
    "s3-call-incoming": ("s3", "home", "call-incoming"),
    "s3-call-connecting": ("s3", "home", "call-connecting"),
    "s3-call-active": ("s3", "home", "call-active"),
    "s3-call-ended": ("s3", "home", "call-ended"),
    "s3-menu": ("s3", "home", "menu"),
    "s3-contacts": ("s3", "home", "contacts"),
    "s3-recognition": ("s3", "gallery", "recognition-ready"),
    "s3-recognition-recording": ("s3", "gallery", "recognition-recording"),
    "s3-recognition-waiting": ("s3", "gallery", "recognition-waiting"),
    "s3-recognition-result": ("s3", "gallery", "recognition-result"),
    "s3-recents": ("s3", "gallery", "recents"),
    "s3-station-home": ("s3", "home", "station-home"),
    "s3-station-calling": ("s3", "home", "station-calling"),
    "s3-station-talking": ("s3", "home", "station-talking"),
    "s3-station-opened": ("s3", "home", "station-opened"),
    "s3-settings": ("s3", "home", "settings"),
    "cores3-alarm-edit": ("cores3", "home", "alarm-edit"),
    "cores3-alarm-timer": ("cores3", "home", "alarm-timer"),
    "cores3-alarm-ringing": ("cores3", "home", "alarm-ringing"),
    "cores3-alarm-snoozed": ("cores3", "home", "alarm-snoozed"),
    "p4-home": ("p4", "home", "ai-ended"),   # as s3-home
    "p4-door-home": ("p4", "home", "door-home"),
    "wt99-door-home": ("wt99", "home", "door-home"),
    "wt99-door-ringing": ("wt99", "home", "door-ringing"),
    "wt99-door-call": ("wt99", "home", "door-call"),
    "wt99-door-open-confirm": ("wt99", "home", "door-open-confirm"),
    "wt99-door-opened": ("wt99", "home", "door-opened"),
    "wt99-station-home": ("wt99", "home", "station-home"),
    "wt99-station-directory": ("wt99", "home", "station-directory"),
    "wt99-station-talking": ("wt99", "home", "station-talking"),
    "wt99-station-looked": ("wt99", "home", "station-looked-at"),
    "wt99-station-looked-ringing": ("wt99", "home", "station-looked-at-ringing"),
    "wt99-station-looked-talking": ("wt99", "home", "station-looked-at-talking"),
    "wt99-station-opened": ("wt99", "home", "station-opened"),
    "wt99-access-home": ("wt99", "home", "access-typed"),
    "wt99-access-which": ("wt99", "home", "access-which"),
    "wt99-access-calling": ("wt99", "home", "access-calling"),
    "p4-access-home": ("p4", "home", "access-typed"),
    "s3-access-home": ("s3", "home", "access-typed"),
}

# output name -> frames placed side by side
STRIPS = {
    "strip-overview": ["s3-home", "s3-ai-listening", "s3-call-incoming", "s3-contacts"],
    "strip-ai": ["s3-home", "s3-ai-connecting", "s3-ai-listening"],
    "strip-call": ["s3-contacts", "s3-call-connecting", "s3-call-active", "s3-call-ended"],
    "strip-incoming": ["s3-call-incoming", "s3-call-active", "s3-call-ended"],
    "strip-recognition": ["s3-recognition", "s3-recognition-recording", "s3-recognition-waiting", "s3-recognition-result"],
    "strip-s3-station": ["s3-station-home", "s3-station-calling", "s3-station-talking", "s3-station-opened"],
    "strip-alarm": ["cores3-alarm-edit", "cores3-alarm-timer", "cores3-alarm-ringing"],
    "strip-door": ["wt99-door-ringing", "wt99-door-call", "wt99-door-open-confirm", "wt99-door-opened"],
    "strip-looked": ["wt99-station-looked", "wt99-station-looked-ringing", "wt99-station-looked-talking"],
    "strip-access": ["wt99-access-home", "wt99-access-which", "wt99-access-calling"],
    "strip-access-small": ["s3-access-home", "s3-station-opened"],
    "strip-station": ["wt99-station-home", "wt99-station-directory", "wt99-station-talking"],
}

GAP = 16          # transparent gap between frames in a strip
TILE_HEIGHT = 480  # every frame in a strip is brought to this height


def find(preview, product, scenario, name):
    folder = preview / PRODUCTS[product] / scenario
    pattern = re.compile(r"^\d+-" + re.escape(name) + r"\.png$")
    hits = sorted(p for p in folder.glob("*.png") if pattern.match(p.name))
    return hits[0] if hits else None


def scaled(image, height):
    if image.height == height:
        return image
    width = round(image.width * height / image.height)
    # whole multiples stay crisp; anything else is smoothed
    kind = Image.NEAREST if height % image.height == 0 else Image.LANCZOS
    return image.resize((width, height), kind)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    preview = pathlib.Path(sys.argv[1])
    frames, missing = {}, []
    for out, (product, scenario, name) in FRAMES.items():
        source = find(preview, product, scenario, name)
        if source is None:
            missing.append(out)
            continue
        image = Image.open(source).convert("RGB")
        if image.height < 320:  # the 320x240 screens are doubled so text stays readable
            image = scaled(image, image.height * 2)
        image.save(HERE / f"{out}.png", optimize=True)
        frames[out] = image
    for out, names in STRIPS.items():
        if any(n not in frames for n in names):
            missing.append(out)
            continue
        tiles = [scaled(frames[n], TILE_HEIGHT) for n in names]
        width = sum(t.width for t in tiles) + GAP * (len(tiles) - 1)
        strip = Image.new("RGBA", (width, TILE_HEIGHT), (0, 0, 0, 0))
        x = 0
        for tile in tiles:
            strip.paste(tile, (x, 0))
            x += tile.width + GAP
        strip.save(HERE / f"{out}.png", optimize=True)
    print(f"{len(frames)} frames, {len(STRIPS)} strips -> {HERE}")
    if missing:
        print("missing in this preview run (left as they were): " + ", ".join(missing))
        sys.exit(1)


if __name__ == "__main__":
    main()
