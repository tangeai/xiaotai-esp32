// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_product.c::product
// Replaces legacy: tests/regression/test_call_hangup.py
// Replaces legacy: tests/test_boot_contract.py
// Replaces legacy: tests/test_hardware_boundaries.py
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "product_test.h"
#include "product_portal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework_config.h"
#include "xiaotai/hal.h"
#include "xiaotai/media.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai_net_host.h"
#include "kv_backend.h"
#if PRODUCT_TEST_START_GUIDE
#include "panel_fake.h"
#endif
#if PRODUCT_TEST_STORAGE_RECOVERY || PRODUCT_TEST_DISCOVERY_ENDPOINT
#include "xiaotai/ui.h"
#endif
#if PRODUCT_TEST_STORAGE_RECOVERY
#include "xiaotai/apps/companion.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/net_driver.h"
#include "xiaotai/platform.h"
#include "xiaotai/transport.h"
#include "xiaotai/ui_theme.h"
#include "xiaotai_kv_host.h"
#endif
#if PRODUCT_TEST_DISCOVERY_ENDPOINT
#include "xiaotai/transport.h"
#endif

#if PRODUCT_TEST_CONSOLE
#include "xiaotai/at.h"
#include "xiaotai_console_host.h"
#endif

#if PRODUCT_TEST_ARBITRATION
int product_arbitration_run(void);
#endif
#if PRODUCT_TEST_REPROVISION_KEYS
void product_reprovision_keys_prepare(void);
void product_reprovision_keys(void);
void product_reprovision_barrier(const char *test);
void product_runtime_wifi_audio(void);
void product_runtime_wifi_cancel_scope(void);
void product_reprovision_partial_prepare(bool cold);
void product_reprovision_partial(bool cold);
#endif
#if PRODUCT_TEST_KEY_AUDIO_SETTINGS
void product_key_audio_settings(const char *phase);
void product_call_cleanup_prepare(void);
void product_call_cleanup(void);
void product_unassigned_room_prepare(void);
void product_unassigned_room_menu(void);
void product_contacts_empty_prepare(void);
void product_contacts_empty_refresh(const char *page);
void product_contacts_initial_view_prepare(const char *scenario);
void product_contacts_initial_view(void);
#endif
#if PRODUCT_TEST_AI_MONO
void product_ai_mono_prepare(void);
void product_ai_mono_elapsed(void);
void product_home_transport_label(bool enabled);
#endif
#if PRODUCT_TEST_STACK_BUSINESS || PRODUCT_TEST_AI_FIXED_RX
#include "product_ai.h"
#endif
#if PRODUCT_TEST_AI_STREAM_LIFECYCLE
void product_ai_stream_lifecycle(bool maintenance_stop);
#endif
#if PRODUCT_TEST_WT99 || PRODUCT_TEST_STACK_BUSINESS || PRODUCT_TEST_AI_FIXED_RX || PRODUCT_TEST_ENVIRONMENT_BINDING
#include "product_platform.h"
#endif
#if PRODUCT_TEST_STACK_BUSINESS
#include "xiaotai/modules/ai_talk.h"
#include "xiaotai/modules/call.h"
#include "xiaotai/modules/wechat.h"
#include "xiaotai/platform.h"
#include "xiaotai_transport_host.h"
#endif
#if PRODUCT_TEST_WT99
void product_wt99_prepare(void);
void product_wt99_presenter(void);
void product_wt99_binding(void);
void product_preferences_reselect_prepare(void);
int product_preferences_reselect_run(bool enabled);
void product_preferences_reselect_soak_prepare(void);
int product_preferences_reselect_soak(void);
#endif
#if PRODUCT_TEST_HISTORY
void product_history(bool clear);
void product_history_maintenance(void);
#endif
#if PRODUCT_TEST_LEDGER
void product_ledger_measurement_start(void);
void product_ledger_prepare(bool provisioned);
int product_ledger(bool provisioned);
#endif
#if PRODUCT_TEST_WECHAT_RX
void product_wechat_receive(void);
void product_wechat_answer_payload(bool strict);
#endif
#if PRODUCT_TEST_PEER_CLOSE
void product_peer_close_prepare(const char *scene);
void product_peer_close_run(const char *scene);
#endif
#if PRODUCT_TEST_WECHAT_AUTHORIZATION
void product_wechat_authorization_prepare(void);
void product_wechat_authorization(bool use_back);
#endif
#if PRODUCT_TEST_ENVIRONMENT_ADMISSION
int product_environment_admission(bool verifying, bool timeout);
#endif
#if PRODUCT_TEST_ENVIRONMENT_BINDING
void product_environment_binding(const char *scenario);
#endif
#if PRODUCT_TEST_ROOM_MAINTENANCE
void product_room_maintenance_prepare(void);
void product_room_maintenance(void);
#endif
#if PRODUCT_TEST_PREFERENCES_FAILURE
int product_preferences_failure(bool unrelated_edit);
bool product_preferences_cold_prepare(const char *scenario);
int product_preferences_cold_run(void);
void product_preferences_choices_prepare(void);
int product_preferences_choices_run(bool invalid);
#endif
#if PRODUCT_TEST_CLOCK_GATE
void product_clock_gate_prepare(bool bound);
void product_clock_gate(bool bound);
void product_environment_journal_prepare(const char *mode);
void product_environment_journal(void);
#endif
#if PRODUCT_TEST_RECOGNITION_HTTP
void product_recognition_http_prepare(void);
void product_recognition_http(void);
void product_recognition_upload_probe_prepare(const char *name);
void product_recognition_upload_probe(void);
void product_recognition_http_cleanup(void);
void product_recognition_audio_saturation_prepare(void);
void product_recognition_audio_saturation(void);
#endif
#if PRODUCT_TEST_DISCOVERY_ENDPOINT
void product_endpoint_prepare(const char *test);
void product_endpoint(const char *test);
#endif
#if PRODUCT_TEST_RTC_RELEASE
void product_rtc_release_prepare(bool report_headroom);
void product_rtc_release(const char *scenario);
#endif

static atomic_bool s_restart_expected, s_restarted;
static atomic_bool s_fail_owner_signal;
static xiaotai_err_t s_owner_signal_error;
static unsigned s_owner_signal_faults;
static pthread_t s_fault_caller;

#if PRODUCT_TEST_XMINI_PREFS
bool product_xmini_prefs_prepare(const char *scenario);
int product_xmini_prefs_revert(void);
void product_xmini_prefs_observe_restart(void);
void product_xmini_prefs_observe_view(const xiaotai_view_t *view);
#endif
#if PRODUCT_TEST_BINDING_GRANT
void product_binding_grant_prepare(bool reload);
void product_binding_grant(bool reload);
#if PRODUCT_TEST_BINDING_SCOPE
void product_binding_scope_prepare(const char *name);
void product_binding_scope(void);
#endif
#endif
#if PRODUCT_TEST_PROFILE_VIDEO
void product_profile_video_prepare(bool expect_h5, const char *board_model);
void product_profile_video_run(void);
#endif

