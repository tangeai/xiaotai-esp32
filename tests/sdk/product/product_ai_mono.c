// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_ui.c::ui
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mono_canvas.h"
#include "panel_fake.h"
#include "product_ai.h"
#include "product_platform.h"
#include "product_test.h"
#include "xiaotai/apps/companion.h"
#include "xiaotai/interaction.h"
#include "xiaotai/media.h"
#include "xiaotai/modules/ai_talk.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/os.h"
#include "xiaotai/ui.h"
#include "xiaotai_transport_host.h"

#define WAIT_MS 5000U
#define RIGHT_X 88U
#define WIDTH 128U

typedef struct {
    xiaotai_ui_info_t ui;
    xiaotai_ai_talk_status_t ai;
    uint8_t header[2][WIDTH - RIGHT_X];
} observation_t;

static uintptr_t peer;

static void remember_peer(uintptr_t conn) { peer = conn; }

void product_ai_mono_prepare(void)
{
    product_platform_prepare(true);
    product_ai_prepare_retirement_backend();
}

static void observe(void *ctx)
{
    observation_t *result = ctx;
    xiaotai_ai_talk_status(&result->ai);
    assert(xiaotai_ui_inspect(0, &result->ui) == XIAOTAI_OK);
    assert(result->ui.owner_timer && result->ui.width == WIDTH && result->ui.height == 64 &&
           !strcmp(result->ui.theme, "mono128") && result->ui.pixel == XIAOTAI_PIXEL_MONO1_VLSB);
    // Panel memory is read on the same owner that draws, including during AI.
    const xt_oled_fake_t *panel = xt_oled_fake_state();
    assert(panel->frame_bytes == 1024);
    memcpy(result->header[0], panel->frame + RIGHT_X, WIDTH - RIGHT_X);
    memcpy(result->header[1], panel->frame + WIDTH + RIGHT_X, WIDTH - RIGHT_X);
}

static bool shows_time(const observation_t *actual, const char *text)
{
    uint8_t frame[1024] = {0};
    const mono_canvas_t canvas = {frame, XIAOTAI_PIXEL_MONO1_VLSB};
    if (text) {
        const unsigned len = mono_utf8_width(text);
        assert(len <= WIDTH - RIGHT_X);
        (void)mono_window(&canvas, WIDTH - (int)len, 0, text, len, 0);
    }
    // Ignore the shared divider at y=13. The clock occupies the top 12 rows.
    for (unsigned x = 0; x < WIDTH - RIGHT_X; ++x) {
        if (actual->header[0][x] != frame[RIGHT_X + x] ||
            ((actual->header[1][x] ^ frame[WIDTH + RIGHT_X + x]) & 0x0fU)) return false;
    }
    return true;
}

static void await_home_transport(xiaotai_transport_state_t state, const char *label)
{
    observation_t result;
    for (unsigned waited = 0; waited < WAIT_MS; waited += 5) {
        product_on_owner(observe, &result);
        xiaotai_platform_status_t platform;
        xiaotai_platform_status(&platform);
        assert(platform.phase == XIAOTAI_PLATFORM_ONLINE && platform.signaling);
        if (xiaotai_transport_state() == state && !result.ui.overlay[0] &&
            !strcmp(result.ui.page, XIAOTAI_APP_COMPANION_PAGE_HOME) && shows_time(&result, label)) {
            fprintf(stderr, "mono HOME: platform online, RTC state=%u, label=%s\n", (unsigned)state, label);
            return;
        }
        xiaotai_delay_ms(5);
    }
    fprintf(stderr, "mono HOME: expected %s, page=%s, RTC state=%u, online label=%u\n",
            label, result.ui.page, (unsigned)xiaotai_transport_state(), shows_time(&result, "在线"));
    assert(false && "RTC-only transition did not render its HOME status");
}

void product_home_transport_label(bool enabled)
{
    product_platform_online();
    await_home_transport(XIAOTAI_TRANSPORT_ONLINE, "在线");
    const uint32_t starts = xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_START);
    xiaotai_transport_host_auto_started(false);
    xiaotai_transport_host_stopped(0);
    for (unsigned waited = 0; waited < WAIT_MS &&
         xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_START) == starts; waited += 5)
        xiaotai_delay_ms(5);
    assert(xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_START) == starts + 1U);
    const char *unavailable = enabled ? "连接中" : "在线";
    await_home_transport(XIAOTAI_TRANSPORT_STARTING, unavailable);
    xiaotai_transport_host_started();
    await_home_transport(XIAOTAI_TRANSPORT_ONLINE, "在线");

    xiaotai_transport_host_fail_start(XIAOTAI_ERR_TIMEOUT, 206, 1U);
    xiaotai_transport_host_stopped(0);
    await_home_transport(XIAOTAI_TRANSPORT_FAULT, unavailable);
    xiaotai_transport_host_auto_started(true);
    assert(xiaotai_transport_restart() == XIAOTAI_OK);
    await_home_transport(XIAOTAI_TRANSPORT_ONLINE, "在线");
}

