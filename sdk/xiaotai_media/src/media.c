// SPDX-License-Identifier: MIT
/* Media startup: seal declarations, prepare supported formats and size the ring. */
#include "xiaotai/media.h"
#include "xiaotai/media_config.h"

#include <stdatomic.h>
#include <string.h>
#include "internal.h"
#include "audio_workers.h"
#include "diag.h"
#include "downlink_ring.h"
#include "preroll.h"
#include "portable_audio.h"
#include "registry.h"
#include "resample.h"
#include "xiaotai/port/log.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/mem.h"

static const char *TAG = "xiaotai_media";

/* Fixed tables: everything is declared during init, before any allocation. */
#define REQUIREMENTS_MAX 16U
#define TUNE_TABLES_MAX 4U
/* A full ring's total audio duration must fit the 32-bit microsecond counter. */
#define DOWNLINK_PACKET_LIMIT_MAX (UINT32_MAX / (1000U * XIAOTAI_MEDIA_PACKET_MS_MAX))
static const char downlink_capacity_packets[] = "downlink.capacity_packets";
static const char downlink_max_queued_ms[] = "downlink.max_queued_ms";

typedef struct {
    const xiaotai_media_param_t *params;
    size_t count;
} tune_table_t;

static xiaotai_media_requirement_t s_needs[REQUIREMENTS_MAX];
static size_t s_need_count;
static tune_table_t s_tunes[TUNE_TABLES_MAX];
static size_t s_tune_count;
static bool s_started;
static atomic_bool s_ready;
static bool s_provisioning;
static xiaotai_downlink_t *s_downlink;
static media_preroll_t *s_history;
static xiaotai_media_info_t s_info;
static xiaotai_processor_info_t s_processor_info;
static const xiaotai_audio_processor_t *s_processor;
static void *s_processor_state;
static const xiaotai_audio_input_t *s_prepared_input;
static media_topology_t s_topology;

typedef struct {
    xiaotai_media_format_t format;
    void *state;
    union {
        const xiaotai_audio_encoder_t *encoder;
        const xiaotai_audio_decoder_t *decoder;
    } codec;
    bool available;  /* encoded input may be passed through without an encoder */
} format_entry_t;

static format_entry_t s_uplink[REQUIREMENTS_MAX], s_rx[REQUIREMENTS_MAX];
static size_t s_uplink_count, s_rx_count;
static const xiaotai_audio_decoder_t *s_capture_decoder;
static void *s_capture_decoder_state;
static size_t stored_packet_bytes(const xiaotai_media_requirement_t *need, uint16_t packet_ms);

bool media_prepared_capture_safe(void)
{
    if (s_history) return false;
    bool available = false;
    for (size_t i = 0; i < s_uplink_count; ++i) {
        if (!s_uplink[i].available) continue;
        const xiaotai_audio_encoder_t *encoder = s_uplink[i].codec.encoder;
        if (!encoder || !encoder->capture_safe) return false;
        available = true;
    }
    return available;
}

bool media_registration_closed(void) { return s_started; }
static bool capabilities_ready(void)
{
    return atomic_load_explicit(&s_ready, memory_order_acquire) && !media_session_runtime_failed();
}

/* ------------------------------------------------------------- requirements */

/* A used direction needs a packet duration and an audio codec; an unused one
 * (rate 0) is not looked at. */
static bool format_malformed(const xiaotai_media_format_t *format, uint16_t fallback_ms)
{
    if (format->rate_hz == 0) return false;
    return (format->packet_ms == 0 && fallback_ms == 0) || !XIAOTAI_CODEC_IS_AUDIO(format->encoding);
}

/* One byte or one word per sample: the downlink entry cuts these into nominal
 * packets (xiaotai/media_format.h); every other encoding is framed. */
static bool sample_encoding(xiaotai_codec_t encoding)
{
    return encoding == XIAOTAI_CODEC_G711A || encoding == XIAOTAI_CODEC_G711U || encoding == XIAOTAI_CODEC_PCM16;
}

