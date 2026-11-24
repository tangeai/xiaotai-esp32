// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_ui.c::ui
#include "product_test.h"
#include "product_platform.h"
#include "audio_io.h"
#include "panel_fake.h"
#include "xiaotai/apps/companion.h"
#include "xiaotai/interaction.h"
#include "xiaotai/media.h"
#include "xiaotai/modules/room.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/port/os.h"
#include "xiaotai/ui.h"
#include "xiaotai_transport_host.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static atomic_bool s_paused, s_fail_next_resume, s_observe_keys;
static atomic_uint s_failed_resumes, s_setting_calls;

xiaotai_err_t __real_xt_audio_io_pause(xt_audio_io_t *io);
xiaotai_err_t __wrap_xt_audio_io_pause(xt_audio_io_t *io)
{
    const xiaotai_err_t err = __real_xt_audio_io_pause(io);
    if (err == XIAOTAI_OK) atomic_store(&s_paused, true);
    return err;
}

xiaotai_err_t __real_xt_audio_io_resume(xt_audio_io_t *io);
xiaotai_err_t __wrap_xt_audio_io_resume(xt_audio_io_t *io)
{
    atomic_store(&s_paused, false);
    if (!atomic_exchange(&s_fail_next_resume, false)) return __real_xt_audio_io_resume(io);
    atomic_fetch_add(&s_failed_resumes, 1U);
    return XIAOTAI_ERR_TIMEOUT;
}

xiaotai_err_t __real_xiaotai_setting_set_int(const char *name, int32_t value);
xiaotai_err_t __wrap_xiaotai_setting_set_int(const char *name, int32_t value)
{
    if (atomic_load(&s_observe_keys) && name &&
        (!strcmp(name, XIAOTAI_SETTING_AUDIO_VOLUME) || !strcmp(name, XIAOTAI_SETTING_AUDIO_MIC_GAIN)))
        atomic_fetch_add(&s_setting_calls, 1U);
    return __real_xiaotai_setting_set_int(name, value);
}

static xiaotai_ui_info_t wait_key_view(const char *page, unsigned focus, const xiaotai_ui_info_t *after)
{
    xiaotai_ui_info_t ui = {0};
    const int64_t deadline = xiaotai_time_us() + INT64_C(5000000);
    do {
        assert(xiaotai_ui_inspect(0, &ui) == XIAOTAI_OK);
        if (ui.ready && !ui.overlay[0] && (focus == UINT16_MAX || ui.focus == focus) &&
            !strcmp(ui.page, page) &&
            (!after || ui.posted > after->posted || ui.ignored > after->ignored)) {
            assert(ui.frames && !ui.present_fault &&
                   !strcmp(ui.theme, "mono128") && ui.width == 128U && ui.height == 64U);
            return ui;
        }
        xiaotai_delay_ms(5);
    } while (xiaotai_time_us() < deadline);
    fprintf(stderr, "%s focus=%u not reached: page=%s focus=%u posted=%u ignored=%u\n",
            page, focus, ui.page, (unsigned)ui.focus, ui.posted, ui.ignored);
    assert(false);
    return ui;
}

static xiaotai_ui_info_t wait_key_page(const char *page, unsigned focus, const xiaotai_ui_info_t *after)
{
    const xiaotai_ui_info_t ui = wait_key_view(page, focus, after);
    assert(ui.page_kind == XIAOTAI_VIEW_LIST);
    return ui;
}

static xiaotai_ui_info_t wait_settings(unsigned focus, const xiaotai_ui_info_t *after)
{
    return wait_key_page(XIAOTAI_APP_COMPANION_PAGE_SETTINGS, focus, after);
}

static void open_settings(void)
{
    const xiaotai_args_t request = {.text = XIAOTAI_APP_COMPANION_PAGE_SETTINGS};
    assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_SHOW, &request) == XIAOTAI_OK);
    (void)wait_settings(1, NULL);
}