void product_expect_restart(void)
{
    assert(!atomic_exchange(&s_restart_expected, true) && !atomic_load(&s_restarted));
}

bool product_restarted(void) { return atomic_load(&s_restarted); }

void __wrap_xiaotai_restart(void)
{
    if (atomic_exchange(&s_restart_expected, false)) {
#if PRODUCT_TEST_XMINI_PREFS
        product_xmini_prefs_observe_restart();
#endif
        assert(!atomic_exchange(&s_restarted, true));
        return;
    }
    fprintf(stderr, "%s: unexpected restart\n", XIAOTAI_PRODUCT_NAME);
    abort();
}

bool __wrap_xiaotai_mem_has_external(void) { return PRODUCT_HOST_EXTERNAL_RAM; }

xiaotai_err_t __real_xiaotai_signal_init(xiaotai_signal_t *signal);
xiaotai_err_t __wrap_xiaotai_signal_init(xiaotai_signal_t *signal)
{
    /* The board publishes its parts before the framework's startup latch. */
    if (atomic_load(&s_fail_owner_signal) && pthread_equal(pthread_self(), s_fault_caller) &&
        xiaotai_hal_network(0)) {
        atomic_store(&s_fail_owner_signal, false);
        ++s_owner_signal_faults;
        return s_owner_signal_error;
    }
    return __real_xiaotai_signal_init(signal);
}

static pthread_t s_start_thread;
static atomic_bool s_start_returned;

static xiaotai_err_t s_nvs_init_error;
static unsigned s_nvs_init_calls;
static bool s_fail_app_init;
static unsigned s_app_init_calls;

#if PRODUCT_TEST_STORAGE_RECOVERY
static atomic_bool s_storage_error_presented;
static atomic_uint s_storage_frame_lit;
#endif

#if PRODUCT_TEST_DISCOVERY_ENDPOINT
void product_discovery_observe_view(const xiaotai_view_t *view);
#endif

#if PRODUCT_TEST_STORAGE_RECOVERY || PRODUCT_TEST_DISCOVERY_ENDPOINT
void __real_xiaotai_view_show(const xiaotai_view_t *view);
void __wrap_xiaotai_view_show(const xiaotai_view_t *view);
void __wrap_xiaotai_view_show(const xiaotai_view_t *view)
{
#if PRODUCT_TEST_STORAGE_RECOVERY
    if (s_nvs_init_error && view && view->status) {
        char expected[16];
        snprintf(expected, sizeof(expected), "ERR %04X", (unsigned)s_nvs_init_error);
        if (!strcmp(view->status, expected)) {
            assert(view->id && !strcmp(view->id, XIAOTAI_APP_COMPANION_PAGE_MAINTENANCE) && !view->action_count);
            atomic_store(&s_storage_error_presented, true);
        }
    }
#endif
#if PRODUCT_TEST_DISCOVERY_ENDPOINT
    product_discovery_observe_view(view);
#endif
#if PRODUCT_TEST_XMINI_PREFS
    product_xmini_prefs_observe_view(view);
#endif
    __real_xiaotai_view_show(view);
}
#endif

#if PRODUCT_TEST_STORAGE_RECOVERY
xiaotai_err_t __real_xiaotai_ui_host_present(const xiaotai_ui_host_t *host, const uint8_t *frame, size_t size);
xiaotai_err_t __wrap_xiaotai_ui_host_present(const xiaotai_ui_host_t *host, const uint8_t *frame, size_t size);
xiaotai_err_t __wrap_xiaotai_ui_host_present(const xiaotai_ui_host_t *host, const uint8_t *frame, size_t size)
{
    const xiaotai_err_t result = __real_xiaotai_ui_host_present(host, frame, size);
    if (result == XIAOTAI_OK && atomic_load(&s_storage_error_presented)) {
        unsigned lit = 0;
        for (size_t i = 0; i < size; ++i) if (frame[i]) ++lit;
        if (lit < size) atomic_store(&s_storage_frame_lit, lit);
    }
    return result;
}
#endif

xiaotai_err_t __real_xiaotai_kv_backend_init(void);
xiaotai_err_t __wrap_xiaotai_kv_backend_init(void)
{
    if (s_nvs_init_error == XIAOTAI_OK) return __real_xiaotai_kv_backend_init();
    ++s_nvs_init_calls;
    return s_nvs_init_error;
}

xiaotai_err_t __real_xiaotai_app_companion_init(void);
xiaotai_err_t __wrap_xiaotai_app_companion_init(void)
{
    if (!s_fail_app_init) return __real_xiaotai_app_companion_init();
    ++s_app_init_calls;
    return XIAOTAI_ERR_NO_MEM;
}

typedef struct {
    void (*fn)(void *);
    void *ctx;
    xiaotai_signal_t done;
} owner_job_t;

static void invoke(void *ctx)
{
    owner_job_t *job = ctx;
    assert(xiaotai_on_owner());
    if (job->fn) job->fn(job->ctx);
    xiaotai_signal_give(&job->done);
}

void product_on_owner(void (*fn)(void *), void *ctx)
{
    owner_job_t job = {.fn = fn, .ctx = ctx};
    assert(!xiaotai_on_owner());
    assert(xiaotai_signal_init(&job.done) == XIAOTAI_OK);
    assert(xiaotai_run(invoke, &job) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&job.done, 10000));
    xiaotai_signal_deinit(&job.done);
}

static void saved_network(void)
{
    static const struct {
        uint8_t version;
        xiaotai_net_credentials_t credentials;
    } record = {.version = 1, .credentials = {.ssid = PRODUCT_TEST_SSID, .password = PRODUCT_TEST_PASSWORD}};
    assert(sizeof(record) == 99);
    const void *data = PRODUCT_HOST_NET_RAW ? (const void *)&record.credentials : (const void *)&record;
    const size_t size = PRODUCT_HOST_NET_RAW ? sizeof(record.credentials) : sizeof(record);
    assert(xiaotai_kv_set(PRODUCT_HOST_NET_SPACE, PRODUCT_HOST_NET_RAW ? "wifi" : "credentials",
                               data, size, XIAOTAI_KV_WAIT_MAX_MS) == XIAOTAI_OK);
}

/* A device already running this firmware: a stored volume there is
 * expressed for the board's volume ceiling (xiaotai/modules/settings.h,
 * "vol_ceiling" in the SDK namespace as a 4-byte integer). */
