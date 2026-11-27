// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_voice_reliability.py::test_product_volume_application
#include <string.h>

#include "settings_harness.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/identity.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai/preferences.h"
#include "xiaotai_kv_host.h"

enum { WAIT_MS = 3000 };
xiaotai_err_t settings_legacy_configure(void); /* generated from product.json */
extern const char settings_legacy_mode[];

int32_t setting(const char *name) /* also used by the fixture media */
{
    int32_t value = -1;
    return xiaotai_setting_get_int(name, &value) == XIAOTAI_OK ? value : -1;
}

/* Register the app-owned storage keys without starting application UI or
 * networking. Reads and writes still go through the real framework store. */
static xiaotai_err_t panel_settings(void)
{
    if (strcmp(settings_legacy_mode, "panel") && strcmp(settings_legacy_mode, "door_panel")) return XIAOTAI_OK;
    static const xiaotai_setting_t entries[] = {
        {.name = "ack", .key = "ack_voice", .kind = XIAOTAI_SETTING_BOOL, .max = 1},
        {.name = "emotion", .key = "emoji", .max = 22},
        {.name = "sender", .key = "door_sender", .kind = XIAOTAI_SETTING_BOOL, .max = 1},
        {.name = "peer", .key = "door_peer", .kind = XIAOTAI_SETTING_TEXT, .default_text = ""},
    };
    const size_t count = !strcmp(settings_legacy_mode, "door_panel") ? 4U : 2U;
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_err_t err = xiaotai_setting_register(&entries[i]);
        if (err != XIAOTAI_OK) return err;
    }
    return XIAOTAI_OK;
}

static int boot(bool screen, bool gain)
{
    static const char *const dependencies[] = {"identity", NULL};
    static const xiaotai_module_t identity = {.name = "identity", .init = xiaotai_engine_identity_init};
    static const xiaotai_module_t settings = {.name = "settings", .depends = dependencies,
        .init = xiaotai_module_settings_init};
    static const xiaotai_module_t extras = {.name = "panel", .depends = dependencies, .init = panel_settings};
    static const xiaotai_module_t *const modules[] = {&identity, &settings, &extras};
    const xiaotai_framework_config_t framework = {.modules = modules, .module_count = sizeof(modules) / sizeof(modules[0])};
    fake_board_set(&(fake_board_t){.output = true, .input = true, .display = screen,
        .gain_db = gain ? 24 : 0, .gain_max_db = gain ? 36 : 0});
    fake_media_reset();
    CHECK(settings_legacy_configure() == XIAOTAI_OK);
    CHECK(xiaotai_framework_start(&framework) == XIAOTAI_OK);
    return 0;
}

static void stop(void)
{
    /* Preserve the host's KV across restarts, but drop RAM caches and the
     * framework's installed store. The next boot must read the bytes again. */
    xiaotai_framework_reset();
    xiaotai_settings_reset();
    xiaotai_identity_reset();
    xiaotai_preferences_reset();
}

typedef struct {
    const char *name;
    int32_t value;
    const char *text;
    xiaotai_err_t result;
    xiaotai_signal_t done;
} change_t;

static void change_on_owner(void *ctx)
{
    change_t *change = ctx;
    change->result = change->text ? xiaotai_setting_set_text(change->name, change->text) :
                                   xiaotai_setting_set_int(change->name, change->value);
    xiaotai_signal_give(&change->done);
}

static int change_value(const char *name, int32_t value, const char *text)
{
    change_t request = {.name = name, .value = value, .text = text, .result = XIAOTAI_FAIL};
    CHECK(xiaotai_signal_init(&request.done) == XIAOTAI_OK);
    CHECK(xiaotai_run(change_on_owner, &request) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&request.done, WAIT_MS));
    xiaotai_signal_deinit(&request.done);
    CHECK(request.result == XIAOTAI_OK);
    return 0;
}

static int change(const char *name, int32_t value) { return change_value(name, value, NULL); }

static int await_write(void)
{
    /* Let the real owner timer commit naturally, without calling flush() to
     * hide a missing scheduled write. */
    for (unsigned i = 0; i < WAIT_MS / 10 && xiaotai_preferences_pending(); ++i) xiaotai_delay_ms(10);
    CHECK(!xiaotai_preferences_pending());
    return 0;
}

static bool byte_is(const char *key, uint8_t expected)
{
    uint8_t value = 0;
    return xiaotai_kv_get_u8("product", key, &value, WAIT_MS) == XIAOTAI_OK && value == expected;
}