xiaotai_err_t xiaotai_media_require(const xiaotai_media_requirement_t *requirement)
{
    if (!requirement || format_malformed(&requirement->uplink, requirement->packet_ms) ||
        format_malformed(&requirement->downlink, requirement->packet_ms) ||
        requirement->max_packet_bytes > XIAOTAI_MEDIA_PACKET_BYTES_MAX) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    /* A framed downlink without any bound would be sized at 1500 bytes a
     * packet: refused everywhere, so the omission shows in host tests. */
    if (requirement->downlink.rate_hz != 0 && !sample_encoding(requirement->downlink.encoding) &&
        requirement->max_packet_bytes == 0 && requirement->max_bitrate_bps == 0) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (s_started) return XIAOTAI_ERR_INVALID_STATE; /* buffers are already sized */
    if (s_need_count == REQUIREMENTS_MAX) return XIAOTAI_ERR_NO_MEM;
    s_needs[s_need_count++] = *requirement;
    return XIAOTAI_OK;
}

xiaotai_downlink_t *xiaotai_media_downlink(void) { return capabilities_ready() ? s_downlink : NULL; }

/* ------------------------------------------------------------------- tuning */

xiaotai_err_t xiaotai_media_tune(const xiaotai_media_param_t *params, size_t count)
{
    if (!params && count) return XIAOTAI_ERR_INVALID_ARG;
    if (s_started) return XIAOTAI_ERR_INVALID_STATE;
    if (!count) return XIAOTAI_OK;
    if (s_tune_count == TUNE_TABLES_MAX) return XIAOTAI_ERR_NO_MEM;
    for (size_t i = 0; i < count; ++i) {
        if (!params[i].name) continue;
        const int32_t value = params[i].value;
        if (strcmp(params[i].name, downlink_capacity_packets) == 0 &&
            value != 0 && (value < 2 || (uint32_t)value > DOWNLINK_PACKET_LIMIT_MAX))
            return XIAOTAI_ERR_INVALID_ARG;
        if (strcmp(params[i].name, downlink_max_queued_ms) == 0 &&
            (value < 0 || (uint32_t)value > UINT32_MAX / 1000U))
            return XIAOTAI_ERR_INVALID_ARG;
        if (strcmp(params[i].name, MEDIA_COMPACT_CAPTURE_STACK_KEY) == 0 && value != 0 && value != 1)
            return XIAOTAI_ERR_INVALID_ARG;
        if (strcmp(params[i].name, MEDIA_UPLINK_MAX_QUEUE_AGE_KEY) == 0 && (value < 0 || value > UINT16_MAX))
            return XIAOTAI_ERR_INVALID_ARG;
    }
    s_tunes[s_tune_count++] = (tune_table_t){.params = params, .count = count};
    return XIAOTAI_OK;
}

int32_t xiaotai_media_param(const char *name, int32_t fallback)
{
    if (!name) return fallback;
    /* Later tables override earlier ones; within a table the last entry wins. */
    for (size_t t = s_tune_count; t-- > 0;) {
        for (size_t i = s_tunes[t].count; i-- > 0;) {
            const xiaotai_media_param_t *param = &s_tunes[t].params[i];
            if (param->name && strcmp(param->name, name) == 0) return param->value;
        }
    }
    return fallback;
}

/* ---------------------------------------------------------- prepared formats */

static bool same_format(const xiaotai_media_format_t *a, const xiaotai_media_format_t *b)
{
    return a->encoding == b->encoding && a->channels == b->channels &&
           a->rate_hz == b->rate_hz && a->packet_ms == b->packet_ms;
}

static xiaotai_media_format_t with_duration(xiaotai_media_format_t format, uint16_t duration)
{
    if (format.packet_ms == 0 && duration <= XIAOTAI_MEDIA_PACKET_MS_MAX) format.packet_ms = (uint8_t)duration;
    return format;
}

static format_entry_t *find_format(format_entry_t *table, size_t count, const xiaotai_media_format_t *format)
{
    if (!format) return NULL;
    for (size_t i = 0; i < count; ++i) {
        if (same_format(&table[i].format, format)) return &table[i];
    }
    return NULL;
}

static bool has_format(format_entry_t *table, size_t count, const xiaotai_media_format_t *format)
{
    const format_entry_t *entry = find_format(table, count, format);
    return entry && entry->available;
}