/* Wait for a real idle DMA pause before injecting one failure. The late
 * fault must not change the successful (sticky) startup result. */
static void wait_audio(bool ready)
{
    const int64_t deadline = xiaotai_time_us() + INT64_C(5000000);
    do {
        xiaotai_media_status_t media;
        xiaotai_media_status(&media);
        if (media.startup_complete && media.startup_error == XIAOTAI_OK && media.ready == ready &&
            (ready ? !media.playing && atomic_load(&s_paused) : atomic_load(&s_failed_resumes) == 1U)) return;
        xiaotai_delay_ms(5);
    } while (xiaotai_time_us() < deadline);
    assert(false && "media did not reach the requested idle/reopen phase");
}

static void fail_resume(void)
{
    static const int16_t pcm[160] = {0};
    static const xiaotai_sound_t sound = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = 8000, .channels = 1,
        .data = (const uint8_t *)pcm, .size = sizeof(pcm)};
    const xiaotai_media_play_t play = {.clips = {{.sound = &sound}}, .clip_count = 1, .priority = XIAOTAI_PLAY_CUE};
    assert(atomic_load(&s_paused));
    atomic_store(&s_fail_next_resume, true);
    assert(xiaotai_media_play_clips(&play, NULL) == XIAOTAI_OK);
    wait_audio(false);
}

static void read_oled_frame(void *frame)
{
    xiaotai_ui_info_t ui;
    assert(xiaotai_ui_inspect(0, &ui) == XIAOTAI_OK && ui.owner_timer && ui.pixel == XIAOTAI_PIXEL_MONO1_VLSB);
    const xt_oled_fake_t *panel = xt_oled_fake_state();
    assert(panel->on && panel->frame_bytes == sizeof(panel->frame));
    memcpy(frame, panel->frame, panel->frame_bytes);
}

static void guidance_frame(int32_t expected, uint8_t frame[1024])
{
    int32_t actual;
    assert(xiaotai_setting_get_int(XIAOTAI_SETTING_GUIDANCE_ENABLED, &actual) == XIAOTAI_OK && actual == expected);
    const char *label = expected ? "语音引导：开" : "语音引导：关";
    const int64_t deadline = xiaotai_time_us() + INT64_C(2000000);
    do {
        xiaotai_ui_control_t controls[4];
        size_t count = 0;
        assert(xiaotai_ui_controls(0, controls, sizeof(controls) / sizeof(controls[0]), &count, 1000) == XIAOTAI_OK);
        for (size_t i = 0; i < count && i < sizeof(controls) / sizeof(controls[0]); ++i) {
            if (controls[i].element != XIAOTAI_UI_ELEMENT_ITEM || controls[i].index != 4U ||
                strcmp(controls[i].text, label)) continue;
            product_on_owner(read_oled_frame, frame);
            return;
        }
        xiaotai_delay_ms(5);
    } while (xiaotai_time_us() < deadline);
    assert(false && "guidance label was not rendered by mono128");
}

static void refresh_settings(void *unused)
{
    (void)unused;
    assert(xiaotai_setting_set_int(XIAOTAI_SETTING_GUIDANCE_ENABLED, 0) == XIAOTAI_OK);
}

static void wait_main_released(void)
{
    const int64_t deadline = xiaotai_time_us() + INT64_C(2000000);
    do {
        xiaotai_interaction_input_t input;
        assert(xiaotai_interaction_input(0, &input) == XIAOTAI_OK && !strcmp(input.role, "main"));
        if (!input.injected && !input.down && !input.clicks) return;
        xiaotai_delay_ms(5);
    } while (xiaotai_time_us() < deadline);
    assert(false && "the Back key did not finish releasing");
}