static bool blob_is(const char *space, const char *key, const uint8_t expected[4])
{
    uint8_t actual[4];
    size_t size = sizeof(actual);
    return xiaotai_kv_get_blob_strict(space, key, actual, &size, WAIT_MS) == XIAOTAI_OK &&
           size == sizeof(actual) && !memcmp(expected, actual, sizeof(actual));
}

static bool no_copy(const char *key)
{
    uint8_t data[4];
    size_t size = sizeof(data);
    return xiaotai_kv_get_blob_strict("xt_settings", key, data, &size, WAIT_MS) == XIAOTAI_ERR_NOT_FOUND;
}

static void finish_speaker_intent(void *ctx)
{
    xiaotai_signal_give(ctx);
}

static int speaker_intent(int32_t value)
{
    const xiaotai_args_t args = {.value = value};
    xiaotai_signal_t done;
    CHECK(xiaotai_signal_init(&done) == XIAOTAI_OK);
    CHECK(xiaotai_intent_post(XIAOTAI_INTENT_AUDIO_SPEAKER_MUTE, &args) == XIAOTAI_OK);
    CHECK(xiaotai_run(finish_speaker_intent, &done) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&done, WAIT_MS));
    xiaotai_signal_deinit(&done);
    return 0;
}

static int speaker_admission(bool strict, bool snapshot)
{
    CHECK(xiaotai_kv_set_u8("product", "spk_mute", 0, WAIT_MS) == XIAOTAI_OK);
    CHECK(!boot(true, false));
    xiaotai_preferences_stats_t before, after;
    xiaotai_settings_stats_t settings_before, settings_after;
    const uint32_t refused_writes = strict ? 0U : 2U;

    fake_media_clear_calls();
    xiaotai_preferences_stats(&before);
    xiaotai_settings_stats(&settings_before);
    fake_media_fail_next(FAKE_MEDIA_SPEAKER_MUTED, XIAOTAI_ERR_NOT_SUPPORTED);
    if (snapshot) xiaotai_kv_host_gate(false);
    CHECK(!change(XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED, 1));
    xiaotai_preferences_stats(&after);
    xiaotai_settings_stats(&settings_after);
    CHECK(fake_media_count(FAKE_MEDIA_SPEAKER_MUTED) == 1);
    CHECK(settings_after.refused == settings_before.refused + 1);
    CHECK(after.stores == before.stores + refused_writes);
    CHECK(after.coalesced == before.coalesced + (strict ? 0U : 1U));
    CHECK(xiaotai_preferences_pending() != strict);
    CHECK(setting(XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED) == 0);
    if (snapshot) {
        CHECK(xiaotai_kv_host_wait_held(WAIT_MS));
        xiaotai_kv_host_gate(true);
    }
    CHECK(!await_write() && byte_is("spk_mute", 0));

    /* The module intent reaches the same policy as a direct registry/AT set.
     * If the second toggle is refused, the previously accepted one stays. */
    fake_media_clear_calls();
    xiaotai_preferences_stats(&before);
    xiaotai_kv_host_counts_t kv_before, kv_after;
    xiaotai_kv_host_counts(&kv_before);
    if (snapshot) xiaotai_kv_host_gate(false);
    CHECK(!speaker_intent(-1));
    if (snapshot) CHECK(xiaotai_kv_host_wait_held(WAIT_MS));
    CHECK(setting(XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED) == 1 && xiaotai_preferences_pending());
    xiaotai_preferences_stats(&after);
    CHECK(after.stores == before.stores + 1 && fake_media_count(FAKE_MEDIA_SPEAKER_MUTED) == 1);
    before = after;
    CHECK(!speaker_intent(1));
    xiaotai_preferences_stats(&after);
    CHECK(after.stores == before.stores && fake_media_count(FAKE_MEDIA_SPEAKER_MUTED) == 1);

    fake_media_fail_next(FAKE_MEDIA_SPEAKER_MUTED, XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(!speaker_intent(-1));
    xiaotai_preferences_stats(&after);
    CHECK(after.stores == before.stores + refused_writes);
    /* A native image is already in flight. Only the failed attempt and its
     * rollback coalesce in the next waiting image, not into the accepted one. */
    CHECK(after.coalesced == before.coalesced + (snapshot ? 1U : refused_writes));
    CHECK(fake_media_count(FAKE_MEDIA_SPEAKER_MUTED) == 2);
    CHECK(setting(XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED) == 1 && xiaotai_preferences_pending());
    if (snapshot) xiaotai_kv_host_gate(true);
    CHECK(!await_write() && byte_is("spk_mute", 1));
    if (snapshot) {
        xiaotai_kv_host_counts(&kv_after);
        CHECK(kv_after.commits == kv_before.commits + 2U);
    }

    /* The same rule holds after an accepted edit reaches the actual KV worker. */
    if (strict) {
        xiaotai_kv_host_gate(false);
        CHECK(!change(XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED, 0));
        const bool held = xiaotai_kv_host_wait_held(WAIT_MS);
        if (!held) xiaotai_kv_host_gate(true);
        CHECK(held);
        xiaotai_preferences_stats(&before);
        fake_media_fail_next(FAKE_MEDIA_SPEAKER_MUTED, XIAOTAI_ERR_NOT_SUPPORTED);
        const int refused = change(XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED, 1);
        xiaotai_kv_host_gate(true);
        CHECK(!refused);
        xiaotai_preferences_stats(&after);
        CHECK(after.stores == before.stores && after.coalesced == before.coalesced);
        CHECK(!await_write() && byte_is("spk_mute", 0));
        CHECK(setting(XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED) == 0);
    }

    /* Genuine async storage failure still leaves the accepted setting in RAM
     * and reports the failed write, without changing the on-disk format. */
    before = after;
    xiaotai_kv_host_fail_key("spk_mute", XIAOTAI_ERR_TIMEOUT);
    CHECK(!change(XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED, strict ? 1 : 0) && !await_write());
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    xiaotai_preferences_stats(&after);
    CHECK(after.stores == before.stores + 1 && after.write_failures == before.write_failures + 1);
    CHECK(setting(XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED) == strict && byte_is("spk_mute", !strict));
    printf("speaker admission=%s synchronous stores per refusal=%u; previous pending kept; in-flight check=%s;"
           " async rejection counted\n", strict ? "on" : "off", refused_writes, strict ? "passed" : "not selected");
    return 0;
}

static int panel(void)
{
    const bool door = !strcmp(settings_legacy_mode, "door_panel");
    char peer[65];
    CHECK(xiaotai_kv_set_u8("product", "volume", 8, WAIT_MS) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_u8("product", "spk_mute", 1, WAIT_MS) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_u8("product", "mic_mute", 0, WAIT_MS) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_u8("product", "sleep", 2, WAIT_MS) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_u8("product", "ack_voice", 1, WAIT_MS) == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_u8("product", "emoji", 4, WAIT_MS) == XIAOTAI_OK);
    if (door) {
        CHECK(xiaotai_kv_set_u8("product", "door_sender", 1, WAIT_MS) == XIAOTAI_OK);
        CHECK(xiaotai_kv_set_str("product", "door_peer", "old-peer", WAIT_MS) == XIAOTAI_OK);
    }
    CHECK(!boot(true, false));
    CHECK(setting(XIAOTAI_SETTING_AUDIO_VOLUME) == 80 && setting(XIAOTAI_SETTING_SCREEN_SLEEP) == 600);
    CHECK(setting(XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED) == 1 && setting(XIAOTAI_SETTING_AUDIO_MIC_MUTED) == 0);
    CHECK(setting("ack") == 1 && setting("emotion") == 4);
    if (door) {
        CHECK(setting("sender") == 1);
        CHECK(xiaotai_setting_get_text("peer", peer, sizeof(peer)) == XIAOTAI_OK && !strcmp(peer, "old-peer"));
    }
    CHECK(!change(XIAOTAI_SETTING_AUDIO_VOLUME, 75)); /* nearest, ties choose the lower U8 index */
    CHECK(!change(XIAOTAI_SETTING_AUDIO_MIC_MUTED, 1));
    CHECK(!change(XIAOTAI_SETTING_SCREEN_SLEEP, 1800));
    CHECK(!await_write());
    CHECK(byte_is("volume", 7) && byte_is("sleep", 3) && byte_is("mic_mute", 1) && byte_is("spk_mute", 1));
    CHECK(byte_is("ack_voice", 1) && byte_is("emoji", 4));
    CHECK(!change(XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED, 0));
    CHECK(!change("ack", 0) && !change("emotion", 22));
    if (door) CHECK(!change("sender", 0) && !change_value("peer", 0, "test-peer"));
    CHECK(!await_write());
    CHECK(byte_is("spk_mute", 0) && byte_is("ack_voice", 0) && byte_is("emoji", 22));
    CHECK(no_copy("volume") && no_copy("sleep") && no_copy("mic_mute") && no_copy("spk_mute"));
    CHECK(no_copy("ack_voice") && no_copy("emoji"));
    if (door) {
        CHECK(byte_is("door_sender", 0) && no_copy("door_sender") && no_copy("door_peer"));
        CHECK(xiaotai_kv_get_str("product", "door_peer", peer, sizeof(peer), WAIT_MS) == XIAOTAI_OK &&
              !strcmp(peer, "test-peer"));
    }
    stop();
    CHECK(!boot(true, false));
    CHECK(setting(XIAOTAI_SETTING_AUDIO_VOLUME) == 70 && setting(XIAOTAI_SETTING_SCREEN_SLEEP) == 1800);
    CHECK(setting(XIAOTAI_SETTING_AUDIO_MIC_MUTED) == 1 && setting(XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED) == 0);
    CHECK(setting("ack") == 0 && setting("emotion") == 22);
    if (door) {
        CHECK(setting("sender") == 0);
        CHECK(xiaotai_setting_get_text("peer", peer, sizeof(peer)) == XIAOTAI_OK && !strcmp(peer, "test-peer"));
    }
    return 0;
}

static int xmini(void)
{
    const uint8_t old[] = {1, 67, 13, 0}, v2[] = {2, 70, 13, 1}, silent[] = {2, 70, 13, 0};
    const uint8_t corrupt[] = {1, 67, 13, 1}, fresh[] = {2, 71, 24, 1};
    CHECK(xiaotai_kv_set("xmini_ui", "audio", old, sizeof(old), WAIT_MS) == XIAOTAI_OK);
    CHECK(!boot(true, true));
    CHECK(setting(XIAOTAI_SETTING_AUDIO_VOLUME) == 67 && setting(XIAOTAI_SETTING_AUDIO_MIC_GAIN) == 13);
    CHECK(setting(XIAOTAI_SETTING_GUIDANCE_ENABLED) == 1 && blob_is("xmini_ui", "audio", old));
    CHECK(!change(XIAOTAI_SETTING_AUDIO_VOLUME, 70) && !await_write());
    CHECK(blob_is("xmini_ui", "audio", v2)); /* only volume changed, v1 now v2 */
    CHECK(!change(XIAOTAI_SETTING_GUIDANCE_ENABLED, 0) && !await_write());
    CHECK(blob_is("xmini_ui", "audio", silent) && no_copy("volume") && no_copy("mic_gain") && no_copy("guidance"));
    stop();
    CHECK(!boot(true, true));
    CHECK(setting(XIAOTAI_SETTING_AUDIO_VOLUME) == 70 && setting(XIAOTAI_SETTING_AUDIO_MIC_GAIN) == 13);
    CHECK(setting(XIAOTAI_SETTING_GUIDANCE_ENABLED) == 0);
    stop();
    CHECK(xiaotai_kv_set("xmini_ui", "audio", corrupt, sizeof(corrupt), WAIT_MS) == XIAOTAI_OK);
    CHECK(!boot(true, true));
    CHECK(setting(XIAOTAI_SETTING_AUDIO_VOLUME) == 40 && setting(XIAOTAI_SETTING_AUDIO_MIC_GAIN) == 24);
    CHECK(setting(XIAOTAI_SETTING_GUIDANCE_ENABLED) == 1);
    CHECK(!change(XIAOTAI_SETTING_AUDIO_VOLUME, 71) && !await_write());
    CHECK(blob_is("xmini_ui", "audio", corrupt) && no_copy("volume"));
    stop();
    CHECK(xiaotai_kv_erase("xmini_ui", "audio", WAIT_MS) == XIAOTAI_OK);
    CHECK(!boot(true, true));
    CHECK(setting(XIAOTAI_SETTING_AUDIO_VOLUME) == 40 && !change(XIAOTAI_SETTING_AUDIO_VOLUME, 71) && !await_write());
    CHECK(blob_is("xmini_ui", "audio", fresh)); /* initial gain and guidance defaults, not zeros */
    return 0;
}

static int volume_admission(bool strict)
{
    const char *const volume = XIAOTAI_SETTING_AUDIO_VOLUME;
    CHECK(xiaotai_kv_set_u8("product", "volume", 7, WAIT_MS) == XIAOTAI_OK);
    CHECK(!boot(true, false) && setting(volume) == 70);
    fake_media_clear_calls();
    xiaotai_preferences_stats_t before, after;
    xiaotai_kv_host_counts_t kv_before, kv_after;
    xiaotai_settings_stats_t settings_before, settings_after;
    xiaotai_preferences_stats(&before);
    xiaotai_kv_host_counts(&kv_before);
    xiaotai_settings_stats(&settings_before);

    fake_media_fail_next(FAKE_MEDIA_VOLUME, XIAOTAI_ERR_TIMEOUT);
    CHECK(!change(volume, 80));
    const uint32_t refused_before_media = fake_media_stores_at_volume_request() - before.stores;
    xiaotai_preferences_stats(&after);
    const uint32_t refused_stores = after.stores - before.stores;
    xiaotai_settings_stats(&settings_after);
    CHECK(fake_media_count(FAKE_MEDIA_VOLUME) == 1 && fake_media_call(0)->value == 80);
    CHECK(settings_after.refused == settings_before.refused + 1 && setting(volume) == 70);
    CHECK(!await_write() && byte_is("volume", 7));
    xiaotai_kv_host_counts(&kv_after);
    const uint32_t refused_writes = kv_after.writes - kv_before.writes;
    CHECK(refused_before_media == (strict ? 0U : 1U) && refused_stores == (strict ? 0U : 2U));
    if (strict) CHECK(!refused_writes && kv_after.commits == kv_before.commits);

    kv_before = kv_after;
    xiaotai_preferences_stats(&before);
    CHECK(!change(volume, 80));
    const uint32_t accepted_before_media = fake_media_stores_at_volume_request() - before.stores;
    xiaotai_preferences_stats(&after);
    CHECK(accepted_before_media == (strict ? 0U : 1U) && after.stores == before.stores + 1);
    CHECK(fake_media_count(FAKE_MEDIA_VOLUME) == 2 && fake_media_call(1)->value == 80 && setting(volume) == 80);
    CHECK(!await_write() && byte_is("volume", 8) && no_copy("volume"));
    xiaotai_kv_host_counts(&kv_after);
    const uint32_t accepted_writes = kv_after.writes - kv_before.writes;
    if (strict) CHECK(accepted_writes == 1 && kv_after.commits == kv_before.commits + 1);

    if (strict) {
        const int32_t endpoints[] = {100, 0};
        for (size_t i = 0; i < sizeof(endpoints) / sizeof(*endpoints); ++i) {
            CHECK(!change(volume, endpoints[i]) && !await_write());
            const size_t calls = fake_media_call_count();
            xiaotai_kv_host_counts(&kv_before);
            CHECK(!change(volume, endpoints[i]) && !await_write());
            xiaotai_kv_host_counts(&kv_after);
            CHECK(setting(volume) == endpoints[i] && byte_is("volume", (uint8_t)(endpoints[i] / 10)));
            CHECK(kv_after.writes == kv_before.writes && fake_media_call_count() == calls);
        }
        stop();
        CHECK(!boot(true, false) && setting(volume) == 0);
    }
    printf("volume admission=%s: reject stores=%u, backend writes=%u; accepted stores before media=%u, writes=%u\n",
           strict ? "on" : "off", refused_stores, refused_writes, accepted_before_media, accepted_writes);
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(xiaotai_kv_init() == XIAOTAI_OK);
    CHECK(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    if (argc == 2 && (!strcmp(argv[1], "volume_strict") || !strcmp(argv[1], "volume_default"))) {
        const int result = volume_admission(!strcmp(argv[1], "volume_strict"));
        stop();
        return result;
    }
    if (argc == 2 && (!strcmp(argv[1], "speaker_strict") || !strcmp(argv[1], "speaker_default") ||
                      !strcmp(argv[1], "speaker_snapshot_default"))) {
        const int result = speaker_admission(!strcmp(argv[1], "speaker_strict"),
                                             !strcmp(argv[1], "speaker_snapshot_default"));
        stop();
        return result;
    }
    if (!strcmp(settings_legacy_mode, "xmini")) CHECK(!xmini());
    else if (!strcmp(settings_legacy_mode, "panel") || !strcmp(settings_legacy_mode, "door_panel")) CHECK(!panel());
    else CHECK(false);
    stop();
    return 0;
}