static void current_volume_ceiling(void)
{
    const int32_t ceiling = PRODUCT_HOST_VOLUME_CEILING;
    if (ceiling >= 100) return;
    assert(xiaotai_kv_set("xt_settings", "vol_ceiling", &ceiling, sizeof(ceiling), XIAOTAI_KV_WAIT_MAX_MS) ==
           XIAOTAI_OK);
}

static void start_selected(void)
{
    const xiaotai_err_t result = xiaotai_start(&xiaotai_product_config);
    if (result != XIAOTAI_OK) fprintf(stderr, "%s: startup failed (%d)\n", XIAOTAI_PRODUCT_NAME, result);
    assert(result == XIAOTAI_OK);
}

static void *start_on_caller(void *unused)
{
    (void)unused;
    start_selected();
    atomic_store(&s_start_returned, true);
    return NULL;
}

static void check_owner_identity(void *unused)
{
    (void)unused;
    const bool borrowed = XIAOTAI_FRAMEWORK_OWNER_ON_MAIN;
    assert(xiaotai_framework_owner_on_caller() == borrowed);
    assert((pthread_equal(s_start_thread, pthread_self()) != 0) == borrowed);
}

void product_check_owner(void)
{
    product_on_owner(check_owner_identity, NULL);
    assert(!atomic_load(&s_start_returned));
}

static unsigned started_events(uint32_t *after)
{
    xiaotai_eventlog_entry_t events[32];
    unsigned started = 0;
    size_t count;
    do {
        count = xiaotai_eventlog_read(*after, events, sizeof(events) / sizeof(events[0]));
        for (size_t i = 0; i < count; ++i) {
            *after = events[i].sequence;
            if (!strcmp(events[i].source, "framework") && !strcmp(events[i].label, "started")) ++started;
        }
    } while (count == sizeof(events) / sizeof(events[0]));
    return started;
}

static void unexpected_owner_job(void *ctx)
{
    (void)ctx;
    assert(false && "failed startup accepted an owner job");
}

static void assert_no_transport_start(void)
{
#if PRODUCT_TEST_DISCOVERY_ENDPOINT
    xiaotai_transport_status_t transport;
    xiaotai_transport_status(&transport);
    assert(!transport.starts && !transport.start_reserve_held && !transport.connect_reserve_held);
#endif
}

#if PRODUCT_TEST_START_GUIDE
static void assert_startup_guide(xiaotai_err_t error, bool panel_fault)
{
    const xt_oled_fake_t *panel = xt_oled_fake_state();
    assert(!product_restarted());
    unsigned draws = 0;
    for (size_t i = 0; i < panel->count; ++i) {
        if (panel->log[i] == XT_OLED_DRAW) ++draws;
    }
    if (panel_fault) {
        assert(!panel->on && !panel->frame_bytes && panel->count > 0 && !draws);
        printf("owner startup fault: error=%d panel_failure=1 framed=0 network=0 started=0 restart=0\n", error);
        return;
    }
    assert(panel->on && panel->frame_bytes == sizeof(panel->frame) && draws == 2);
    unsigned lit[3] = {0};
    uint32_t code = 2166136261u;
    for (size_t page = 0; page < 8U; ++page) {
        for (size_t x = 0; x < 128U; ++x) {
            const uint8_t bits = panel->frame[page * 128U + x];
            if (page >= 6U) code = (code ^ bits) * 16777619u;
            if (!bits) continue;
            assert(page <= 1U || (page >= 3U && page <= 4U) || page >= 6U);
            ++lit[page <= 1U ? 0 : page <= 4U ? 1 : 2];
        }
    }
    assert(lit[0] > 0 && lit[1] > 0 && lit[2] > 0);
    /* Hash the error line only: ERR 0101, ERR 0107, ERR FFFFFFFF. */
    const uint32_t expected = error == XIAOTAI_ERR_NO_MEM ? 0x792526d4u
                            : error == XIAOTAI_ERR_TIMEOUT ? 0x82d5bcb8u : 0x04106644u;
    assert(code == expected);
    printf("owner startup fault: error=%d framed=1 title=%u message=%u code=%08x network=0 started=0 restart=0\n",
           error, lit[0], lit[1], (unsigned)code);
}
#endif

static int start_owner_fault(xiaotai_err_t error, bool panel_fault)
{
#if PRODUCT_TEST_START_GUIDE
    xt_oled_fake_reset();
    if (panel_fault) xt_oled_fake_fail(XT_OLED_NEW_PANEL, XIAOTAI_FAIL);
#else
    assert(!panel_fault && xiaotai_product_config.start_failure_restart_ms != XIAOTAI_START_HALT);
    product_expect_restart();
#endif
    s_fault_caller = pthread_self();
    s_owner_signal_error = error;
    atomic_store(&s_fail_owner_signal, true);
    assert(xiaotai_start(&xiaotai_product_config) == error);
    uint32_t after = 0;
    xiaotai_net_host_record_t record;
    assert(s_owner_signal_faults == 1);
    assert(xiaotai_hal_needs() == XIAOTAI_BOARD_DECLARED_NEEDS);
    assert(!started_events(&after) && xiaotai_net_host_records(&record, 1) == 0);
    assert_no_transport_start();
    assert(xiaotai_run(unexpected_owner_job, NULL) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_start(&xiaotai_product_config) == error && s_owner_signal_faults == 1);
#if PRODUCT_TEST_START_GUIDE
    assert_startup_guide(error, panel_fault);
#else
    assert(product_restarted());
    printf("PASS: %s %s owner startup fault: original error, no start event or network, one restart\n",
           XIAOTAI_PRODUCT_NAME, XIAOTAI_FRAMEWORK_OWNER_ON_MAIN ? "caller" : "task");
#endif
    return 0;
}

static int start_nvs_fault(xiaotai_err_t error)
{
    /* Seed storage before the failing boot without warming the KV worker. */
    xiaotai_kv_handle_t handle = 0;
    assert(xiaotai_kv_backend_open("product", true, &handle) == XIAOTAI_OK);
    assert(xiaotai_kv_backend_set_int(handle, "volume", XIAOTAI_KV_U8, 5) == XIAOTAI_OK);
    assert(xiaotai_kv_backend_commit(handle) == XIAOTAI_OK);
    xiaotai_kv_backend_close(handle);
    s_nvs_init_error = error;
    product_expect_restart();
    assert(xiaotai_start(&xiaotai_product_config) == error);
    uint32_t after = 0;
    xiaotai_net_host_record_t net;
    assert(product_restarted() && s_nvs_init_calls == 1);
    assert(!started_events(&after) && !xiaotai_net_host_records(&net, 1));
    assert_no_transport_start();
    assert(xiaotai_run(unexpected_owner_job, NULL) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_start(&xiaotai_product_config) == error && s_nvs_init_calls == 1);
    int64_t volume = 0;
    assert(xiaotai_kv_backend_open("product", false, &handle) == XIAOTAI_OK);
    assert(xiaotai_kv_backend_get_int(handle, "volume", XIAOTAI_KV_U8, &volume) == XIAOTAI_OK && volume == 5);
    xiaotai_kv_backend_close(handle);
    printf("NVS init: original error=%d attempts=%u network=0 start=0 volume_u8=%lld\n",
           error, s_nvs_init_calls, (long long)volume);
    return 0;
}

