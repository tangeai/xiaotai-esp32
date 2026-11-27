#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bake the character's sprite pack.

The device does not deform meshes. It stacks pictures and moves them, so this
tool makes those pictures at the size the display shows them and writes one
file, sdk/themes/xiaotai_theme_character/assets/character.pack, which is
checked in: building the firmware needs neither the source pictures nor this
tool.

A recipe (tools/character/recipes/*.json) says who she is and what she is
made from. The pack carries everything the firmware needs to know about the
figure (where the eyes, the mouth and the hands are, which parts exist), so a
different recipe is a different character without a change to the firmware's
code. Two kinds of recipe:

  flat     one base picture of the whole figure, patches for the eyes and the
           mouth cut from edited copies of it, and whole-figure poses
           (flat.py). The shipped character is made this way:

               python tools/character/bake.py --pictures <folder>

  puppets  Inochi2D puppets posed by their own parameters and cut into layers
           (hair, face, one picture per eye state and mouth shape, arms, ...):

               python tools/character/bake.py --recipe tools/character/recipes/aka-midori.json \\
                   --puppet head=Aka.inx --puppet body=Midori.inx

The source pictures and puppets are not part of this repository; each recipe
says where they come from and under which terms. Needs numpy and Pillow.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import flat  # noqa: E402
import inx  # noqa: E402
import pack  # noqa: E402
from pack import (ARM_STATES, EMPTY, HEAD_BROW_L, HEAD_BROW_R, HEAD_EYE_L, HEAD_EYE_R, HEAD_FACE,  # noqa: E402
                  HEAD_HAIR_BACK, HEAD_HAIR_FRONT, HEAD_MOUTH, HEAD_SLOTS, LAYOUT, SLOT_ARM_L, SLOT_ARM_R, SLOT_BODY,
                  SLOT_EAR_L, SLOT_EAR_R, SLOT_HEAD, SLOT_TAIL, YAWS)

ROOT = Path(__file__).resolve().parents[2]
COMPONENT = ROOT / "sdk" / "themes" / "xiaotai_theme_character"

BLINK = (0.0, 0.25, 0.5, 0.75, 1.0)     # open, half, nearly shut, smiling shut, shut
GAZE = (-1.0, 0.0, 1.0)
MOUTH_FORM = (0.0, 0.5, 1.0)            # pout, plain, smile
MOUTH_OPEN = (0.0, 0.25, 0.5, 0.75, 1.0)
EAR = (-1.0, 0.0, 1.0)

HEAD_GROUPS = {"face", "eye", "iris", "mouth", "brow", "fronthair", "leaf", "hairshadow", "backhair", "ears"}


def group_of(node):
    """Which part of the figure a node belongs to. The names are those of the puppets' common template."""
    path = node.path()
    name = path[0]

    def has(*prefixes):
        return any(part.startswith(prefixes) for part in path)

    if has("Ear::"):
        return "ears"
    if name == "Tail":
        return "tail"
    if name == "Leaf":
        return "leaf"
    if has("Eyebrow::"):
        return "brow"
    if "Hair:: Shadows:: Composite" in path:
        return "hairshadow"
    if has("Eye") and ("Eyeball" in name or name.endswith(":: Light")):
        return "iris"
    if has("Eye"):
        return "eye"
    if has("Lip", "Mouth"):
        return "mouth"
    if has("Front Hair::", "Side Hair::"):
        return "fronthair"
    if has("Back Side"):
        return "backhair"
    if name in ("Face", "Nose"):
        return "face"
    if has("Neck"):
        return "neck"
    if has("Heart Robe"):
        return "heart"
    if has("Ribbon"):
        return "ribbon"
    if has("Collar"):
        return "collar"
    if has("Skirt"):
        return "skirt"
    if has("Arm::", "Hand::"):
        return "arm_l" if has("Arm:: Left", "Hand:: Left") else "arm_r"
    if has("Leg::"):
        return "leg"
    if has("Body", "Breast"):
        return "body"
    return "other"


def rgb_to_hsv(rgb):
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    high, low = rgb.max(-1), rgb.min(-1)
    spread = high - low
    safe = np.where(spread > 1e-6, spread, 1.0)
    hue = np.where(high == r, (g - b) / safe, np.where(high == g, 2.0 + (b - r) / safe, 4.0 + (r - g) / safe))
    hue = np.where(spread > 1e-6, (hue / 6.0) % 1.0, 0.0)
    saturation = np.where(high > 1e-6, spread / np.where(high > 1e-6, high, 1.0), 0.0)
    return np.stack([hue, saturation, high], -1)


def hsv_to_rgb(hsv):
    h, s, v = hsv[..., 0], hsv[..., 1], hsv[..., 2]
    sector = np.floor(h * 6.0).astype(np.int32) % 6
    f = h * 6.0 - np.floor(h * 6.0)
    p, q, t = v * (1 - s), v * (1 - f * s), v * (1 - (1 - f) * s)
    out = np.zeros(hsv.shape, dtype=np.float32)
    for index, (r, g, b) in enumerate(((v, t, p), (q, v, p), (p, v, t), (p, q, v), (t, p, v), (v, p, q))):
        chosen = sector == index
        out[..., 0][chosen], out[..., 1][chosen], out[..., 2][chosen] = r[chosen], g[chosen], b[chosen]
    return out


def recolour(puppet, node, rule):
    """Rewrites a node's texture: a new hue (or a shift), saturation and value scaled, value lifted.
    "only": [low, high] limits it to the pixels whose hue lies in that range."""
    if node.texture is None:
        return
    image = puppet.texture(node.texture).copy()
    alpha = image[..., 3:4]
    rgb = np.clip(np.where(alpha > 1e-4, image[..., :3] / np.maximum(alpha, 1e-4), 0.0), 0.0, 1.0)
    hsv = rgb_to_hsv(rgb)
    chosen = np.ones(hsv.shape[:2], dtype=bool)
    if "only" in rule:
        low, high = rule["only"]
        hue = hsv[..., 0]
        chosen = ((hue >= low) & (hue <= high)) if low <= high else ((hue >= low) | (hue <= high))
        chosen &= hsv[..., 1] > rule.get("min_sat", 0.15)
    new = hsv.copy()
    if "hue" in rule:
        new[..., 0] = rule["hue"]
    if "shift" in rule:
        new[..., 0] = (new[..., 0] + rule["shift"]) % 1.0
    new[..., 1] = np.clip(new[..., 1] * rule.get("sat", 1.0), 0.0, 1.0)
    new[..., 2] = np.clip(new[..., 2] * rule.get("val", 1.0) + rule.get("lift", 0.0), 0.0, 1.0)
    hsv[chosen] = new[chosen]
    image[..., :3] = hsv_to_rgb(hsv) * alpha
    puppet.replace_texture(node.texture, image)


def prepare(puppet, colours, head_scale):
    """Applies a recipe's colours and head size to one puppet."""
    done = set()
    for node in puppet.root.walk():
        group = group_of(node)
        rule = colours.get(group) or colours.get(group.split("_")[0])
        if rule and node.texture is not None and node.texture not in done:
            for step in (rule if isinstance(rule, list) else [rule]):   # several rules: one after another
                recolour(puppet, node, step)
            done.add(node.texture)
    for node in puppet.root.walk():       # the brows multiply onto the hair; alone they are plain pictures
        if node.name.startswith("Eyebrow::"):
            node.blend = "Normal"
    if head_scale != 1.0:
        face = next(node for node in puppet.root.walk() if node.name == "Face")
        before = puppet.bounds({}, lambda node: node is face)
        face.scale = [face.scale[0] * head_scale, face.scale[1] * head_scale]
        after = puppet.bounds({}, lambda node: node is face)
        # The chin stays where it was and the head grows upwards from it.
        face.trans[0] += ((before[0] + before[2]) - (after[0] + after[2])) * 0.5 / max(_parent_scale(face), 1e-6)
        face.trans[1] += (before[3] - after[3]) / max(_parent_scale(face), 1e-6)


def _parent_scale(node):
    scale = 1.0
    node = node.parent
    while node is not None:
        scale *= node.scale[1]
        node = node.parent
    return scale


def plan(recipe):
    """Every sprite to bake: (name, slots, puppet, accepted groups, parameter values[, shift in puppet units
    [, (node, radians) to turn that node by]])."""
    keep = lambda part: part not in recipe.get("drop", [])  # noqa: E731
    out = []
    if keep("tail"):
        out.append(("TAIL", [SLOT_TAIL], "head", {"tail"}, {}, tuple(recipe.get("tail_shift", (0.0, 0.0)))))
    if keep("ears"):
        for side, sign, slot in (("L", "Left", SLOT_EAR_L), ("R", "Right", SLOT_EAR_R)):
            for index, value in enumerate(EAR):
                group = lambda node, sign=sign: any(p.startswith(f"Ear:: {sign}") for p in node.path())  # noqa: E731
                out.append((f"EAR_{side}{index}", [slot + index], "head", group, {f"Ear:: {sign}:: Physics": [value, 0.0]}))
    out.append(("BODY", [SLOT_BODY], "body", lambda node: group_of(node) not in HEAD_GROUPS | {"tail", "arm_l", "arm_r"}, {}))
    arms = recipe.get("arms") or []
    for side, sign, slot in (("L", "Left", SLOT_ARM_L), ("R", "Right", SLOT_ARM_R)):
        for index, state in enumerate(arms[:ARM_STATES]):
            # "lift" raises the arm further than the puppet's own parameter goes: it turns about the shoulder.
            turn = (f"Arm:: {sign}", math.radians(state.get("lift", 0.0)) * (-1.0 if side == "L" else 1.0))
            out.append((f"ARM_{side}{index}", [slot + index], "body", {f"arm_{side.lower()}"},
                        {f"Arm:: {sign}:: Move": state["move"]}, (0.0, 0.0), turn))
    front = {"fronthair"} | ({"leaf"} if keep("leaf") else set())
    for y, yaw in enumerate(recipe.get("yaw", [0.0])[:YAWS]):
        base = SLOT_HEAD + y * HEAD_SLOTS
        turn = {"Head:: Yaw-Pitch": [yaw, 0.0]}
        tag = f"_Y{y}" if y else ""
        out.append((f"HAIR_BACK{tag}", [base + HEAD_HAIR_BACK], "head", {"backhair"}, turn))
        out.append((f"FACE{tag}", [base + HEAD_FACE], "head", {"face", "hairshadow"}, turn))
        for o, opened in enumerate(MOUTH_OPEN):
            for f, form in enumerate(MOUTH_FORM):
                out.append((f"MOUTH_{f}{o}{tag}", [base + HEAD_MOUTH + o * 3 + f], "head", {"mouth"},
                            {**turn, "Mouth:: Shape": [form, opened]}))
        for side, sign, slot in (("L", "Left", HEAD_EYE_L), ("R", "Right", HEAD_EYE_R)):
            group = lambda node, sign=sign: group_of(node) in ("eye", "iris") and any(  # noqa: E731
                p.startswith(f"Eye:: {sign}") for p in node.path())
            for b, blink in enumerate(BLINK):
                for g, gaze in enumerate(GAZE):
                    if g != 1 and (b >= 2 or y):
                        continue
                    values = {**turn, f"Eye:: {sign}:: Blink": blink, f"Eye:: {sign}:: Move": gaze}
                    if b < 2:   # a turned head has one picture per lid state: the other gazes show it too
                        slots = [base + slot + b * 3 + g] if not y else [base + slot + b * 3 + k for k in range(3)]
                    else:
                        slots = [base + slot + 6 + (b - 2)]
                    out.append((f"EYE_{side}{b}{g}{tag}", slots, "head", group, values))
        out.append((f"HAIR_FRONT{tag}", [base + HEAD_HAIR_FRONT], "head", front, turn))
        out.append((f"BROW_L{tag}", [base + HEAD_BROW_L], "head",
                    lambda node: any(p.startswith("Eyebrow:: Left") for p in node.path()), turn))
        out.append((f"BROW_R{tag}", [base + HEAD_BROW_R], "head",
                    lambda node: any(p.startswith("Eyebrow:: Right") for p in node.path()), turn))
    return out


def layout_of(boxes, slots, yaws):
    """Where the firmware finds the parts, from the boxes of the baked sprites (display pixels from the anchor)."""
    def box(slot):
        index = slots[slot]
        return None if index == EMPTY else boxes[index]

    def centre(b):
        return (b[0] + b[2] * 0.5, b[1] + b[3] * 0.5)

    head = SLOT_HEAD
    face, back, front = box(head + HEAD_FACE), box(head + HEAD_HAIR_BACK), box(head + HEAD_HAIR_FRONT)
    eye_l, eye_r = centre(box(head + HEAD_EYE_L + 1)), centre(box(head + HEAD_EYE_R + 1))
    brow_l, brow_r = centre(box(head + HEAD_BROW_L)), centre(box(head + HEAD_BROW_R))
    mouth = centre(box(head + HEAD_MOUTH + 1))
    apart = eye_l[0] - eye_r[0]
    eye_y = (eye_l[1] + eye_r[1]) * 0.5
    hair_left, hair_right = min(back[0], front[0]), max(back[0] + back[2], front[0] + front[2])
    out = {name: 0 for name in LAYOUT}
    out.update(hair_top=min(back[1], front[1]), brow_y=(brow_l[1] + brow_r[1]) * 0.5, eye_y=eye_y, mouth_y=mouth[1],
               cheek_y=eye_y + 0.69 * (mouth[1] - eye_y), neck_y=face[1] + face[3] - 6, eye_lx=eye_l[0], eye_rx=eye_r[0],
               face_x=(eye_l[0] + eye_r[0]) * 0.5, head_half=(face[2] + hair_right - hair_left) * 0.25,
               brow_lx=brow_l[0], brow_rx=brow_r[0], cheek_dx=0.3 * apart, blush_rx=0.25 * apart, blush_ry=0.12 * apart)
    out["ears_top"] = out["ear_base_y"] = out["hair_top"]
    ear_l, ear_r = box(SLOT_EAR_L + 1), box(SLOT_EAR_R + 1)
    if ear_l and ear_r:
        out.update(ears_top=min(ear_l[1], ear_r[1]), ear_base_y=max(ear_l[1] + ear_l[3], ear_r[1] + ear_r[3]) - 4,
                   ear_lx0=ear_l[0], ear_lx1=ear_l[0] + ear_l[2], ear_rx0=ear_r[0], ear_rx1=ear_r[0] + ear_r[2])
    tail = box(SLOT_TAIL)
    if tail:
        out.update(tail_x0=tail[0], tail_x1=tail[0] + tail[2], tail_top=tail[1])
    body = box(SLOT_BODY)
    out["shoulder_half"] = min((body[2] if body else 2.5 * out["head_half"]) * 0.5, 1.3 * out["head_half"])
    arm_l, arm_r = box(SLOT_ARM_L), box(SLOT_ARM_R)
    if arm_l and arm_r:
        # At rest the arms hang beside the body; raised, the hand is the outer end of the highest picture.
        out.update(arm_lx=arm_l[0] + arm_l[2] * 0.5, arm_rx=arm_r[0] + arm_r[2] * 0.5, arm_top=min(arm_l[1], arm_r[1]))
        up_l = next(b for b in (box(SLOT_ARM_L + k) for k in reversed(range(ARM_STATES))) if b)
        up_r = next(b for b in (box(SLOT_ARM_R + k) for k in reversed(range(ARM_STATES))) if b)
        half = max(up_l[2], up_r[2]) * 0.16
        out.update(hand_lx=up_l[0] + up_l[2] - half, hand_rx=up_r[0] + half, hand_half=half)
    if yaws > 1:
        turned = centre(box(SLOT_HEAD + HEAD_SLOTS + HEAD_EYE_L + 1)), centre(box(SLOT_HEAD + HEAD_SLOTS + HEAD_EYE_R + 1))
        out["yaw_shift"] = abs((turned[0][0] + turned[1][0]) * 0.5 - out["face_x"])
    return out


def hand_rows(image_alpha, left, top, x_from, x_to):
    """The middle row of what a picture shows between two columns (the raised hand)."""
    columns = image_alpha[:, max(x_from - left, 0):max(x_to - left, 0)]
    rows = np.nonzero(columns.any(axis=1))[0]
    return top + (int(rows.min()) + int(rows.max())) // 2 if rows.size else top


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--puppet", action="append", default=[], metavar="ROLE=FILE",
                        help="puppet recipes: a puppet the recipe names (head=..., body=...); a single FILE is both")
    parser.add_argument("--pictures", type=Path, help="flat recipes: the folder that holds the recipe's pictures")
    parser.add_argument("--recipe", type=Path, default=Path(__file__).resolve().parent / "recipes" / "xiaotai.json")
    parser.add_argument("--out", type=Path, default=COMPONENT / "assets" / "character.pack")
    parser.add_argument("--preview", type=Path, help="folder for one PNG per sprite")
    parser.add_argument("--cache", type=Path, help="folder that keeps baked sprites: a sprite found there is not "
                        "drawn again (empty it after changing the recipe)")
    parser.add_argument("--part", metavar="K/N", help="with --cache: only draw every N-th sprite starting at K and "
                        "write no pack, so that N runs side by side fill the cache")
    args = parser.parse_args()
    part = tuple(int(v) for v in args.part.split("/")) if args.part else None
    if part and not args.cache:
        parser.error("--part needs --cache")
    recipe = json.loads(args.recipe.read_text(encoding="utf-8"))
    if recipe.get("kind") == "flat":
        return flat.bake(recipe, args)
    files = {}
    for item in args.puppet:
        role, _, path = item.rpartition("=")
        for name in ([role] if role else ["head", "body"]):
            files[name] = Path(path)
    puppets = {}
    for role in ("head", "body"):
        source = recipe["puppets"][role]
        if role not in files:
            raise SystemExit(f"bake: the recipe needs --puppet {role}={source['file']}")
        digest = hashlib.sha256(files[role].read_bytes()).hexdigest()
        if digest != source["sha256"]:
            print(f"WARN: {files[role]} is not the {source['file']} this recipe was written for (sha256 {digest})",
                  file=sys.stderr)
        puppets[role] = inx.Puppet.load(files[role])
        prepare(puppets[role], recipe.get("colour", {}).get(role, {}), recipe.get("head_scale", 1.0) if role == "head" else 1.0)

    frame = recipe["frame"]
    top_edge, bottom_edge, anchor_x = frame["top"], frame["bottom"], frame["anchor_x"]
    height = int(recipe.get("height", 520))
    fade_part = float(recipe.get("fade", 0.14))
    scale = height / (bottom_edge - top_edge)
    sprites = pack.Sprites()
    names, boxes, alphas, slots = sprites.names, sprites.boxes, sprites.alphas, sprites.slots
    for number, (name, wanted, role, groups, values, *more) in enumerate(plan(recipe)):
        cached = args.cache / f"{name}.npz" if args.cache else None
        if part and number % part[1] != part[0]:
            continue
        if cached and cached.exists():
            saved = np.load(cached)
            box = tuple(int(v) for v in saved["box"])
            sprites.add_coded(name, wanted, saved["a"], saved["colour"], box[0], box[1])
            continue
        puppet = puppets[role]
        include = groups if callable(groups) else (lambda node, groups=groups: group_of(node) in groups)
        shift = more[0] if more else (0.0, 0.0)
        turned = None
        if len(more) > 1 and more[1][1]:
            turned = next(node for node in puppet.root.walk() if node.name == more[1][0])
            turned.rot += more[1][1]
        bounds = puppet.bounds(values, include)
        box = (bounds[0] - 20, max(bounds[1] - 20, top_edge - shift[1]), bounds[2] + 20, bottom_edge - shift[1])
        # Snap the box to whole display pixels from the anchor, so every sprite shares one grid.
        left = int(np.floor((box[0] - anchor_x) * scale))
        top = int(np.floor((box[1] - bottom_edge) * scale))
        right = int(np.ceil((box[2] - anchor_x) * scale))
        bottom = int(np.ceil((box[3] - bottom_edge) * scale))
        snapped = (left / scale + anchor_x, top / scale + bottom_edge, right / scale + anchor_x, bottom / scale + bottom_edge)
        image = puppet.render(values, scale=scale, box=snapped, include=include, oversample=3)
        left += int(round(shift[0] * scale))
        top += int(round(shift[1] * scale))
        image = image[:max(-top, 0)]
        rows = np.arange(top, top + image.shape[0], dtype=np.float32)
        fade = np.clip((0 - rows) / (height * fade_part), 0, 1)
        image = image * (fade * fade * (3 - 2 * fade))[:, None, None]
        a, colour = pack.quantize(image, left, top)
        ys, xs = np.nonzero(a)
        if not ys.size:
            raise SystemExit(f"bake: sprite {name} is empty")
        y0, y1, x0, x1 = int(ys.min()), int(ys.max()) + 1, int(xs.min()), int(xs.max()) + 1
        a, colour = a[y0:y1, x0:x1], colour[y0:y1, x0:x1]
        if turned is not None:
            turned.rot -= more[1][1]
        if cached:
            args.cache.mkdir(parents=True, exist_ok=True)
            unfinished = cached.with_suffix(".tmp.npz")
            np.savez_compressed(unfinished, a=a, colour=colour, box=np.array([left + x0, top + y0, x1 - x0, y1 - y0]))
            unfinished.replace(cached)
        sprites.add_coded(name, wanted, a, colour, left + x0, top + y0)
        if args.preview:
            args.preview.mkdir(parents=True, exist_ok=True)
            inx.save(image[y0:y1, x0:x1], args.preview / f"{name}.png")
        print(f"{name}: {x1 - x0}x{y1 - y0} at ({left + x0}, {top + y0}), {sprites.size_of(len(names) - 1) // 1024} KB", flush=True)

    if part:
        print(f"part {args.part}: {len(names)} sprites in {args.cache}")
        return 0
    layout = layout_of(boxes, slots, len(recipe.get("yaw", [0.0])[:YAWS]))
    for side, slot in (("l", SLOT_ARM_L), ("r", SLOT_ARM_R)):
        raised = next((slots[slot + k] for k in reversed(range(ARM_STATES)) if slots[slot + k] != EMPTY), EMPTY)
        if raised != EMPTY:
            x, half = int(layout[f"hand_{side}x"]), int(layout["hand_half"])
            layout[f"hand_{side}y"] = hand_rows(alphas[raised], boxes[raised][0], boxes[raised][1], x - half, x + half)

    layout["shoulder_y"] = layout["neck_y"] + 12
    sprites.write(args.out, height, layout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
