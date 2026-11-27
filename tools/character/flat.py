# SPDX-License-Identifier: MIT
"""Bake a sprite pack from flat pictures of one character.

A flat figure is not taken apart into layers. It is one base picture of the
whole figure (mouth shut, eyes open, arms down) on a green screen, plus:

  patches   pictures that were edited from the base in one place only (the
            eyes shut, the mouth open, ...). Where such a picture differs from
            the base inside the box of that place, the difference becomes a
            small sprite that is drawn over the base.
  poses     other pictures of the whole figure (waving, cheering, ...), shown
            in place of the base; green screen, or cut out with their own alpha.
  clips     gestures played with several pictures, each for its own time.

The device never bends a picture: it moves and grows all of whatever picture
it shows as one piece, and draws the patches on top of the base.

The recipe names the pictures and says where things are on their canvas (the
boxes of the eyes and the mouth, where the eyes are, where the neck ends).
Needs numpy and Pillow.
"""

import numpy as np
from PIL import Image, ImageFilter

import pack

GAZES = 3                              # left, centre, right
# Mouth pictures by openness (0 shut .. 4 wide) and form (pout, plain, smile): names in the recipe's
# "mouth", None for the base picture's own mouth.
MOUTHS = (("pout", "flat", None), ("small", "small", "small"), ("medium", "medium", "medium"),
          ("large", "large", "large"), ("round", "large", "laugh"))


def keyed(path, size):
    """A picture as straight float RGB (0..255) and alpha (0..1): on a green screen, with the green
    spill removed, or one that was cut out already and brings its own alpha."""
    image = Image.open(path)
    if image.size != tuple(size):
        raise SystemExit(f"bake: {path} is {image.size[0]}x{image.size[1]}, the recipe's canvas is {size[0]}x{size[1]}")
    if image.mode == "RGBA" and image.getextrema()[3][0] < 255:
        cut = np.asarray(image, dtype=np.float32)
        return cut[..., :3].copy(), cut[..., 3] / 255.0
    image = image.convert("RGB")
    a = np.asarray(image, dtype=np.float32)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    # How much greener a pixel is than it is red or blue: the screen itself is far above the range
    # here, the figure (who wears no green) below it, and the mixed pixels of hair tips and gaps
    # between strands fall inside and become partly transparent.
    excess = g - np.maximum(r, b)
    alpha = np.clip(1.0 - (excess - 8.0) / 40.0, 0.0, 1.0)
    return np.dstack([r, np.minimum(g, np.maximum(r, b)), b]), alpha


def premultiplied(rgb, alpha):
    return np.dstack([rgb / 255.0 * alpha[..., None], alpha]).astype(np.float32)


def shrink(image, width, height):
    """Premultiplied float RGBA scaled to the display's size."""
    channels = [np.asarray(Image.fromarray(image[..., c], "F").resize((width, height), Image.LANCZOS), dtype=np.float32)
                for c in range(4)]
    out = np.clip(np.dstack(channels), 0.0, 1.0)
    out[..., :3] = np.minimum(out[..., :3], out[..., 3:4])
    return out


def difference(base_rgb, rgb, box, columns=None):
    """How much of a picture replaces the base inside a box: 0..1 per pixel of the canvas, soft at its
    rim and never reaching the box's edge. `columns` limits it to a range of columns (one eye)."""
    x0, y0, x1, y1 = box
    differs = np.abs(rgb[y0:y1, x0:x1] - base_rgb[y0:y1, x0:x1]).max(axis=2) > 22
    mask = Image.fromarray((differs * 255).astype(np.uint8))
    mask = mask.filter(ImageFilter.MaxFilter(13)).filter(ImageFilter.GaussianBlur(5))
    inside = np.clip(np.asarray(mask, dtype=np.float32) / 255.0 * 1.6, 0.0, 1.0)
    rim = np.ones_like(inside)
    for k in range(10):
        f = (k + 1) / 10.0
        rim[k, :] = np.minimum(rim[k, :], f)
        rim[-1 - k, :] = np.minimum(rim[-1 - k, :], f)
        rim[:, k] = np.minimum(rim[:, k], f)
        rim[:, -1 - k] = np.minimum(rim[:, -1 - k], f)
    out = np.zeros(rgb.shape[:2], dtype=np.float32)
    out[y0:y1, x0:x1] = inside * rim
    if columns:
        keep = np.zeros(rgb.shape[1], dtype=np.float32)
        keep[columns[0]:columns[1]] = 1.0
        out *= keep[None, :]
    return out


