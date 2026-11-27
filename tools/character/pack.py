# SPDX-License-Identifier: MIT
"""The character's sprite pack: its numbers, its pixel coding and its file.

Shared by the two ways of making a pack (bake.py): from puppets (layers posed
by their own parameters) and from flat pictures (one base picture, patches for
the eyes and the mouth, whole-figure poses). The firmware reads the same
numbers from src/chr_sprites.h and src/chr_internal.h; both sides only ever
add at the end.
"""

import struct
import zlib

import numpy as np
from PIL import Image, ImageFilter

# Slots: what the firmware asks the pack for. A slot the pack leaves empty is a
# part this character does not have.
SLOT_TAIL, SLOT_EAR_L, SLOT_EAR_R, SLOT_BODY, SLOT_ARM_L, SLOT_ARM_R, SLOT_HEAD = 0, 1, 4, 7, 8, 12, 16
ARM_STATES, YAWS = 4, 3
HEAD_HAIR_BACK, HEAD_FACE, HEAD_HAIR_FRONT, HEAD_BROW_L, HEAD_BROW_R, HEAD_MOUTH, HEAD_EYE_L, HEAD_EYE_R, HEAD_SLOTS = \
    0, 1, 2, 3, 4, 5, 20, 29, 38
# After the three head blocks: pictures a flat figure adds.
SLOT_EYE_L_WIDE, SLOT_EYE_R_WIDE, SLOT_EYE_L_UP, SLOT_EYE_R_UP = 130, 131, 132, 133
SLOT_BROWS = 134          # 3: raised, cross, sad (both brows in one picture)
SLOT_FACE = 137           # 3 whole-face pictures: puffed cheeks, crying, (free)
SLOT_POSE = 140           # POSES whole-figure pictures, in the order of POSE_NAMES
POSES = 24
POSE_NAMES = ("wave", "cheer", "think", "shy", "sulk", "stretch", "sleep", "highfive", "shrug", "dance",
              "laugh", "nod", "tilt", "listen", "surprise", "heart", "pet")
# How she gets from one picture to the next (a clip's style).
STYLES = {"pop": 0, "soft": 1, "beat": 2}
# Added later: open eyes with another look, and the brows of someone who comforts or is serious.
SLOT_EYE_L_SOFT, SLOT_EYE_R_SOFT, SLOT_EYE_L_BRIGHT, SLOT_EYE_R_BRIGHT = 164, 165, 166, 167
SLOT_BROWS_COMFORT, SLOT_BROWS_SERIOUS = 168, 169
# The eyes and the closed mouth that go with those brows.
SLOT_EYE_L_COMFORT, SLOT_EYE_R_COMFORT, SLOT_EYE_L_SERIOUS, SLOT_EYE_R_SERIOUS = 170, 171, 172, 173
SLOT_MOUTH_COMFORT, SLOT_MOUTH_SERIOUS = 174, 175
SLOT_EYE_L_PONDER, SLOT_EYE_R_PONDER = 176, 177   # eyes lowered to one side
# Eyes that smile half open, eyes cast down, eyes with a line of tears; a smile higher on one side, a mouth whose
# corners are a little down.
SLOT_EYE_L_GLAD, SLOT_EYE_R_GLAD, SLOT_EYE_L_DOWN, SLOT_EYE_R_DOWN, SLOT_EYE_L_TEARY, SLOT_EYE_R_TEARY = 178, 179, 180, 181, 182, 183
SLOT_MOUTH_SMIRK, SLOT_MOUTH_FROWN = 184, 185
# Effects: FX_FRAMES slots for each kind, in the order of FX_NAMES (the firmware's chr_fx_t); the frames of an
# animation, or variants of one thing. "puff" has no picture (the firmware draws it).
SLOT_FX, FX_FRAMES = 186, 12
FX_NAMES = ("heart", "zed", "note", "sparkle_gold", "drop", "question", "exclaim", "puff", "heart_small", "sparkle_sky",
            "glint", "ring_gold", "ring_sky", "burst", "petal", "rose", "ribbon", "note2", "orb_gold", "orb_pink", "orb_sky",
            "orb_lilac", "aura")
SLOT_COUNT = SLOT_FX + len(FX_NAMES) * FX_FRAMES
assert SLOT_COUNT == 462
EMPTY = 0xFFFF

