/* Bounded tick evidence: store opaque IDs, never dereference another TCB in
 * the ISR. Resolve current task names only during an explicit AT query. */
#define AFE_PREEMPT_SLOTS 16U
static DRAM_ATTR struct {
    struct { atomic_uintptr_t task; atomic_uint ticks; } slot[AFE_PREEMPT_SLOTS];
    atomic_uint omitted;
} s_afe_preempt;
_Static_assert(sizeof(s_afe_preempt) == 132U, "Preemption sampler DRAM budget changed");

static void IRAM_ATTR afe_preempt_sample(uintptr_t current)
{
    if (!current) return;
    for (unsigned n = 0; n < AFE_PREEMPT_SLOTS; ++n) {
        uintptr_t task = atomic_load_explicit(&s_afe_preempt.slot[n].task, memory_order_relaxed);
        if (!task || task == current) {
            if (!task)
                atomic_store_explicit(&s_afe_preempt.slot[n].task, current, memory_order_relaxed);
            atomic_fetch_add_explicit(&s_afe_preempt.slot[n].ticks, 1U, memory_order_relaxed);
            return;
        }
    }
    atomic_fetch_add_explicit(&s_afe_preempt.omitted, 1U, memory_order_relaxed);
}

static void afe_preempt_log(void)
{
    static const char *const names[] = {
        "board_audio_in", "board_audio_tx", "board_audio_rx", "rtc_audio_tx",
        "afe_feed", "AFE", "esp_afe", "rtc_thread", "starter_session",
        "mqtt_task", "platform_http", "platform_worker", "lvgl_port",
        "voicute_wake", "wifi", "tiT", "Tmr Svc", "esp_timer", "ipc1",
        "IDLE0", "IDLE1", "main", "sys_evt",
    };
    ESP_LOGI(TAG, "AFE preempt: omitted=%u hz=%u names=current-at-query",
             atomic_load(&s_afe_preempt.omitted), (unsigned)configTICK_RATE_HZ);
    for (unsigned n = 0; n < AFE_PREEMPT_SLOTS; ++n) {
        uintptr_t task = atomic_load_explicit(&s_afe_preempt.slot[n].task, memory_order_relaxed);
        if (!task) continue;
        const char *name = "unknown";
        char sdk_name[] = "tiA";
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            if ((uintptr_t)xTaskGetHandle(names[i]) == task) {
                name = names[i];
                break;
            }
        }
        /* Resolve the SDK's short names through the live task registry only. */
        if (strcmp(name, "unknown") == 0) {
            for (char letter = 'A'; letter <= 'Z'; ++letter) {
                sdk_name[2] = letter;
                if ((uintptr_t)xTaskGetHandle(sdk_name) == task) {
                    name = sdk_name;
                    break;
                }
            }
        }
        ESP_LOGI(TAG, "AFE preempt: slot=%u task=0x%08x name=%s ticks=%u", n,
                 (unsigned)task, name, atomic_load(&s_afe_preempt.slot[n].ticks));
    }
}
