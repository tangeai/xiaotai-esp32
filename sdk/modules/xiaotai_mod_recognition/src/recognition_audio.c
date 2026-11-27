// SPDX-License-Identifier: MIT
#include "recognition_audio.h"
#include "recognition_compact.h"
#include "recognition_json.h"
#include "xiaotai/port/mem.h"

#include <stdio.h>
#include <string.h>

#define AUDIO_BUFFER_SIZE 4096U
#define AUDIO_RESERVE (256U * 1024U)
#define AUDIO_MAX_BYTES (10U * 1024U * 1024U)
#define AUDIO_MIN_BYTES 44U

typedef struct {
    bool (*cancelled)(void *ctx);
    rc_audio_reply_fn_t done;
    void *ctx;
    uint8_t *data;
    size_t budget, received, capacity;
    int64_t declared;
    int status;
    bool clipped;
    bool compact;           /* kept as A-law: it did not fit as it comes */
    size_t stored;          /* bytes kept, compact */
    rc_compact_t *converter;
} audio_transfer_t;

static size_t budget_bytes(void)
{
    xiaotai_mem_stats_t memory;
    xiaotai_mem_stats(XIAOTAI_MEM_BULK, &memory);
    size_t budget = memory.free > AUDIO_RESERVE ? memory.free - AUDIO_RESERVE : 0;
    if (budget > memory.largest_block) budget = memory.largest_block;
    return budget <= AUDIO_MAX_BYTES ? budget : AUDIO_MAX_BYTES;
}

static const char *skip_ws(const char *text)
{
    while (*text == ' ' || *text == '\t') ++text;
    return text;
}

static const char *ascii_prefix(const char *text, const char *lower)
{
    while (*lower) {
        char c = *text++;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != *lower++) return NULL;
    }
    return skip_ws(text);
}

static bool wav_headers(const xiaotai_platform_response_head_t *head)
{
    if (head->truncated || head->content_type_repeated || head->content_encoding_repeated ||
        !head->content_type) return false;
    const char *tail = ascii_prefix(skip_ws(head->content_type), "audio/wav");
    if (!tail || (*tail && *tail != ';')) return false;
    if (!head->content_encoding || !*head->content_encoding) return true;
    tail = ascii_prefix(skip_ws(head->content_encoding), "identity");
    return tail && !*tail;
}

static xiaotai_err_t begin(void *ctx, const xiaotai_platform_response_head_t *head)
{
    audio_transfer_t *audio = ctx;
    if (audio->cancelled(audio->ctx)) return XIAOTAI_ERR_PLATFORM_CANCELLED;
    audio->status = head->http_status;
    audio->declared = head->content_length;
    if (audio->status == 200) {
        if (!wav_headers(head)) return RC_INVALID_RESPONSE;
        if (!audio->declared) return RC_INVALID_RESPONSE;
        const bool fits = audio->declared <= (int64_t)audio->budget;
        audio->capacity = audio->declared > 0 ? (size_t)audio->declared :
            audio->budget < AUDIO_BUFFER_SIZE ? audio->budget : AUDIO_BUFFER_SIZE;
        if (fits && (audio->data = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, audio->capacity))) return XIAOTAI_OK;
        /* Too large for memory as it comes: kept as A-law, half the bytes. */
        if (audio->declared < 0) return fits ? XIAOTAI_ERR_NO_MEM : XIAOTAI_ERR_INVALID_SIZE;
        audio->capacity = rc_compact_capacity((size_t)audio->declared);
        if (audio->capacity > audio->budget) return XIAOTAI_ERR_INVALID_SIZE;
        audio->converter = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1U, sizeof(*audio->converter));
        if (!audio->converter) return XIAOTAI_ERR_NO_MEM;
        audio->compact = true;
    } else {
        if (audio->declared >= AUDIO_BUFFER_SIZE) audio->clipped = true;
        if (audio->clipped) return RC_INVALID_RESPONSE;
        audio->capacity = AUDIO_BUFFER_SIZE;
    }
    audio->data = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, audio->capacity);
    return audio->data ? XIAOTAI_OK : XIAOTAI_ERR_NO_MEM;
}

static xiaotai_err_t feed_compact(audio_transfer_t *audio, const uint8_t *data, size_t size)
{
    if (audio->declared >= 0 && size > (uint64_t)audio->declared - audio->received) return RC_INVALID_RESPONSE;
    audio->received += size;
    const xiaotai_err_t error = rc_compact_feed(audio->converter, data, size, audio->data, audio->capacity,
                                                &audio->stored);
    return error == XIAOTAI_FAIL ? RC_INVALID_RESPONSE : error;
}

/* No room to grow: what came so far becomes A-law where it lies (the samples move to the front at half
 * their size, never past what is still to be read), and the rest follows it into the same buffer. */
static xiaotai_err_t go_compact(audio_transfer_t *audio)
{
    audio->converter = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1U, sizeof(*audio->converter));
    if (!audio->converter) return XIAOTAI_ERR_NO_MEM;
    audio->compact = true;
    const size_t had = audio->received;
    audio->received = 0;
    audio->stored = 0;
    return feed_compact(audio, audio->data, had);
}

