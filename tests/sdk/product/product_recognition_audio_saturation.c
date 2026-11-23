// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_recognition_audio.py
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "product_platform.h"
#include "recognition_audio.h"
#include "xiaotai/framework.h"
#include "xiaotai/modules/recognition.h"
#include "xiaotai/modules/recognition_config.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/sounds.h"

void product_recognition_audio_saturation_prepare(void);
void product_recognition_audio_saturation(void);
void product_recognition_audio_report_headroom(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *stats);
bool product_recognition_upload_has_fixture_headroom(void);
void *__real_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t bytes);
void *__wrap_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t bytes);
enum { WAV_HEADER = 44, SAMPLE_BYTES = 4096 * 2, WAIT_MS = 6000, WAV_SLOTS = 4 };
static uint8_t wav[WAV_HEADER + SAMPLE_BYTES];
static atomic_uint gets, replies;
static xiaotai_signal_t entered, release;
static _Thread_local bool wav_budget, fail_audio_alloc;

void product_recognition_audio_report_headroom(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *stats)
{
    if (kind != XIAOTAI_MEM_BULK || !stats || (!wav_budget && !product_recognition_upload_has_fixture_headroom())) return;
    /* The host memory port does not report heap geometry. Model enough free
     * external memory for the WAV and route allocations through the real port. */
    stats->total = stats->free = stats->largest_block = 512U * 1024U;
}

#if !PRODUCT_TEST_RTC_RELEASE
void __real_xiaotai_mem_stats(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *stats);
void __wrap_xiaotai_mem_stats(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *stats);

void __wrap_xiaotai_mem_stats(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *stats)
{
    __real_xiaotai_mem_stats(kind, stats);
    product_recognition_audio_report_headroom(kind, stats);
}
#endif

void *__wrap_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t bytes)
{
    if (fail_audio_alloc && kind == XIAOTAI_MEM_BULK && count == 1U) {
        fail_audio_alloc = false;
        return NULL;
    }
    return __real_xiaotai_mem_calloc(kind, count, bytes);
}

static void pcm16le(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8U);
}

static void wav_u32(size_t offset, uint32_t value)
{
    pcm16le(wav + offset, (uint16_t)value);
    pcm16le(wav + offset + 2U, (uint16_t)(value >> 16U));
}

static void fixture_pcm(void)
{
    memcpy(wav, "RIFF", 4);
    memcpy(wav + 8, "WAVEfmt ", 8);
    memcpy(wav + 36, "data", 4);
    wav_u32(4, sizeof(wav) - 8U);
    wav_u32(16, 16);
    pcm16le(wav + 20, 1);
    pcm16le(wav + 22, 1);
    wav_u32(24, 8000);
    wav_u32(28, 16000);
    pcm16le(wav + 32, 2);
    pcm16le(wav + 34, 16);
    wav_u32(40, SAMPLE_BYTES);
    for (unsigned i = 0; i < SAMPLE_BYTES / 2U; ++i)
        pcm16le(wav + WAV_HEADER + 2U * i, (uint16_t)(i * 137U - 32768U));
}

static void server(void *ctx, const xiaotai_http_host_request_t *req, xiaotai_http_host_reply_t *reply)
{
    (void)ctx;
    if (!strcmp(req->url, "http://fixture.invalid/recognition" XIAOTAI_RECOGNITION_CONFIG_PATH)) {
        reply->status = 403;
        return;
    }
    assert(!strcmp(req->url, "http://fixture.invalid/recognition/v1/recognition/requests/task_1/audio/item_1"));
    assert(req->method == XIAOTAI_HTTP_GET && !req->body && !req->body_size && !req->has_content_length);
    unsigned auth = 0;
    for (size_t i = 0; i < req->header_count; ++i) {
        if (strcmp(req->headers[i].name, "Authorization")) continue;
        assert(!strcmp(req->headers[i].value, "Bearer a.eyJleHAiOjE3MDAwMDA1MDB9.b"));
        ++auth;
    }
    assert(auth == 1);
    if (!atomic_fetch_add(&gets, 1U)) {
        xiaotai_signal_give(&entered);
        assert(xiaotai_signal_take(&release, WAIT_MS));
    }
    static const xiaotai_http_header_t headers[] = {{"Content-Type", "audio/wav"}};
    reply->headers = headers;
    reply->header_count = sizeof(headers) / sizeof(*headers);
    reply->body = wav;
    reply->body_size = sizeof(wav);
    reply->content_length = sizeof(wav);
    reply->read_chunk_bytes = 997;
}

