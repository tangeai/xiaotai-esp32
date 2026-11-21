// SPDX-License-Identifier: MIT
#include "trace.h"

#if XIAOTAI_MEDIA_TRACE
#include <inttypes.h>
#include <stdatomic.h>

#include "xiaotai/at_command.h"
#include "xiaotai/at.h"
#include "xiaotai/media.h"
#include "xiaotai/media_format.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

enum { TX, RX, DIRECTIONS };

typedef struct {
    uint32_t fnv;
    uint16_t t_ms, length;
    uint8_t level, pre, flags, duration_ms;
} frame_t;

typedef struct {
    frame_t frame;
    uint32_t sender_ts_ms;
} received_t;

_Static_assert(sizeof(frame_t) == 12 && sizeof(received_t) == 16, "trace bytes per frame");

typedef struct {
    frame_t *tx;
    received_t *rx;
    uint32_t count[DIRECTIONS], capacity;
    uint32_t readers;
    bool retired;
} trace_buffer_t;

typedef struct {
    trace_buffer_t *buffer;
    uint32_t count[DIRECTIONS];
    uint32_t index;
    uint32_t skipped;
    unsigned direction;
    bool header, active, idle_only;
} trace_dump_t;

static xiaotai_mutex_t s_direction[DIRECTIONS];
static bool s_initialized;
static atomic_bool s_armed;
static bool s_ingress;
static atomic_uint s_skipped;
static trace_buffer_t *s_buffer;

#if XIAOTAI_MEDIA_TRACE_ARRIVALS
enum { ARRIVAL_LIMIT = 1024 };

typedef struct {
    uint32_t arrival_ms, source_ms, length;
    uint8_t toc, frame_count;
} arrival_t;

_Static_assert(sizeof(arrival_t) == 16, "legacy arrival budget");

typedef struct {
    uint32_t generation, epoch, count, next;
    bool header;
} arrival_dump_t;

static xiaotai_critical_t s_arrival_lock;
static arrival_t *s_arrivals;
static atomic_bool s_arrival_ready;
static atomic_uint s_arrival_capture;
static uint32_t s_arrival_generation, s_arrival_epoch, s_arrival_count;

void media_trace_session_open(uint32_t generation, uint32_t flags)
{
    if (!atomic_load(&s_arrival_ready)) return;
    xiaotai_critical_enter(&s_arrival_lock);
    if (flags & XIAOTAI_MEDIA_SESSION_TRACE_ARRIVALS) {
        s_arrival_generation = generation;
        s_arrival_count = 0;
        ++s_arrival_epoch;
        atomic_store(&s_arrival_capture, generation);
    } else {
        atomic_store(&s_arrival_capture, 0);
    }
    xiaotai_critical_exit(&s_arrival_lock);
}

void media_trace_session_close(uint32_t generation)
{
    if (!atomic_load(&s_arrival_ready)) return;
    xiaotai_critical_enter(&s_arrival_lock);
    if (atomic_load(&s_arrival_capture) == generation) atomic_store(&s_arrival_capture, 0);
    xiaotai_critical_exit(&s_arrival_lock);
}

void media_trace_arrival(uint32_t generation, uint32_t local_ms, const xiaotai_media_packet_t *packet)
{
    if (!generation || atomic_load(&s_arrival_capture) != generation ||
        !packet || !packet->data || !packet->size || packet->size > XIAOTAI_MEDIA_PACKET_BYTES_MAX)
        return;
    const uint8_t toc = packet->data[0];
    const arrival_t row = {.arrival_ms = local_ms, .source_ms = packet->timestamp_ms,
        .length = (uint32_t)packet->size, .toc = toc,
        .frame_count = (toc & 3U) == 3U && packet->size > 1U ? packet->data[1] & 0x3FU : 0};
    xiaotai_critical_enter(&s_arrival_lock);
    if (s_arrivals && atomic_load(&s_arrival_capture) == generation && s_arrival_count < ARRIVAL_LIMIT)
        s_arrivals[s_arrival_count++] = row;
    xiaotai_critical_exit(&s_arrival_lock);
}

static bool arrivals_busy(void)
{
    xiaotai_media_status_t status;
    xiaotai_media_status(&status);
    return status.generation != 0;
}

