"""Legacy filename: S3 H5 video, all other S3 applications stay audio-only."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_tirtc/src/starter_tirtc.c").read_text()
header = (root / "components/starter_tirtc/include/starter_tirtc.h").read_text(encoding="utf-8")
constants = "\n".join(re.findall(r"^#define STARTER_H5_.*$", header, re.M)) + "\n"
callbacks = []
for name in ("on_subscribe_audio", "on_subscribe_video", "on_unsubscribe_audio", "on_unsubscribe_video",
             "starter_tirtc_send_alaw", "on_audio", "clear_subscriptions"):
    match = re.search(r"(?:static )?(?:int|void) " + name + r"\([^)]*\)\s*\{.*?\n\}", source, re.S)
    assert match, name
    callbacks.append(match[0])
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#ifdef NULL
#undef NULL
#endif
#define NULL 0
#define TIRTC_E_INVALID_PARAMETER -1
#define TIRTC_AUDIO_ALAW 2
#define TIRTC_AUDIOSAMPLE_8K16B1C 0
#define H5_AUDIO_STREAM STARTER_H5_UP_AUDIO_STREAM_ID
#define H5_VIDEO_STREAM STARTER_H5_UP_VIDEO_STREAM_ID
#define AI_AUDIO_STREAM 1U
#define CALL_AUDIO_STREAM 10U
typedef uintptr_t tirtc_conn_t;
typedef enum { STARTER_TIRTC_NONE, STARTER_TIRTC_H5, STARTER_TIRTC_AI,
               STARTER_TIRTC_VOIP, STARTER_TIRTC_CALL, STARTER_TIRTC_ROOM } starter_tirtc_mode_t;
static atomic_int s_mode;
static atomic_bool s_audio_subscribed, s_video_subscribed;
static atomic_uintptr_t s_connection = 7;
static atomic_uint s_active_generation = 3, s_h5_audio_rx_generation;
static atomic_uint s_downlink_audio_accepted, s_downlink_audio_rejected;
typedef struct { uint8_t stream_id, media, flags; uint32_t ts, length; } TIRTCFRAMEINFO;
typedef struct { uint8_t stream_id, media, flags; uint32_t timestamp_ms, length; } starter_tirtc_frame_t;
static unsigned sent, received;
static uint8_t expected_tx;
static int TiRtcSendAudioStream(tirtc_conn_t c, const TIRTCFRAMEINFO *f, const void *data) {
    assert(c == 7 && data && f->stream_id == expected_tx);
    assert(f->media == TIRTC_AUDIO_ALAW && f->flags == 0 && f->length == 160);
    ++sent; return 0;
}
static void receive(starter_tirtc_mode_t mode, uint32_t gen,
                    const starter_tirtc_frame_t *f, const void *data, void *context) {
    assert((int)mode == s_mode && gen == 3 && f->length == 160 && data && context == NULL);
    ++received;
}
static struct {
    void (*on_audio)(starter_tirtc_mode_t, uint32_t, const starter_tirtc_frame_t *, const void *, void *);
    void *user_data;
} s_handlers = { .on_audio = receive };
#if CONFIG_IDF_TARGET_ESP32P4
static int s_bitrate_lock;
static uint32_t s_bitrate_generation, s_bitrate_target;
#define taskENTER_CRITICAL(lock) ((void)(lock))
#define taskEXIT_CRITICAL(lock) ((void)(lock))
#endif
static unsigned key_requests;
static bool connection_matches(tirtc_conn_t c) { return c == 7; }
static starter_tirtc_mode_t starter_tirtc_mode(void) { return atomic_load(&s_mode); }
static void on_request_key_frame(tirtc_conn_t c, uint8_t id) { (void)c; (void)id; ++key_requests; }
'''
main = r'''
int main(void) {
    for (int mode=STARTER_TIRTC_NONE; mode<=STARTER_TIRTC_ROOM; ++mode) {
        atomic_store(&s_mode, mode);
        for (unsigned id=0; id<256; ++id) {
            s_audio_subscribed=false;
            bool audio=((mode==STARTER_TIRTC_AI || mode==STARTER_TIRTC_ROOM) && id==AI_AUDIO_STREAM) ||
                (mode==STARTER_TIRTC_H5 && id==H5_AUDIO_STREAM) ||
                ((mode==STARTER_TIRTC_CALL || mode==STARTER_TIRTC_VOIP) && id==CALL_AUDIO_STREAM);
            assert(on_subscribe_audio(7,id)==(audio?0:-1));
            assert(s_audio_subscribed==audio);
            assert(on_subscribe_audio(8,id)==-1);
            s_video_subscribed=false;
            unsigned before=key_requests;
            bool video=id==H5_VIDEO_STREAM &&
                (mode==STARTER_TIRTC_H5 || (CONFIG_IDF_TARGET_ESP32P4 &&
                 (mode==STARTER_TIRTC_CALL || mode==STARTER_TIRTC_VOIP)));
            assert(on_subscribe_video(7,id)==(video?0:-1));
            assert(s_video_subscribed==video);
            assert(key_requests==before+(video && CONFIG_IDF_TARGET_ESP32P4?1:0));
            assert(on_subscribe_video(8,id)==-1);
            assert(s_video_subscribed==video);
            on_unsubscribe_video(8,H5_VIDEO_STREAM);
            assert(s_video_subscribed==video);
            on_unsubscribe_video(7,H5_AUDIO_STREAM);
            assert(s_video_subscribed==video);
            on_unsubscribe_video(7,H5_VIDEO_STREAM);
            assert(!s_video_subscribed);
            assert(s_audio_subscribed==audio);
            on_unsubscribe_audio(8,id);
            assert(s_audio_subscribed==audio);
            on_unsubscribe_audio(7,255);
            assert(s_audio_subscribed==audio); /* Wrong stream must not mute. */
            on_unsubscribe_audio(7,id);
            assert(!s_audio_subscribed);
        }
    }
    unsigned char data[160] = {0};
    s_mode=STARTER_TIRTC_H5;
    clear_subscriptions();
    assert(starter_tirtc_send_alaw(100,data,sizeof(data))<0 && sent==0);
    assert(on_subscribe_audio(7,14)<0); /* Our receive ID is not our TX ID. */
    assert(starter_tirtc_send_alaw(100,data,sizeof(data))<0 && sent==0);
    assert(on_subscribe_audio(7,10)==0);
    expected_tx=10;
    assert(starter_tirtc_send_alaw(100,data,sizeof(data))==0 && sent==1);
    on_unsubscribe_audio(7,14);
    assert(starter_tirtc_send_alaw(100,data,sizeof(data))==0 && sent==2);
    on_unsubscribe_audio(7,10);
    assert(starter_tirtc_send_alaw(100,data,sizeof(data))<0 && sent==2);
    on_subscribe_audio(7,10); on_subscribe_video(7,11);
    clear_subscriptions();
    assert(!s_audio_subscribed && !s_video_subscribed);
    assert(starter_tirtc_send_alaw(100,data,sizeof(data))<0 && sent==2);
    TIRTCFRAMEINFO frame={.stream_id=14,.media=TIRTC_AUDIO_ALAW,.flags=0,.length=160};
    on_audio(7,&frame,data);
    assert(received==0); /* Frames are not admitted before our subscription. */
    s_h5_audio_rx_generation=3;
    for (unsigned id=0; id<16; ++id) {
        frame.stream_id=id; on_audio(7,&frame,data);
    }
    assert(received==1 && s_downlink_audio_rejected==16);
    frame.stream_id=14; on_audio(8,&frame,data);
    frame.media=99; on_audio(7,&frame,data);
    frame.media=TIRTC_AUDIO_ALAW; frame.flags=1; on_audio(7,&frame,data);
    assert(received==1);
    for (int mode=STARTER_TIRTC_AI; mode<=STARTER_TIRTC_ROOM; ++mode) {
        s_mode=mode;
        expected_tx=(mode==STARTER_TIRTC_AI || mode==STARTER_TIRTC_ROOM)?1:10;
        assert(starter_tirtc_send_alaw(100,data,sizeof(data))==0);
    }
    s_connection=0;
    assert(starter_tirtc_send_alaw(100,data,sizeof(data))<0);
}
'''
with tempfile.TemporaryDirectory(prefix="s3-audio-only-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(constants + prefix + "\n".join(callbacks) + main)
    for p4 in (0, 1):
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                        "-Werror", "-Wno-unused-function", "-fsanitize=address,undefined",
                        f"-DCONFIG_IDF_TARGET_ESP32P4={p4}", str(path / "test.c"),
                        "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)
print("PASS: H5 stream IDs, TX subscription gate, RX filter, unsubscribe/session isolation, other audio protocols unchanged")