static void remaining_settings_menu(void)
{
    assert(xiaotai_interaction_inject("main", 1, 0) == XIAOTAI_OK);
    xiaotai_ui_info_t previous = wait_settings(5, NULL);
    uint8_t on[1024], off[1024];
    guidance_frame(1, on);
    for (int32_t next = 0; next <= 1; ++next) {
        assert(xiaotai_interaction_inject("main", 2, 0) == XIAOTAI_OK);
        previous = wait_settings(5, &previous);
        product_on_owner(NULL, NULL);
        guidance_frame(next, off);
        if (next == 0) assert(memcmp(on, off, sizeof(on)) != 0);
    }

    for (unsigned focus = 6; focus <= 7; ++focus) {
        assert(xiaotai_interaction_inject("main", 1, 0) == XIAOTAI_OK);
        (void)wait_settings(focus, NULL);
    }
    assert(xiaotai_interaction_inject("main", 2, 0) == XIAOTAI_OK);
    (void)wait_key_page(XIAOTAI_APP_COMPANION_PAGE_AUDIO, 1, NULL);
    assert(xiaotai_interaction_inject("main", 0, 1300) == XIAOTAI_OK);
    (void)wait_settings(7, NULL);
    wait_main_released();
    assert(xiaotai_interaction_inject("main", 1, 0) == XIAOTAI_OK);
    const xiaotai_ui_info_t moved = wait_settings(8, NULL);
    product_on_owner(refresh_settings, NULL);
    product_on_owner(NULL, NULL);
    size_t controls;
    assert(xiaotai_ui_controls(0, NULL, 0U, &controls, 1000) == XIAOTAI_OK);
    assert(xiaotai_ui_inspect(0, &previous) == XIAOTAI_OK && previous.updated > moved.updated && previous.focus == 8U);
    puts("audio diagnostics Back restored 7; Next moved to 8 and a settings refresh kept it");
}

void product_key_audio_settings(const char *phase)
{
    const bool ready = !strcmp(phase, "ready"), fresh = !strcmp(phase, "fresh");
    assert(ready || fresh || !strcmp(phase, "stale"));
    product_platform_online();
    wait_audio(true);
    if (!fresh) open_settings();
    if (!ready) fail_resume();
    if (fresh) open_settings();

    const xiaotai_ui_info_t before = wait_settings(1, NULL);
    xiaotai_ui_info_t after = before;
    atomic_store(&s_observe_keys, true);
    for (unsigned focus = 1; focus <= 4; ++focus) {
        if (focus > 1) {
            assert(xiaotai_interaction_inject("main", 1, 0) == XIAOTAI_OK);
            after = wait_settings(focus, NULL);
        }
        assert(xiaotai_interaction_inject("main", 2, 0) == XIAOTAI_OK);
        after = wait_settings(focus, &after);
        product_on_owner(NULL, NULL);
    }
    atomic_store(&s_observe_keys, false);
    const unsigned calls = atomic_load(&s_setting_calls);
    const uint32_t posted = after.posted - before.posted, ignored = after.ignored - before.ignored;
    fprintf(stderr, "key audio settings: %s, resume_failures=%u posted=%u ignored=%u setter_calls=%u\n",
            phase, atomic_load(&s_failed_resumes), posted, ignored, calls);
    assert(calls == (ready ? 4U : 0U));
    assert(posted == (ready ? 4U : fresh ? 0U : 1U) && posted + ignored == 4U);
    if (ready) remaining_settings_menu();
}

static atomic_uint room_requests;

static void unassigned_room_http(void *unused, const xiaotai_http_host_request_t *request,
                                 xiaotai_http_host_reply_t *reply)
{
    (void)unused;
    static const char assignment[] = "{\"code\":200,\"data\":{\"device_id\":\"" PRODUCT_FIXTURE_DEVICE
        "\",\"desired_state\":\"left\",\"assignment_version\":1}}";
    reply->status = 404;
    if (strstr(request->url, "/v1/call/group/")) atomic_fetch_add(&room_requests, 1U);
    if (request->method != XIAOTAI_HTTP_GET ||
        strcmp(request->url, "http://fixture.invalid/call/v1/call/group/device/assignment")) return;
    reply->status = 200;
    reply->body = (const uint8_t *)assignment;
    reply->body_size = sizeof(assignment) - 1U;
}