bool media_rx_format_at(uint8_t slot, media_rx_format_t *prepared)
{
    if (!slot || slot > s_rx_count || !s_rx[slot - 1U].available) return false;
    const format_entry_t *entry = &s_rx[slot - 1U];
    *prepared = (media_rx_format_t){.format = entry->format, .decoder = entry->codec.decoder, .state = entry->state};
    for (size_t i = 0; i < s_need_count; ++i) {
        const xiaotai_media_requirement_t *need = &s_needs[i];
        const xiaotai_media_format_t format = with_duration(need->downlink, need->packet_ms);
        if (!same_format(&format, &entry->format)) continue;
        const size_t bound = stored_packet_bytes(need, format.packet_ms);
        if (bound > prepared->max_bytes) prepared->max_bytes = bound;
    }
    return true;
}

bool media_rx_format_select(const xiaotai_media_format_t *wanted, uint8_t *slot, media_rx_format_t *prepared)
{
    for (size_t i = 0; i < s_rx_count; ++i) {
        if (s_rx[i].available && same_format(&s_rx[i].format, wanted)) {
            *slot = (uint8_t)(i + 1U);
            return media_rx_format_at(*slot, prepared);
        }
    }
    /* A different declared encoding may carry its own nominal duration. */
    for (size_t i = 0; i < s_rx_count; ++i) {
        if (s_rx[i].available && s_rx[i].format.encoding == wanted->encoding &&
            s_rx[i].format.channels == wanted->channels && s_rx[i].format.rate_hz == wanted->rate_hz) {
            *slot = (uint8_t)(i + 1U);
            return media_rx_format_at(*slot, prepared);
        }
    }
    return false;
}

uint32_t media_max_decoder_stack(void)
{
    uint32_t stack = 0;
    for (size_t i = 0; i < s_rx_count; ++i) {
        const xiaotai_audio_decoder_t *decoder = s_rx[i].codec.decoder;
        if (decoder && decoder->stack_bytes > stack) stack = decoder->stack_bytes;
    }
    return stack;
}

uint32_t media_prepared_downlink_ms(void) { return s_info.downlink_ms; }

bool media_tx_codec(const xiaotai_media_format_t *format, media_tx_codec_t *out)
{
    const format_entry_t *entry = find_format(s_uplink, s_uplink_count, format);
    if (!entry || !entry->available) return false;
    *out = (media_tx_codec_t){.encoder = entry->codec.encoder, .state = entry->state};
    return true;
}

uint32_t media_max_encoder_stack(void)
{
    uint32_t stack = 0;
    for (size_t i = 0; i < s_uplink_count; ++i) {
        const xiaotai_audio_encoder_t *encoder = s_uplink[i].codec.encoder;
        if (encoder && encoder->stack_bytes > stack) stack = encoder->stack_bytes;
    }
    return stack;
}

bool media_prepared_uplink(void)
{
    for (size_t i = 0; i < s_uplink_count; ++i) if (s_uplink[i].available) return true;
    return false;
}

bool media_prepared_uplink_needs_resample(uint32_t input_rate_hz)
{
    for (size_t i = 0; i < s_uplink_count; ++i) {
        const format_entry_t *entry = &s_uplink[i];
        if (entry->available && entry->codec.encoder && entry->format.rate_hz != input_rate_hz) return true;
    }
    return false;
}

size_t media_max_uplink_samples(void)
{
    size_t largest = 0;
    for (size_t i = 0; i < s_uplink_count; ++i) {
        const format_entry_t *entry = &s_uplink[i];
        if (!entry->codec.encoder) continue;
        const size_t samples = (size_t)entry->format.rate_hz * entry->format.packet_ms / 1000U;
        if (samples > largest) largest = samples;
    }
    return largest;
}

size_t media_max_uplink_packet(void)
{
    size_t largest = 0;
    for (size_t i = 0; i < s_uplink_count; ++i) {
        const format_entry_t *entry = &s_uplink[i];
        if (!entry->codec.encoder) continue;
        const xiaotai_media_format_t *format = &entry->format;
        size_t bytes = XIAOTAI_MEDIA_PACKET_BYTES_MAX;
        if (sample_encoding(format->encoding)) {
            bytes = (size_t)format->rate_hz * format->packet_ms / 1000U;
            if (format->encoding == XIAOTAI_CODEC_PCM16) bytes *= sizeof(int16_t);
        }
        if (bytes > largest) largest = bytes;
    }
    return largest;
}

const xiaotai_audio_decoder_t *media_capture_decoder(void **state)
{
    *state = s_capture_decoder_state;
    return s_capture_decoder;
}