static xiaotai_err_t write_audio(void *ctx, const uint8_t *data, size_t size)
{
    audio_transfer_t *audio = ctx;
    if (audio->cancelled(audio->ctx)) return XIAOTAI_ERR_PLATFORM_CANCELLED;
    if (!audio->data) return RC_INVALID_RESPONSE;
    if (!size) return XIAOTAI_OK;
    if (audio->compact) return feed_compact(audio, data, size);
    if (audio->status != 200) {
        if (size >= AUDIO_BUFFER_SIZE - audio->received) {
            audio->clipped = true;
            return RC_INVALID_RESPONSE;
        }
    } else if (size > audio->capacity - audio->received) {
        if (audio->declared >= 0) return RC_INVALID_RESPONSE;
        /* No length was said: the buffer grows while the budget allows and memory gives it. */
        const size_t need = audio->received + size;
        size_t capacity = audio->capacity;
        while (capacity < need && capacity < audio->budget) {
            capacity *= 2U;
            if (capacity > audio->budget) capacity = audio->budget;
        }
        uint8_t *grown = capacity >= need ? xiaotai_mem_alloc(XIAOTAI_MEM_BULK, capacity) : NULL;
        if (!grown) {
            const xiaotai_err_t error = go_compact(audio);
            return error == XIAOTAI_OK ? feed_compact(audio, data, size) : error;
        }
        memcpy(grown, audio->data, audio->received);
        xiaotai_mem_free(audio->data);
        audio->data = grown;
        audio->capacity = capacity;
    }
    memcpy(audio->data + audio->received, data, size);
    audio->received += size;
    return XIAOTAI_OK;
}

static int response_code(const audio_transfer_t *audio)
{
    rc_json_t json;
    int code = -1;
    if (rc_json_open(&json, (const char *)audio->data, audio->received, AUDIO_BUFFER_SIZE - 1U) == XIAOTAI_OK) {
        if (rc_json_unique(&json, 0)) (void)rc_json_code(&json, 0, false, &code);
        rc_json_close(&json);
    }
    return code;
}

static void complete(void *ctx, const xiaotai_platform_transfer_result_t *result)
{
    audio_transfer_t *audio = ctx;
    rc_audio_reply_t reply = {.http_status = result->http_status, .code = -1,
        .generation = result->generation, .error = result->result};
    if (reply.error == XIAOTAI_OK) {
        if (!audio->data || !audio->received || audio->received != result->received ||
            audio->status != result->http_status || audio->clipped ||
            (audio->declared >= 0 && (uint64_t)audio->declared != audio->received)) {
            reply.error = RC_INVALID_RESPONSE;
        } else if (audio->status != 200) {
            reply.code = response_code(audio);
            reply.error = RC_INVALID_RESPONSE;
        } else if (audio->compact && !rc_compact_started(audio->converter)) {
            reply.error = RC_INVALID_RESPONSE;
        } else {
            reply.data = audio->data;
            reply.size = audio->compact ? audio->stored : audio->received;
            audio->data = NULL;
        }
    } else if (reply.http_status && reply.http_status != 200 && reply.error == XIAOTAI_ERR_INVALID_SIZE) {
        reply.error = RC_INVALID_RESPONSE;
    }
    rc_audio_reply_fn_t done = audio->done;
    void *const user = audio->ctx;
    xiaotai_mem_free(audio->data);
    xiaotai_mem_free(audio->converter);
    xiaotai_mem_free(audio);
    done(&reply, user);
}

xiaotai_err_t rc_audio_transfer(const char *request_id, const char *item_id,
                                bool (*cancelled)(void *), rc_audio_reply_fn_t done,
                                void *ctx, uint32_t *transfer_id)
{
    if (!rc_id_valid(request_id) || !rc_id_valid(item_id) || !cancelled || !done || !transfer_id)
        return XIAOTAI_ERR_INVALID_ARG;
    const size_t budget = budget_bytes();
    if (budget < AUDIO_MIN_BYTES) return XIAOTAI_ERR_NO_MEM;
    char path[XIAOTAI_PLATFORM_PATH_MAX];
    const int length = snprintf(path, sizeof(path), "/v1/recognition/requests/%s/audio/%s", request_id, item_id);
    if (length < 0 || (size_t)length >= sizeof(path)) return XIAOTAI_ERR_INVALID_SIZE;
    audio_transfer_t *audio = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1U, sizeof(*audio));
    if (!audio) return XIAOTAI_ERR_NO_MEM;
    audio->cancelled = cancelled;
    audio->done = done;
    audio->ctx = ctx;
    audio->budget = budget;
    const xiaotai_platform_transfer_t request = {
        .request = {.method = XIAOTAI_PLATFORM_GET, .service = "recognition", .path = path,
                    .auth = XIAOTAI_PLATFORM_AUTH_SESSION, .timeout_ms = 20000U,
                    .flags = XIAOTAI_PLATFORM_TRANSFER_FULL_TIMEOUT},
        /* Kept as A-law, twice the budget fits in it. */
        .response_max = budget > AUDIO_MAX_BYTES / 2U ? AUDIO_MAX_BYTES : budget * 2U, .stall_timeout_ms = 5000U,
    };
    const xiaotai_platform_stream_t stream = {.begin = begin, .write = write_audio, .done = complete, .ctx = audio};
    const xiaotai_err_t error = xiaotai_platform_transfer(&request, &stream, transfer_id);
    if (error != XIAOTAI_OK) xiaotai_mem_free(audio);
    return error;
}
