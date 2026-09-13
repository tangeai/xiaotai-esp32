#!/usr/bin/env python3
"""Replay captured VoIP frame metadata through the real ingress and drain."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "components/p4_hardware/p4_video.c").read_text()
def function(signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#define VIDEO_INGRESS_SLOTS 4
#define VIDEO_INGRESS_BYTES 64
#define pdTRUE 1
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE -1
#define STARTER_TIRTC_CALL 2
#define STARTER_TIRTC_VOIP 3
#define TIRTC_VIDEO_JPEG 65
#define TIRTC_VIDEO_H264 66
#define TIRTC_FRAME_FLAG_KEY_FRAME 1
typedef int esp_err_t;
typedef int starter_tirtc_mode_t;
typedef struct { uint8_t stream_id, media, flags; uint32_t length, timestamp_ms; } starter_tirtc_frame_t;
static atomic_uint s_live_generation=1, s_rx_seen, s_rx_last_media, s_rx_last_stream, s_rx_dropped;
static atomic_bool s_need_idr;
static int64_t s_last_idr_us;
static struct { uint32_t generation; starter_tirtc_frame_t frame; uint8_t data[64]; } s_ingress[4];
static int s_free=1, s_pending=2, pending, mode=STARTER_TIRTC_VOIP, jpeg, h264;
static int starter_tirtc_mode(void) { return mode; }
static int xQueueReceive(int q, uint8_t *slot, int timeout) {
    assert(timeout==0); *slot=0;
    if(q==s_free) return 1;
    if(pending) { pending=0; return 1; } return 0;
}
static int xQueueSend(int q, const uint8_t *slot, int timeout) {
    assert(*slot==0 && timeout==0); if(q==s_pending) pending=1; return 1;
}
static int call_video_renderer_submit_mjpeg(const void *data, unsigned len, unsigned ts) {
    assert(len==4 && ts==123 && memcmp(data,"JPEG",4)==0); ++jpeg; return 0;
}
static int call_video_renderer_submit_h264(const void *data, unsigned len, bool key, unsigned ts) {
    (void)data; (void)len; (void)key; (void)ts; ++h264; return 0;
}
static bool call_video_renderer_requires_key_frame(void) { return false; }
static int64_t esp_timer_get_time(void) { return 0; }
static int starter_tirtc_request_remote_key_frame(void) { return 0; }
'''
code += function("void p4_video_submit(") + function("static void drain_video(")
code += r'''
int main(void) {
    starter_tirtc_frame_t f={.stream_id=1,.media=65,.length=4,.timestamp_ms=123};
    p4_video_submit(1,&f,"JPEG"); drain_video();
    assert(jpeg==1); /* Exact captured stream=1 media=65 must reach MJPEG. */
    p4_video_submit(2,&f,"JPEG"); drain_video(); assert(jpeg==1);
    f.media=TIRTC_VIDEO_H264;
    p4_video_submit(1,&f,"JPEG"); drain_video(); assert(jpeg==1 && h264==0);
    mode=STARTER_TIRTC_CALL; f.stream_id=11;
    p4_video_submit(1,&f,"JPEG"); drain_video(); assert(h264==1);
    f.media=TIRTC_VIDEO_JPEG;
    p4_video_submit(1,&f,"JPEG"); drain_video(); assert(jpeg==1);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="p4-ingress-") as directory:
    path=Path(directory)
    (path / "test.c").write_text(code)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(path/"test.c"), "-o", str(path/"test")], check=True)
    subprocess.run([str(path/"test")], check=True)
print("PASS: captured VoIP stream 1 JPEG reaches renderer; stale generation and wrong codecs rejected")