static int start_app_fault(void)
{
    assert(xiaotai_kv_set_u8("product", "volume", 5, XIAOTAI_KV_WAIT_MAX_MS) == XIAOTAI_OK);
    s_fail_app_init = true;
    product_expect_restart();
    assert(xiaotai_start(&xiaotai_product_config) == XIAOTAI_ERR_NO_MEM);
    uint32_t after = 0;
    xiaotai_net_host_record_t net;
    assert(product_restarted() && s_app_init_calls == 1);
    assert(!started_events(&after) && !xiaotai_net_host_records(&net, 1));
    assert_no_transport_start();
    assert(xiaotai_run(unexpected_owner_job, NULL) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_start(&xiaotai_product_config) == XIAOTAI_ERR_NO_MEM && s_app_init_calls == 1);
    uint8_t volume = 0;
    assert(xiaotai_kv_get_u8("product", "volume", &volume, XIAOTAI_KV_WAIT_MAX_MS) == XIAOTAI_OK && volume == 5);
    printf("companion init: error=257 attempts=%u network=0 start=0 volume_u8=%u restart=1\n",
           s_app_init_calls, (unsigned)volume);
    return 0;
}

static void start_product(void)
{
    uint32_t after = 0;
    unsigned started = 0;
    if (XIAOTAI_FRAMEWORK_OWNER_ON_MAIN) {
        assert(pthread_create(&s_start_thread, NULL, start_on_caller, NULL) == 0);
        const int64_t deadline = xiaotai_time_us() + INT64_C(10000000);
        do {
            assert(!atomic_load(&s_start_returned));
            started = started_events(&after);
            if (started) break;
            xiaotai_delay_ms(1);
        } while (xiaotai_time_us() < deadline);
        assert(started == 1U);
    } else {
        s_start_thread = pthread_self();
        start_selected();
    }
    product_check_owner();
    started += started_events(&after);
    assert(started == 1U);
    assert(xiaotai_hal_needs() == XIAOTAI_BOARD_DECLARED_NEEDS);
    /* The volume ceiling board.json declares is the one the output reports. */
    const xiaotai_audio_output_t *output = xiaotai_hal_audio_output(0);
    assert(!output || (output->info.volume_max_percent && output->info.volume_max_percent < 100U
                       ? output->info.volume_max_percent : 100U) == PRODUCT_HOST_VOLUME_CEILING);
    xiaotai_media_info_t info;
    xiaotai_media_info(&info);
    const uint32_t needs = xiaotai_hal_needs();
    if ((needs & (XIAOTAI_NEED_PCM_IN | XIAOTAI_NEED_AUDIO_OUT)) ==
        (XIAOTAI_NEED_PCM_IN | XIAOTAI_NEED_AUDIO_OUT)) {
        assert(info.capture && info.capture_pcm && info.capture_rate_hz && info.playback);
    }
    printf("%s: system.started needs=0x%x capture=%u pcm=%u rate=%u\n", XIAOTAI_PRODUCT_NAME,
           (unsigned)needs, (unsigned)info.capture, (unsigned)info.capture_pcm, (unsigned)info.capture_rate_hz);
}

#if PRODUCT_TEST_STORAGE_RECOVERY
static int start_nvs_local(xiaotai_err_t error)
{
    static const uint8_t saved[] = {2, 65, 12, 1};
    xiaotai_kv_handle_t handle;
    assert(xiaotai_kv_backend_open("xmini_ui", true, &handle) == XIAOTAI_OK);
    assert(xiaotai_kv_backend_set(handle, "audio", false, saved, sizeof(saved)) == XIAOTAI_OK);
    assert(xiaotai_kv_backend_commit(handle) == XIAOTAI_OK);
    xiaotai_kv_backend_close(handle);
    xiaotai_kv_host_counts_t before, after;
    xiaotai_kv_host_counts(&before);
    s_nvs_init_error = error;
    assert(xiaotai_product_config.local_on_storage_boot_failure);
    start_product();

    const bool check_audio_defaults = !strcmp(XIAOTAI_PRODUCT_NAME, "xiaotai-xmini-c3-v3");
    xiaotai_media_status_t media;
    xiaotai_ui_info_t ui;
    const int64_t until = xiaotai_time_us() + INT64_C(3000000);
    do {
        xiaotai_media_status(&media);
        assert(xiaotai_ui_inspect(0, &ui) == XIAOTAI_OK);
        /* The media parts are ready before the worker applies their controls. */
        const bool audio_applied = !check_audio_defaults ||
            (media.volume_applied == 40 && media.input_gain_db == 24);
        if (media.ready && media.startup_complete && audio_applied &&
            ui.frames && atomic_load(&s_storage_frame_lit)) break;
        xiaotai_delay_ms(5);
    } while (xiaotai_time_us() < until);
    assert(media.ready && media.startup_complete && media.startup_error == XIAOTAI_OK &&
           ui.ready && ui.frames && !ui.present_fault && atomic_load(&s_storage_frame_lit));
    if (check_audio_defaults) {
        int32_t value;
        assert(xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_VOLUME, &value) == XIAOTAI_OK && value == 40);
        assert(xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_MIC_GAIN, &value) == XIAOTAI_OK && value == 24);
        assert(media.volume_applied == 40 && media.input_gain_db == 24);
    }
    xiaotai_net_host_record_t net;
    xiaotai_platform_status_t cloud;
    xiaotai_transport_status_t rtc;
    xiaotai_platform_status(&cloud);
    xiaotai_transport_status(&rtc);
    assert(!cloud.discovered && !cloud.signaling && !cloud.authorized && cloud.last_error == error);
    assert(!rtc.starts && !rtc.start_reserve_held && !rtc.connect_reserve_held);
    assert(!xiaotai_net_driver_bound() && !xiaotai_net_provisioning_boot());
    assert(xiaotai_net_retry() == XIAOTAI_ERR_NOT_SUPPORTED &&
           xiaotai_net_reprovision(true) == XIAOTAI_ERR_NOT_SUPPORTED && !xiaotai_net_host_records(&net, 1));
    assert(xiaotai_storage_boot_error() == error && s_nvs_init_calls == 1 &&
           !xiaotai_restart_pending() && !product_restarted());
    assert(xiaotai_kv_set_u8("xmini_ui", "audio", 99, 1000) == XIAOTAI_ERR_INVALID_STATE &&
           xiaotai_kv_erase_space("xmini_ui", 1000) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_kv_host_counts(&after);
    assert(before.opens == after.opens && before.writes == after.writes && before.commits == after.commits);
    uint8_t raw[sizeof(saved)];
    size_t size = sizeof(raw);
    assert(xiaotai_kv_backend_open("xmini_ui", false, &handle) == XIAOTAI_OK);
    assert(xiaotai_kv_backend_get_blob_strict(handle, "audio", raw, &size) == XIAOTAI_OK &&
           size == sizeof(saved) && !memcmp(saved, raw, sizeof(saved)));
    xiaotai_kv_backend_close(handle);
    printf("NVS local: error=%d frame=%u lit=%u audio=%u/%d network=0 cloud=0 write=0 restart=0\n",
           error, (unsigned)ui.frames, atomic_load(&s_storage_frame_lit), (unsigned)media.volume_applied,
           (int)media.input_gain_db);
    xiaotai_reset();
    if (XIAOTAI_FRAMEWORK_OWNER_ON_MAIN)
        assert(pthread_join(s_start_thread, NULL) == 0 && atomic_load(&s_start_returned));
    return 0;
}
#endif

