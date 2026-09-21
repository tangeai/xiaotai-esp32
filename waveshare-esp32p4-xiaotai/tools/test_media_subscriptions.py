#!/usr/bin/env python3
"""Exercise production subscription callbacks and send gates without a device."""
from pathlib import Path
import re

from test_runtime_resources import run_case

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_tirtc/src/starter_tirtc.c").read_text(encoding="utf-8")
header = (root / "components/starter_tirtc/include/starter_tirtc.h").read_text(encoding="utf-8")


def function(name):
    start = re.search(r"(?m)^(?:static )?(?:bool|void|int) " + name + r"\(", source).start()
    return source[start:source.index("\n}", start) + 2] + "\n"


code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
typedef enum { STARTER_TIRTC_NONE, STARTER_TIRTC_H5, STARTER_TIRTC_AI,
    STARTER_TIRTC_CALL, STARTER_TIRTC_VOIP, STARTER_TIRTC_ROOM } starter_tirtc_mode_t;
typedef void *tirtc_conn_t;
typedef struct { uint8_t stream_id, media, flags; uint32_t ts, length; } TIRTCFRAMEINFO;
#define CONFIG_IDF_TARGET_ESP32P4 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define TIRTC_E_BUSY -2
#define TIRTC_E_INVALID_PARAMETER -1
#define TIRTC_E_INVALID_HANDLE -3
#define TIRTC_AUDIO_ALAW 1
#define TIRTC_VIDEO_JPEG 65
#define TIRTC_VIDEO_H264 66
#define TIRTC_AUDIOSAMPLE_8K16B1C 0
#define TIRTC_FRAME_FLAG_KEY_FRAME 1
static atomic_uintptr_t s_connection;
static atomic_int s_mode;
static atomic_bool s_audio_subscribed, s_video_subscribed;
static atomic_uint s_h5_audio_rx_generation, s_h5_video_rx_generation, s_active_generation;
static atomic_uint s_downlink_audio_rejected, s_downlink_audio_accepted;
static unsigned audio_sends, video_sends, key_requests;
static unsigned audio_subscribe_calls, video_subscribe_calls;
static unsigned audio_deliveries, video_deliveries;
static int audio_subscribe_result, video_subscribe_result;
static bool subscribe_replaces_connection, audio_subscribe_receives_inline;
static bool video_subscribe_receives_inline;
static TIRTCFRAMEINFO last_frame;
typedef struct { uint8_t stream_id, media, flags; uint32_t timestamp_ms, length; } starter_tirtc_frame_t;
static void receive_audio(starter_tirtc_mode_t mode, uint32_t generation,
                          const starter_tirtc_frame_t *frame, void *data, void *user) {
    (void)mode; (void)user;
    assert(generation == s_active_generation && frame && data);
    ++audio_deliveries;
}
static void receive_video(starter_tirtc_mode_t mode, uint32_t generation,
                          const starter_tirtc_frame_t *frame, void *data, void *user) {
    (void)mode; (void)user;
    assert(generation == s_active_generation && frame && data);
    ++video_deliveries;
}
static struct {
    void (*on_audio)(starter_tirtc_mode_t, uint32_t, const starter_tirtc_frame_t *, void *, void *);
    void (*on_video)(starter_tirtc_mode_t, uint32_t, const starter_tirtc_frame_t *, void *, void *);
    void *user_data;
} s_handlers = { .on_audio = receive_audio, .on_video = receive_video };
static bool room_transmitting;
static bool connection_matches(tirtc_conn_t c) { return c && (uintptr_t)c == s_connection; }
static bool starter_tirtc_connected(void) { return s_connection != 0; }
static starter_tirtc_mode_t starter_tirtc_mode(void) { return atomic_load(&s_mode); }
static uint32_t starter_tirtc_generation(void) { return atomic_load(&s_active_generation); }
static bool starter_tirtc_room_transmitting(uint32_t generation) {
    assert(generation == 7); return room_transmitting;
}
static void on_request_key_frame(tirtc_conn_t c, uint8_t stream) {
    assert(connection_matches(c)); (void)stream; ++key_requests;
}
static int TiRtcSendAudioStream(tirtc_conn_t c, const TIRTCFRAMEINFO *f, const void *data) {
    assert(connection_matches(c) && data); ++audio_sends; last_frame = *f; return f->length;
}
static int TiRtcSendVideoStream(tirtc_conn_t c, const TIRTCFRAMEINFO *f, const void *data) {
    assert(connection_matches(c) && data); ++video_sends; last_frame = *f; return f->length;
}
static void on_audio(tirtc_conn_t connection, const TIRTCFRAMEINFO *frame, void *data);
static void on_video(tirtc_conn_t connection, const TIRTCFRAMEINFO *frame, void *data);
static int TiRtcSubscribeAudio(tirtc_conn_t c, uint8_t stream) {
    assert(connection_matches(c) && stream == 14); ++audio_subscribe_calls;
    if (audio_subscribe_receives_inline) {
        TIRTCFRAMEINFO frame = { .stream_id = stream, .media = TIRTC_AUDIO_ALAW,
            .flags = TIRTC_AUDIOSAMPLE_8K16B1C, .length = 1 };
        char pcm = 0;
        on_audio(c, &frame, &pcm);
    }
    if (subscribe_replaces_connection) {
        s_connection = 2;
        s_active_generation = 8;
        s_h5_audio_rx_generation = 8;
        s_h5_video_rx_generation = 8;
    }
    return audio_subscribe_result;
}
static int TiRtcSubscribeVideo(tirtc_conn_t c, uint8_t stream) {
    assert(connection_matches(c) && stream == 15); ++video_subscribe_calls;
    if (video_subscribe_receives_inline) {
        TIRTCFRAMEINFO frame = { .stream_id = stream, .media = TIRTC_VIDEO_H264,
            .flags = TIRTC_FRAME_FLAG_KEY_FRAME, .length = 1 };
        char h264 = 0;
        on_video(c, &frame, &h264);
    }
    return video_subscribe_result;
}
'''
for name in ("STARTER_H5_UP_AUDIO_STREAM_ID", "STARTER_H5_UP_VIDEO_STREAM_ID",
             "STARTER_H5_DOWN_AUDIO_STREAM_ID", "STARTER_H5_DOWN_VIDEO_STREAM_ID"):
    code += re.search(r"(?m)^#define " + name + r" .+$", header).group(0) + "\n"
for name in ("H5_AUDIO_STREAM", "H5_VIDEO_STREAM", "H5_TALKBACK_AUDIO_STREAM",
             "H5_TALKBACK_VIDEO_STREAM", "AI_AUDIO_STREAM", "ROOM_AUDIO_STREAM",
             "CALL_AUDIO_STREAM"):
    code += re.search(r"(?m)^#define " + name + r" .+$", source).group(0) + "\n"
for name in ("audio_subscription_matches", "on_subscribe_audio", "on_subscribe_video",
             "on_unsubscribe_audio", "on_unsubscribe_video", "starter_tirtc_audio_ready",
             "starter_tirtc_video_ready", "starter_tirtc_send_alaw", "starter_tirtc_send_mjpeg",
             "starter_tirtc_send_h264", "on_audio", "on_video",
             "starter_tirtc_subscribe_h5_audio", "starter_tirtc_subscribe_h5_video"):
    code += function(name)
code += r'''
int main(void) {
    const char pcm[] = "frame";
    s_mode = STARTER_TIRTC_H5; s_connection = 1; s_active_generation = 7;
    TIRTCFRAMEINFO received = { .stream_id = 14, .media = TIRTC_AUDIO_ALAW,
        .flags = TIRTC_AUDIOSAMPLE_8K16B1C, .length = 1 };
    char rx = 0;
    on_audio((void*)1, &received, &rx);
    TIRTCFRAMEINFO remote_video = { .stream_id = 15, .media = TIRTC_VIDEO_H264,
        .flags = TIRTC_FRAME_FLAG_KEY_FRAME, .length = 1 };
    on_video((void*)1, &remote_video, &rx);
    assert(audio_deliveries == 0 && video_deliveries == 0);
    assert(starter_tirtc_subscribe_h5_audio(0) < 0);
    assert(starter_tirtc_subscribe_h5_audio(6) < 0 && audio_subscribe_calls == 0);
    assert(starter_tirtc_subscribe_h5_video(6) < 0 && video_subscribe_calls == 0);
    audio_subscribe_result = -5;
    assert(starter_tirtc_subscribe_h5_audio(7) == -5 && s_h5_audio_rx_generation == 0);
    video_subscribe_result = -6;
    assert(starter_tirtc_subscribe_h5_video(7) == -6 && s_h5_video_rx_generation == 0);
    on_audio((void*)1, &received, &rx);
    on_video((void*)1, &remote_video, &rx);
    assert(audio_deliveries == 0 && video_deliveries == 0);
    audio_subscribe_result = 1; audio_subscribe_receives_inline = true;
    assert(starter_tirtc_subscribe_h5_audio(7) == 1 && audio_deliveries == 1);
    assert(starter_tirtc_subscribe_h5_audio(7) == 0 && audio_subscribe_calls == 2);
    video_subscribe_result = 1; video_subscribe_receives_inline = true;
    assert(starter_tirtc_subscribe_h5_video(7) == 1 && video_deliveries == 1);
    assert(starter_tirtc_subscribe_h5_video(7) == 0 && video_subscribe_calls == 2);
    audio_subscribe_receives_inline = false;
    video_subscribe_receives_inline = false;
    on_audio((void*)2, &received, &rx);
    for (int stream = 10; stream <= 15; ++stream) {
        received.stream_id = stream;
        on_audio((void*)1, &received, &rx);
        remote_video.stream_id = stream;
        on_video((void*)1, &remote_video, &rx);
    }
    assert(audio_deliveries == 2);  /* Only the subscribed peer stream 14. */
    assert(video_deliveries == 2);  /* Only the subscribed peer stream 15. */
    received.stream_id = 14; received.flags = 99;
    on_audio((void*)1, &received, &rx);
    received.flags = TIRTC_AUDIOSAMPLE_8K16B1C; received.media = 99;
    on_audio((void*)1, &received, &rx);
    remote_video.stream_id = 15; remote_video.media = TIRTC_VIDEO_JPEG;
    on_video((void*)1, &remote_video, &rx);
    assert(audio_deliveries == 2 && video_deliveries == 2);
    received.media = TIRTC_AUDIO_ALAW;
    remote_video.media = TIRTC_VIDEO_H264;
    s_h5_audio_rx_generation = 0; subscribe_replaces_connection = true;
    assert(starter_tirtc_subscribe_h5_audio(7) == TIRTC_E_INVALID_HANDLE);
    assert(s_h5_audio_rx_generation == 8); /* Do not revoke a newer connection. */
    s_connection = 1; s_active_generation = 7; s_h5_audio_rx_generation = 6;
    s_h5_video_rx_generation = 6;
    on_audio((void*)1, &received, &rx);
    on_video((void*)1, &remote_video, &rx);
    assert(audio_deliveries == 2 && video_deliveries == 2);
    s_h5_audio_rx_generation = 0; s_h5_video_rx_generation = 0;
    subscribe_replaces_connection = false;
    assert(starter_tirtc_subscribe_h5_audio(7) == 1);
    assert(starter_tirtc_subscribe_h5_video(7) == 1);
    /* Our RX subscriptions must not grant the peer permission to receive our TX. */
    assert(!starter_tirtc_audio_ready() && !starter_tirtc_video_ready());
    assert(starter_tirtc_send_alaw(0, pcm, 5) < 0);
    assert(starter_tirtc_send_mjpeg(0, pcm, 5) < 0);
    assert(starter_tirtc_send_h264(0, pcm, 5, true) < 0);
    assert(audio_sends == 0 && video_sends == 0);
    assert(on_subscribe_audio((void*)2, H5_AUDIO_STREAM) < 0);
    assert(on_subscribe_audio((void*)1, 99) < 0);
    assert(on_subscribe_audio((void*)1, H5_AUDIO_STREAM) == 0);
    assert(starter_tirtc_audio_ready());
    assert(starter_tirtc_send_alaw(20, pcm, 5) == 5);
    assert(last_frame.stream_id == H5_AUDIO_STREAM && last_frame.ts == 20);
    on_unsubscribe_audio((void*)2, H5_AUDIO_STREAM);
    on_unsubscribe_audio((void*)1, 99);
    assert(starter_tirtc_audio_ready());
    on_unsubscribe_audio((void*)1, H5_AUDIO_STREAM);
    assert(starter_tirtc_send_alaw(40, pcm, 5) < 0 && audio_sends == 1);
    assert(on_subscribe_video((void*)2, H5_VIDEO_STREAM) < 0);
    assert(on_subscribe_video((void*)1, 99) < 0);
    assert(on_subscribe_video((void*)1, H5_VIDEO_STREAM) == 0 && key_requests == 1);
    assert(starter_tirtc_send_mjpeg(40, pcm, 5) == 5);
    assert(starter_tirtc_send_h264(60, pcm, 5, true) == 5);
    assert(last_frame.stream_id == H5_VIDEO_STREAM && video_sends == 2);
    on_unsubscribe_video((void*)2, H5_VIDEO_STREAM);
    on_unsubscribe_video((void*)1, 99);
    assert(starter_tirtc_video_ready());
    on_unsubscribe_video((void*)1, H5_VIDEO_STREAM);
    assert(starter_tirtc_send_h264(80, pcm, 5, false) < 0);
    assert(starter_tirtc_send_mjpeg(80, pcm, 5) < 0 && video_sends == 2);
    /* Unrelated business paths keep their existing handshake contract. */
    for (int mode = STARTER_TIRTC_AI; mode <= STARTER_TIRTC_VOIP; ++mode) {
        s_mode = mode;
        assert(starter_tirtc_subscribe_h5_audio(7) == TIRTC_E_INVALID_PARAMETER);
        assert(starter_tirtc_subscribe_h5_video(7) == TIRTC_E_INVALID_PARAMETER);
        assert(starter_tirtc_audio_ready());
        assert(starter_tirtc_send_alaw(100, pcm, 5) == 5);
        assert(last_frame.stream_id == (mode == STARTER_TIRTC_AI ? AI_AUDIO_STREAM : CALL_AUDIO_STREAM));
    }
    s_mode = STARTER_TIRTC_ROOM;
    assert(starter_tirtc_send_alaw(100, pcm, 5) < 0);
    room_transmitting = true;
    assert(starter_tirtc_send_alaw(100, pcm, 5) == 5 && last_frame.stream_id == ROOM_AUDIO_STREAM);
    s_connection = 0;
    assert(!starter_tirtc_audio_ready() && !starter_tirtc_video_ready());
    assert(starter_tirtc_send_alaw(120, pcm, 5) < 0);
    assert(starter_tirtc_subscribe_h5_audio(7) < 0);
    assert(starter_tirtc_subscribe_h5_video(7) < 0);
    return 0;
}
'''
run_case("H5 directional subscriptions, generations, send gates and business isolation", code)