const xiaotai_audio_processor_t *media_capture_prepared(void **state, xiaotai_processor_info_t *info)
{
    *state = s_processor_state;
    *info = s_processor_info;
    return s_processor;
}

static bool wire_format(const xiaotai_media_format_t *format)
{
    if (format->rate_hz == 0 || format->packet_ms == 0 || format->packet_ms > XIAOTAI_MEDIA_PACKET_MS_MAX ||
        format->channels != 1) return false;
    const uint64_t sample_ms = (uint64_t)format->rate_hz * format->packet_ms;
    if (sample_ms % 1000U) return false; /* fixed nominal packets contain whole samples */
    if (!sample_encoding(format->encoding)) return true;
    const unsigned sample_bytes = format->encoding == XIAOTAI_CODEC_PCM16 ? 2U : 1U;
    return sample_ms / 1000U * sample_bytes <= XIAOTAI_MEDIA_PACKET_BYTES_MAX;
}

static xiaotai_err_t prepare_downlink(const xiaotai_media_format_t *format)
{
    if (!wire_format(format) || !xiaotai_media_resample_plan(format->rate_hz, s_info.playback_rate_hz) ||
        find_format(s_rx, s_rx_count, format)) return XIAOTAI_OK;
    format_entry_t *entry = &s_rx[s_rx_count++];
    *entry = (format_entry_t){.format = *format};
    const xiaotai_audio_decoder_t *decoder = media_select_decoder(format);
    if (!decoder) return XIAOTAI_OK;
    const xiaotai_err_t err = decoder->open(format, &entry->state);
    if (err == XIAOTAI_ERR_NOT_SUPPORTED) return XIAOTAI_OK;
    if (err == XIAOTAI_OK) {
        entry->codec.decoder = decoder;
        entry->available = true;
    }
    return err;
}

static xiaotai_err_t prepare_uplink(const xiaotai_media_format_t *format, const xiaotai_audio_input_t *input)
{
    if (!s_processor || !wire_format(format) || format->packet_ms != 20U ||
        !xiaotai_media_resample_plan(s_info.capture_rate_hz, format->rate_hz) ||
        find_format(s_uplink, s_uplink_count, format)) return XIAOTAI_OK;
    format_entry_t *entry = &s_uplink[s_uplink_count++];
    *entry = (format_entry_t){.format = *format};
    const xiaotai_media_format_t source = {
        .encoding = input->desc.encoding, .rate_hz = s_info.capture_rate_hz,
        .channels = input->desc.channels, .packet_ms = input->desc.frame_ms,
    };
    const bool encoded = (s_processor_info.flags & XIAOTAI_PROCESSOR_ENCODED) != 0;
    if (encoded && same_format(&source, format)) {
        entry->available = true;
        return XIAOTAI_OK;
    }
    const xiaotai_audio_encoder_t *encoder = media_select_encoder(format);
    if (!encoder) return XIAOTAI_OK;
    if (encoded && !s_capture_decoder) {
        const xiaotai_audio_decoder_t *decoder = media_select_decoder(&source);
        if (!decoder) return XIAOTAI_OK;
        const xiaotai_err_t err = decoder->open(&source, &s_capture_decoder_state);
        if (err != XIAOTAI_OK) return err == XIAOTAI_ERR_NOT_SUPPORTED ? XIAOTAI_OK : err;
        s_capture_decoder = decoder;
    }
    const xiaotai_err_t err = encoder->open(format, &entry->state);
    if (err == XIAOTAI_ERR_NOT_SUPPORTED) return XIAOTAI_OK;
    if (err == XIAOTAI_OK) {
        entry->codec.encoder = encoder;
        entry->available = true;
    }
    return err;
}

void xiaotai_media_info(xiaotai_media_info_t *info)
{
    if (info) *info = capabilities_ready() ? s_info : (xiaotai_media_info_t){0};
}

bool xiaotai_media_can_send(const xiaotai_media_format_t *format)
{
    return capabilities_ready() && has_format(s_uplink, s_uplink_count, format);
}

bool xiaotai_media_can_receive(const xiaotai_media_format_t *format)
{
    return capabilities_ready() && has_format(s_rx, s_rx_count, format);
}

