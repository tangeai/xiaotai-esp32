#!/usr/bin/env python3
"""Check unified capability snapshots and unchanged WeChat media parameters."""
import ast
import json
import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_runtime/src/starter_runtime.c").read_text(encoding="utf-8")
start = source.index("static void request_device_profile(void)")
profiles = re.findall(r"static const char profile\[\] =\s*(.*?);", source[start:], re.S)
def decode(text):
    return json.loads("".join(ast.literal_eval(s) for s in re.findall(r'"(?:\\.|[^"\\])*"', text)))
snapshots = list(map(decode, profiles[:2]))
# Keep the reporting allowlist separate from the SDK's codec inventory.
common_fields = {
    "audio_rate", "audio_channels", "camera_rotation", "hor_mirror",
    "vert_mirror", "no_video", "aspect_ratio", "object_fit",
}
codec_fields = {"up_audio_mt", "down_audio_mt", "up_video_mt", "down_video_mt"}
stream_ids = {
    "up_audio_streamid": 10, "up_video_streamid": 11,
    "down_audio_streamid": 14, "down_video_streamid": 15,
}
voip_fields = (common_fields | codec_fields | {
    "down_video_rotation", "screen_width", "screen_height",
    "video_res_mode", "calling_timeout_sec",
}) - {"up_audio_mt"}
assert len(snapshots) == 2
for snapshot in snapshots:
    assert set(snapshot) == {"profiles"}  # identity comes from the formal JWT
    assert set(snapshot["profiles"]) == {"stream", "call", "voip"}
    assert len(json.dumps(snapshot, separators=(",", ":")).encode()) < 2048
    for scene in ("stream", "call"):
        profile = snapshot["profiles"][scene]
        assert profile["up_audio_mt"] == profile["down_audio_mt"] == ["alaw"]
        assert (profile["audio_rate"], profile["audio_channels"]) == (8000, 1)
        assert isinstance(profile["up_video_mt"], list)
        assert isinstance(profile["down_video_mt"], list)
        allowed = common_fields | codec_fields | (set(stream_ids) if scene == "stream" else set())
        assert set(profile) <= allowed
        if scene == "stream":
            assert {key: profile[key] for key in stream_ids} == stream_ids
    assert set(snapshot["profiles"]["voip"]) <= voip_fields

p4_scenes, voice_scenes = (item["profiles"] for item in snapshots)
assert p4_scenes["stream"]["up_video_mt"] == p4_scenes["stream"]["down_video_mt"] == ["h264"]
assert p4_scenes["call"]["up_video_mt"] == p4_scenes["call"]["down_video_mt"] == ["h264"]
for scene in ("stream", "call"):
    assert set(p4_scenes[scene]) == common_fields | codec_fields | (set(stream_ids) if scene == "stream" else set())
    assert p4_scenes[scene]["camera_rotation"] == 0
    assert p4_scenes[scene]["hor_mirror"] is False
    assert p4_scenes[scene]["vert_mirror"] is False
    assert p4_scenes[scene]["object_fit"] == "contain"
    assert p4_scenes[scene]["no_video"] is False
    assert voice_scenes[scene]["up_video_mt"] == voice_scenes[scene]["down_video_mt"] == []
    assert voice_scenes[scene]["no_video"] is True
p4, voice = (item["profiles"]["voip"] for item in snapshots)
assert set(p4) == voip_fields
assert p4["no_video"] is False
assert (p4["up_video_mt"], p4["down_video_mt"]) == ("h264", "mjpeg")
assert (p4["screen_width"], p4["screen_height"]) == (480, 320)
assert p4["camera_rotation"] == 270  # additional WeChat UI CCW90 per device test
assert p4["aspect_ratio"] == 960 / 1280
assert p4["hor_mirror"] is False and p4["vert_mirror"] is False
assert p4["object_fit"] == "contain"
assert p4["down_video_rotation"] == 0  # do not rotate the MJPEG source twice
assert p4["video_res_mode"] == "auto"  # same upstream behavior as omission
assert p4["calling_timeout_sec"] == 30
assert len(json.dumps(p4, separators=(",", ":")).encode()) <= 512
assert "camera_rotation" not in voice and "aspect_ratio" not in voice
assert (p4["audio_rate"], p4["audio_channels"], p4["down_audio_mt"]) == (8000, 1, "alaw")
assert voice["no_video"] is True and voice["down_video_mt"] == "none"
# Geometry must match the active encoder profiles, not sensor size or stale prose.
tuning = (root / "main/media/media_tuning.h").read_text(encoding="utf-8")
def dimension(name):
    match = re.search(r"^#define\s+" + re.escape(name) + r"\s+(\d+)U?\s*$", tuning, re.M)
    assert match, name
    return int(match.group(1))
for scene, prefix in (("stream", "WECHAT"), ("call", "CALL"), ("voip", "WECHAT")):
    width = dimension(f"APP_MEDIA_{prefix}_VIDEO_WIDTH")
    height = dimension(f"APP_MEDIA_{prefix}_VIDEO_HEIGHT")
    assert p4_scenes[scene]["aspect_ratio"] == width / height
assert 'PLATFORM_SERVICE_DEVICE, "/v1/device/profile", profile' in source
assert "/v1/voip/device/profile" not in source
assert "EVENT_VOIP_PROFILE" not in source
assert "event.platform_epoch != platform_client_epoch()" in source
header = (root / "components/starter_tirtc/include/starter_tirtc.h").read_text(encoding="utf-8")
for field, constant in (("up_audio_streamid", "STARTER_H5_UP_AUDIO_STREAM_ID"),
                        ("up_video_streamid", "STARTER_H5_UP_VIDEO_STREAM_ID"),
                        ("down_audio_streamid", "STARTER_H5_DOWN_AUDIO_STREAM_ID"),
                        ("down_video_streamid", "STARTER_H5_DOWN_VIDEO_STREAM_ID")):
    value = re.search(r"^#define\s+" + constant + r"\s+(\d+)U?\s*$", header, re.M)
    assert value and int(value.group(1)) == stream_ids[field]
connection = source[source.index("static void handle_connection("):]
connection = connection[:connection.index("\n}")]
assert connection.index("starter_media_start(STARTER_TIRTC_H5") < connection.index("starter_tirtc_subscribe_h5_audio(")
assert connection.index("starter_tirtc_subscribe_h5_audio(") < connection.index("publish_state(STARTER_RUNTIME_H5_ACTIVE)")
p4_video = (root / "components/p4_hardware/p4_video.c").read_text(encoding="utf-8")
assert "starter_tirtc_subscribe_h5_video(generation)" in p4_video
assert "mode == STARTER_TIRTC_H5 || mode == STARTER_TIRTC_CALL" in p4_video
renderer = (root / "main/services/call_video_renderer.c").read_text(encoding="utf-8")
assert re.search(r"video_frame_rotation_t display_rotation\s*=\s*VIDEO_FRAME_ROTATION_CLOCKWISE_90;", renderer)
assert "rotation=cw90 source_rotation=not-signaled" in renderer
print("PASS: complete stream/call/voip fields; H5 stream IDs and subscribe order; geometry and WeChat defaults preserved")