#if PRODUCT_TEST_STACK_BUSINESS
static atomic_uint call_release_requests;

static void call_release_http(void *ctx, const xiaotai_http_host_request_t *request,
                              xiaotai_http_host_reply_t *reply)
{
    (void)ctx;
    if (request->method == XIAOTAI_HTTP_POST &&
        (strstr(request->url, "/v1/call/") || strstr(request->url, "/v1/voip/")) &&
        (strstr(request->url, "/hangup") || strstr(request->url, "/cancel") || strstr(request->url, "/reject"))) {
        ++call_release_requests;
    }
    reply->status = 404;
}

typedef struct {
    xiaotai_ai_talk_status_t talk;
    uint32_t activity;
} ai_owner_snapshot_t;

static void ai_owner_snapshot(void *ctx)
{
    ai_owner_snapshot_t *snapshot = ctx;
    xiaotai_ai_talk_status(&snapshot->talk);
    snapshot->activity = xiaotai_activity_current(NULL);
}

static void hangup_without_owner(void *unused)
{
    (void)unused;
    assert(xiaotai_call_hangup(0) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_wechat_hangup(0) == XIAOTAI_ERR_INVALID_STATE);
}

static void discard_reply(void *unused, const char *line) { (void)unused; (void)line; }

static void check_hangup_during_ai(uint32_t session)
{
    const xiaotai_reply_t reply = {.write = discard_reply};
    ai_owner_snapshot_t before, after;
    xiaotai_transport_status_t transport_before, transport_after;
    xiaotai_media_status_t media;
    assert(xiaotai_transport_host_settle(3000));
    product_on_owner(ai_owner_snapshot, &before);
    xiaotai_transport_status(&transport_before);
    xiaotai_media_status(&media);
    assert(before.activity && before.talk.session == session && before.talk.link &&
           before.talk.phase == XIAOTAI_AI_TALK_LISTENING && media.active &&
           media.generation == before.talk.link && transport_before.links_open);
    const uint32_t wire_commands = xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_COMMAND);
    const uint32_t disconnects = xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_DISCONNECT);

    product_on_owner(hangup_without_owner, NULL);
    assert(xiaotai_command_run("AT+CALL HANGUP", &reply, 3000) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_transport_host_settle(3000));
    product_on_owner(ai_owner_snapshot, &after);
    xiaotai_transport_status(&transport_after);
    xiaotai_media_status(&media);
    assert(after.activity == before.activity && after.talk.session == session &&
           after.talk.phase == XIAOTAI_AI_TALK_LISTENING && after.talk.link == before.talk.link &&
           media.active && media.generation == before.talk.link);
    assert(transport_before.links_open == transport_after.links_open &&
           transport_before.service_requests == transport_after.service_requests &&
           transport_before.service_failures == transport_after.service_failures &&
           wire_commands == xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_COMMAND) &&
           disconnects == xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_DISCONNECT) &&
           !atomic_load(&call_release_requests));
    printf("%s: no call owner; AI activity=%u session=%u link=%u and media remain active; no release\n",
           XIAOTAI_PRODUCT_NAME, (unsigned)after.activity, (unsigned)session, (unsigned)after.talk.link);
}
#endif

#if PRODUCT_TEST_CONSOLE
static void saw_state(void *ctx, const char *text)
{
    if (strstr(text, "+STATE: started=1")) *(bool *)ctx = true;
}

#if PRODUCT_TEST_XMINI_DIAGNOSTICS_AT
void product_xmini_diagnostics_at(bool cancel);
#endif

static void check_console(bool cancel)
{
    bool state = false;
    const xiaotai_reply_t reply = {.write = saw_state, .ctx = &state};
    if (PRODUCT_HOST_AT_TABLES) {
        assert(xiaotai_command_run("AT+STATE", &reply, 1000) == XIAOTAI_OK && state);
    } else {
        assert(xiaotai_command_run("AT+STATE", &reply, 1000) == XIAOTAI_ERR_NOT_FOUND && !state);
    }
    state = false;
    xiaotai_diag_report("state", &reply);
    assert(state);

    static const char command[] = "AT+CLI\r\n"; /* Also allowed on a locked console. */
    char wire[2048] = {0};
    const bool active = PRODUCT_HOST_PLATFORM_CONSOLE != 0;
    assert(xiaotai_console_host_feed(command, sizeof(command) - 1) == (active ? sizeof(command) - 1 : 0));
    if (!active) {
        assert(!xiaotai_at_active());
        assert(!xiaotai_console_host_take(wire, sizeof(wire)));
        return;
    }
    size_t used = 0;
    const int64_t deadline = xiaotai_time_us() + INT64_C(2000000);
    do {
        used += xiaotai_console_host_take(wire + used, sizeof(wire) - 1 - used);
        if (strstr(wire, "\r\nOK\r\n")) break;
        xiaotai_delay_ms(1);
    } while (xiaotai_time_us() < deadline);
    assert(strstr(wire, "+CLI: enabled=") && strstr(wire, "\r\nOK\r\n"));
#if PRODUCT_TEST_XMINI_DIAGNOSTICS_AT
    product_xmini_diagnostics_at(cancel);
#else
    (void)cancel;
#endif
}
#endif

#if PRODUCT_TEST_RECOGNITION_CONFIG
void product_recognition_config_prepare(void);
void product_recognition_config_run(void);
#endif

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *scenario = argv[1];
#if PRODUCT_TEST_CONSOLE
#if PRODUCT_TEST_XMINI_DIAGNOSTICS_AT
    const bool console_cancel = !strcmp(scenario, "console-cancel");
    if (console_cancel) {
        assert(xiaotai_kv_init() == XIAOTAI_OK);
        assert(xiaotai_kv_set_u8(PRODUCT_HOST_NET_SPACE, "wifi_provision", 1, XIAOTAI_KV_WAIT_MAX_MS) == XIAOTAI_OK);
    }
