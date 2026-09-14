#!/usr/bin/env python3
"""Verify the actual JSON constants submitted by both target branches."""
import ast
import json
import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_runtime/src/starter_runtime.c").read_text()
start = source.index("static void request_voip_profile(void)")
profiles = re.findall(r"static const char profile\[\] =\s*(.*?);", source[start:], re.S)
def decode(text):
    return json.loads("".join(ast.literal_eval(s) for s in re.findall(r'"(?:\\.|[^"\\])*"', text)))
p4, s3 = map(decode, profiles[:2])
assert p4["no_video"] is False
assert (p4["up_video_mt"], p4["down_video_mt"]) == ("h264", "mjpeg")
assert (p4["screen_width"], p4["screen_height"]) == (480, 320)
assert p4["camera_rotation"] == 270  # additional WeChat UI CCW90 per device test
assert p4["aspect_ratio"] == 960 / 1280
assert p4["hor_mirror"] is False and p4["vert_mirror"] is False
assert p4["object_fit"] == "contain"
assert len(json.dumps(p4, separators=(",", ":")).encode()) <= 512
assert "camera_rotation" not in s3 and "aspect_ratio" not in s3
assert (p4["audio_rate"], p4["audio_channels"], p4["down_audio_mt"]) == (8000, 1, "alaw")
assert s3["no_video"] is True and s3["down_video_mt"] == "none"
renderer = (root / "main/services/call_video_renderer.c").read_text()
assert re.search(r"video_frame_rotation_t display_rotation\s*=\s*VIDEO_FRAME_ROTATION_CLOCKWISE_90;", renderer)
assert "rotation=cw90 source_rotation=not-signaled" in renderer
print("PASS: P4 H264/MJPEG 480x320, MJPEG CW90; S3 voice-only unchanged")