# Layout values, in display pixels from the anchor.
LAYOUT = ("ears_top", "hair_top", "brow_y", "eye_y", "cheek_y", "mouth_y", "neck_y", "ear_base_y", "eye_lx", "eye_rx",
          "face_x", "head_half", "tail_x0", "tail_x1", "tail_top", "brow_lx", "brow_rx", "cheek_dx", "blush_rx",
          "blush_ry", "ear_lx0", "ear_lx1", "ear_rx0", "ear_rx1", "shoulder_half", "arm_lx", "arm_rx", "arm_top",
          "hand_lx", "hand_ly", "hand_rx", "hand_ry", "hand_half", "yaw_shift", "shoulder_y")

BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], dtype=np.float32) / 16.0


def quantize(image, left, top):
    """Premultiplied float RGBA -> (alpha 0..32, RGB565), dithered on the figure's own grid."""
    alpha = image[..., 3]
    with np.errstate(divide="ignore", invalid="ignore"):
        rgb = np.where(alpha[..., None] > 1e-4, image[..., :3] / alpha[..., None], 0.0)
    rgb = np.clip(rgb, 0.0, 1.0)
    a = np.rint(np.clip(alpha, 0, 1) * 32).astype(np.int32)
    height, width = alpha.shape
    ys, xs = np.mgrid[0:height, 0:width]
    noise = BAYER[(ys + top) & 3, (xs + left) & 3]
    levels = np.array([31, 63, 31], dtype=np.float32)
    q = np.clip(np.floor(rgb * levels + noise[..., None]), 0, levels).astype(np.int32)
    colour = (q[..., 0] << 11) | (q[..., 1] << 5) | q[..., 2]
    return a, colour


def indexed(image, focus=None):
    """Premultiplied float RGBA -> (alpha 0..32, index into a palette, 256 RGB565 colours). The palette
    is this picture's own; the pixels are dithered against it, which costs nothing in a pack that
    stores every opaque pixel anyway. `focus` (x0, y0, x1, y1) is the part that is looked at most, her
    face: it counts several times over when the colours are chosen, or the few dark browns of her
    lashes would be given up for one more blue of her hair."""
    alpha = image[..., 3]
    with np.errstate(divide="ignore", invalid="ignore"):
        rgb = np.where(alpha[..., None] > 1e-4, image[..., :3] / alpha[..., None], 0.0)
    a = np.rint(np.clip(alpha, 0, 1) * 32).astype(np.int32)
    rgb8 = np.rint(np.clip(rgb, 0.0, 1.0) * 255).astype(np.uint8)
    seen = a > 0
    if seen.any():
        rgb8[~seen] = rgb8[seen].mean(axis=0).astype(np.uint8)      # what is not seen takes no colour of its own
    picture = Image.fromarray(rgb8, "RGB")
    sample = picture
    if focus:
        part = picture.crop(tuple(int(v) for v in focus))
        if part.width > 0 and part.height > 0:
            times = 6
            fill = tuple(int(v) for v in rgb8[seen].mean(axis=0)) if seen.any() else (0, 0, 0)
            sample = Image.new("RGB", (max(picture.width, part.width), picture.height + times * part.height), fill)
            sample.paste(picture, (0, 0))
            for i in range(times):
                sample.paste(part, (0, picture.height + i * part.height))
    chosen = np.array(sample.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
                      .getpalette()[:768], dtype=np.int32).reshape(-1, 3)
    chosen = np.vstack([chosen, np.zeros((256 - len(chosen), 3), dtype=np.int32)])
    r, g, b = (chosen[:, 0] * 31 + 127) // 255, (chosen[:, 1] * 63 + 127) // 255, (chosen[:, 2] * 31 + 127) // 255
    shown = np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], axis=1)   # as the display shows them
    holder = Image.new("P", (1, 1))
    holder.putpalette([int(v) for v in shown.flatten()])
    index = np.asarray(picture.quantize(palette=holder, dither=Image.Dither.FLOYDSTEINBERG), dtype=np.int32)
    return a, index, ((r << 11) | (g << 5) | b).astype(np.int32)


