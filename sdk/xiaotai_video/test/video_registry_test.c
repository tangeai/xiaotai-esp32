// SPDX-License-Identifier: MIT
/* Backend registries (xiaotai/video_backend.h): validation, ranking by match
 * score with registration order breaking ties, the `after` fallback of
 * transforms, the capacity build option, tables allocated from BULK by the
 * first registration and freed by reset, refusal after start, and the
 * persistent decoders claimed in init. Built twice: with the default capacity
 * and with CONFIG_XIAOTAI_VIDEO_BACKENDS_MAX=2. */
#include <string.h>
#include "fakes.h"
#include "internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai_mem_host.h"

_Static_assert(XIAOTAI_VIDEO_BACKENDS_MAX == VIDEO_TEST_BACKENDS_MAX,
               "the registry capacity must come from the build option");
_Static_assert(VIDEO_TEST_BACKENDS_MAX + 1 <= FAKE_BACKENDS, "not enough fakes to overfill a table");

static void setup(void)
{
    xiaotai_video_reset();
    fake_backends_reset();
    fake_hal_reset();
    xiaotai_mem_host_clear();
    fake_port_clear();
}

static unsigned allocations(void)
{
    return fake_mem_allocations(XIAOTAI_MEM_BULK) + fake_mem_allocations(XIAOTAI_MEM_INTERNAL) +
           fake_mem_allocations(XIAOTAI_MEM_DMA);
}

/* True when an event with this label was recorded after sequence `after`;
 * *value gets its value. */