static bool dump_arrival(void *ctx, const xiaotai_reply_t *reply)
{
    arrival_dump_t *dump = ctx;
    if (arrivals_busy()) {
        (void)xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_STATE, "AUDIO_TRACE_BUSY");
        return false;
    }
    arrival_t row;
    xiaotai_critical_enter(&s_arrival_lock);
    const bool valid = s_arrivals && s_arrival_epoch == dump->epoch && dump->count <= s_arrival_count;
    const bool have_row = valid && dump->next < dump->count;
    if (have_row) row = s_arrivals[dump->next];
    xiaotai_critical_exit(&s_arrival_lock);
    if (!valid) {
        (void)xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_STATE, "AUDIO_TRACE_BUSY");
        return false;
    }
    if (!dump->header) {
        xiaotai_at_printf(reply, "+ATRACE:BEGIN,g=%" PRIu32 ",count=%" PRIu32, dump->generation, dump->count);
        dump->header = true;
    } else if (have_row) {
        xiaotai_at_printf(reply, "+ATRACE:%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%u,%u",
                          dump->next++, row.arrival_ms, row.source_ms, row.length,
                          (unsigned)row.toc, (unsigned)row.frame_count);
    } else {
        xiaotai_at_printf(reply, "+ATRACE:END");
        return false;
    }
    return true;
}

static xiaotai_err_t xiaotai_media_trace_arrivals_query(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argv;
    if (argc != 1) return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "ARGUMENT_COUNT");
    if (arrivals_busy()) return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_STATE, "AUDIO_TRACE_BUSY");
    arrival_dump_t *dump = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*dump));
    if (!dump) return XIAOTAI_ERR_NO_MEM;
    xiaotai_critical_enter(&s_arrival_lock);
    dump->generation = s_arrival_generation;
    dump->count = s_arrival_count;
    dump->epoch = s_arrival_epoch;
    xiaotai_critical_exit(&s_arrival_lock);
    const xiaotai_at_stream_t stream = {.next = dump_arrival, .release = xiaotai_mem_free, .ctx = dump};
    const xiaotai_err_t err = xiaotai_at_stream(reply, &stream);
    if (err != XIAOTAI_OK) xiaotai_mem_free(dump);
    return err;
}
#endif

static void lock_both(void)
{
    (void)xiaotai_mutex_lock(&s_direction[TX], XIAOTAI_WAIT_FOREVER);
    (void)xiaotai_mutex_lock(&s_direction[RX], XIAOTAI_WAIT_FOREVER);
}

static void unlock_both(void)
{
    xiaotai_mutex_unlock(&s_direction[RX]);
    xiaotai_mutex_unlock(&s_direction[TX]);
}

static void free_buffer(trace_buffer_t *buffer)
{
    if (!buffer) return;
    xiaotai_mem_free(buffer->rx);
    xiaotai_mem_free(buffer->tx);
    xiaotai_mem_free(buffer);
}

static void replace(trace_buffer_t *next)
{
    lock_both();
    trace_buffer_t *old = s_buffer;
    s_buffer = next;
    atomic_store(&s_armed, next != NULL);
    atomic_store(&s_skipped, 0);
    if (old) {
        old->retired = true;
        if (old->readers) old = NULL;
    }
    unlock_both();
    free_buffer(old);
}

void media_trace_reset(void)
{
    if (!s_initialized) return;
    replace(NULL);
#if XIAOTAI_MEDIA_TRACE_ARRIVALS
    atomic_store(&s_arrival_ready, false);
    xiaotai_critical_enter(&s_arrival_lock);
    arrival_t *arrivals = s_arrivals;
    s_arrivals = NULL;
    atomic_store(&s_arrival_capture, 0);
    s_arrival_generation = s_arrival_count = 0;
    ++s_arrival_epoch;
    xiaotai_critical_exit(&s_arrival_lock);
    xiaotai_mem_free(arrivals);
#endif
}

bool media_trace_active(void)
{
    return atomic_load_explicit(&s_armed, memory_order_acquire);
}

bool media_trace_rx_ingress(void) { return s_ingress; }

uint8_t media_trace_db(const int16_t *pcm, size_t samples)
{
    if (!pcm || !samples) return 0;
    int64_t sum = 0;
    uint64_t squares = 0;
    for (size_t i = 0; i < samples; ++i) {
        const int32_t sample = pcm[i];
        sum += sample;
        squares += (uint32_t)(sample * sample);
    }
    const int64_t mean = sum / (int64_t)samples;
    const uint64_t power = squares / samples - (uint64_t)(mean * mean);
    if (!power) return 0;
    // Approximate 10*log10(power) with the legacy 4-bit log2 fraction.
    const unsigned msb = 63U - (unsigned)__builtin_clzll(power);
    const unsigned frac = msb >= 4 ? (unsigned)(power >> (msb - 4)) & 15U :
                                               (unsigned)(power << (4 - msb)) & 15U;
    return (uint8_t)(((msb * 16U + frac) * 193U) >> 10);
}