def encode(a, colour, wide=True, spans=None):
    """Rows of runs: 0x00+n skip n+1, 0x80+n n+1 opaque pixels, 0xC0+n n+1 pixels with alpha. A pixel
    is its RGB565 colour, or (`wide` false) one byte: its place in the sprite's palette. `spans`: per
    row the columns (from, to) this sprite replaces in the picture under it; each row then begins
    with those two numbers, and holds nothing outside them."""
    rows = []
    for y in range(a.shape[0]):
        row = bytearray()
        line, pixels = a[y], colour[y]
        if spans is not None:
            row += struct.pack("<HH", *spans[y])
            line = line.copy()
            line[:spans[y][0]] = 0
            line[spans[y][1]:] = 0
        last = np.nonzero(line)[0]
        end = int(last[-1]) + 1 if last.size else 0
        x = 0
        while x < end:
            kind = 0 if line[x] == 0 else 1 if line[x] == 32 else 2
            limit = 128 if kind == 0 else 64
            run = 1
            while x + run < end and run < limit and (0 if line[x + run] == 0 else 1 if line[x + run] == 32 else 2) == kind:
                run += 1
            if kind == 0:
                row.append(run - 1)
            elif kind == 1:
                row.append(0x80 + run - 1)
                row += struct.pack(f"<{run}{'H' if wide else 'B'}", *[int(v) for v in pixels[x:x + run]])
            else:
                row.append(0xC0 + run - 1)
                for i in range(x, x + run):
                    row += struct.pack("<BH" if wide else "<BB", int(line[i]), int(pixels[i]))
            x += run
        rows.append(bytes(row))
    return rows