xiaotai_err_t media_session_validate(const xiaotai_media_session_t *session)
{
    if (!session || !session->generation) return XIAOTAI_ERR_INVALID_ARG;
    const bool sends = session->uplink.rate_hz != 0;
    const bool receives = session->downlink.rate_hz != 0;
    if ((!sends && !receives) ||
        (sends && (!session->sink || !session->sink->send ||
                   format_malformed(&session->uplink, 0) || !wire_format(&session->uplink))) ||
        (receives && (format_malformed(&session->downlink, 0) || !wire_format(&session->downlink)))) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (!capabilities_ready() || s_provisioning) return XIAOTAI_ERR_INVALID_STATE;
    if ((sends && !xiaotai_media_can_send(&session->uplink)) ||
        (receives && !xiaotai_media_can_receive(&session->downlink))) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (sends && session->sink->send_stack_bytes > media_uplink_sink_stack()) return XIAOTAI_ERR_INVALID_SIZE;
    if (!receives) return XIAOTAI_OK;

    /* The largest ring can belong to a different format. Admission must
     * enforce the declaration for this session, not that ring's capacity. */
    uint32_t max_burst = 0;
    for (size_t i = 0; i < s_need_count; ++i) {
        const xiaotai_media_requirement_t *need = &s_needs[i];
        const xiaotai_media_format_t format = with_duration(need->downlink, need->packet_ms);
        if (same_format(&format, &session->downlink) && need->downlink_burst_ms > max_burst) {
            max_burst = need->downlink_burst_ms;
        }
    }
    return session->downlink_burst_ms > max_burst ? XIAOTAI_ERR_INVALID_SIZE : XIAOTAI_OK;
}

const xiaotai_audio_processor_t *xiaotai_media_processor(void)
{
    return capabilities_ready() ? s_processor : NULL;
}

static bool echo_gate_configured(void)
{
    return xiaotai_media_processor() == &media_echo_gate && (s_processor_info.flags & XIAOTAI_PROCESSOR_GATES);
}

xiaotai_err_t xiaotai_media_echo_gate_set(bool enabled)
{
    if (!xiaotai_on_owner() || !capabilities_ready()) return XIAOTAI_ERR_INVALID_STATE;
    if (!echo_gate_configured()) return XIAOTAI_ERR_NOT_SUPPORTED;
    media_echo_gate_set(s_processor_state, enabled);
    return XIAOTAI_OK;
}

bool xiaotai_media_echo_gate_enabled(void)
{
    return xiaotai_on_owner() && echo_gate_configured() && media_echo_gate_enabled(s_processor_state);
}

void xiaotai_media_processor_stats(xiaotai_processor_stats_t *stats)
{
    if (!stats) return;
    *stats = (xiaotai_processor_stats_t){0};
    const xiaotai_audio_processor_t *processor = xiaotai_media_processor();
    if (processor && processor->stats) processor->stats(s_processor_state, stats);
}

/* ----------------------------------------------------------- downlink sizing */

/* Largest packet one requirement stores, before the entry header. A
 * sample-per-byte or sample-per-word encoding is cut into nominal packets on
 * the way in (xiaotai/media_format.h). A framed encoding stores whole packets:
 * the declared largest, or what the longest decodable packet carries at the
 * declared bitrate. */
static size_t stored_packet_bytes(const xiaotai_media_requirement_t *need, uint16_t packet_ms)
{
    const xiaotai_media_format_t *format = &need->downlink;
    const size_t samples = (size_t)format->rate_hz * packet_ms / 1000U * format->channels;
    if (format->encoding == XIAOTAI_CODEC_G711A || format->encoding == XIAOTAI_CODEC_G711U) return samples;
    if (format->encoding == XIAOTAI_CODEC_PCM16) return samples * 2U;
    if (need->max_packet_bytes) return need->max_packet_bytes;
    const uint64_t bytes = ((uint64_t)need->max_bitrate_bps * XIAOTAI_MEDIA_PACKET_MS_MAX + 7999U) / 8000U;
    if (bytes > XIAOTAI_MEDIA_PACKET_BYTES_MAX) return XIAOTAI_MEDIA_PACKET_BYTES_MAX;
    return bytes ? (size_t)bytes : 1U;
}

/* Ring bytes one requirement needs for packets of up to piece bytes.
 * A framed encoding with a declared bitrate takes the smaller of the two
 * bounds: both hold for every stretch of its audio. */
