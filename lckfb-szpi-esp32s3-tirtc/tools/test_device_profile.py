"""Exercise the S3 capability snapshot and its existing asynchronous owner."""
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_runtime/src/starter_runtime.c").read_text(encoding="utf-8")
contract = (root / "components/starter_tirtc/include/starter_tirtc.h").read_text(encoding="utf-8")
stream_ids = {name.lower() + "_streamid": int(value) for name, value in
              re.findall(r"#define STARTER_H5_(UP_AUDIO|UP_VIDEO|DOWN_AUDIO|DOWN_VIDEO)_STREAM_ID (\d+)", contract)}
assert stream_ids == {"up_audio_streamid": 10, "up_video_streamid": 11,
                      "down_audio_streamid": 14, "down_video_streamid": 15}
platform = (root / "components/platform_client/src/platform_client.c").read_text(encoding="utf-8")
body_limit = int(re.search(r"#define PLATFORM_REQUEST_BODY_MAX\s+(\d+)", platform)[1])
match = re.search(r"static void request_voip_profile\(void\)\s*\{.*?\n\}", source, re.S)
assert match
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_IDF_TARGET_ESP32P4 0
#define ESP_OK 0
#define PLATFORM_SERVICE_DEVICE 2
#define VOIP_PROFILE_RETRY_MS 15000
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
static bool s_voip_profile_inflight, ready, online;
static int64_t s_voip_profile_retry_at_ms;
static int calls, result;
static const char *snapshot;
static bool platform_client_ready(void) {return ready;}
static bool platform_client_mqtt_connected(void) {return online;}
static int64_t now_ms(void) {return 1000;}
static void voip_profile_response(void) {}
static esp_err_t platform_client_request_metadata(int service, const char *path,
    const char *body, unsigned timeout, void (*cb)(void), void *context) {
    assert(service == PLATFORM_SERVICE_DEVICE);
    assert(strcmp(path, "/v1/device/profile") == 0);
    assert(timeout == 10000 && cb == voip_profile_response && context == NULL);
    assert(body && strlen(body) < 2048);
    if (snapshot) assert(strcmp(body, snapshot) == 0);
    snapshot = body; ++calls; return result;
}
'''
main = r'''
int main(void) {
    request_voip_profile(); assert(calls == 0);
    ready = true; request_voip_profile(); assert(calls == 0);
    online = true; result = -1;
    request_voip_profile();
    assert(calls == 1 && !s_voip_profile_inflight && s_voip_profile_retry_at_ms == 16000);
    result = ESP_OK; request_voip_profile();
    assert(calls == 2 && s_voip_profile_inflight);
    request_voip_profile(); assert(calls == 2);
    s_voip_profile_inflight = false; request_voip_profile(); assert(calls == 3);
    puts(snapshot);
}
'''
with tempfile.TemporaryDirectory(prefix="s3-profile-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(prefix + match[0] + main, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(path / "test.c"), "-o", str(path / "test")], check=True)
    output = subprocess.check_output([str(path / "test")], text=True)
    payload_bytes = len(output.rstrip("\r\n").encode("utf-8"))
    assert payload_bytes + 1 <= body_limit and payload_bytes <= 16 * 1024
    body = json.loads(output)
    assert set(body) == {"profiles"}
    profiles = body["profiles"]
    assert set(profiles) == {"stream", "call", "voip"}
    # API contract: no unknown fields/nulls and full replacement per scene.
    common = {"audio_rate", "audio_channels", "no_video", "camera_rotation",
              "hor_mirror", "vert_mirror", "aspect_ratio", "object_fit"}
    stream_call = common | {"up_audio_mt", "down_audio_mt", "up_video_mt", "down_video_mt"}
    voip = common | {"screen_width", "screen_height", "up_video_mt", "down_video_mt",
                     "down_audio_mt", "down_video_rotation", "video_res_mode", "calling_timeout_sec"}
    assert len(stream_call) == 12 and len(voip) == 16
    for name, profile in profiles.items():
        fields = voip if name == "voip" else stream_call
        if name == "stream":
            fields = fields | set(stream_ids)
            for key, value in stream_ids.items():
                assert type(profile[key]) is int and profile[key] == value
        assert set(profile) == fields
        assert all(value is not None for value in profile.values())
        for field in ("audio_rate", "audio_channels", "camera_rotation"):
            assert type(profile[field]) is int
        for field in ("no_video", "hor_mirror", "vert_mirror"):
            assert type(profile[field]) is bool
        assert profile["audio_rate"] == 8000 and profile["audio_channels"] == 1
        assert profile["no_video"] is (name != "stream")
        assert profile["camera_rotation"] == 0
        assert profile["hor_mirror"] is False and profile["vert_mirror"] is False
        assert profile["aspect_ratio"] == "4:3" and profile["object_fit"] == "contain"
    for name in ("stream", "call"):
        assert profiles[name]["up_audio_mt"] == ["alaw"]
        assert profiles[name]["down_audio_mt"] == ["alaw"]
        assert profiles[name]["down_video_mt"] == []
    assert profiles["stream"]["up_video_mt"] == ["mjpeg"]
    assert profiles["stream"]["aspect_ratio"] == "4:3"
    assert profiles["call"]["up_video_mt"] == []
    assert profiles["voip"]["down_audio_mt"] == "alaw"
    assert profiles["voip"]["up_video_mt"] == profiles["voip"]["down_video_mt"] == "none"
    assert profiles["voip"]["screen_width"] == 320 and profiles["voip"]["screen_height"] == 240
    for field in ("screen_width", "screen_height", "down_video_rotation", "calling_timeout_sec"):
        assert type(profiles["voip"][field]) is int
    assert profiles["voip"]["down_video_rotation"] == 0
    assert profiles["voip"]["video_res_mode"] == "auto"
    assert profiles["voip"]["calling_timeout_sec"] == 30
print(f"PASS: device profile 16/12/16 fields, stream IDs, types, {payload_bytes}/{body_limit} bytes, "
      "codec contract, online/inflight gate, retry snapshot")