static bool logged(uint32_t after, const char *label, int32_t *value)
{
    xiaotai_eventlog_entry_t entries[16];
    const size_t n = xiaotai_eventlog_read(after, entries, 16);
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(entries[i].source, "video") == 0 && strcmp(entries[i].label, label) == 0) {
            if (value) *value = entries[i].value;
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------- validation */

static void test_invalid(void)
{
    setup();
    xiaotai_video_encoder_t encoder = fake_encoders[0];
    xiaotai_video_decoder_t decoder = fake_decoders[0];
    xiaotai_video_transform_t transform = fake_transforms[0];

    CHECK(xiaotai_video_encoder_register(NULL) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_video_decoder_register(NULL) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_video_transform_register(NULL) == XIAOTAI_ERR_INVALID_ARG);

    /* No name, an empty name, and each required function missing. */
    xiaotai_video_encoder_t e = encoder;
    e.name = NULL;
    CHECK(xiaotai_video_encoder_register(&e) == XIAOTAI_ERR_INVALID_ARG);
    e = encoder, e.name = "";
    CHECK(xiaotai_video_encoder_register(&e) == XIAOTAI_ERR_INVALID_ARG);
    e = encoder, e.match = NULL;
    CHECK(xiaotai_video_encoder_register(&e) == XIAOTAI_ERR_INVALID_ARG);
    e = encoder, e.open = NULL;
    CHECK(xiaotai_video_encoder_register(&e) == XIAOTAI_ERR_INVALID_ARG);
    e = encoder, e.encode = NULL;
    CHECK(xiaotai_video_encoder_register(&e) == XIAOTAI_ERR_INVALID_ARG);
    e = encoder, e.close = NULL;
    CHECK(xiaotai_video_encoder_register(&e) == XIAOTAI_ERR_INVALID_ARG);

    xiaotai_video_decoder_t d = decoder;
    d.name = "";
    CHECK(xiaotai_video_decoder_register(&d) == XIAOTAI_ERR_INVALID_ARG);
    d = decoder, d.match = NULL;
    CHECK(xiaotai_video_decoder_register(&d) == XIAOTAI_ERR_INVALID_ARG);
    d = decoder, d.open = NULL;
    CHECK(xiaotai_video_decoder_register(&d) == XIAOTAI_ERR_INVALID_ARG);
    d = decoder, d.decode = NULL;
    CHECK(xiaotai_video_decoder_register(&d) == XIAOTAI_ERR_INVALID_ARG);
    d = decoder, d.close = NULL;
    CHECK(xiaotai_video_decoder_register(&d) == XIAOTAI_ERR_INVALID_ARG);

    xiaotai_video_transform_t t = transform;
    t.name = NULL;
    CHECK(xiaotai_video_transform_register(&t) == XIAOTAI_ERR_INVALID_ARG);
    t = transform, t.match = NULL;
    CHECK(xiaotai_video_transform_register(&t) == XIAOTAI_ERR_INVALID_ARG);
    t = transform, t.limits = NULL;
    CHECK(xiaotai_video_transform_register(&t) == XIAOTAI_ERR_INVALID_ARG);
    t = transform, t.open = NULL;
    CHECK(xiaotai_video_transform_register(&t) == XIAOTAI_ERR_INVALID_ARG);
    t = transform, t.run = NULL;
    CHECK(xiaotai_video_transform_register(&t) == XIAOTAI_ERR_INVALID_ARG);
    t = transform, t.close = NULL;
    CHECK(xiaotai_video_transform_register(&t) == XIAOTAI_ERR_INVALID_ARG);

    /* A refusal allocates nothing and registers nothing. */
    CHECK(allocations() == 0, "a refused registration allocated %u times", allocations());
    for (size_t kind = 0; kind < VIDEO_BACKEND_KINDS; ++kind) CHECK(video_registry_count(kind) == 0);

    /* The optional members may be NULL (fake encoders have no
     * internal_bytes and no reconfigure). */
    CHECK(fake_encoders[0].internal_bytes == NULL && fake_encoders[0].reconfigure == NULL);
    CHECK(xiaotai_video_encoder_register(&fake_encoders[0]) == XIAOTAI_OK);
}

/* ------------------------------------------------------------------ memory */

static void test_tables_in_bulk(void)
{
    setup();
    /* Nothing registered: no table. */
    CHECK(allocations() == 0);

    /* The first registration of each kind allocates that kind's table from
     * BULK, once; INTERNAL and DMA are never touched. */
    CHECK(xiaotai_video_encoder_register(&fake_encoders[0]) == XIAOTAI_OK);
    CHECK(fake_mem_allocations(XIAOTAI_MEM_BULK) == 1, "BULK allocations %u", fake_mem_allocations(XIAOTAI_MEM_BULK));
    CHECK(xiaotai_video_encoder_register(&fake_encoders[1]) == XIAOTAI_OK);
    CHECK(fake_mem_allocations(XIAOTAI_MEM_BULK) == 1, "second encoder allocated");
    CHECK(xiaotai_video_decoder_register(&fake_decoders[0]) == XIAOTAI_OK);
    CHECK(fake_mem_allocations(XIAOTAI_MEM_BULK) == 2);
    CHECK(xiaotai_video_transform_register(&fake_transforms[0]) == XIAOTAI_OK);
    CHECK(fake_mem_allocations(XIAOTAI_MEM_BULK) == 3);
    CHECK(fake_mem_allocations(XIAOTAI_MEM_INTERNAL) == 0 && fake_mem_allocations(XIAOTAI_MEM_DMA) == 0);
    CHECK(xiaotai_mem_host_outstanding() == 3, "tables held: %lld", (long long)xiaotai_mem_host_outstanding());

    /* Lookups allocate nothing. */
    fake_encoder_ctl[0].score = 1;
    const xiaotai_video_encoder_config_t config = {.codec = XIAOTAI_VIDEO_CODEC_JPEG};
    CHECK(xiaotai_video_encoder_find(&config) == &fake_encoders[0]);
    CHECK(fake_mem_allocations(XIAOTAI_MEM_BULK) == 3);

    /* Reset returns every table and forgets every backend. */
    xiaotai_video_reset();
    CHECK(xiaotai_mem_host_outstanding() == 0, "reset left %lld blocks", (long long)xiaotai_mem_host_outstanding());
    for (size_t kind = 0; kind < VIDEO_BACKEND_KINDS; ++kind) CHECK(video_registry_count(kind) == 0);
    CHECK(xiaotai_video_encoder_find(&config) == NULL);

    /* After the reset, the first registration allocates again. */
    CHECK(xiaotai_video_encoder_register(&fake_encoders[0]) == XIAOTAI_OK);
    CHECK(fake_mem_allocations(XIAOTAI_MEM_BULK) == 4);
    CHECK(xiaotai_mem_host_outstanding() == 1);
}

static void test_allocation_failure(void)
{
    setup();
    xiaotai_mem_host_fail_nth(1);
    CHECK(xiaotai_video_transform_register(&fake_transforms[0]) == XIAOTAI_ERR_NO_MEM);
    CHECK(video_registry_count(VIDEO_BACKEND_TRANSFORM) == 0);
    CHECK(xiaotai_mem_host_outstanding() == 0);
    fake_transform_ctl[0].score = 4;
    const xiaotai_video_transform_config_t config = {.in_pixel = XIAOTAI_VIDEO_PIXEL_YUYV422};
    CHECK(xiaotai_video_transform_find(&config, NULL) == NULL);
    /* The next registration tries again and succeeds. */
    CHECK(xiaotai_video_transform_register(&fake_transforms[0]) == XIAOTAI_OK);
    CHECK(xiaotai_video_transform_find(&config, NULL) == &fake_transforms[0]);
    CHECK(xiaotai_mem_host_outstanding() == 1);
}

static void test_capacity(void)
{
    setup();
    for (size_t i = 0; i < VIDEO_TEST_BACKENDS_MAX; ++i) {
        CHECK(xiaotai_video_encoder_register(&fake_encoders[i]) == XIAOTAI_OK, "encoder %zu refused", i);
    }
    CHECK(video_registry_count(VIDEO_BACKEND_ENCODER) == VIDEO_TEST_BACKENDS_MAX);
    /* One more is refused, loudly. */
    CHECK(xiaotai_video_encoder_register(&fake_encoders[VIDEO_TEST_BACKENDS_MAX]) == XIAOTAI_ERR_NO_MEM);
    CHECK(video_registry_count(VIDEO_BACKEND_ENCODER) == VIDEO_TEST_BACKENDS_MAX);
    /* The same descriptor again is no new backend, even with a full table. */
    CHECK(xiaotai_video_encoder_register(&fake_encoders[0]) == XIAOTAI_OK);
    CHECK(video_registry_count(VIDEO_BACKEND_ENCODER) == VIDEO_TEST_BACKENDS_MAX);
    CHECK(fake_mem_allocations(XIAOTAI_MEM_BULK) == 1, "a full table took %u allocations",
          fake_mem_allocations(XIAOTAI_MEM_BULK));

    /* Each kind has its own capacity. */
    for (size_t i = 0; i < VIDEO_TEST_BACKENDS_MAX; ++i) {
        CHECK(xiaotai_video_decoder_register(&fake_decoders[i]) == XIAOTAI_OK);
        CHECK(xiaotai_video_transform_register(&fake_transforms[i]) == XIAOTAI_OK);
    }
    CHECK(xiaotai_video_decoder_register(&fake_decoders[VIDEO_TEST_BACKENDS_MAX]) == XIAOTAI_ERR_NO_MEM);
    CHECK(xiaotai_video_transform_register(&fake_transforms[VIDEO_TEST_BACKENDS_MAX]) == XIAOTAI_ERR_NO_MEM);
}

/* ----------------------------------------------------------------- ranking */

static void test_ranking(void)
{
    setup();
    const xiaotai_video_encoder_config_t h264 = {.codec = XIAOTAI_VIDEO_CODEC_H264,
                                                 .pixel = XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED};
    const xiaotai_video_encoder_config_t jpeg = {.codec = XIAOTAI_VIDEO_CODEC_JPEG,
                                                 .pixel = XIAOTAI_VIDEO_PIXEL_YUYV422};
    /* Two equal scores: the one registered first wins, whatever its index. */
    fake_encoder_ctl[1].score = 9;
    fake_encoder_ctl[2].score = 9;
    CHECK(xiaotai_video_encoder_register(&fake_encoders[2]) == XIAOTAI_OK);
    CHECK(xiaotai_video_encoder_register(&fake_encoders[1]) == XIAOTAI_OK);
    CHECK(xiaotai_video_encoder_find(&h264) == &fake_encoders[2]);
    /* The score is asked per config: a backend that cannot do this one (0)
     * is not chosen. */
    fake_encoder_ctl[2].codec = XIAOTAI_VIDEO_CODEC_JPEG;
    CHECK(xiaotai_video_encoder_find(&h264) == &fake_encoders[1]);
    CHECK(xiaotai_video_encoder_find(&jpeg) == &fake_encoders[2]);
    CHECK(fake_encoder_ctl[1].last_encoder.codec == XIAOTAI_VIDEO_CODEC_JPEG, "match() did not see the config");
    /* Nothing matches: NULL. */
    fake_encoder_ctl[1].score = 0;
    CHECK(xiaotai_video_encoder_find(&h264) == NULL);
    CHECK(xiaotai_video_encoder_find(NULL) == NULL);

#if VIDEO_TEST_BACKENDS_MAX >= 4
    /* A higher score wins over registration order. */
    setup();
    fake_encoder_ctl[0].score = 5;
    fake_encoder_ctl[1].score = 9;
    fake_encoder_ctl[2].score = 9;
    fake_encoder_ctl[3].score = 0;
    for (size_t i = 0; i < 4; ++i) CHECK(xiaotai_video_encoder_register(&fake_encoders[i]) == XIAOTAI_OK);
    CHECK(xiaotai_video_encoder_find(&h264) == &fake_encoders[1]);
    fake_encoder_ctl[1].score = 4;
    CHECK(xiaotai_video_encoder_find(&h264) == &fake_encoders[2]);
#endif

    /* Decoders rank the same way. */
    setup();
    const xiaotai_video_decoder_config_t decode = {.codec = XIAOTAI_VIDEO_CODEC_H264, .max_width = 384,
                                                   .max_height = 256};
    fake_decoder_ctl[0].score = 3;
    fake_decoder_ctl[1].score = 6;
    CHECK(xiaotai_video_decoder_register(&fake_decoders[0]) == XIAOTAI_OK);
    CHECK(xiaotai_video_decoder_register(&fake_decoders[1]) == XIAOTAI_OK);
    CHECK(xiaotai_video_decoder_find(&decode) == &fake_decoders[1]);
    CHECK(fake_decoder_ctl[0].last_decoder.max_width == 384 && fake_decoder_ctl[0].last_decoder.max_height == 256);
    fake_decoder_ctl[1].codec = XIAOTAI_VIDEO_CODEC_JPEG;
    CHECK(xiaotai_video_decoder_find(&decode) == &fake_decoders[0]);
}

static void test_transform_after(void)
{
    const xiaotai_video_transform_config_t config = {.in_pixel = XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED,
                                                     .out_pixel = XIAOTAI_VIDEO_PIXEL_RGB565};
    setup();
    /* Two backends: the fallback chain is best, then the other, then none. */
    fake_transform_ctl[0].score = 3;
    fake_transform_ctl[1].score = 7;
    CHECK(xiaotai_video_transform_register(&fake_transforms[0]) == XIAOTAI_OK);
    CHECK(xiaotai_video_transform_register(&fake_transforms[1]) == XIAOTAI_OK);
    CHECK(xiaotai_video_transform_find(&config, NULL) == &fake_transforms[1]);
    CHECK(xiaotai_video_transform_find(&config, &fake_transforms[1]) == &fake_transforms[0]);
    CHECK(xiaotai_video_transform_find(&config, &fake_transforms[0]) == NULL);
    /* A backend that is not registered has no place in the ranking. */
    CHECK(xiaotai_video_transform_find(&config, &fake_transforms[5]) == NULL);
    /* A registered backend that does not match this config neither. */
    fake_transform_ctl[0].pixel = XIAOTAI_VIDEO_PIXEL_RGB888;
    CHECK(xiaotai_video_transform_find(&config, &fake_transforms[0]) == NULL);
    CHECK(xiaotai_video_transform_find(&config, &fake_transforms[1]) == NULL);
    CHECK(xiaotai_video_transform_find(NULL, NULL) == NULL);

#if VIDEO_TEST_BACKENDS_MAX >= 4
    /* Ties: registration order inside the same score, through `after`. */
    setup();
    fake_transform_ctl[0].score = 3;
    fake_transform_ctl[1].score = 7;
    fake_transform_ctl[2].score = 7;
    fake_transform_ctl[3].score = 0;
    for (size_t i = 0; i < 4; ++i) CHECK(xiaotai_video_transform_register(&fake_transforms[i]) == XIAOTAI_OK);
    const xiaotai_video_transform_t *chain[4] = {NULL};
    size_t n = 0;
    for (const xiaotai_video_transform_t *t = xiaotai_video_transform_find(&config, NULL); t && n < 4;
         t = xiaotai_video_transform_find(&config, t)) {
        chain[n++] = t;
    }
    CHECK(n == 3, "fallback chain has %zu entries", n);
    CHECK(chain[0] == &fake_transforms[1] && chain[1] == &fake_transforms[2] && chain[2] == &fake_transforms[0]);
    /* The non-matching one is never offered, and skipping it gives nothing. */
    CHECK(xiaotai_video_transform_find(&config, &fake_transforms[3]) == NULL);
#endif
}

/* ------------------------------------------------------------ lifecycle */

static void test_after_start(void)
{
    setup();
    CHECK(xiaotai_engine_video_init() == XIAOTAI_OK);
    /* The host port registers no backend. The engine adds its own fallback. */
    CHECK(video_registry_count(VIDEO_BACKEND_ENCODER) == 0 && video_registry_count(VIDEO_BACKEND_DECODER) == 0);
    CHECK(video_registry_count(VIDEO_BACKEND_TRANSFORM) == 1);
    const unsigned before = allocations();
    xiaotai_engine_video_start();
    CHECK(xiaotai_video_encoder_register(&fake_encoders[0]) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_video_decoder_register(&fake_decoders[0]) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_video_transform_register(&fake_transforms[0]) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(allocations() == before, "refused registrations allocated");
    /* Reset makes registration possible again. */
    xiaotai_video_reset();
    CHECK(xiaotai_video_encoder_register(&fake_encoders[0]) == XIAOTAI_OK);
}

static void test_persistent_decoders(void)
{
    setup();
    /* A persistent JPEG decoder is opened once, in init, at its own limit. */
    fake_decoders[0].flags = XIAOTAI_VIDEO_DECODER_PERSISTENT;
    fake_decoders[0].load = XIAOTAI_VIDEO_LOAD_HARDWARE;
    fake_decoder_ctl[0].codec = XIAOTAI_VIDEO_CODEC_JPEG;
    fake_decoder_ctl[0].score = 5;
    /* A persistent H.264 decoder whose claim fails. */
    fake_decoders[1].flags = XIAOTAI_VIDEO_DECODER_PERSISTENT;
    fake_decoder_ctl[1].codec = XIAOTAI_VIDEO_CODEC_H264;
    fake_decoder_ctl[1].score = 5;
    fake_decoder_ctl[1].open_result = XIAOTAI_ERR_NO_MEM;
    CHECK(xiaotai_video_decoder_register(&fake_decoders[0]) == XIAOTAI_OK);
    CHECK(xiaotai_video_decoder_register(&fake_decoders[1]) == XIAOTAI_OK);
    CHECK(fake_decoder_ctl[0].open_calls == 0, "opened before init");

    const uint32_t before = xiaotai_eventlog_latest();
    /* A failed claim does not fail init. */
    CHECK(xiaotai_engine_video_init() == XIAOTAI_OK);
    CHECK(fake_decoder_ctl[0].open_calls == 1);
    CHECK(fake_decoder_ctl[0].last_decoder.codec == XIAOTAI_VIDEO_CODEC_JPEG &&
          fake_decoder_ctl[0].last_decoder.max_width == 0 && fake_decoder_ctl[0].last_decoder.max_height == 0);
    CHECK(video_registry_persistent_state(&fake_decoders[0]) == &fake_decoder_ctl[0]);
    CHECK(fake_decoder_ctl[1].open_calls == 1);
    /* The failed one is gone, and with it its codec from the capabilities. */
    CHECK(video_registry_count(VIDEO_BACKEND_DECODER) == 1);
    const xiaotai_video_decoder_config_t h264 = {.codec = XIAOTAI_VIDEO_CODEC_H264};
    CHECK(xiaotai_video_decoder_find(&h264) == NULL);
    xiaotai_video_capabilities_t caps;
    xiaotai_video_capabilities(&caps);
    CHECK(caps.downlink_codecs == XIAOTAI_VIDEO_CODEC_BIT(XIAOTAI_VIDEO_CODEC_JPEG), "downlink codecs 0x%x",
          (unsigned)caps.downlink_codecs);
    int32_t value = 0;
    CHECK(logged(before, "decoder.claim_failed", &value) && value == XIAOTAI_ERR_NO_MEM);

    /* Start claims nothing twice. */
    xiaotai_engine_video_start();
    CHECK(fake_decoder_ctl[0].open_calls == 1);
    /* Reset closes what was claimed, with the state open() gave. */
    xiaotai_video_reset();
    CHECK(fake_decoder_ctl[0].close_calls == 1 && fake_decoder_ctl[0].state == &fake_decoder_ctl[0]);
    CHECK(fake_decoder_ctl[1].close_calls == 0);
    CHECK(xiaotai_mem_host_outstanding() == 0);

    /* A persistent decoder registered after init (host tests) is claimed at
     * start, before the first session. */
    setup();
    fake_decoders[0].flags = XIAOTAI_VIDEO_DECODER_PERSISTENT;
    fake_decoder_ctl[0].score = 5;
    CHECK(xiaotai_engine_video_init() == XIAOTAI_OK);
    CHECK(xiaotai_video_decoder_register(&fake_decoders[0]) == XIAOTAI_OK);
    CHECK(fake_decoder_ctl[0].open_calls == 0);
    xiaotai_engine_video_start();
    CHECK(fake_decoder_ctl[0].open_calls == 1);
    /* A decoder without the flag is never opened by the registry. */
    CHECK(fake_decoder_ctl[1].open_calls == 0);
    xiaotai_video_reset();
}

int main(void)
{
    CHECK(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    test_invalid();
    test_tables_in_bulk();
    test_allocation_failure();
    test_capacity();
    test_ranking();
    test_transform_after();
    test_after_start();
    test_persistent_decoders();
    xiaotai_video_reset();
    return 0;
}
