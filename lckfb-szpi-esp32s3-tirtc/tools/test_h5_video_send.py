"""Compile the actual S3 JPEG adapter: format, generation, and disconnect gate."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_tirtc/src/starter_tirtc.c").read_text(encoding="utf-8")
header = (root / "components/starter_tirtc/include/starter_tirtc.h").read_text(encoding="utf-8")
constants = "\n".join(re.findall(r"^#define STARTER_H5_.*$", header, re.M)) + "\n"


def function(name):
    match = re.search(r"(?:int|bool) " + name + r"\([^)]*\)\s*\{.*?\n\}", source, re.S)
    assert match, name
    return match[0]


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#define CONFIG_IDF_TARGET_ESP32S3 1
#define CONFIG_IDF_TARGET_ESP32P4 0
#define ESP_LOGI(...) ((void)0)
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define TIRTC_E_BUSY -10
#define TIRTC_E_INVALID_PARAMETER -11
#define TIRTC_E_INVALID_HANDLE -12
#define H5_VIDEO_STREAM STARTER_H5_UP_VIDEO_STREAM_ID
#define H5_VIDEO_BACKLOG_BYTES (32U * 1024U)
#define TIRTC_VIDEO_JPEG 9
#define TIRTC_FRAME_FLAG_KEY_FRAME 1
enum {STARTER_TIRTC_NONE, STARTER_TIRTC_H5, STARTER_TIRTC_AI,
      STARTER_TIRTC_CALL, STARTER_TIRTC_VOIP, STARTER_TIRTC_ROOM};
typedef void *tirtc_conn_t;
typedef struct {unsigned stream_id, media, flags, ts, length;} TIRTCFRAMEINFO;
static atomic_uintptr_t s_connection;
static atomic_uint s_active_generation, s_pending_request, s_expected_call_tag;
static atomic_uint s_h5_audio_rx_generation;
static atomic_int s_mode;
static atomic_bool s_video_subscribed;
static int mutex_marker, *s_video_send_mutex = &mutex_marker;
static bool locked;
static int sends, closes, next_result = 4;
static unsigned subscriptions;
static size_t backlog;
static unsigned queries;
static int xSemaphoreTake(int *m, unsigned ticks) {
    assert(m == s_video_send_mutex);
    (void)ticks;
    if (locked) return 0;
    locked = true; return pdTRUE;
}
static void xSemaphoreGive(int *m) {assert(m && locked); locked = false;}
static bool starter_tirtc_connected(void) {return atomic_load(&s_connection) != 0;}
static int starter_tirtc_mode(void) {return atomic_load(&s_mode);}
static uint32_t starter_tirtc_generation(void) {return atomic_load(&s_active_generation);}
static bool connection_matches(tirtc_conn_t c) {
    return c != NULL && (uintptr_t)c == atomic_load(&s_connection);
}
static void clear_subscriptions(void) {
    atomic_store(&s_h5_audio_rx_generation, 0);
    atomic_store(&s_video_subscribed, false);
}
static int TiRtcSubscribeAudio(tirtc_conn_t c, uint8_t id) {
    assert(locked && c == (void *)7 && id == 14);
    ++subscriptions; return next_result;
}
static size_t TiRtcGetSendBufferUsed(tirtc_conn_t c) {
    assert(locked && c == (void *)7 && s_connection == 7);
    ++queries; return backlog;
}
static int TiRtcSendVideoStream(tirtc_conn_t c, const TIRTCFRAMEINFO *f, const void *p) {
    assert(locked && c == (void *)7 && p);
    assert(f->stream_id == 11 && f->media == TIRTC_VIDEO_JPEG);
    assert(f->flags == TIRTC_FRAME_FLAG_KEY_FRAME && f->ts == 123 && f->length == 4);
    ++sends; return next_result;
}
static int TiRtcDisconnect(tirtc_conn_t c) {
    assert(locked && c == (void *)7);
    assert(!starter_tirtc_connected() && !s_active_generation && !s_video_subscribed);
    ++closes; return 0;
}
'''
main = r'''
int main(void) {
    unsigned char jpeg[] = {0xff, 0xd8, 0xff, 0xd9};
    s_connection = 7; s_active_generation = 2; s_mode = STARTER_TIRTC_H5;
    assert(starter_tirtc_subscribe_h5_audio(0)<0);
    assert(starter_tirtc_subscribe_h5_audio(1)<0 && subscriptions==0 && !locked);
    assert(starter_tirtc_subscribe_h5_audio(2)==4 && subscriptions==1 && !locked);
    next_result=TIRTC_E_BUSY;
    assert(starter_tirtc_subscribe_h5_audio(2)==0 && subscriptions==1 && !locked);
    s_h5_audio_rx_generation=0;
    assert(starter_tirtc_subscribe_h5_audio(2)==TIRTC_E_BUSY && subscriptions==2 && !locked);
    next_result=4;
    assert(!s_video_subscribed); /* RX subscription cannot open TX. */
    assert(starter_tirtc_h5_video_congested(2) && queries == 0);
    assert(starter_tirtc_send_h5_jpeg(2,123,jpeg,4)<0); /* No subscription. */
    s_video_subscribed = true;
    assert(starter_tirtc_h5_video_congested(1) && queries == 0);
    assert(!starter_tirtc_h5_video_congested(2) && queries == 1 && !locked);
    assert(starter_tirtc_send_h5_jpeg(1,123,jpeg,4)<0); /* Old frame. */
    assert(starter_tirtc_send_h5_jpeg(0,123,jpeg,4)<0);
    for (int mode=STARTER_TIRTC_AI; mode<=STARTER_TIRTC_ROOM; ++mode) {
        s_mode = mode; assert(starter_tirtc_send_h5_jpeg(2,123,jpeg,4)<0);
        assert(starter_tirtc_subscribe_h5_audio(2)<0 && subscriptions==2);
    }
    s_mode = STARTER_TIRTC_H5;
    assert(starter_tirtc_send_h5_jpeg(2,123,NULL,4)<0);
    assert(starter_tirtc_send_h5_jpeg(2,123,jpeg,0)<0);
    locked = true;
    assert(starter_tirtc_send_h5_jpeg(2,123,jpeg,4)==TIRTC_E_BUSY);
    locked = false;
    assert(sends==0);
    backlog = H5_VIDEO_BACKLOG_BYTES;
    assert(starter_tirtc_h5_video_congested(2));
    assert(starter_tirtc_send_h5_jpeg(2,123,jpeg,4)==TIRTC_E_BUSY && sends==0 && !locked);
    backlog = 0;
    assert(starter_tirtc_send_h5_jpeg(2,123,jpeg,4)==4 && sends==1 && !locked);
    next_result = TIRTC_E_BUSY;
    assert(starter_tirtc_send_h5_jpeg(2,123,jpeg,4)==TIRTC_E_BUSY && !locked);
    assert(starter_tirtc_disconnect()==0 && closes==1 && !locked);
    assert(starter_tirtc_disconnect()==TIRTC_E_INVALID_HANDLE && closes==1);
    assert(starter_tirtc_subscribe_h5_audio(2)<0 && subscriptions==2);
    assert(starter_tirtc_send_h5_jpeg(2,123,jpeg,4)<0 && sends==2);
    unsigned before = queries;
    assert(starter_tirtc_h5_video_congested(2) && queries == before);
}
'''
with tempfile.TemporaryDirectory(prefix="s3-h5-video-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(constants + prefix + function("starter_tirtc_video_ready") +
        function("starter_tirtc_subscribe_h5_audio") +
        function("starter_tirtc_h5_video_congested") +
        function("starter_tirtc_send_h5_jpeg") + function("starter_tirtc_disconnect") + main)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: S3 JPEG format, subscription/generation guard, busy/error propagation, disconnect")