void product_unassigned_room_prepare(void)
{
    product_platform_http_responder(unassigned_room_http, NULL);
    product_platform_prepare(true);
}

static void room_is_unassigned(void *ctx)
{
    xiaotai_room_status_t state;
    xiaotai_room_status(&state);
    *(bool *)ctx = state.attending && state.assignment_known && !state.assigned &&
                  !state.generation && !state.transmitting;
}

static void check_room_row(unsigned focus, const char *label, bool disabled)
{
    xiaotai_ui_control_t controls[16];
    size_t count = 0;
    assert(xiaotai_ui_controls(0, controls, 16, &count, 1000) == XIAOTAI_OK && count <= 16);
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_ui_control_t *row = &controls[i];
        if (row->element != XIAOTAI_UI_ELEMENT_ITEM || row->index + 1U != focus) continue;
        fprintf(stderr, "room row %u: %s state=0x%x\n", focus, row->text, row->state);
        assert(!strcmp(row->text, label) && ((row->state & XIAOTAI_STATE_DISABLED) != 0) == disabled);
        return;
    }
    assert(false && "focused room row was not rendered by mono128");
}

// Replaces legacy: tests/xmini/test_ui.c
void product_unassigned_room_menu(void)
{
    static const struct { const char *label; bool disabled; } rows[] = {
        {"开始发言", true}, {"加入房间", false}, {"创建房间", false},
        {"加入密码房间", false}, {"创建密码房间", false},
        {"连接房间", true}, {"断开连接", true}, {"退出房间", true}, {"刷新", false},
    };
    const char *const page = XIAOTAI_APP_COMPANION_PAGE_ROOM;
    product_platform_online();
    const xiaotai_args_t request = {.text = XIAOTAI_APP_COMPANION_PAGE_ROOM};
    assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_SHOW, &request) == XIAOTAI_OK);
    bool unassigned = false;
    const int64_t deadline = xiaotai_time_us() + INT64_C(5000000);
    do {
        product_on_owner(room_is_unassigned, &unassigned);
        if (!unassigned) xiaotai_delay_ms(5);
    } while (!unassigned && xiaotai_time_us() < deadline);
    assert(unassigned && atomic_load(&room_requests) == 1U);
    const uint32_t connects = xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_CONNECT);
    const uint32_t disconnects = xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_DISCONNECT);
    const uint32_t commands = xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_COMMAND);
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        const unsigned focus = i + 1U;
        const xiaotai_ui_info_t before = wait_key_page(page, focus, NULL);
        check_room_row(focus, rows[i].label, rows[i].disabled);
        if (rows[i].disabled) {
            assert(xiaotai_interaction_inject("main", 2, 0) == XIAOTAI_OK);
            const xiaotai_ui_info_t after = wait_key_page(page, focus, &before);
            assert(after.ignored == before.ignored + 1U && after.posted == before.posted);
        }
        assert(xiaotai_interaction_inject("main", 1, 0) == XIAOTAI_OK);
    }
    (void)wait_key_page(page, 1, NULL);
    assert(xiaotai_interaction_inject("main", 0, 1300) == XIAOTAI_OK);
    (void)wait_key_view(XIAOTAI_APP_COMPANION_PAGE_HOME, UINT16_MAX, NULL);
    wait_main_released();
    product_on_owner(NULL, NULL);
    assert(atomic_load(&room_requests) == 1U);
    assert(connects == xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_CONNECT) &&
           disconnects == xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_DISCONNECT) &&
           commands == xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_COMMAND));
    puts("unassigned room: all nine menu rows wrap, disabled keys post nothing and Back returns home");
}
