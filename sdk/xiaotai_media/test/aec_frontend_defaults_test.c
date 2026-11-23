// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "esp_afe_sr_models.h"
#include "processors/frontend_pairing.h"
#include "xiaotai/media.h"
#include "xiaotai_mem_host.h"

extern const xiaotai_audio_processor_t media_aec_single_frontend;

struct esp_afe_sr_data { int feed, fetch; };
static esp_afe_sr_data_t geometry;
static afe_config_t vendor;
static unsigned opened, closed, pair_calls;
static int observed_core;
static media_frontend_pairing_config_t seen;

static esp_afe_sr_data_t *create(afe_config_t *config)
{
    assert(config == &vendor && opened == closed);
    observed_core = config->afe_perferred_core;
    ++opened;
    return &geometry;
}

static void destroy(esp_afe_sr_data_t *data)
{
    assert(data == &geometry && opened == closed + 1);
    ++closed;
}

static int feed_size(esp_afe_sr_data_t *data) { return data->feed; }
static int fetch_size(esp_afe_sr_data_t *data) { return data->fetch; }
static int feed_channels(esp_afe_sr_data_t *data) { (void)data; return 2; }
static int fetch_channels(esp_afe_sr_data_t *data) { (void)data; return 1; }
static int rate(esp_afe_sr_data_t *data) { (void)data; return 16000; }
static int no_feed(esp_afe_sr_data_t *data, const int16_t *pcm) { (void)data; (void)pcm; return 0; }

static afe_fetch_result_t *no_fetch(esp_afe_sr_data_t *data, TickType_t ticks)
{
    (void)data;
    (void)ticks;
    return NULL;
}

const esp_afe_sr_iface_t *esp_afe_handle_from_config(const afe_config_t *config)
{
    assert(config == &vendor);
    static const esp_afe_sr_iface_t iface = {
        .create_from_config = create, .feed = no_feed, .fetch_with_delay = no_fetch,
        .destroy = destroy, .get_feed_chunksize = feed_size,
        .get_fetch_chunksize = fetch_size, .get_feed_channel_num = feed_channels,
        .get_fetch_channel_num = fetch_channels, .get_samp_rate = rate,
    };
    return &iface;
}

afe_config_t *afe_config_init(const char *format, void *models, int type, int mode)
{
    assert(!strcmp(format, "MR") && !models && type == AFE_TYPE_FD && mode == AFE_MODE_LOW_COST);
    vendor = (afe_config_t){.pcm_config = {.ref_num = 1, .mic_num = 1, .total_ch_num = 2}};
    return &vendor;
}

void afe_config_check(afe_config_t *config)
{
    assert(config == &vendor && config->aec_init && !config->se_init && config->afe_ringbuf_size == 17);
    assert(config->memory_alloc_mode == AFE_MEMORY_ALLOC_MORE_PSRAM);
}

void afe_config_free(afe_config_t *config) { assert(config == &vendor); }

xiaotai_err_t __real_media_frontend_pairing_open(const media_frontend_pairing_config_t *config,
                                               const media_frontend_ops_t *ops, media_frontend_pairing_t **out);
xiaotai_err_t __wrap_media_frontend_pairing_open(const media_frontend_pairing_config_t *config,
                                               const media_frontend_ops_t *ops, media_frontend_pairing_t **out)
{
    ++pair_calls;
    seen = *config;
    return __real_media_frontend_pairing_open(config, ops, out);
}

static void check(const xiaotai_media_param_t *explicit_tune, size_t count, int feed, int fetch,
                  uint8_t slots, bool strict, bool count_monitor, int core)
{
    /* This is the capture slot layout offered by the product, before the
     * adapter maps it to one vendor mic and one playback reference. */
    static const xiaotai_audio_input_t input = {.desc = {.rate_hz = 16000, .channels = 4,
        .encoding = XIAOTAI_CODEC_PCM16,
        .roles = {XIAOTAI_CHANNEL_MIC, XIAOTAI_CHANNEL_REFERENCE, XIAOTAI_CHANNEL_MONITOR, XIAOTAI_CHANNEL_UNUSED}}};
    static const xiaotai_media_param_t shared[] = {
        {"aec.ring_frames", 17}, {"aec.stamp_wait_ms", 20},
    };
    xiaotai_media_reset();
    assert(xiaotai_media_tune(shared, sizeof(shared) / sizeof(shared[0])) == XIAOTAI_OK);
    assert(xiaotai_media_tune(explicit_tune, count) == XIAOTAI_OK);
    geometry = (esp_afe_sr_data_t){feed, fetch};
    observed_core = -1;
    const unsigned calls = pair_calls;
    xiaotai_processor_info_t info = {0};
    void *state = NULL;
    assert(media_aec_single_frontend.match(&input.desc, NULL) == 2);
    const xiaotai_err_t result = media_aec_single_frontend.open(&input, NULL, &info, &state);
    assert(observed_core == core);
    if (!slots) {
        assert(result == XIAOTAI_ERR_NOT_SUPPORTED && !state && pair_calls == calls);
    } else {
        assert(result == XIAOTAI_OK && state && pair_calls == calls + 1);
        assert(seen.input == &input.desc && seen.microphones == 1);
        assert(seen.feed_samples == feed && seen.fetch_samples == fetch);
        assert(seen.ring_frames == 17 && seen.stamp_wait_ms == 20);
        assert(seen.input_slots == slots && seen.strict_feed == strict && seen.count_monitor_clipping == count_monitor);
        assert(info.feed_frames == feed && info.fetch_max == fetch && info.rate_hz == 16000);
        media_aec_single_frontend.close(state);
    }
    assert(opened == closed);
    assert(xiaotai_mem_host_outstanding() == 0);
}

int main(void)
{
    static const xiaotai_media_param_t former[] = {{"aec.strict_feed", 0}, {"aec.input_slots", 0},
        {"aec.count_monitor_clipping", 0}, {"aec.task_core", 0}};
    static const xiaotai_media_param_t override[] = {{"aec.strict_feed", 1}, {"aec.input_slots", 4},
        {"aec.count_monitor_clipping", 1}};
    static const xiaotai_media_param_t core_override[] = {{"aec.task_core", 1}};
    for (unsigned configured = 0; configured < 2; ++configured) {
        const xiaotai_media_param_t *params = configured ? former : NULL;
        const size_t count = configured ? sizeof(former) / sizeof(former[0]) : 0;
        check(params, count, 512, 512, 8, false, false, 0);
        check(params, count, 1024, 512, 4, false, false, 0);
        check(params, count, 256, 256, 0, false, false, 0);
    }
    check(override, sizeof(override) / sizeof(override[0]), 512, 512, 4, true, true, 0);
    check(core_override, sizeof(core_override) / sizeof(core_override[0]), 512, 512, 8, false, false, 1);
    assert(opened == 8 && pair_calls == 6);
    puts("aec frontend: omitted and explicit zero match; nonzero overrides still reach vendor and pairing");
    return 0;
}
