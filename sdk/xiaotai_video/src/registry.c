// SPDX-License-Identifier: MIT
/* Backend registries of the video engine (xiaotai/video_backend.h).
 *
 * One table per kind, XIAOTAI_VIDEO_BACKENDS_MAX slots, allocated from
 * XIAOTAI_MEM_BULK by the first registration of that kind and kept until
 * xiaotai_video_reset(): a product without video backends holds no table.
 * Registration happens on the owner task before the engine starts (init()
 * functions, xiaotai_video_port_init()); the engine's tasks are created after
 * it, so lookups from them see the complete tables without a lock. Lookups
 * call the backends' match() and therefore never run under a lock either. */
#include <string.h>
#include "internal.h"
#include "geometry.h"
#include "xiaotai/eventlog.h"

typedef struct {
    const void *backend;
    /* Persistent decoders: the state open() returned in init, and whether
     * the claim was made. */
    void *state;
    bool claimed;
} video_slot_t;

typedef struct {
    video_slot_t *slots;
    size_t count;
} video_table_t;

static video_table_t s_tables[VIDEO_BACKEND_KINDS];
static size_t s_builtin_index = SIZE_MAX;

xiaotai_err_t video_registry_install_builtin(void)
{
    if (s_builtin_index != SIZE_MAX) return XIAOTAI_OK;
    if (s_tables[VIDEO_BACKEND_TRANSFORM].count >= XIAOTAI_VIDEO_BACKENDS_MAX) return XIAOTAI_ERR_NO_MEM;
    s_builtin_index = s_tables[VIDEO_BACKEND_TRANSFORM].count;
    return XIAOTAI_OK;
}

static xiaotai_err_t table_add(video_backend_kind_t kind, const void *backend)
{
    if (video_started()) return XIAOTAI_ERR_INVALID_STATE;
    video_table_t *table = &s_tables[kind];
    for (size_t i = 0; i < table->count; ++i) {
        /* The same descriptor again changes nothing: a second slot would rank
         * it twice and make the fallback order of `after` ambiguous. */
        if (table->slots[i].backend == backend) return XIAOTAI_OK;
    }
    if (video_registry_count(kind) >= (size_t)XIAOTAI_VIDEO_BACKENDS_MAX) return XIAOTAI_ERR_NO_MEM;
    if (!table->slots) {
        table->slots = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, (size_t)XIAOTAI_VIDEO_BACKENDS_MAX, sizeof(video_slot_t));
        if (!table->slots) return XIAOTAI_ERR_NO_MEM;
    }
    table->slots[table->count].backend = backend;
    table->slots[table->count].state = NULL;
    table->slots[table->count].claimed = false;
    ++table->count;
    return XIAOTAI_OK;
}

static bool named(const char *name) { return name && name[0] != '\0'; }

xiaotai_err_t xiaotai_video_encoder_register(const xiaotai_video_encoder_t *encoder)
{
    if (!encoder || !named(encoder->name) || !encoder->match || !encoder->open || !encoder->encode ||
        !encoder->close) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    return table_add(VIDEO_BACKEND_ENCODER, encoder);
}

xiaotai_err_t xiaotai_video_decoder_register(const xiaotai_video_decoder_t *decoder)
{
    if (!decoder || !named(decoder->name) || !decoder->match || !decoder->open ||
        (!decoder->decode && !decoder->decode_configured) ||
        !decoder->close) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    return table_add(VIDEO_BACKEND_DECODER, decoder);
}

xiaotai_err_t xiaotai_video_transform_register(const xiaotai_video_transform_t *transform)
{
    if (!transform || !named(transform->name) || !transform->match || !transform->limits || !transform->open ||
        !transform->run || !transform->close) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    return table_add(VIDEO_BACKEND_TRANSFORM, transform);
}

size_t video_registry_count(video_backend_kind_t kind)
{
    return kind < VIDEO_BACKEND_KINDS
        ? s_tables[kind].count + (s_builtin_index != SIZE_MAX && kind == VIDEO_BACKEND_TRANSFORM) : 0U;
}

static const void *table_at(video_backend_kind_t kind, size_t index)
{
    if (kind == VIDEO_BACKEND_TRANSFORM && s_builtin_index != SIZE_MAX) {
        if (index == s_builtin_index) return &video_transform_sw;
        if (index > s_builtin_index) --index;
    }
    return index < s_tables[kind].count ? s_tables[kind].slots[index].backend : NULL;
}

const xiaotai_video_encoder_t *video_encoder_at(size_t index) { return table_at(VIDEO_BACKEND_ENCODER, index); }
const xiaotai_video_decoder_t *video_decoder_at(size_t index) { return table_at(VIDEO_BACKEND_DECODER, index); }
const xiaotai_video_transform_t *video_transform_at(size_t index) { return table_at(VIDEO_BACKEND_TRANSFORM, index); }

/* Ranking: higher score first, then registration order; a score of 0 is not
 * ranked. Returns the index of the best backend, or with has_after of the
 * best one ranked strictly below (after_score, after_index); SIZE_MAX: none. */
typedef int (*video_score_fn_t)(const void *backend, const void *config);