static void await_view(uint32_t session, xiaotai_ai_talk_phase_t phase, const char *time)
{
    observation_t result;
    for (unsigned waited = 0; waited < WAIT_MS; waited += 5) {
        product_on_owner(observe, &result);
        if (result.ai.session == session && result.ai.phase == phase &&
            !strcmp(result.ui.page, XIAOTAI_APP_COMPANION_PAGE_TALK) && shows_time(&result, time)) {
            fprintf(stderr, "mono AI: phase=%u session=%u timer=%s\n", (unsigned)phase, (unsigned)session,
                    time ? time : "hidden");
            return;
        }
        xiaotai_delay_ms(5);
    }
    fprintf(stderr, "mono AI timer: expected phase=%u session=%u timer=%s, actual phase=%u session=%u page=%s\n",
            (unsigned)phase, (unsigned)session, time ? time : "hidden",
            (unsigned)result.ai.phase, (unsigned)result.ai.session, result.ui.page);
    assert(false);
}

static void talk_round(const char *command)
{
    assert(peer);
    xiaotai_transport_host_command(peer, 0, 0x2100U, (const uint8_t *)command, strlen(command));
}

static void await_signaling_resume(void)
{
    for (unsigned waited = 0; waited < WAIT_MS; waited += 5) {
        xiaotai_platform_status_t platform;
        xiaotai_platform_status(&platform);
        if (!platform.held && platform.signaling) return;
        xiaotai_delay_ms(5);
    }
    assert(false && "signaling did not resume after RTC retirement");
}

static void end_from_key(uint32_t session)
{
    const uint32_t link = product_ai_link(session);
    const uint32_t polls = xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_PENDING);
    xiaotai_transport_host_pending(1);
    const uint32_t disconnects = xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_DISCONNECT);
    const xiaotai_err_t inject = xiaotai_interaction_inject("main", 0, 1400);
    if (inject != XIAOTAI_OK) fprintf(stderr, "mono AI: key injection failed: %d\n", (int)inject);
    assert(inject == XIAOTAI_OK);
    product_ai_wait_disconnect(disconnects);
    xiaotai_transport_host_closed_for(peer, link);
    assert(xiaotai_transport_host_settle(WAIT_MS));
    assert(xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_PENDING) > polls);
    assert(xiaotai_link_release_state(link) == XIAOTAI_LINK_RELEASE_PENDING);
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    assert(platform.held && !platform.signaling);
    xiaotai_transport_host_pending(0);
    for (unsigned waited = 0; waited < WAIT_MS; waited += 5) {
        observation_t result;
        xiaotai_media_status_t media;
        product_on_owner(observe, &result);
        xiaotai_media_status(&media);
        if (!result.ai.session && result.ai.phase == XIAOTAI_AI_TALK_IDLE && !media.active &&
            !strcmp(result.ui.page, XIAOTAI_APP_COMPANION_PAGE_HOME)) {
            fprintf(stderr, "mono AI: long press stopped AI and returned home\n");
            await_signaling_resume();
            return;
        }
        xiaotai_delay_ms(5);
    }
    assert(false && "AI did not release and return home after long press");
}

void product_ai_mono_elapsed(void)
{
    product_platform_online();
    product_ai_prepare();
    const uint32_t first = product_ai_request();
    await_view(first, XIAOTAI_AI_TALK_CONNECTING, NULL);
    product_ai_connect_armed(first, remember_peer);
    await_view(first, XIAOTAI_AI_TALK_LISTENING, "00:00");
    await_view(first, XIAOTAI_AI_TALK_LISTENING, "00:01");
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    assert(platform.held && !platform.signaling);
    talk_round("{\"method\":\"round_start\"}");
    await_view(first, XIAOTAI_AI_TALK_SPEAKING, "00:02");
    talk_round("{\"method\":\"round_end\"}");
    await_view(first, XIAOTAI_AI_TALK_LISTENING, "00:03");
    end_from_key(first);
    product_ai_prepare();
    const uint32_t second = product_ai_request();
    assert(first != second);
    await_view(second, XIAOTAI_AI_TALK_CONNECTING, NULL);
    product_ai_connect_armed(second, remember_peer);
    await_view(second, XIAOTAI_AI_TALK_LISTENING, "00:00");
    // The preceding synthetic long press still has its release edge to deliver.
    await_view(second, XIAOTAI_AI_TALK_LISTENING, "00:01");
    end_from_key(second);
}