static uint32_t payload_fnv(const uint8_t *payload, size_t size)
{
    if (!payload || !size) return 0;
    uint32_t h = UINT32_C(2166136261);
    for (size_t i = 0; i < size; ++i) h = (h ^ payload[i]) * UINT32_C(16777619);
    return h;
}

void media_trace_frame(bool uplink, uint32_t local_ms, const uint8_t *payload, size_t size,
                       uint8_t level, uint8_t pre, uint8_t flags, uint32_t sender_ts_ms, uint16_t duration_ms)
{
    if (!atomic_load_explicit(&s_armed, memory_order_acquire)) return;
    const unsigned dir = uplink ? TX : RX;
    if (!xiaotai_mutex_lock(&s_direction[dir], 0)) {
        atomic_fetch_add_explicit(&s_skipped, 1, memory_order_relaxed);
        return;
    }
    trace_buffer_t *buffer = s_buffer;
    if (atomic_load(&s_armed) && buffer && buffer->count[dir] < buffer->capacity) {
        const frame_t record = {
            .fnv = size <= XIAOTAI_MEDIA_PACKET_BYTES_MAX ? payload_fnv(payload, size) : 0,
            .t_ms = (uint16_t)local_ms,
            .length = size <= UINT16_MAX ? (uint16_t)size : UINT16_MAX,
            .level = level, .pre = pre, .flags = flags,
            .duration_ms = duration_ms <= UINT8_MAX ? (uint8_t)duration_ms : 0,
        };
        const uint32_t index = buffer->count[dir]++;
        if (uplink) buffer->tx[index] = record;
        else buffer->rx[index] = (received_t){record, sender_ts_ms};
    }
    xiaotai_mutex_unlock(&s_direction[dir]);
}

static xiaotai_err_t xiaotai_media_trace_arm(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    const bool external = xiaotai_mem_has_external();
    uint32_t capacity = external ? 3000 : 750;
    if (argc && !xiaotai_at_parse_u32(argv[0], 50, external ? 30000 : 3000, &capacity)) {
        return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "FRAME_COUNT");
    }
    // Do not hold two full board-sized traces just to replace a previous one.
    replace(NULL);
    trace_buffer_t *buffer = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*buffer));
    if (!buffer) return XIAOTAI_ERR_NO_MEM;
    buffer->capacity = capacity;
    buffer->tx = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, capacity * sizeof(*buffer->tx));
    buffer->rx = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, capacity * sizeof(*buffer->rx));
    if (!buffer->tx || !buffer->rx) {
        free_buffer(buffer);
        return XIAOTAI_ERR_NO_MEM;
    }
    replace(buffer);
    return XIAOTAI_OK;
}

static xiaotai_err_t xiaotai_media_trace_off(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc; (void)argv; (void)reply;
    replace(NULL);
    return XIAOTAI_OK;
}

static bool dump_next(void *ctx, const xiaotai_reply_t *reply)
{
    trace_dump_t *dump = ctx;
    if (dump->idle_only) {
        xiaotai_media_status_t status;
        xiaotai_media_status(&status);
        if (status.generation) {
            (void)xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_STATE, "ACTIVITY");
            return false;
        }
    }
    const trace_buffer_t *buffer = dump->buffer;
    if (!dump->header) {
        xiaotai_at_printf(reply, "+TRACE: active=%u tx=%" PRIu32 " rx=%" PRIu32 " capacity=%" PRIu32
                          " skipped=%" PRIu32, (unsigned)dump->active, dump->count[TX], dump->count[RX],
                          buffer ? buffer->capacity : 0, dump->skipped);
        dump->header = true;
    } else {
        const unsigned dir = dump->direction;
        const uint32_t index = dump->index++;
        const frame_t *r = dir == TX ? &buffer->tx[index] : &buffer->rx[index].frame;
        if (dir == TX) {
            xiaotai_at_printf(reply, "+TRACE:%u,%" PRIu32 ",%u,%u,%08" PRIx32 ",%u,%u,%u", dir, index,
                              (unsigned)r->t_ms, (unsigned)r->length, r->fnv,
                              (unsigned)r->level, (unsigned)r->pre, (unsigned)r->flags);
        } else {
            xiaotai_at_printf(reply, "+TRACE:%u,%" PRIu32 ",%u,%u,%08" PRIx32 ",%u,%u,%u,%" PRIu32 ",%u", dir, index,
                              (unsigned)r->t_ms, (unsigned)r->length, r->fnv,
                              (unsigned)r->level, (unsigned)r->pre, (unsigned)r->flags,
                              buffer->rx[index].sender_ts_ms, (unsigned)r->duration_ms);
        }
    }
    if (!buffer) return false;
    while (dump->direction < DIRECTIONS && dump->index >= dump->count[dump->direction]) {
        ++dump->direction;
        dump->index = 0;
    }
    return dump->direction < DIRECTIONS;
}