static size_t best_index(video_backend_kind_t kind, video_score_fn_t score, const void *config, bool has_after,
                         int after_score, size_t after_index)
{
    size_t best = SIZE_MAX;
    int best_score = 0;
    for (size_t i = 0; i < video_registry_count(kind); ++i) {
        const int s = score(table_at(kind, i), config);
        if (s <= 0) continue;
        if (has_after && (s > after_score || (s == after_score && i <= after_index))) continue;
        if (best == SIZE_MAX || s > best_score) {
            best = i;
            best_score = s;
        }
    }
    return best;
}

static int encoder_score(const void *backend, const void *config)
{
    return ((const xiaotai_video_encoder_t *)backend)->match(config);
}

static int decoder_score(const void *backend, const void *config)
{
    return ((const xiaotai_video_decoder_t *)backend)->match(config);
}

static int transform_score(const void *backend, const void *config)
{
    return ((const xiaotai_video_transform_t *)backend)->match(config);
}

const xiaotai_video_encoder_t *xiaotai_video_encoder_find(const xiaotai_video_encoder_config_t *config)
{
    if (!config) return NULL;
    const size_t i = best_index(VIDEO_BACKEND_ENCODER, encoder_score, config, false, 0, 0);
    return i == SIZE_MAX ? NULL : video_encoder_at(i);
}

const xiaotai_video_decoder_t *xiaotai_video_decoder_find(const xiaotai_video_decoder_config_t *config)
{
    if (!config) return NULL;
    const size_t i = best_index(VIDEO_BACKEND_DECODER, decoder_score, config, false, 0, 0);
    return i == SIZE_MAX ? NULL : video_decoder_at(i);
}

const xiaotai_video_transform_t *xiaotai_video_transform_find(const xiaotai_video_transform_config_t *config,
                                                              const xiaotai_video_transform_t *after)
{
    if (!config) return NULL;
    if (!after) {
        const size_t i = best_index(VIDEO_BACKEND_TRANSFORM, transform_score, config, false, 0, 0);
        return i == SIZE_MAX ? NULL : video_transform_at(i);
    }
    for (size_t a = 0; a < video_registry_count(VIDEO_BACKEND_TRANSFORM); ++a) {
        if (video_transform_at(a) != after) continue;
        const int after_score = after->match(config);
        /* A backend that does not match this config has no place in its
         * ranking, so there is nothing to fall back to after it. */
        if (after_score <= 0) return NULL;
        const size_t i = best_index(VIDEO_BACKEND_TRANSFORM, transform_score, config, true, after_score, a);
        return i == SIZE_MAX ? NULL : video_transform_at(i);
    }
    return NULL;   /* not registered: not part of any ranking */
}

/* ------------------------------------------------------ persistent decoders */

static void table_remove(video_backend_kind_t kind, size_t index)
{
    video_table_t *table = &s_tables[kind];
    for (size_t i = index + 1U; i < table->count; ++i) table->slots[i - 1U] = table->slots[i];
    --table->count;
    memset(&table->slots[table->count], 0, sizeof(table->slots[table->count]));
}

void video_registry_claim_persistent(void)
{
    static const xiaotai_video_codec_t codecs[] = {XIAOTAI_VIDEO_CODEC_JPEG, XIAOTAI_VIDEO_CODEC_H264};
    video_table_t *table = &s_tables[VIDEO_BACKEND_DECODER];
    size_t i = 0;
    while (i < table->count) {
        video_slot_t *slot = &table->slots[i];
        const xiaotai_video_decoder_t *decoder = slot->backend;
        if (!(decoder->flags & XIAOTAI_VIDEO_DECODER_PERSISTENT) || slot->claimed) {
            ++i;
            continue;
        }
        xiaotai_err_t err = XIAOTAI_ERR_NOT_SUPPORTED;
        for (size_t c = 0; c < sizeof(codecs) / sizeof(codecs[0]); ++c) {
            const xiaotai_video_decoder_config_t config = {.codec = codecs[c], .max_width = 0, .max_height = 0};
            if (decoder->match(&config) <= 0) continue;
            void *state = NULL;
            err = decoder->open(&config, &state);
            if (err == XIAOTAI_OK) {
                slot->state = state;
                slot->claimed = true;
            }
            break;
        }
        if (err == XIAOTAI_OK) {
            ++i;
            continue;
        }
        /* Video never stops the product from starting: the decoder goes, and
         * with it its codec from the capabilities. */
        xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "decoder.claim_failed", 0, err);
        table_remove(VIDEO_BACKEND_DECODER, i);
    }
}

void *video_registry_persistent_state(const xiaotai_video_decoder_t *decoder)
{
    const video_table_t *table = &s_tables[VIDEO_BACKEND_DECODER];
    for (size_t i = 0; i < table->count; ++i) {
        if (table->slots[i].backend == decoder) return table->slots[i].claimed ? table->slots[i].state : NULL;
    }
    return NULL;
}

void video_registry_reset(void)
{
    s_builtin_index = SIZE_MAX;
    video_table_t *decoders = &s_tables[VIDEO_BACKEND_DECODER];
    for (size_t i = 0; i < decoders->count; ++i) {
        if (!decoders->slots[i].claimed) continue;
        const xiaotai_video_decoder_t *decoder = decoders->slots[i].backend;
        decoder->close(decoders->slots[i].state);
    }
    for (size_t kind = 0; kind < VIDEO_BACKEND_KINDS; ++kind) {
        xiaotai_mem_free(s_tables[kind].slots);
        s_tables[kind].slots = NULL;
        s_tables[kind].count = 0;
    }
}