def bake(recipe, args):
    if not args.pictures:
        raise SystemExit("bake: a flat recipe needs --pictures <folder with the recipe's pictures>")
    root = args.pictures
    canvas_w, canvas_h = recipe["canvas"]
    height = int(recipe.get("height", 560))
    scale = height / canvas_h
    width = int(round(canvas_w * scale))
    centre = recipe.get("centre_x", canvas_w // 2)
    # The canvas's corner, from the anchor. `sink` puts her that many pixels below the bottom of the
    # stage, so that the cut edge of the picture stays out of sight when she hops.
    sink = int(recipe.get("sink", 0))
    left, top = -int(round(centre * scale)), -height + sink
    fade_part = float(recipe.get("fade", 0.05))
    rows = np.arange(-height, 0, dtype=np.float32)
    fade = np.clip((0 - rows) / max(height * fade_part, 1.0), 0, 1) if fade_part > 0 else np.ones_like(rows)
    fade = (fade * fade * (3 - 2 * fade))[:, None, None]
    sprites = pack.Sprites()
    loaded = {}

    def picture(name):
        if name not in loaded:
            loaded[name] = keyed(root / name, (canvas_w, canvas_h))
        return loaded[name]

    def report(name, box, index):
        print(f"{name}: {box[2]}x{box[3]} at ({box[0]}, {box[1]}), {sprites.size_of(index) // 1024} KB", flush=True)
        if args.preview:
            args.preview.mkdir(parents=True, exist_ok=True)
            a = sprites.alphas[index]
            Image.fromarray((np.clip(a / 32.0, 0, 1) * 255).astype(np.uint8), "L").save(args.preview / f"{name}.alpha.png")

    small = {}

    def shrunk(file):
        if file not in small:
            rgb, alpha = picture(file)
            small[file] = shrink(premultiplied(rgb, alpha), width, height) * fade
        return small[file]

    # Her face, with room for a head that a pose holds a little differently.
    fx0, fy0, fx1, fy1 = recipe["boxes"]["face"]
    focus = (max(0, (fx0 - 60) * scale), max(0, (fy0 - 60) * scale), min(width, (fx1 + 60) * scale),
             min(height, (fy1 + 40) * scale))
    indexed = bool(recipe.get("palette", False))

    wholes = {}

    def whole(name, slots, file, shift=(0, 0), covers=()):
        """A whole-figure picture; `shift` moves it on the canvas (a pose drawn lower than the base);
        `covers`: pictures that are patched over it, whose pixels its box must hold as well. A picture that is
        baked already (a pose that is also a step of a dance) is not baked again. Returns the sprite's name."""
        key = (file, tuple(shift))
        if key in wholes and not covers:
            sprites.alias(slots, wholes[key])
            return wholes[key]
        dx, dy = int(round(shift[0] * scale)), int(round(shift[1] * scale))
        cover = None
        for other in covers:
            cover = shrunk(other) if cover is None else np.maximum(cover, shrunk(other))
        box = sprites.add(name, slots, shrunk(file), left + dx, top + dy, palette=indexed, focus=focus, cover=cover)
        report(name, box, len(sprites.names) - 1)
        wholes.setdefault(key, name)
        return name

    # The patches are what differs from `base`; `body` may name a cleaner cut of the same picture.
    base_rgb, _ = picture(recipe["base"])
    boxes, marks = recipe["boxes"], recipe["marks"]
    whole("BODY", [pack.SLOT_BODY], recipe.get("body", recipe["base"]))

    def patch(name, slots, file, box, columns=None):
        """True when the picture differs from the base in the box and became a sprite."""
        rgb, alpha = picture(file)
        cover = difference(base_rgb, rgb, box, columns) * alpha
        if cover.max() < 0.5:
            print(f"{name}: {file} does not differ from the base there, no sprite")
            return False
        small = shrink(premultiplied(rgb, cover), width, height)
        if np.rint(small[..., 3] * 32).max() < 1:
            return False
        report(name, sprites.add(name, slots, small, left, top), len(sprites.names) - 1)
        return True

    middle = (marks["eye_l"][0] + marks["eye_r"][0]) // 2
    head = pack.SLOT_HEAD
    # The figure's left eye is on the viewer's right.
    sides = (("L", pack.HEAD_EYE_L, (middle, canvas_w), {"wide": pack.SLOT_EYE_L_WIDE, "up": pack.SLOT_EYE_L_UP,
                                                         "ponder": pack.SLOT_EYE_L_PONDER, "glad": pack.SLOT_EYE_L_GLAD,
                                                         "down": pack.SLOT_EYE_L_DOWN, "teary": pack.SLOT_EYE_L_TEARY,
                                                         "soft": pack.SLOT_EYE_L_SOFT, "bright": pack.SLOT_EYE_L_BRIGHT,
                                                         "comfort": pack.SLOT_EYE_L_COMFORT, "serious": pack.SLOT_EYE_L_SERIOUS}),
             ("R", pack.HEAD_EYE_R, (0, middle), {"wide": pack.SLOT_EYE_R_WIDE, "up": pack.SLOT_EYE_R_UP,
                                                  "ponder": pack.SLOT_EYE_R_PONDER, "glad": pack.SLOT_EYE_R_GLAD,
                                                  "down": pack.SLOT_EYE_R_DOWN, "teary": pack.SLOT_EYE_R_TEARY,
                                                  "soft": pack.SLOT_EYE_R_SOFT, "bright": pack.SLOT_EYE_R_BRIGHT,
                                                  "comfort": pack.SLOT_EYE_R_COMFORT, "serious": pack.SLOT_EYE_R_SERIOUS}))
    eyes = recipe.get("eyes", {})
    for side, first, columns, looks in sides:
        # Slots: open and half open at three gazes, then nearly shut, smiling shut, shut.
        for gaze, key in ((0, "left"), (2, "right")):
            if key in eyes:
                patch(f"EYE_{side}_{key.upper()}", [head + first + gaze], eyes[key], boxes["eyes"], columns)
        if "half" in eyes:
            patch(f"EYE_{side}_HALF", [head + first + 3 + g for g in range(GAZES)], eyes["half"], boxes["eyes"], columns)
        for slot, key in ((6, "nearly"), (7, "smile"), (8, "shut")):
            if key in eyes:
                patch(f"EYE_{side}_{key.upper()}", [head + first + slot], eyes[key], boxes["eyes"], columns)
        if "nearly" not in eyes and "half" in eyes and f"EYE_{side}_HALF" in sprites.names:
            sprites.alias([head + first + 6], f"EYE_{side}_HALF")
        for key, slot in looks.items():
            if key in eyes:
                patch(f"EYE_{side}_{key.upper()}", [slot], eyes[key], boxes["eyes"], columns)
    mouths = recipe.get("mouth", {})
    for key, file in mouths.items():
        slots = [head + pack.HEAD_MOUTH + o * 3 + f for o, row in enumerate(MOUTHS) for f, name in enumerate(row) if name == key]
        slots += {"comfort": [pack.SLOT_MOUTH_COMFORT], "serious": [pack.SLOT_MOUTH_SERIOUS], "smirk": [pack.SLOT_MOUTH_SMIRK],
                  "frown": [pack.SLOT_MOUTH_FROWN]}.get(key, [])
        if slots:
            patch(f"MOUTH_{key.upper()}", slots, file, boxes["mouth"])
    for index, key in enumerate(("raised", "cross", "sad")):
        if key in recipe.get("brows", {}):
            patch(f"BROWS_{key.upper()}", [pack.SLOT_BROWS + index], recipe["brows"][key], boxes["brows"])
    for slot, key in ((pack.SLOT_BROWS_COMFORT, "comfort"), (pack.SLOT_BROWS_SERIOUS, "serious")):
        if key in recipe.get("brows", {}):
            patch(f"BROWS_{key.upper()}", [slot], recipe["brows"][key], boxes["brows"])
    for index, key in enumerate(("puff", "cry")):
        if key in recipe.get("face", {}):
            patch(f"FACE_{key.upper()}", [pack.SLOT_FACE + index], recipe["face"][key], boxes["face"])
    pose_names = {}
    for index, key in enumerate(pack.POSE_NAMES):
        if key in recipe.get("poses", {}):
            pose = recipe["poses"][key]
            clip = recipe.get("clips", {}).get(key, {})
            patched = [frame[0] for part in ("enter", "loop", "leave") for frame in clip.get(part, [])
                       if len(frame) > 2 and frame[2] == "@pose"]
            file = pose["file"]
            if pose.get("face"):
                # {"eyes": file, "mouth": file, ...}: edits of the base's face put on a pose whose face is the base's
                # (lined up with it): only where the edit differs from the base, inside the box of that part.
                rgb, alpha = picture(file)
                rgb = rgb.copy()
                for part, edit in sorted(pose["face"].items()):
                    edit_rgb, edit_alpha = picture(edit)
                    cover = (difference(base_rgb, edit_rgb, boxes[part]) * edit_alpha)[..., None]
                    rgb = rgb * (1.0 - cover) + edit_rgb * cover
                file = file + "|" + "|".join(f"{part}={edit}" for part, edit in sorted(pose["face"].items()))
                loaded[file] = (rgb, alpha)
            pose_names[key] = whole(f"POSE_{key.upper()}", [pack.SLOT_POSE + index], file, pose.get("shift", (0, 0)), patched)
    # A gesture played with several pictures: {"enter": [[file, ms], ...], "loop": [...], "leave": [...],
    # "style": "pop" | "soft" | "beat", "shift": [dx, dy]}. A file used twice is baked once, "@base" is
    # the base picture, "@pose" the gesture's picture from "poses"; the gesture's slot takes the first
    # picture, which is what says that this figure has the gesture. A frame [file, ms, "@pose"] is a
    # second picture of the pose that differs from it in a hand or an eye: only what differs is stored.
    for index, key in enumerate(pack.POSE_NAMES):
        clip = recipe.get("clips", {}).get(key)
        if not clip:
            continue
        baked = {}

        def frames(part):
            out = []
            for frame in clip.get(part, []):
                file, ms, under = frame[0], frame[1], frame[2] if len(frame) > 2 else None
                if under not in (None, "@pose"):
                    raise SystemExit(f"bake: clip {key}: a picture can only be a patch over \"@pose\"")
                if file == "@base":
                    baked[file] = "BODY"
                elif file == "@pose":
                    baked[file] = pose_names.get(key, f"POSE_{key.upper()}")
                elif file not in baked:
                    baked[file] = f"CLIP_{key.upper()}_{len(baked)}"
                    if under:
                        box, share = sprites.add_patch(baked[file], shrunk(recipe["poses"][key]["file"]), shrunk(file), left, top,
                                                       palette=indexed, focus=focus)
                        report(baked[file], box, len(sprites.names) - 1)
                        print(f"{baked[file]}: a patch over POSE_{key.upper()}, {share * 100:.1f}% of the picture")
                    else:
                        baked[file] = whole(baked[file], [], file, clip.get("shift", (0, 0)))
                out.append((baked[file], ms, pose_names.get(key, f"POSE_{key.upper()}") if under else None))
            return out

        enter, loop, leave = frames("enter"), frames("loop"), frames("leave")
        sprites.clip(index + 1, enter, loop, leave, pack.STYLES[clip.get("style", "pop")])
        sprites.alias([pack.SLOT_POSE + index], (enter or loop)[0][0])

    # Effects: {"kind": {"file": ..., "frames": n, "scale": s, "every": k, "palette": b}} with a kind of pack.FX_NAMES.
    # The file is drawn at the stage's size already (a picture of several frames is a row of n equal cells); `scale`
    # bakes it larger or smaller, `every` keeps every k-th frame (they stay in memory: fewer frames, less of it),
    # `palette` stores it in 256 colours of its own. Each frame is placed around its middle, which the device puts where
    # the effect is.
    for kind, spec in recipe.get("fx", {}).items():
        if kind not in pack.FX_NAMES or kind == "puff":
            raise SystemExit(f"bake: no effect is called {kind}")
        spec = spec if isinstance(spec, dict) else {"file": spec}
        count, size, every = int(spec.get("frames", 1)), float(spec.get("scale", 1.0)), int(spec.get("every", 1))
        kept = list(range(0, count, max(every, 1)))
        if not 1 <= len(kept) <= pack.FX_FRAMES:
            raise SystemExit(f"bake: effect {kind} keeps {len(kept)} frames, at most {pack.FX_FRAMES}")
        sheet = Image.open(root / spec["file"]).convert("RGBa")
        cell = sheet.width // count
        for i, source in enumerate(kept):
            image = sheet.crop((source * cell, 0, (source + 1) * cell, sheet.height))
            if abs(size - 1.0) > 1e-3:
                image = image.resize((max(1, round(image.width * size)), max(1, round(image.height * size))), Image.LANCZOS)
            pm = np.asarray(image, dtype=np.float32) / 255.0
            slot = pack.SLOT_FX + pack.FX_NAMES.index(kind) * pack.FX_FRAMES + i
            name = f"FX_{kind.upper()}_{i}"
            report(name, sprites.add(name, [slot], pm, -(image.width // 2), -(image.height // 2), palette=bool(spec.get("palette"))),
                   len(sprites.names) - 1)
    fx_bytes = sum(sprites.size_of(i) for i, name in enumerate(sprites.names) if name.startswith("FX_"))
    if fx_bytes:
        print(f"effects: {fx_bytes // 1024} KB of the pack")

    def x_of(value):
        return (value - centre) * scale

    def y_of(value):
        return (value - canvas_h) * scale + sink

    eye_l, eye_r, mouth = marks["eye_l"], marks["eye_r"], marks["mouth"]
    apart = x_of(eye_l[0]) - x_of(eye_r[0])
    eye_y = y_of((eye_l[1] + eye_r[1]) / 2)
    layout = {
        "hair_top": y_of(marks["hair_top"]), "ears_top": y_of(marks["hair_top"]), "ear_base_y": y_of(marks["hair_top"]),
        "brow_y": y_of(marks["brow_y"]), "eye_y": eye_y, "mouth_y": y_of(mouth[1]),
        "cheek_y": y_of(marks.get("cheek_y", eye_l[1] + 0.53 * (mouth[1] - eye_l[1]))),
        "neck_y": y_of(marks["chin_y"]), "shoulder_y": y_of(marks["shoulder_y"]),
        "eye_lx": x_of(eye_l[0]), "eye_rx": x_of(eye_r[0]), "face_x": (x_of(eye_l[0]) + x_of(eye_r[0])) / 2,
        "head_half": marks["head_half"] * scale, "brow_lx": x_of(eye_l[0]), "brow_rx": x_of(eye_r[0]),
        "cheek_dx": 0.3 * apart, "blush_rx": 0.25 * apart, "blush_ry": 0.12 * apart,
        "shoulder_half": marks["shoulder_half"] * scale,
    }
    if "arm_l" in marks:
        layout.update(arm_lx=x_of(marks["arm_l"][0]), arm_rx=x_of(marks["arm_r"][0]),
                      arm_top=y_of(min(marks["arm_l"][1], marks["arm_r"][1])))
    if "hand_l" in marks:
        layout.update(hand_lx=x_of(marks["hand_l"][0]), hand_ly=y_of(marks["hand_l"][1]), hand_half=marks["hand_half"] * scale)
    if "hand_r" in marks:
        layout.update(hand_rx=x_of(marks["hand_r"][0]), hand_ry=y_of(marks["hand_r"][1]), hand_half=marks["hand_half"] * scale)
    sprites.write(args.out, height, layout)
    whole_bytes = sum(sprites.size_of(i) for i, name in enumerate(sprites.names) if name == "BODY" or name.startswith("POSE_"))
    print(f"whole-figure pictures: {whole_bytes // 1024} KB of the pack")
    return 0