static uint64_t requirement_bytes(const xiaotai_media_requirement_t *need, uint64_t packets, uint16_t packet_ms,
                                  size_t piece)
{
    const size_t span = XIAOTAI_DOWNLINK_ENTRY_BYTES(piece);
    uint64_t bytes = (packets + 1U) * span;
    if (!sample_encoding(need->downlink.encoding) && need->max_bitrate_bps) {
        const uint64_t payload = (packets * packet_ms * need->max_bitrate_bps + 7999U) / 8000U;
        const uint64_t at_rate = payload + packets * (XIAOTAI_DOWNLINK_ENTRY_HEADER + 3U) + span;
        if (!need->max_packet_bytes || at_rate < bytes) bytes = at_rate;
    }
    return (bytes + 3U) & ~(uint64_t)3U;
}

typedef struct {
    size_t bytes;            /* ring size */
    size_t max_packet_bytes; /* largest stored packet */
    uint32_t hold_ms;        /* audio the largest requirement holds */
    uint32_t packet_limit;   /* explicit bound on buffered entries, zero for automatic */
} downlink_size_t;

static downlink_size_t downlink_size(void)
{
    static const xiaotai_media_latency_t bursty = XIAOTAI_MEDIA_LATENCY_BURSTY;
    int32_t headroom = xiaotai_media_param("downlink.headroom_ms", (int32_t)XIAOTAI_DOWNLINK_HEADROOM_MS);
    if (headroom < 0) headroom = 0;
    const uint32_t packet_limit = (uint32_t)xiaotai_media_param(downlink_capacity_packets, 0);
    downlink_size_t size = {.packet_limit = packet_limit};
    for (size_t i = 0; i < s_need_count; ++i) {
        const xiaotai_media_requirement_t *need = &s_needs[i];
        const xiaotai_media_format_t format = with_duration(need->downlink, need->packet_ms);
        if (!has_format(s_rx, s_rx_count, &format)) continue;
        const uint16_t packet_ms = format.packet_ms;
        const xiaotai_media_latency_t *latency = need->latency.max_ms ? &need->latency : &bursty;
        /* The burst with headroom, and what the pacer may legitimately keep
         * queued before it plays out fast (its highest target plus surplus). */
        const uint64_t burst_ms = (uint64_t)need->downlink_burst_ms + (uint32_t)headroom;
        const uint32_t pacing_ms = (uint32_t)latency->max_ms + latency->surplus_ms;
        const uint64_t hold_ms = burst_ms > pacing_ms ? burst_ms : pacing_ms;
        if (hold_ms > UINT32_MAX - XIAOTAI_MEDIA_PACKET_MS_MAX) return (downlink_size_t){.bytes = SIZE_MAX};
        const uint64_t packets = packet_limit ? packet_limit : (hold_ms + packet_ms - 1U) / packet_ms;
        const size_t piece = stored_packet_bytes(need, packet_ms);
        /* An explicit slot count must also fit when each packet is at its
         * size limit. The extra entry still allows a variable-size wrap. */
        const uint64_t bytes = packet_limit ? (packets + 1U) * XIAOTAI_DOWNLINK_ENTRY_BYTES(piece)
                                            : requirement_bytes(need, packets, packet_ms, piece);
        if (bytes > SIZE_MAX) return (downlink_size_t){.bytes = SIZE_MAX};
        if (bytes > size.bytes) {
            size.bytes = (size_t)bytes;
            size.hold_ms = (uint32_t)(packets * packet_ms);
        }
        if (piece > size.max_packet_bytes) size.max_packet_bytes = piece;
    }
    /* A smaller requirement may still store larger pieces: the ring must take one of them. */
    if (size.bytes && size.bytes < XIAOTAI_DOWNLINK_BYTES(1U, size.max_packet_bytes)) {
        size.bytes = XIAOTAI_DOWNLINK_BYTES(1U, size.max_packet_bytes);
    }
    return size;
}

/* ---------------------------------------------------------------- lifecycle */