#else
    const bool console_cancel = false;
#endif
    const bool console = !strcmp(scenario, "console") || console_cancel;
    if (console) xiaotai_console_host_pipe();
#endif
    bool provisioned = strcmp(scenario, "provision") && strcmp(scenario, "ledger-provision");
#if PRODUCT_TEST_START_GUIDE
    if (!strcmp(scenario, "start-owner-fault-generic")) provisioned = false;
#endif
#if PRODUCT_TEST_SCREENS
    const bool screens = !strncmp(scenario, "screens", 7);
    if (!strcmp(scenario, "screens-setup")) provisioned = false;
#endif
#if PRODUCT_TEST_REPROVISION_KEYS
    const bool partial_cold = !strcmp(scenario, "reprovision-partial-cold");
    const bool partial_store = partial_cold || !strcmp(scenario, "reprovision-partial-write");
    if (partial_store) provisioned = false;
#endif
#if PRODUCT_TEST_XMINI_PREFS
    if (!strncmp(scenario, "xmini-prefs-portal-", sizeof("xmini-prefs-portal-") - 1U)) provisioned = false;
#endif
#if PRODUCT_TEST_ENVIRONMENT_ADMISSION
    const bool env_verifying = !strcmp(scenario, "environment-provision-verifying");
    const bool env_timeout = !strcmp(scenario, "environment-provision-timeout");
    const bool env_admission = env_verifying || env_timeout || !strcmp(scenario, "environment-provision-idle");
    if (env_admission) provisioned = false;
#endif
#if PRODUCT_TEST_LEDGER
    const bool ledger = !strcmp(scenario, "ledger-session") || !strcmp(scenario, "ledger-provision");
    if (ledger) product_ledger_measurement_start();
#endif
    /* Do not warm up the backend on the host main thread for the cold boot.
     * The first storage init there must come from the real framework owner. */
    const bool nvs_no_pages = !strcmp(scenario, "start-nvs-no-free-pages");
    const bool nvs_new_version = !strcmp(scenario, "start-nvs-new-version");
    if (strcmp(scenario, "provision") && !nvs_no_pages && !nvs_new_version) {
        assert(xiaotai_kv_init() == XIAOTAI_OK);
        if (provisioned) saved_network();
        current_volume_ceiling();
    }
#if PRODUCT_TEST_REPROVISION_KEYS
    if (partial_store) product_reprovision_partial_prepare(partial_cold);
#endif
#if PRODUCT_TEST_XMINI_PREFS
    const bool prefs_revert = product_xmini_prefs_prepare(scenario);
#endif
#if PRODUCT_TEST_BINDING_GRANT
    const bool reload_grant = !strcmp(scenario, "binding-grant-reload");
    const bool binding_grant_test = reload_grant || !strcmp(scenario, "binding-grant-save");
    if (binding_grant_test) product_binding_grant_prepare(reload_grant);
#if PRODUCT_TEST_BINDING_SCOPE
    static const char binding_scope_prefix[] = "binding-scope-";
    const bool binding_scope_test = !strncmp(scenario, binding_scope_prefix, sizeof(binding_scope_prefix) - 1U);
    if (binding_scope_test) product_binding_scope_prepare(scenario + sizeof(binding_scope_prefix) - 1U);
#endif
#endif
#if PRODUCT_TEST_PROFILE_VIDEO
    const bool profile_video = !strcmp(scenario, "profile-video");
    if (profile_video) product_profile_video_prepare(PRODUCT_TEST_PROFILE_EXPECT_VIDEO, PRODUCT_TEST_PROFILE_BOARD);
#endif
    product_board_prepare();
    product_portal_prepare();
#if PRODUCT_TEST_STORAGE_RECOVERY
    if (nvs_no_pages || nvs_new_version)
        return start_nvs_local(nvs_no_pages ? 0x110D : 0x1110);
#endif
    if (nvs_no_pages || nvs_new_version)
        return start_nvs_fault(nvs_no_pages ? 0x110D : 0x1110);
    if (!strcmp(scenario, "start-app-fault")) return start_app_fault();
    if (!strcmp(scenario, "start-owner-fault")) return start_owner_fault(XIAOTAI_ERR_NO_MEM, false);
#if PRODUCT_TEST_START_GUIDE
    if (!strcmp(scenario, "start-owner-fault-timeout")) return start_owner_fault(XIAOTAI_ERR_TIMEOUT, false);
    if (!strcmp(scenario, "start-owner-fault-generic")) return start_owner_fault(XIAOTAI_FAIL, false);
    if (!strcmp(scenario, "start-owner-fault-no-display")) return start_owner_fault(XIAOTAI_ERR_NO_MEM, true);
#endif
    const xiaotai_net_host_step_t start = {
        .result = XIAOTAI_OK, .event_count = 1,
        .events = {{.kind = XIAOTAI_NET_DRIVER_STARTED}},
    };
    assert(xiaotai_net_host_step(XIAOTAI_NET_HOST_START, &start) == XIAOTAI_OK);
#if PRODUCT_TEST_LEDGER
    if (ledger) product_ledger_prepare(provisioned);
#endif
#if PRODUCT_TEST_REPROVISION_KEYS
    const bool reprovision_keys = !strcmp(scenario, "reprovision-keys");
    const char reprovision_barrier_prefix[] = "reprovision-barrier-";
    const bool reprovision_barrier = !strncmp(scenario, reprovision_barrier_prefix, sizeof(reprovision_barrier_prefix) - 1U);
    const bool runtime_wifi_audio = !strcmp(scenario, "runtime-wifi-audio");
    const bool runtime_wifi_cancel_scope = !strcmp(scenario, "runtime-wifi-cancel-scope");
    if (reprovision_keys || runtime_wifi_cancel_scope) product_reprovision_keys_prepare();
    if (reprovision_barrier || runtime_wifi_audio) product_platform_prepare(true);
#endif
#if PRODUCT_TEST_KEY_AUDIO_SETTINGS
    const char audio_settings_prefix[] = "key-audio-settings-";
    const bool call_cleanup = !strcmp(scenario, "call-cleanup-quota");
    if (call_cleanup) product_call_cleanup_prepare();
    const bool audio_settings = !strncmp(scenario, audio_settings_prefix, sizeof(audio_settings_prefix) - 1U);
    const bool unassigned_room = !strcmp(scenario, "room-unassigned-key-menu");
    const char empty_contacts_prefix[] = "contacts-empty-refresh-";
    const bool empty_contacts = !strncmp(scenario, empty_contacts_prefix, sizeof(empty_contacts_prefix) - 1U);
    const char initial_view_prefix[] = "contacts-initial-view-";
    const bool initial_view = !strncmp(scenario, initial_view_prefix, sizeof(initial_view_prefix) - 1U);
    if (audio_settings) product_platform_prepare(true);
    if (unassigned_room) product_unassigned_room_prepare();
    if (empty_contacts) product_contacts_empty_prepare();
    if (initial_view) product_contacts_initial_view_prepare(scenario + sizeof(initial_view_prefix) - 1U);
