"""Compile the real TiRTC adapter lock paths against a freeing SDK fake.

The fake frees a handle on its own thread after TiRtcDisconnect, like the SDK.
ASan reports any send/query that still uses it. TiRtcDisconnect also waits for
on_disconnected, so a callback that took the lifetime lock would hang the test.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_tirtc/src/starter_tirtc.c").read_text(encoding="utf-8")
header = (root / "components/starter_tirtc/include/starter_tirtc.h").read_text(encoding="utf-8")
constants = "\n".join(re.findall(r"^#define (?:STARTER_H5_|STARTER_TIRTC_AUDIO_).*$", header, re.M) +
                      re.findall(r"^#define CONNECTION_LOCK_.*$", source, re.M)) + "\n"


def function(name):
    match = re.search(r"^(?:static )?(?:int|bool|void|size_t) " + name + r"\([^)]*\)\s*\{.*?\n\}",
                      source, re.M | re.S)
    assert match, name
    return match[0]


prefix = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define CONFIG_IDF_TARGET_ESP32P4 0
#define CONFIG_IDF_TARGET_ESP32S3 1
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#define TIRTC_E_INVALID_PARAMETER -40001
#define TIRTC_E_INVALID_HANDLE -40002
#define TIRTC_E_BUSY -40006
#define TIRTC_AUDIO_ALAW 2
#define TIRTC_AUDIO_OPUS 4
#define TIRTC_AUDIOSAMPLE_8K16B1C 0
#define TIRTC_AUDIOSAMPLE_16K16B1C 1
#define H5_AUDIO_STREAM STARTER_H5_UP_AUDIO_STREAM_ID
#define AI_AUDIO_STREAM 1U
#define CALL_AUDIO_STREAM 10U
#define H5_VIDEO_STREAM STARTER_H5_UP_VIDEO_STREAM_ID
#define H5_VIDEO_BACKLOG_BYTES (32U * 1024U)
#define TIRTC_VIDEO_JPEG 9
#define TIRTC_FRAME_FLAG_KEY_FRAME 1
#define CONN_MAGIC 0x434f4e4eU
typedef enum { STARTER_TIRTC_NONE, STARTER_TIRTC_H5, STARTER_TIRTC_AI,
               STARTER_TIRTC_VOIP, STARTER_TIRTC_CALL, STARTER_TIRTC_ROOM } starter_tirtc_mode_t;
typedef struct { uint8_t stream_id, media, flags; uint32_t ts, length; } TIRTCFRAMEINFO;
struct _tirtc_conn { uint32_t magic; };
typedef struct _tirtc_conn *tirtc_conn_t;
typedef pthread_mutex_t *SemaphoreHandle_t;

static const char *TAG = "starter_tirtc";
static atomic_uint warnings;
static void log_warning(const char *tag, const char *format, ...) {
    assert(strcmp(tag, "starter_tirtc") == 0 && strstr(format, "connection lock busy"));
    atomic_fetch_add(&warnings, 1);
}
#define ESP_LOGW(tag, ...) log_warning(tag, __VA_ARGS__)
#define ESP_LOGI(...) ((void)0)

static atomic_uintptr_t s_connection;
static atomic_int s_mode;
static atomic_uint_fast32_t s_active_generation, s_pending_request, s_expected_call_tag;
static atomic_bool s_audio_subscribed, s_video_subscribed;
static atomic_uint s_h5_audio_rx_generation;
static pthread_mutex_t lock_storage;
static SemaphoreHandle_t s_connection_lock = &lock_storage;
static atomic_uint_fast32_t s_connection_lock_busy;
static atomic_uint s_video_inflight;

/* FreeRTOS mutex model: bounded waits are real timed waits. */
static _Thread_local bool i_hold;
static _Thread_local uint32_t last_wait;
static atomic_bool force_busy;
static void (*take_hook)(void);
static int xSemaphoreTake(SemaphoreHandle_t m, uint32_t wait) {
    assert(m == &lock_storage);
    last_wait = wait;
    if (atomic_load(&force_busy)) return 0;
    int rc;
    if (wait == portMAX_DELAY) {
        rc = pthread_mutex_lock(m);
    } else {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_nsec += (long)wait * 1000000L;
        t.tv_sec += t.tv_nsec / 1000000000L;
        t.tv_nsec %= 1000000000L;
        rc = pthread_mutex_timedlock(m, &t);
    }
    if (rc != 0) { assert(rc == ETIMEDOUT); return 0; }
    i_hold = true;
    if (take_hook) take_hook();
    return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t m) {
    assert(i_hold);
    i_hold = false;
    int rc = pthread_mutex_unlock(m);
    assert(rc == 0); (void)rc;
}
static void vTaskDelay(unsigned ticks) {assert(ticks == 1); usleep(1000);}

static void (*mode_hook)(void);
static uint32_t starter_tirtc_generation(void) { return (uint32_t)atomic_load(&s_active_generation); }
static bool starter_tirtc_connected(void) {return atomic_load(&s_connection) != 0;}
static starter_tirtc_mode_t starter_tirtc_mode(void) {
    if (mode_hook) mode_hook();
    return (starter_tirtc_mode_t)atomic_load(&s_mode);
}
static atomic_uint notifications;
static void notify_connection(starter_tirtc_mode_t mode, uint32_t generation, uint32_t tag,
                              bool connected, int error) {
    (void)mode; (void)generation; (void)tag; (void)connected; (void)error;
    atomic_fetch_add(&notifications, 1);
}

/* SDK fake: every handle use must hold the lock and see a live object. */
static atomic_uint sdk_audio, sdk_commands, sdk_queries, sdk_disconnects;
static atomic_bool block_audio, audio_in_sdk;
static atomic_bool block_video, video_in_sdk;
static atomic_uint sdk_video;
static uint8_t expected_stream, expected_media;
static void use_handle(tirtc_conn_t c) {
    assert(i_hold);
    assert(c->magic == CONN_MAGIC);
    sched_yield();
    assert(c->magic == CONN_MAGIC);
}
static int TiRtcSendAudioStream(tirtc_conn_t c, const TIRTCFRAMEINFO *f, const void *data) {
    use_handle(c);
    assert(f && data && f->length == 60);
    if (expected_stream) assert(f->stream_id == expected_stream && f->media == expected_media);
    if (atomic_load(&block_audio)) {
        atomic_store(&audio_in_sdk, true);
        while (atomic_load(&block_audio)) sched_yield();
    }
    use_handle(c);
    atomic_fetch_add(&sdk_audio, 1);
    return (int)f->length;
}
static int TiRtcSendCommand(tirtc_conn_t c, uint32_t command, const void *data, uint32_t length) {
    use_handle(c);
    assert(command == 0x2100U && data && length == 4);
    atomic_fetch_add(&sdk_commands, 1);
    return (int)length;
}
static size_t TiRtcGetSendBufferUsed(tirtc_conn_t c) {
    use_handle(c);
    atomic_fetch_add(&sdk_queries, 1);
    return 123;
}
static int TiRtcSendVideoStream(tirtc_conn_t c, const TIRTCFRAMEINFO *f, const void *data) {
    assert(!i_hold && atomic_load(&s_video_inflight) == 1 && c->magic == CONN_MAGIC);
    assert(f && data && f->stream_id == 11 && f->length == 60);
    atomic_store(&video_in_sdk, true);
    while (atomic_load(&block_video)) sched_yield();
    assert(c->magic == CONN_MAGIC);
    atomic_fetch_add(&sdk_video, 1);
    return (int)f->length;
}
static void on_disconnected(tirtc_conn_t connection);
static _Atomic(tirtc_conn_t) closing;
static atomic_bool closed, stop_sdk;
static int TiRtcDisconnect(tirtc_conn_t c) {
    assert(i_hold && atomic_load(&s_connection) == 0 && atomic_load(&s_active_generation) == 0);
    assert(!atomic_load(&s_video_inflight));
    atomic_store(&closed, false);
    atomic_store(&closing, c);
    /* Strictest model: wait for the SDK thread's callback and free. */
    while (!atomic_load(&closed)) sched_yield();
    atomic_fetch_add(&sdk_disconnects, 1);
    return 0;
}
static void *sdk_thread(void *arg) {
    (void)arg;
    while (!atomic_load(&stop_sdk)) {
        tirtc_conn_t c = atomic_exchange(&closing, NULL);
        if (!c) { sched_yield(); continue; }
        on_disconnected(c);
        c->magic = 0;
        free(c);
        atomic_store(&closed, true);
    }
    return NULL;
}
'''

main = r'''
static tirtc_conn_t publish(starter_tirtc_mode_t mode, uint32_t generation) {
    tirtc_conn_t c = malloc(sizeof(*c));
    assert(c);
    c->magic = CONN_MAGIC;
    atomic_store(&s_connection, (uintptr_t)c);
    atomic_store(&s_mode, mode);
    atomic_store(&s_active_generation, generation);
    return c;
}
static void revoke_slot(void) { atomic_store(&s_connection, 0); }
static void bump_generation(void) { atomic_fetch_add(&s_active_generation, 1); }

static unsigned char packet[60], command[4] = {'{', '}', ' ', ' '};
static void *blocked_sender(void *arg) {
    (void)arg;
    assert(starter_tirtc_send_audio(1, packet, sizeof(packet)) == 60);
    return NULL;
}
static void *disconnector(void *arg) {
    (void)arg;
    assert(starter_tirtc_disconnect() == 0);
    return NULL;
}
static void *blocked_video_sender(void *arg) {
    uint32_t gen = *(uint32_t *)arg;
    assert(starter_tirtc_send_h5_jpeg(gen, 1, packet, sizeof(packet)) == 60);
    return NULL;
}

static atomic_bool stop_senders;
static atomic_uint audio_ok, audio_busy, command_ok, command_busy, query_ok, query_zero;
static void *audio_sender(void *arg) {
    (void)arg;
    while (!atomic_load(&stop_senders)) {
        int rc = starter_tirtc_send_audio(1, packet, sizeof(packet));
        if (rc == 60) atomic_fetch_add(&audio_ok, 1);
        else if (rc == TIRTC_E_BUSY) atomic_fetch_add(&audio_busy, 1);
        else assert(rc == TIRTC_E_INVALID_PARAMETER);
    }
    return NULL;
}
static void *command_sender(void *arg) {
    (void)arg;
    while (!atomic_load(&stop_senders)) {
        int rc = starter_tirtc_send_command(0x2100U, command, sizeof(command));
        if (rc == 4) atomic_fetch_add(&command_ok, 1);
        else if (rc == TIRTC_E_BUSY) atomic_fetch_add(&command_busy, 1);
        else assert(rc == TIRTC_E_INVALID_HANDLE);
    }
    return NULL;
}
static void *query_sender(void *arg) {
    (void)arg;
    while (!atomic_load(&stop_senders)) {
        size_t used = starter_tirtc_send_buffer_used();
        assert(used == 0 || used == 123);
        atomic_fetch_add(used ? &query_ok : &query_zero, 1);
    }
    return NULL;
}

int main(void) {
    alarm(120); /* A callback taking the lock would deadlock here. */
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&lock_storage, &attr);
    pthread_t sdk;
    assert(pthread_create(&sdk, NULL, sdk_thread, NULL) == 0);

    /* No connection: no lock traffic, nothing counted. */
    last_wait = 7;
    assert(starter_tirtc_send_audio(1, packet, sizeof(packet)) == TIRTC_E_INVALID_PARAMETER);
    assert(starter_tirtc_send_command(0x2100U, command, sizeof(command)) == TIRTC_E_INVALID_HANDLE);
    assert(starter_tirtc_send_buffer_used() == 0);
    assert(starter_tirtc_disconnect() == TIRTC_E_INVALID_HANDLE);
    assert(last_wait == 7 && s_connection_lock_busy == 0 && sdk_disconnects == 0);

    /* Bounded waits per caller class; SDK calls run under the lock. */
    publish(STARTER_TIRTC_AI, 1);
    expected_stream = AI_AUDIO_STREAM; expected_media = TIRTC_AUDIO_ALAW;
    assert(starter_tirtc_send_audio(1, packet, sizeof(packet)) == 60 && sdk_audio == 1);
    assert(last_wait == CONNECTION_LOCK_MEDIA_MS && CONNECTION_LOCK_MEDIA_MS <= 2U && !i_hold);
    assert(starter_tirtc_send_command(0x2100U, command, sizeof(command)) == 4 && sdk_commands == 1);
    assert(last_wait == CONNECTION_LOCK_CONTROL_MS && !i_hold);
    assert(starter_tirtc_send_buffer_used() == 123 && sdk_queries == 1);
    assert(last_wait == CONNECTION_LOCK_MEDIA_MS && !i_hold);
    assert(starter_tirtc_send_audio(1, NULL, sizeof(packet)) == TIRTC_E_INVALID_PARAMETER);
    assert(starter_tirtc_send_audio(1, packet, 0) == TIRTC_E_INVALID_PARAMETER && sdk_audio == 1);

    /* Busy: drop/zero, never call the SDK, count every miss, log rate-limited. */
    atomic_store(&force_busy, true);
    assert(starter_tirtc_send_audio(1, packet, sizeof(packet)) == TIRTC_E_BUSY);
    assert(starter_tirtc_send_command(0x2100U, command, sizeof(command)) == TIRTC_E_BUSY);
    assert(starter_tirtc_send_buffer_used() == 0);
    assert(s_connection_lock_busy == 3 && warnings == 1);
    while (s_connection_lock_busy < 100) (void)starter_tirtc_send_buffer_used();
    assert(warnings == 2 && sdk_audio == 1 && sdk_commands == 1 && sdk_queries == 1);
    atomic_store(&force_busy, false);
    atomic_store(&s_connection_lock_busy, 0);
    atomic_store(&warnings, 0);

    /* Slot revoked while waiting for the lock: reload wins, no SDK call. */
    tirtc_conn_t live = (tirtc_conn_t)atomic_load(&s_connection);
    take_hook = revoke_slot;
    assert(starter_tirtc_send_audio(1, packet, sizeof(packet)) == TIRTC_E_INVALID_PARAMETER);
    atomic_store(&s_connection, (uintptr_t)live);
    assert(starter_tirtc_send_command(0x2100U, command, sizeof(command)) == TIRTC_E_INVALID_HANDLE);
    atomic_store(&s_connection, (uintptr_t)live);
    assert(starter_tirtc_send_buffer_used() == 0);
    take_hook = NULL;
    atomic_store(&s_connection, (uintptr_t)live);
    assert(sdk_audio == 1 && sdk_commands == 1 && sdk_queries == 1 && !i_hold);

    /* Generation must be nonzero and stable around the handle load. */
    mode_hook = bump_generation;
    assert(starter_tirtc_send_audio(1, packet, sizeof(packet)) == TIRTC_E_INVALID_PARAMETER);
    mode_hook = NULL;
    atomic_store(&s_active_generation, 0);
    assert(starter_tirtc_send_audio(1, packet, sizeof(packet)) == TIRTC_E_INVALID_PARAMETER);
    atomic_store(&s_active_generation, 2);
    atomic_store(&s_mode, STARTER_TIRTC_NONE);
    assert(starter_tirtc_send_audio(1, packet, sizeof(packet)) == TIRTC_E_INVALID_PARAMETER);
    atomic_store(&s_mode, STARTER_TIRTC_H5);
    assert(starter_tirtc_send_audio(1, packet, sizeof(packet)) == TIRTC_E_INVALID_PARAMETER);
    atomic_store(&s_audio_subscribed, true);
    expected_stream = H5_AUDIO_STREAM;
    assert(starter_tirtc_send_audio(1, packet, sizeof(packet)) == 60);
    atomic_store(&s_mode, STARTER_TIRTC_CALL);
    expected_stream = CALL_AUDIO_STREAM; expected_media = TIRTC_AUDIO_OPUS;
    assert(starter_tirtc_send_audio(1, packet, sizeof(packet)) == 60 && sdk_audio == 3 && !i_hold);
    assert(s_connection_lock_busy == 0);
    expected_stream = 0;

    /* Local disconnect revokes first, then drains an in-flight send. */
    atomic_store(&block_audio, true);
    pthread_t sender, closer;
    assert(pthread_create(&sender, NULL, blocked_sender, NULL) == 0);
    while (!atomic_load(&audio_in_sdk)) sched_yield();
    assert(pthread_create(&closer, NULL, disconnector, NULL) == 0);
    while (atomic_load(&s_connection) != 0) sched_yield();
    struct timespec delay = {0, 20 * 1000000L};
    nanosleep(&delay, NULL);
    assert(sdk_disconnects == 0);
    assert(starter_tirtc_send_audio(1, packet, sizeof(packet)) == TIRTC_E_INVALID_PARAMETER);
    atomic_store(&block_audio, false);
    pthread_join(sender, NULL);
    pthread_join(closer, NULL);
    assert(sdk_disconnects == 1 && sdk_audio == 4 && notifications == 0);

    /* A blocked JPEG enqueue cannot hold up audio. Disconnect still waits
     * for that exact pinned handle before the SDK frees it. */
    for (uint32_t gen = 4000; gen < 4100; ++gen) {
        publish(STARTER_TIRTC_H5, gen);
        atomic_store(&s_audio_subscribed, true);
        atomic_store(&s_video_subscribed, true);
        atomic_store(&video_in_sdk, false);
        atomic_store(&block_video, true);
        assert(pthread_create(&sender, NULL, blocked_video_sender, &gen) == 0);
        while (!atomic_load(&video_in_sdk)) sched_yield();
        unsigned before = atomic_load(&sdk_audio);
        assert(starter_tirtc_send_audio(1, packet, sizeof(packet)) == 60);
        assert(sdk_audio == before + 1 && s_video_inflight == 1);
        unsigned closed_before = atomic_load(&sdk_disconnects);
        assert(pthread_create(&closer, NULL, disconnector, NULL) == 0);
        while (atomic_load(&s_connection)) sched_yield();
        nanosleep(&delay, NULL);
        assert(sdk_disconnects == closed_before && s_video_inflight == 1);
        assert(starter_tirtc_send_h5_jpeg(gen, 1, packet, sizeof(packet)) < 0);
        atomic_store(&block_video, false);
        pthread_join(sender, NULL);
        pthread_join(closer, NULL);
        assert(sdk_disconnects == closed_before + 1 && !s_video_inflight);
    }
    assert(sdk_video == 100);

    /* Stress: senders race connect/disconnect cycles; ASan catches UAF. */
    pthread_t workers[3];
    assert(pthread_create(&workers[0], NULL, audio_sender, NULL) == 0);
    assert(pthread_create(&workers[1], NULL, command_sender, NULL) == 0);
    assert(pthread_create(&workers[2], NULL, query_sender, NULL) == 0);
    for (uint32_t i = 0; i < 3000U; ++i) {
        publish(i % 2U ? STARTER_TIRTC_AI : STARTER_TIRTC_ROOM, i + 10U);
        for (int k = 0; k < 20; ++k) sched_yield();
        assert(starter_tirtc_disconnect() == 0);
    }
    atomic_store(&stop_senders, true);
    for (unsigned i = 0; i < 3; ++i) pthread_join(workers[i], NULL);
    atomic_store(&stop_sdk, true);
    pthread_join(sdk, NULL);
    assert(sdk_disconnects == 3101 && notifications == 0);
    assert(audio_ok > 0 && command_ok > 0 && query_ok > 0);
    unsigned busy = (unsigned)atomic_load(&s_connection_lock_busy);
    unsigned dropped = atomic_load(&audio_busy) + atomic_load(&command_busy);
    assert(busy >= dropped && busy <= dropped + atomic_load(&query_zero));
    printf("stress ok/busy audio=%u/%u command=%u/%u query_ok=%u lock_busy=%u\n",
           atomic_load(&audio_ok), atomic_load(&audio_busy), atomic_load(&command_ok),
           atomic_load(&command_busy), atomic_load(&query_ok), busy);
    pthread_mutex_destroy(&lock_storage);
    pthread_mutexattr_destroy(&attr);
}
'''

with tempfile.TemporaryDirectory(prefix="s3-tirtc-lock-") as tmp:
    path = Path(tmp)
    body = "\n".join(function(name) for name in (
        "clear_subscriptions", "connection_lock", "on_disconnected", "starter_tirtc_disconnect",
        "starter_tirtc_send_command", "starter_tirtc_send_audio", "starter_tirtc_send_buffer_used",
        "starter_tirtc_video_ready", "starter_tirtc_send_h5_jpeg"))
    (path / "test.c").write_text(constants + prefix + body + "\n#include <stdio.h>\n" + main)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-pthread", str(path / "test.c"),
                    "-o", str(path / "test")], check=True)
    result = subprocess.run([str(path / "test")], check=True, stdout=subprocess.PIPE, text=True)
print("PASS: TiRTC lifetime lock: JPEG/audio overlap, disconnect drains pins, 3100 concurrent cycles;",
      result.stdout.strip())