static void close_prepared(void)
{
    media_preroll_destroy(s_history);
    s_history = NULL;
    xiaotai_downlink_destroy(s_downlink);
    s_downlink = NULL;
    while (s_rx_count) {
        format_entry_t *entry = &s_rx[--s_rx_count];
        if (entry->codec.decoder) entry->codec.decoder->close(entry->state);
    }
    while (s_uplink_count) {
        format_entry_t *entry = &s_uplink[--s_uplink_count];
        if (entry->codec.encoder) entry->codec.encoder->close(entry->state);
    }
    if (s_capture_decoder) s_capture_decoder->close(s_capture_decoder_state);
    if (s_processor) s_processor->close(s_processor_state);
    s_capture_decoder = NULL;
    s_processor = NULL;
    s_capture_decoder_state = s_processor_state = NULL;
    s_prepared_input = NULL;
    s_topology = MEDIA_TOPOLOGY_SPLIT;
    s_processor_info = (xiaotai_processor_info_t){0};
    s_info = (xiaotai_media_info_t){0};
}

static void prepare_preroll(void)
{
    if (s_provisioning || !s_info.capture_pcm || !xiaotai_mem_has_external()) return;
    uint32_t duration = 0;
    for (size_t i = 0; i < s_need_count; ++i) {
        if (s_needs[i].preroll_ms > duration) duration = s_needs[i].preroll_ms;
    }
    if (!duration) return;
    const xiaotai_err_t err = media_preroll_create(s_info.capture_rate_hz, duration, &s_history);
    if (err != XIAOTAI_OK) {
        XIAOTAI_LOGW(TAG, "pre-roll unavailable: %d", err);
        return;
    }
    s_info.preroll_ms = media_preroll_capacity_ms(s_history);
}

media_preroll_t *media_preroll_current(void)
{
    return atomic_load_explicit(&s_ready, memory_order_acquire) ? s_history : NULL;
}

