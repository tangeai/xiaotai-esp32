# SPDX-License-Identifier: MIT
"""Read an Inochi2D puppet (.inx / .inp) and draw it with numpy.

Only what baking sprites needs: the node tree with its transforms, meshes and
textures, parameters with their bindings (deform, translate, rotate, scale,
sort), masks, composites and the blend modes Normal, Multiply, ClipToLower and
LinearDodge. Physics nodes are not simulated; their parameters are set like
any other. Mesh groups are read as plain nodes: what they deform is drawn
undeformed, which is right as long as their parameters stay at rest.

    puppet = Puppet.load("Aka.inx")
    image = puppet.render({"Eye:: Left:: Blink": 1.0}, scale=0.25, box=(-600, -1500, 600, 0))

`image` is float32 RGBA, premultiplied, height x width x 4.
"""

import io
import json
import math
import struct

import numpy as np
from PIL import Image


class Node:
    def __init__(self, data, parent):
        self.uuid = data["uuid"]
        self.name = (data.get("name") or "").rstrip(chr(0))   # some editors pad names
        self.kind = data.get("type")
        self.parent = parent
        self.enabled = data.get("enabled", True)
        self.zsort = float(data.get("zsort", 0.0))
        transform = data.get("transform", {})
        self.trans = list(transform.get("trans", [0, 0, 0]))[:2]
        self.rot = float(list(transform.get("rot", [0, 0, 0]))[2])
        self.scale = list(transform.get("scale", [1, 1]))
        self.lock_to_root = data.get("lockToRoot", False)
        self.blend = data.get("blend_mode", "Normal")
        self.opacity = float(data.get("opacity", 1.0))
        self.tint = list(data.get("tint", [1, 1, 1]))
        self.threshold = float(data.get("mask_threshold", 0.5))
        self.masks = [(mask["source"], mask.get("mode", "Mask")) for mask in data.get("masks") or []]
        self.texture = (data.get("textures") or [None])[0]
        mesh = data.get("mesh")
        if mesh:
            self.verts = np.array(mesh["verts"], dtype=np.float32).reshape(-1, 2)
            self.uvs = np.array(mesh["uvs"], dtype=np.float32).reshape(-1, 2)
            self.indices = np.array(mesh["indices"], dtype=np.int32).reshape(-1, 3)
            self.verts = self.verts - np.array(mesh.get("origin", [0, 0]), dtype=np.float32)
        else:
            self.verts = None
        self.children = [Node(child, self) for child in data.get("children", [])]

    def walk(self):
        yield self
        for child in self.children:
            yield from child.walk()

    def path(self):
        names = []
        node = self
        while node is not None:
            names.append(node.name)
            node = node.parent
        return names