#endif
#if PRODUCT_TEST_AI_MONO
    const bool home_transport_enabled = !strcmp(scenario, "home-transport-label");
    const bool home_transport = home_transport_enabled || !strcmp(scenario, "home-transport-label-disabled");
    if (home_transport) product_platform_prepare(true);
    const bool ai_mono_elapsed = !strcmp(scenario, "ai-mono-elapsed");
    if (ai_mono_elapsed) product_ai_mono_prepare();
#endif
#if PRODUCT_TEST_WT99
    const bool reselect_enabled = !strcmp(scenario, "wt99-preferences-reselect");
    const bool reselect = reselect_enabled || !strcmp(scenario, "wt99-preferences-reselect-disabled");
    if (reselect) product_preferences_reselect_prepare();
    const bool reselect_soak = !strcmp(scenario, "wt99-preferences-reselect-soak");
    if (reselect_soak) product_preferences_reselect_soak_prepare();
    if (!strcmp(scenario, "presenter") || !strcmp(scenario, "wt99-binding")) {
        product_platform_prepare(!strcmp(scenario, "presenter"));
        product_wt99_prepare();
    }
#endif
#if PRODUCT_TEST_STACK_BUSINESS
    const bool ai_hangup = !strcmp(scenario, "ai-ignore-hangup");
    const bool business = ai_hangup || !strcmp(scenario, "stack-business");
    if (business) product_platform_prepare(true);
    if (ai_hangup) product_platform_http_responder(call_release_http, NULL);
#endif
#if PRODUCT_TEST_AI_STREAM_LIFECYCLE
    const bool ai_pending_maintenance = !strcmp(scenario, "ai-pending-maintenance");
    const bool ai_stream_lifecycle = ai_pending_maintenance || !strcmp(scenario, "ai-stream-lifecycle");
    if (ai_stream_lifecycle) product_platform_prepare(true);
#endif
#if PRODUCT_TEST_ENVIRONMENT_BINDING
    const bool env_binding = !strncmp(scenario, "environment-binding-", sizeof("environment-binding-") - 1);
    if (env_binding) product_platform_prepare(true);
#endif
#if PRODUCT_TEST_ROOM_MAINTENANCE
    const bool room_maintenance = !strcmp(scenario, "room-maintenance");
    if (room_maintenance) product_room_maintenance_prepare();
#endif
#if PRODUCT_TEST_PREFERENCES_FAILURE
    const bool unrelated_edit = !strcmp(scenario, "preferences-unrelated-write");
    const bool preferences_failure = unrelated_edit || !strcmp(scenario, "preferences-write-failure");
    const bool preferences_cold = product_preferences_cold_prepare(scenario);
    const bool preferences_choice_invalid = !strcmp(scenario, "preferences-choice-invalid");
    const bool preferences_choices = preferences_choice_invalid || !strcmp(scenario, "preferences-choice-reselect");
    if (preferences_choices) product_preferences_choices_prepare();
    if (preferences_failure) {
        assert(xiaotai_kv_set_int("product", "volume", XIAOTAI_KV_U8, 5, 3000) == XIAOTAI_OK);
        assert(xiaotai_kv_set_int("product", "mic_mute", XIAOTAI_KV_U8, 0, 3000) == XIAOTAI_OK);
        product_platform_prepare(true);
    }
#endif
#if PRODUCT_TEST_AI_FIXED_RX
    const bool ai_fixed_rx = !strcmp(scenario, "ai-fixed-rx");
    if (ai_fixed_rx) product_platform_prepare(true);
#endif
#if PRODUCT_TEST_WECHAT_RX
    const bool wechat_rx = !strcmp(scenario, "wechat-rx");
    const bool wechat_answer_strict = !strcmp(scenario, "wechat-answer-strict");
    const bool wechat_answer = wechat_answer_strict || !strcmp(scenario, "wechat-answer-opaque");
    if (wechat_rx || wechat_answer) product_platform_prepare(true);
#endif
#if PRODUCT_TEST_WECHAT_AUTHORIZATION
    const bool wechat_auth_back = !strcmp(scenario, "wechat-authorization-back");
    const bool wechat_authorization = wechat_auth_back || !strcmp(scenario, "wechat-authorization-double");
    if (wechat_authorization) product_wechat_authorization_prepare();
#endif
#if PRODUCT_TEST_PEER_CLOSE
    const char peer_close_prefix[] = "peer-close-";
    const bool peer_close = !strncmp(scenario, peer_close_prefix, sizeof(peer_close_prefix) - 1);
    if (peer_close) product_peer_close_prepare(scenario + sizeof(peer_close_prefix) - 1);
#endif
#if PRODUCT_TEST_CLOCK_GATE
    const bool clock_bound = !strcmp(scenario, "clock-gate-bound");
    const bool clock_test = clock_bound || !strcmp(scenario, "clock-gate-binding");
    if (clock_test) product_clock_gate_prepare(clock_bound);
    const char journal_prefix[] = "environment-journal-";
    const bool journal = !strncmp(scenario, journal_prefix, sizeof(journal_prefix) - 1);
    if (journal) {
        product_platform_prepare(true);
        product_environment_journal_prepare(scenario + sizeof(journal_prefix) - 1);
    }
#endif
#if PRODUCT_TEST_DISCOVERY_ENDPOINT
    const bool endpoint = !strncmp(scenario, "endpoint-", sizeof("endpoint-") - 1);
    if (endpoint) product_endpoint_prepare(scenario + sizeof("endpoint-") - 1);
#endif
#if PRODUCT_TEST_RTC_RELEASE
    const bool rtc_release = !strncmp(scenario, "rtc-release", sizeof("rtc-release") - 1U);
    if (rtc_release) product_rtc_release_prepare(strcmp(scenario, "rtc-release-low-memory") != 0);
#endif
#if PRODUCT_TEST_RECOGNITION_HTTP
    const bool recognition_http = !strcmp(scenario, "recognition-query-wire");
    if (recognition_http) product_recognition_http_prepare();
    const bool recognition_upload_probe = !strncmp(scenario, "recognition-upload-", sizeof("recognition-upload-") - 1U);
    if (recognition_upload_probe) product_recognition_upload_probe_prepare(scenario + sizeof("recognition-upload-") - 1U);
    const bool recognition_audio_saturation = !strcmp(scenario, "recognition-audio-saturation");
    if (recognition_audio_saturation) product_recognition_audio_saturation_prepare();