static xiaotai_err_t prepare_audio(bool provisioning)
{
    const xiaotai_audio_output_t *output = xiaotai_hal_audio_output(0);
    const xiaotai_audio_input_t *input = xiaotai_hal_audio_input(0);
    const bool external = xiaotai_mem_has_external();
    if (provisioning && !media_topology_can_fold_parts(input, output, external)) input = NULL;
    s_info.capture = input != NULL;
    s_info.playback = output != NULL;
    s_info.mixer = output && output->write && xiaotai_media_param("playout.mixer", xiaotai_mem_has_external() ? 1 : 0) != 0;
    s_info.processor = "";
    if (input) s_info.capture_encoding = input->desc.encoding;
    if (output && output->desc.encoding == XIAOTAI_CODEC_PCM16) s_info.playback_rate_hz = output->desc.rate_hz;
    const xiaotai_audio_processor_t *processor =
        media_select_processor(input ? &input->desc : NULL, output ? &output->desc : NULL);
    if (processor) {
        xiaotai_err_t err = processor->open(input, output, &s_processor_info, &s_processor_state);
        if (err != XIAOTAI_OK) return err;
        s_processor = processor;
        const bool encoded = (s_processor_info.flags & XIAOTAI_PROCESSOR_ENCODED) != 0;
        if (!s_processor_info.rate_hz || !s_processor_info.fetch_max ||
            (encoded && (input->desc.encoding == XIAOTAI_CODEC_PCM16 || s_processor_info.feed_frames != 0 ||
                         !XIAOTAI_CODEC_IS_AUDIO(input->desc.encoding) ||
                         s_processor_info.rate_hz != input->desc.rate_hz ||
                         s_processor_info.fetch_max > XIAOTAI_MEDIA_PACKET_BYTES_MAX)) ||
            (!encoded && (input->desc.encoding != XIAOTAI_CODEC_PCM16 || !s_processor_info.feed_frames ||
                          s_processor_info.fetch_max > XIAOTAI_PROCESSOR_SAMPLES_MAX)) ||
            ((s_processor_info.flags & XIAOTAI_PROCESSOR_FAR_END) && !processor->far_end)) {
            return XIAOTAI_ERR_INVALID_STATE;
        }
        s_info.processor = processor->name;
        s_info.capture_pcm = !encoded;
        s_info.capture_rate_hz = s_processor_info.rate_hz;
        s_info.cancels_echo = (s_processor_info.flags & XIAOTAI_PROCESSOR_CANCELS_ECHO) != 0;
    }
    s_topology = media_topology_select(input, output, s_processor ? &s_processor_info : NULL, external);
    if (provisioning && s_topology != MEDIA_TOPOLOGY_FOLDED) {
        if (s_processor) s_processor->close(s_processor_state);
        s_processor = NULL;
        s_processor_state = NULL;
        s_processor_info = (xiaotai_processor_info_t){0};
        s_info.capture = s_info.capture_pcm = s_info.cancels_echo = false;
        s_info.capture_encoding = XIAOTAI_CODEC_NONE;
        s_info.capture_rate_hz = 0;
        s_info.processor = "";
        input = NULL;
    }
    s_prepared_input = input;
    if (provisioning) return XIAOTAI_OK;
    for (size_t i = 0; i < s_need_count; ++i) {
        const xiaotai_media_requirement_t *need = &s_needs[i];
        const xiaotai_media_format_t uplink = with_duration(need->uplink, need->packet_ms);
        const xiaotai_media_format_t downlink = with_duration(need->downlink, need->packet_ms);
        xiaotai_err_t err = prepare_uplink(&uplink, input);
        if (err == XIAOTAI_OK) err = prepare_downlink(&downlink);
        if (err != XIAOTAI_OK) return err;
    }
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_engine_media_init(void)
{
#if XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
    if (xiaotai_media_param(downlink_capacity_packets, 0) <= 0) {
        XIAOTAI_LOGE(TAG, "fixed A-law frames require positive %s", downlink_capacity_packets);
        return XIAOTAI_ERR_INVALID_ARG;
    }
#endif
    const xiaotai_err_t err = media_platform_register();
    if (err != XIAOTAI_OK) return err;
    if (xiaotai_media_param("noise_gate.enabled", 0)) {
        const xiaotai_err_t noise = media_noise_gate_topics_init();
        if (noise != XIAOTAI_OK) return noise;
    }
    /* The product graph declares both owners; stand-alone audio fixtures do
     * not set up an AT console or register providers for unused services. */
    if (xiaotai_module_present("eventlog") && xiaotai_module_present("at")) return media_diag_register();
    return XIAOTAI_OK;
}

void xiaotai_engine_media_start(void)
{
    if (s_started) return;
    s_started = true;
    s_provisioning = media_provisioning_boot();
    xiaotai_err_t err = prepare_audio(s_provisioning);
    if (err != XIAOTAI_OK) {
        XIAOTAI_LOGE(TAG, "prepare audio: %d", err);
        close_prepared();
        media_session_startup_complete(err);
        return;
    }
    const downlink_size_t size = s_provisioning ? (downlink_size_t){0} : downlink_size();
    if (size.bytes) {
        err = size.bytes == SIZE_MAX ? XIAOTAI_ERR_INVALID_SIZE
                                    : xiaotai_downlink_create(size.bytes, size.max_packet_bytes, &s_downlink);
        if (err != XIAOTAI_OK) {
            XIAOTAI_LOGE(TAG, "create downlink: %d", err);
            close_prepared();
            media_session_startup_complete(err);
            return;
        }
        const uint32_t limit_ms = (uint32_t)xiaotai_media_param(downlink_max_queued_ms, 0);
        media_downlink_tagged(s_downlink, xiaotai_media_param("downlink.drop_oldest_when_full", 0) != 0,
                              size.packet_limit, limit_ms * 1000U);
        s_info.downlink_ms = limit_ms && limit_ms < size.hold_ms ? limit_ms : size.hold_ms;
        XIAOTAI_LOGI(TAG, "downlink holds %u ms in %u bytes, packets of up to %u bytes", (unsigned)s_info.downlink_ms,
                     (unsigned)size.bytes, (unsigned)size.max_packet_bytes);
    }
    prepare_preroll();
    err = media_session_runtime_start(s_prepared_input, xiaotai_hal_audio_output(0), s_downlink, s_topology);
    if (err != XIAOTAI_OK) {
        XIAOTAI_LOGE(TAG, "start session task: %d", err);
        close_prepared();
        media_session_startup_complete(err);
        return;
    }
    atomic_store_explicit(&s_ready, true, memory_order_release);
}

void xiaotai_media_reset(void)
{
    if (media_runtime_uplink_is_executor()) {
        XIAOTAI_LOGE(TAG, "cannot reset from the uplink executor");
        return;
    }
    atomic_store_explicit(&s_ready, false, memory_order_release);
    media_session_runtime_stop();
    media_diag_detach();
    media_session_startup_reset();
    close_prepared();
    s_started = false;
    s_provisioning = false;
    s_need_count = s_tune_count = 0;
    media_registry_reset();
}

bool media_stage_initial_controls(void)
{
    return xiaotai_on_owner() && !s_started && xiaotai_module_present("media");
}