class Sprites:
    """The sprites of a pack as they are baked: add() each, then write()."""

    def __init__(self):
        self.names, self.boxes, self.blobs, self.alphas, self.palettes, self.spanned = [], [], [], [], [], []
        self.slots = [EMPTY] * SLOT_COUNT
        self.clips = []

    def clip(self, gesture, enter, loop, leave, style=0):
        """A gesture (1 + its place in POSE_NAMES) played with several pictures. Each of the three
        lists holds (sprite name, milliseconds, name of the sprite it is a patch over or None);
        0 milliseconds: the picture stays. `style`: one of
        STYLES, how she gets into the gesture and from one of its pictures to the next."""
        if not enter and not loop:
            raise SystemExit("bake: a clip needs pictures to enter with or to loop")
        self.clips.append((gesture, enter, loop, leave, style))

    def add(self, name, slots, image, left, top, palette=False, focus=None, cover=None):
        """`image`: premultiplied float RGBA whose top-left pixel lies (left, top) from the anchor.
        `palette`: store it in 256 colours of its own, a byte a pixel (for the large pictures);
        `focus`: the part of it whose colours matter most (see indexed()); `cover`: another picture
        of the same size whose pixels the sprite's box must also hold (one that is patched over it).
        Returns the sprite's box (x, y, width, height), cropped to what is not transparent."""
        also = None if cover is None else np.rint(np.clip(cover[..., 3], 0, 1) * 32) > 0
        if palette:
            a, index, colours = indexed(image, focus)
            return self.add_coded(name, slots, a, index, left, top, colours, also)
        a, colour = quantize(image, left, top)
        return self.add_coded(name, slots, a, colour, left, top, None, also)

    def add_patch(self, name, under, image, left, top, palette=False, focus=None):
        """A picture stored as what differs from another one (`under`, the same size, drawn first):
        in every row the columns from the first to the last pixel that differs. Inside them it takes
        the place of the picture under it, transparent pixels included, so a hand that has moved
        leaves nothing behind. Returns the sprite's box and the share of the picture it stores."""
        differs = np.abs(image - under).max(axis=2) > 6.0 / 255.0
        mask = np.asarray(Image.fromarray((differs * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(3))) > 0
        ys, xs = np.nonzero(mask)
        if not ys.size:
            raise SystemExit(f"bake: {name} does not differ from the picture it is a patch over")
        y0, y1, x0, x1 = int(ys.min()), int(ys.max()) + 1, int(xs.min()), int(xs.max()) + 1
        if palette:
            a, colour, colours = indexed(image, focus)
        else:
            (a, colour), colours = quantize(image, left, top), None
        spans = []
        for y in range(y0, y1):
            columns = np.nonzero(mask[y])[0]
            spans.append((int(columns[0]) - x0, int(columns[-1]) + 1 - x0) if columns.size else (0, 0))
        a, colour = a[y0:y1, x0:x1], colour[y0:y1, x0:x1]
        box = (left + x0, top + y0, x1 - x0, y1 - y0)
        self.names.append(name)
        self.boxes.append(box)
        self.blobs.append(encode(a, colour, colours is None, spans))
        self.alphas.append(a)
        self.palettes.append(colours)
        self.spanned.append(True)
        return box, sum(to - start for start, to in spans) / float(image.shape[0] * image.shape[1])

    def add_coded(self, name, slots, a, colour, left, top, palette=None, also=None):
        ys, xs = np.nonzero(a if also is None else (a > 0) | also)
        if not ys.size:
            raise SystemExit(f"bake: sprite {name} is empty")
        y0, y1, x0, x1 = int(ys.min()), int(ys.max()) + 1, int(xs.min()), int(xs.max()) + 1
        a, colour = a[y0:y1, x0:x1], colour[y0:y1, x0:x1]
        for slot in slots:
            self.slots[slot] = len(self.names)
        box = (left + x0, top + y0, x1 - x0, y1 - y0)
        self.names.append(name)
        self.boxes.append(box)
        self.blobs.append(encode(a, colour, palette is None))
        self.alphas.append(a)
        self.palettes.append(palette)
        self.spanned.append(False)
        return box

    def alias(self, slots, name):
        """More slots for a sprite that is already there."""
        index = self.names.index(name)
        for slot in slots:
            self.slots[slot] = index

    def size_of(self, index):
        return sum(len(row) for row in self.blobs[index])

    def write(self, path, height, layout):
        """Header, layout, slots, sprite table, then per sprite a row-offset table and the rows."""
        def padded(data):
            return data + bytes(-len(data) % 4)

        layout_bytes = padded(struct.pack(f"<{len(LAYOUT)}h", *[int(round(layout.get(name, 0))) for name in LAYOUT]))
        slot_bytes = padded(struct.pack(f"<{SLOT_COUNT}H", *self.slots))
        # Clips, in 16-bit words: clips, frames; per clip gesture, first frame, enter, loop, leave, style;
        # per frame sprite, milliseconds, the sprite it is a patch over.
        words, frames = [], []
        for gesture, enter, loop, leave, style in self.clips:
            words += [gesture, len(frames), len(enter), len(loop), len(leave), style]
            frames += [(self.names.index(name), int(ms), EMPTY if under is None else self.names.index(under))
                       for name, ms, under in enter + loop + leave]
        words = [len(self.clips), len(frames)] + words + [value for frame in frames for value in frame] if self.clips else []
        clip_bytes = padded(struct.pack(f"<{len(words)}H", *words))
        header_size = 28 + len(layout_bytes) + len(slot_bytes) + len(clip_bytes) + 20 * len(self.names)
        body = bytearray()
        table = bytearray()
        for (x, y, w, h), rows, palette, spanned in zip(self.boxes, self.blobs, self.palettes, self.spanned):
            while len(body) % 4:
                body.append(0)
            start = header_size + len(body)
            # A sprite with a palette keeps it between its row offsets and its rows, and says so in
            # the top byte of its length (1); so does one whose rows are spans (2).
            colours = b"" if palette is None else struct.pack("<256H", *[int(v) for v in palette])
            offsets, at = [], 4 * (h + 1) + len(colours)
            for row in rows:
                offsets.append(at)
                at += len(row)
            offsets.append(at)
            bytes_of = struct.pack(f"<{h + 1}I", *offsets) + colours + b"".join(rows)
            body += bytes_of
            table += struct.pack("<hhHHIII", x, y, w, h, start, at | ((0 if palette is None else 1) | (2 if spanned else 0)) << 24,
                                 zlib.crc32(bytes_of))
        # The head (all but the sprites) carries a checksum of itself, and the table one of each sprite:
        # the device reads them from flash one by one.
        head = struct.pack("<4sHHHHHHIII", b"XTCH", 4, len(self.names), height, len(LAYOUT), SLOT_COUNT, len(words),
                           header_size + len(body), header_size, 0) + layout_bytes + slot_bytes + clip_bytes + bytes(table)
        head = head[:24] + struct.pack("<I", zlib.crc32(head)) + head[28:]
        pack = head + bytes(body)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(pack)
        print("layout: " + " ".join(f"{name}={int(round(layout.get(name, 0)))}" for name in LAYOUT))
        print(f"{path}: {len(pack) // 1024} KB, {len(self.names)} sprites, "
              f"{sum(slot != EMPTY for slot in self.slots)} of {SLOT_COUNT} slots, {len(self.clips)} clips")