static void dump_release(void *ctx)
{
    trace_dump_t *dump = ctx;
    trace_buffer_t *freeable = NULL;
    lock_both();
    if (dump->buffer && --dump->buffer->readers == 0 && dump->buffer->retired) freeable = dump->buffer;
    unlock_both();
    free_buffer(freeable);
    xiaotai_mem_free(dump);
}

static xiaotai_err_t xiaotai_media_trace_dump(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc; (void)argv;
    if (!xiaotai_at_reply_is_console(reply)) return XIAOTAI_ERR_NOT_SUPPORTED;
    const bool idle_only = xiaotai_media_param("diagnostics.trace_dump_idle_only", 0) != 0;
    if (idle_only) {
        xiaotai_media_status_t status;
        xiaotai_media_status(&status);
        if (status.generation) return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_STATE, "ACTIVITY");
    }
    trace_dump_t *dump = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*dump));
    if (!dump) return XIAOTAI_ERR_NO_MEM;
    const bool stop_on_dump = xiaotai_media_param("diagnostics.trace_stop_on_dump", 0) != 0;
    lock_both();
    if (stop_on_dump) atomic_store(&s_armed, false);
    dump->active = atomic_load(&s_armed);
    dump->idle_only = idle_only;
    dump->buffer = s_buffer;
    dump->skipped = atomic_load(&s_skipped);
    if (dump->buffer) {
        ++dump->buffer->readers;
        for (unsigned dir = 0; dir < DIRECTIONS; ++dir) dump->count[dir] = dump->buffer->count[dir];
    }
    unlock_both();
    const xiaotai_at_stream_t stream = {.next = dump_next, .release = dump_release, .ctx = dump};
    const xiaotai_err_t err = xiaotai_at_stream(reply, &stream);
    if (err != XIAOTAI_OK) dump_release(dump);
    return err;
}

xiaotai_err_t media_trace_register(void)
{
    static const xiaotai_at_verb_t verbs[] = {
        {.word = "ARM", .max_args = 1, .run = xiaotai_media_trace_arm, .usage = "[<50..limit>]"},
        {.word = "DUMP", .run = xiaotai_media_trace_dump},
        {.word = "OFF", .run = xiaotai_media_trace_off},
    };
    if (!s_initialized) {
        xiaotai_err_t err = xiaotai_mutex_init(&s_direction[TX]);
        if (err != XIAOTAI_OK) return err;
        err = xiaotai_mutex_init(&s_direction[RX]);
        if (err != XIAOTAI_OK) {
            xiaotai_mutex_deinit(&s_direction[TX]);
            return err;
        }
#if XIAOTAI_MEDIA_TRACE_ARRIVALS
        xiaotai_critical_init(&s_arrival_lock);
#endif
        s_initialized = true;
    }
    s_ingress = xiaotai_media_param("diagnostics.trace_at_ingress", 0) != 0;
    xiaotai_err_t err = xiaotai_at_verbs_register("AT+TRACE", verbs, sizeof(verbs) / sizeof(*verbs));
#if XIAOTAI_MEDIA_TRACE_ARRIVALS
    if (err == XIAOTAI_OK && !s_arrivals) {
        if (!xiaotai_mem_has_external()) return XIAOTAI_ERR_NOT_SUPPORTED;
        s_arrivals = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, ARRIVAL_LIMIT * sizeof(*s_arrivals));
        if (!s_arrivals) return XIAOTAI_ERR_NO_MEM;
        atomic_store(&s_arrival_ready, true);
    }
    if (err == XIAOTAI_OK) err = xiaotai_at_local_register("AT+AUDIOTRACE?", NULL, xiaotai_media_trace_arrivals_query);
#endif
    return err;
}
#endif