#endif
#if PRODUCT_TEST_RECOGNITION_CONFIG
    const bool recognition_config_contract = !strcmp(scenario, "recognition-config-contract");
    if (recognition_config_contract) product_recognition_config_prepare();
#endif
#if PRODUCT_TEST_SCREENS
    if (screens) product_screens_prepare(provisioned);
#endif
    start_product();
    int result = 0;
    if (!strcmp(scenario, "start") || !strcmp(scenario, "provision")) {
        assert(xiaotai_net_host_wait_calls(provisioned ? XIAOTAI_NET_HOST_JOIN : XIAOTAI_NET_HOST_AP_START, 1, 10000));
    }
#if PRODUCT_TEST_SCREENS
    else if (screens) product_screens_run(provisioned);
#endif
#if PRODUCT_TEST_RECOGNITION_CONFIG
    else if (recognition_config_contract) product_recognition_config_run();
#endif
#if PRODUCT_TEST_XMINI_PREFS
    else if (prefs_revert) result = product_xmini_prefs_revert();
#endif
#if PRODUCT_TEST_BINDING_GRANT
    else if (binding_grant_test) product_binding_grant(reload_grant);
#if PRODUCT_TEST_BINDING_SCOPE
    else if (binding_scope_test) product_binding_scope();
#endif
#endif
#if PRODUCT_TEST_PROFILE_VIDEO
    else if (profile_video) product_profile_video_run();
#endif
#if PRODUCT_TEST_CONSOLE
    else if (console) check_console(console_cancel);
#endif
#if PRODUCT_TEST_ARBITRATION
    else if (!strcmp(scenario, "arbitration")) result = product_arbitration_run();
#endif
#if PRODUCT_TEST_ENVIRONMENT_BINDING
    else if (env_binding) product_environment_binding(scenario + sizeof("environment-binding-") - 1);
#endif
#if PRODUCT_TEST_ROOM_MAINTENANCE
    else if (room_maintenance) product_room_maintenance();
#endif
#if PRODUCT_TEST_PREFERENCES_FAILURE
    else if (preferences_failure) result = product_preferences_failure(unrelated_edit);
    else if (preferences_cold) result = product_preferences_cold_run();
    else if (preferences_choices) result = product_preferences_choices_run(preferences_choice_invalid);
#endif
#if PRODUCT_TEST_STACK_BUSINESS
    else if (business) {
        xiaotai_media_info_t media;
        xiaotai_media_info(&media);
        assert(!media.mixer);
        product_platform_online();
        product_ai_prepare();
        const uint32_t session = product_ai_request();
        product_ai_connect(session);
        if (ai_hangup) check_hangup_during_ai(session);
        product_ai_stop(session);
    }
#endif
#if PRODUCT_TEST_AI_FIXED_RX
    else if (ai_fixed_rx) product_ai_fixed_receive();
#endif
#if PRODUCT_TEST_AI_STREAM_LIFECYCLE
    else if (ai_stream_lifecycle) product_ai_stream_lifecycle(ai_pending_maintenance);
#endif
#if PRODUCT_TEST_WECHAT_RX
    else if (wechat_rx) product_wechat_receive();
    else if (wechat_answer) product_wechat_answer_payload(wechat_answer_strict);
#endif
#if PRODUCT_TEST_WECHAT_AUTHORIZATION
    else if (wechat_authorization) product_wechat_authorization(wechat_auth_back);
#endif
#if PRODUCT_TEST_PEER_CLOSE
    else if (peer_close) product_peer_close_run(scenario + sizeof(peer_close_prefix) - 1);
#endif
#if PRODUCT_TEST_CLOCK_GATE
    else if (clock_test) product_clock_gate(clock_bound);
    else if (journal) product_environment_journal();
#endif
#if PRODUCT_TEST_DISCOVERY_ENDPOINT
    else if (endpoint) product_endpoint(scenario + sizeof("endpoint-") - 1);
#endif
#if PRODUCT_TEST_RTC_RELEASE
    else if (rtc_release) product_rtc_release(scenario);
#endif
#if PRODUCT_TEST_RECOGNITION_HTTP
    else if (recognition_http) product_recognition_http();
    else if (recognition_upload_probe) product_recognition_upload_probe();
    else if (recognition_audio_saturation) product_recognition_audio_saturation();
#endif
#if PRODUCT_TEST_WT99
    else if (reselect) result = product_preferences_reselect_run(reselect_enabled);
    else if (reselect_soak) result = product_preferences_reselect_soak();
    else if (!strcmp(scenario, "presenter")) product_wt99_presenter();
    else if (!strcmp(scenario, "wt99-binding")) product_wt99_binding();
#endif
#if PRODUCT_TEST_HISTORY
    else if (!strcmp(scenario, "history-maintenance")) product_history_maintenance();
    else if (!strcmp(scenario, "history-clear") || !strcmp(scenario, "history-first"))
        product_history(!strcmp(scenario, "history-clear"));
#endif
#if PRODUCT_TEST_LEDGER
    else if (ledger) result = product_ledger(provisioned);
#endif
#if PRODUCT_TEST_REPROVISION_KEYS
    else if (reprovision_keys) product_reprovision_keys();
    else if (reprovision_barrier) product_reprovision_barrier(scenario + sizeof(reprovision_barrier_prefix) - 1U);
    else if (runtime_wifi_audio) product_runtime_wifi_audio();
    else if (runtime_wifi_cancel_scope) product_runtime_wifi_cancel_scope();
    else if (partial_store) product_reprovision_partial(partial_cold);
#endif
#if PRODUCT_TEST_KEY_AUDIO_SETTINGS
    else if (audio_settings) product_key_audio_settings(scenario + sizeof(audio_settings_prefix) - 1U);
    else if (call_cleanup) product_call_cleanup();
    else if (unassigned_room) product_unassigned_room_menu();
    else if (empty_contacts) product_contacts_empty_refresh(scenario + sizeof(empty_contacts_prefix) - 1U);
    else if (initial_view) product_contacts_initial_view();
#endif
#if PRODUCT_TEST_AI_MONO
    else if (ai_mono_elapsed) product_ai_mono_elapsed();
    else if (home_transport) product_home_transport_label(home_transport_enabled);
#endif
#if PRODUCT_TEST_ENVIRONMENT_ADMISSION
    else if (env_admission) result = product_environment_admission(env_verifying, env_timeout);
#endif
    else assert(false && "unknown product host test scenario");
    xiaotai_reset();
    if (XIAOTAI_FRAMEWORK_OWNER_ON_MAIN)
        assert(pthread_join(s_start_thread, NULL) == 0 && atomic_load(&s_start_returned));
#if PRODUCT_TEST_RECOGNITION_HTTP
    if (recognition_http) product_recognition_http_cleanup();
#endif
    return result;
}