class Puppet:
    def __init__(self, description, textures):
        self.meta = description.get("meta", {})
        self.root = Node(description["nodes"], None)
        self.nodes = {node.uuid: node for node in self.root.walk()}
        self.params = {param["name"]: param for param in description.get("param", [])}
        self.textures = textures
        self.premultiplied = True
        self._premultiplied = {}

    @classmethod
    def load(cls, path):
        data = open(path, "rb").read()
        if data[:8] != b"TRNSRTS\0":
            raise ValueError(f"{path}: not an Inochi2D puppet")
        size = struct.unpack(">I", data[8:12])[0]
        description = json.loads(data[12:12 + size].decode("utf-8"))
        at = 12 + size
        textures = []
        if data[at:at + 8] == b"TEX_SECT":
            count = struct.unpack(">I", data[at + 8:at + 12])[0]
            at += 12
            for _ in range(count):
                length = struct.unpack(">I", data[at:at + 4])[0]
                textures.append(data[at + 5:at + 5 + length])
                at += 5 + length
        return cls(description, textures)

    def texture(self, index):
        if index not in self._premultiplied:
            image = np.asarray(Image.open(io.BytesIO(self.textures[index])).convert("RGBA"), dtype=np.float32) / 255.0
            # Puppets store their textures premultiplied, as the renderer keeps them.
            image = image.copy()
            if not self.premultiplied:
                image[..., :3] *= image[..., 3:4]
            self._premultiplied[index] = image
        return self._premultiplied[index]

    def replace_texture(self, index, image):
        """Draws with `image` (premultiplied float RGBA) instead of texture `index`."""
        self._premultiplied[index] = image

    # ------------------------------------------------------------ parameters

    def offsets(self, values):
        """What every binding adds at these parameter values: node uuid -> {target: value}."""
        out = {}
        for name, param in self.params.items():
            value = values.get(name, param["defaults"])
            if not isinstance(value, (list, tuple)):
                value = [value, param["defaults"][1]]
            where = []
            for axis in range(2):
                points = param["axis_points"][axis]
                low, high = param["min"][axis], param["max"][axis]
                t = 0.0 if high == low else (min(max(value[axis], low), high) - low) / (high - low)
                index = max(i for i, point in enumerate(points) if point <= t + 1e-9)
                if index >= len(points) - 1:
                    where.append((len(points) - 1, len(points) - 1, 0.0))
                else:
                    where.append((index, index + 1, (t - points[index]) / (points[index + 1] - points[index])))
            (x0, x1, tx), (y0, y1, ty) = where
            for binding in param["bindings"]:
                grid = binding["values"]

                def at(x, y):
                    return np.asarray(grid[x][y], dtype=np.float32)

                value_here = ((at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty)
                              + (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty)
                slot = out.setdefault(binding["node"], {})
                target = binding["param_name"]
                if target == "deform":
                    slot[target] = slot.get(target, 0) + value_here
                elif target.startswith("transform.s."):
                    slot[target] = slot.get(target, 1.0) * float(value_here)
                else:
                    slot[target] = slot.get(target, 0.0) + float(value_here)
        return out

    def posed(self, values):
        """Per node: world matrix (3x3), sort value and deformed vertices."""
        offsets = self.offsets(values)
        pose = {}

        def visit(node, parent_matrix, parent_sort):
            off = offsets.get(node.uuid, {})
            tx = node.trans[0] + off.get("transform.t.x", 0.0)
            ty = node.trans[1] + off.get("transform.t.y", 0.0)
            angle = node.rot + off.get("transform.r.z", 0.0)
            sx = node.scale[0] * off.get("transform.s.x", 1.0)
            sy = node.scale[1] * off.get("transform.s.y", 1.0)
            c, s = math.cos(angle), math.sin(angle)
            local = np.array([[c * sx, -s * sy, tx], [s * sx, c * sy, ty], [0, 0, 1]], dtype=np.float64)
            matrix = local if node.lock_to_root else parent_matrix @ local
            sort = parent_sort + node.zsort + off.get("zSort", 0.0)
            verts = None
            if node.verts is not None:
                local_verts = node.verts + off["deform"] if "deform" in off else node.verts
                verts = (local_verts.astype(np.float64) @ matrix[:2, :2].T) + matrix[:2, 2]
            pose[node.uuid] = (matrix, sort, verts)
            for child in node.children:
                visit(child, matrix, sort)

        visit(self.root, np.eye(3), 0.0)
        return pose

    # --------------------------------------------------------------- drawing

    def _part(self, node, verts, scale, box, shape):
        """One part alone, as premultiplied RGBA on the output grid."""
        height, width = shape
        out = np.zeros((height, width, 4), dtype=np.float32)
        if node.texture is None or verts is None:
            return out
        texture = self.texture(node.texture)
        th, tw = texture.shape[:2]
        points = (verts - np.array([box[0], box[1]])) * scale
        for a, b, c in node.indices:
            p0, p1, p2 = points[a], points[b], points[c]
            x0 = max(int(math.floor(min(p0[0], p1[0], p2[0]))), 0)
            x1 = min(int(math.ceil(max(p0[0], p1[0], p2[0]))), width - 1)
            y0 = max(int(math.floor(min(p0[1], p1[1], p2[1]))), 0)
            y1 = min(int(math.ceil(max(p0[1], p1[1], p2[1]))), height - 1)
            if x1 < x0 or y1 < y0:
                continue
            area = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1])
            if abs(area) < 1e-9:
                continue
            ys, xs = np.mgrid[y0:y1 + 1, x0:x1 + 1]
            px, py = xs + 0.5, ys + 0.5
            w1 = ((px - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (py - p0[1])) / area
            w2 = ((p1[0] - p0[0]) * (py - p0[1]) - (px - p0[0]) * (p1[1] - p0[1])) / area
            w0 = 1.0 - w1 - w2
            inside = (w0 >= -1e-4) & (w1 >= -1e-4) & (w2 >= -1e-4)
            if not inside.any():
                continue
            u = (w0 * node.uvs[a][0] + w1 * node.uvs[b][0] + w2 * node.uvs[c][0]) * tw - 0.5
            v = (w0 * node.uvs[a][1] + w1 * node.uvs[b][1] + w2 * node.uvs[c][1]) * th - 0.5
            # Outside the texture there is nothing: clamping would smear its border.
            inside &= (u > -1.0) & (u < tw) & (v > -1.0) & (v < th)
            u0 = np.clip(np.floor(u).astype(np.int32), 0, tw - 1)
            v0 = np.clip(np.floor(v).astype(np.int32), 0, th - 1)
            u1 = np.clip(u0 + 1, 0, tw - 1)
            v1 = np.clip(v0 + 1, 0, th - 1)
            fu = np.clip(u - u0, 0, 1)[..., None].astype(np.float32)
            fv = np.clip(v - v0, 0, 1)[..., None].astype(np.float32)
            sample = ((texture[v0, u0] * (1 - fu) + texture[v0, u1] * fu) * (1 - fv)
                      + (texture[v1, u0] * (1 - fu) + texture[v1, u1] * fu) * fv)
            region = out[y0:y1 + 1, x0:x1 + 1]
            region[inside] = sample[inside]
        out[..., :3] *= np.array(node.tint, dtype=np.float32)
        return out * node.opacity

    @staticmethod
    def _blend(target, source, mode):
        alpha = source[..., 3:4]
        if mode == "Multiply":
            target[..., :3] = target[..., :3] * source[..., :3] + target[..., :3] * (1 - alpha)
        elif mode == "LinearDodge":
            target[..., :3] = np.minimum(target[..., :3] + source[..., :3] * target[..., 3:4], 1.0)
        elif mode == "ClipToLower":
            below = target[..., 3:4].copy()
            target[..., :3] = source[..., :3] * below + target[..., :3] * (1 - alpha * below)
        else:
            target[...] = source + target * (1 - alpha)

    def render(self, values=None, scale=0.25, box=(-1000, -2000, 1000, 2000), include=None, oversample=2):
        """Draws the nodes `include(node)` accepts (all when None), back to front."""
        pose = self.posed(values or {})
        inner = scale * oversample
        width = int(math.ceil((box[2] - box[0]) * scale)) * oversample
        height = int(math.ceil((box[3] - box[1]) * scale)) * oversample
        shape = (height, width)
        cache = {}

        def layer(node):
            if node.uuid not in cache:
                cache[node.uuid] = self._part(node, pose[node.uuid][2], inner, box, shape)
            return cache[node.uuid]

        def drawables(parent, top):
            """Parts and composites under `parent` that sort together."""
            found = []
            for child in parent.children:
                if not child.enabled:
                    continue
                if child.kind == "Composite":
                    found.append(child)
                else:
                    if child.kind == "Part":
                        found.append(child)
                    found.extend(drawables(child, False))
            return found

        def draw(items, target):
            for node in sorted(items, key=lambda item: -pose[item.uuid][1]):
                if node.kind == "Composite":
                    buffer = np.zeros_like(target)
                    draw(drawables(node, False), buffer)
                    if include is None or any(include(child) for child in node.walk()):
                        self._blend(target, buffer * node.opacity, node.blend)
                    continue
                if include is not None and not include(node):
                    continue
                source = layer(node)
                for uuid, mode in node.masks:
                    mask = layer(self.nodes[uuid])[..., 3:4] > node.threshold
                    source = source * (mask if mode == "Mask" else ~mask)
                self._blend(target, source, node.blend)

        frame = np.zeros((height, width, 4), dtype=np.float32)
        draw(drawables(self.root, True), frame)
        if oversample > 1:
            frame = frame.reshape(height // oversample, oversample, width // oversample, oversample, 4).mean(axis=(1, 3))
        return frame

    def bounds(self, values=None, include=None):
        """Model-space box of the mesh vertices of the accepted parts."""
        pose = self.posed(values or {})
        points = [pose[node.uuid][2] for node in self.root.walk()
                  if node.kind == "Part" and node.enabled and (include is None or include(node))]
        points = np.concatenate([p for p in points if p is not None])
        return (float(points[:, 0].min()), float(points[:, 1].min()), float(points[:, 0].max()), float(points[:, 1].max()))


def save(image, path, background=None):
    """Writes a premultiplied float image as PNG (straight alpha, or over a colour)."""
    rgba = image.copy()
    alpha = rgba[..., 3:4]
    if background is not None:
        rgb = rgba[..., :3] + np.array(background, dtype=np.float32) / 255.0 * (1 - alpha)
        Image.fromarray((np.clip(rgb, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB").save(path)
        return
    with np.errstate(divide="ignore", invalid="ignore"):
        rgba[..., :3] = np.where(alpha > 1e-6, rgba[..., :3] / alpha, 0)
    Image.fromarray((np.clip(rgba, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA").save(path)