void product_recognition_audio_saturation_prepare(void)
{
    fixture_pcm();
    assert(xiaotai_signal_init(&entered) == XIAOTAI_OK && xiaotai_signal_init(&release) == XIAOTAI_OK);
    product_platform_prepare(true);
    product_platform_http_responder(server, NULL);
}

static bool cancelled(void *ctx) { (void)ctx; return false; }

static void downloaded(const rc_audio_reply_t *reply, void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner() && reply->error == XIAOTAI_OK && reply->http_status == 200);
    assert(reply->data && reply->size == sizeof(wav) && !memcmp(reply->data, wav, sizeof(wav)));
    const xiaotai_sound_t sound = {.encoding = XIAOTAI_CODEC_PCM16, .container = XIAOTAI_SOUND_WAV,
        .data = reply->data, .size = reply->size};
    xiaotai_sound_info_t info;
    assert(xiaotai_sounds_inspect(&sound, &info) == XIAOTAI_OK && info.rate_hz == 8000 && info.channels == 1 &&
           info.frames == SAMPLE_BYTES / 2 && info.payload == reply->data + WAV_HEADER && info.payload_size == SAMPLE_BYTES);
    xiaotai_mem_free(reply->data);
    atomic_fetch_add(&replies, 1U);
}

static xiaotai_err_t submit(uint32_t *id)
{
    wav_budget = true;
    const xiaotai_err_t result = rc_audio_transfer("task_1", "item_1", cancelled, downloaded, NULL, id);
    wav_budget = false;
    return result;
}

void product_recognition_audio_saturation(void)
{
    product_platform_online();
    xiaotai_recognition_status_t status = {0};
    for (unsigned i = 0; i < WAIT_MS; i += 5) {
        if (xiaotai_recognition_status(&status) == XIAOTAI_OK && status.config_http_status == 403 &&
            !xiaotai_platform_exchanges_busy()) break;
        xiaotai_delay_ms(5);
    }
    assert(status.config_http_status == 403 && !xiaotai_platform_exchanges_busy());

    uint32_t ids[WAV_SLOTS] = {0};
    assert(submit(&ids[0]) == XIAOTAI_OK && ids[0] && xiaotai_signal_take(&entered, WAIT_MS));
    for (size_t n = 1; n < WAV_SLOTS; ++n) {
        const xiaotai_err_t result = submit(&ids[n]);
        if (result != XIAOTAI_OK)
            fprintf(stderr, "WAV request %zu was refused: %#x, GET=%u callbacks=%u\n", n + 1U,
                    (unsigned)result, atomic_load(&gets), atomic_load(&replies));
        assert(result == XIAOTAI_OK && ids[n]);
        for (size_t i = 0; i < n; ++i) assert(ids[i] != ids[n]);
    }
    uint32_t id = 1;
    const xiaotai_err_t full = submit(&id);
    printf("WAV queue: accepted=%u next=%#x GET=%u callbacks=%u\n", (unsigned)WAV_SLOTS, (unsigned)full,
           atomic_load(&gets), atomic_load(&replies));
    fflush(stdout);
    assert(full == XIAOTAI_ERR_TIMEOUT && !id && atomic_load(&gets) == 1 && !atomic_load(&replies));

    fail_audio_alloc = true;
    const xiaotai_err_t oom = submit(&id);
    assert(oom == XIAOTAI_ERR_NO_MEM && !fail_audio_alloc && atomic_load(&gets) == 1 && !atomic_load(&replies));
    xiaotai_signal_give(&release);
    for (unsigned i = 0; i < WAIT_MS; i += 5) {
        if (atomic_load(&replies) == WAV_SLOTS && !xiaotai_platform_exchanges_busy()) break;
        xiaotai_delay_ms(5);
    }
    assert(atomic_load(&gets) == WAV_SLOTS && atomic_load(&replies) == WAV_SLOTS && !xiaotai_platform_exchanges_busy());
    printf("WAV queue: allocation=%#x drained=%u byte-exact replies\n", (unsigned)oom, atomic_load(&replies));
    xiaotai_signal_deinit(&release);
    xiaotai_signal_deinit(&entered);
}
