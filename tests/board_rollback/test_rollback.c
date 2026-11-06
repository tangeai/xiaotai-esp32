#include <stdio.h>
#include "board_host.h"
#include BOARD_SOURCE

static void finish(void)
{
    fail_step = 0;
    release_display();
    release_codecs();
    release_prepare();
    assert(live == 0 && spi_live == 0);
    s_prepared = s_audio_ready = s_display_ready = s_cleanup_failed = false;
    s_backlight_configured = false;
    steps = 0;
}

int main(void)
{
    assert(lck_audio_prepare() == ESP_OK);
    int prepare_steps = steps;
    finish();
    for (int n = 1; n <= prepare_steps; ++n) {
        fail_step = n;
        assert(lck_audio_prepare() != ESP_OK);
        assert(!s_prepared && !s_cleanup_failed && live == 0);
        fail_step = 0;
        assert(lck_audio_prepare() == ESP_OK);
        finish();
    }
    assert(lck_audio_prepare() == ESP_OK);
    steps = 0;
    assert(lck_audio_open() == ESP_OK);
    int audio_steps = steps;
    finish();
    for (int n = 1; n <= audio_steps; ++n) {
        assert(lck_audio_prepare() == ESP_OK);
        int prepared_live = live;
        steps = 0; fail_step = n;
        assert(lck_audio_open() != ESP_OK);
        assert(!s_audio_ready && !s_cleanup_failed && live == prepared_live);
        fail_step = 0;
        assert(lck_audio_open() == ESP_OK);
        finish();
    }
    assert(lck_audio_prepare() == ESP_OK);
    steps = 0;
    assert(lck_display_open() == ESP_OK);
    int display_steps = steps;
    finish();
    for (int n = 1; n <= display_steps; ++n) {
        assert(lck_audio_prepare() == ESP_OK);
        int prepared_live = live;
        steps = 0; fail_step = n;
        assert(lck_display_open() != ESP_OK);
        assert(!s_display_ready && !s_cleanup_failed && live == prepared_live && !spi_live);
        fail_step = 0;
        assert(lck_display_open() == ESP_OK);
        finish();
    }
    fail_step = prepare_steps;
    cleanup_fail = 1;
    assert(lck_audio_prepare() != ESP_OK);
    assert(s_cleanup_failed && s_i2s_rx && live == 1);
    assert(lck_audio_prepare() == ESP_ERR_INVALID_STATE);
    assert(lck_audio_open() == ESP_ERR_INVALID_STATE);
    assert(lck_display_open() == ESP_ERR_INVALID_STATE);
    finish();
    printf("S3 rollback: %d prepare, %d audio, %d display failure points and retained-handle cleanup failure passed\n",
           prepare_steps, audio_steps, display_steps);
    return 0;
}
