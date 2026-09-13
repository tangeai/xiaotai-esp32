#!/usr/bin/env python3
"""Run the real prompt owner and MQTT completion callback on a virtual clock."""
from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
app = (project / "main/app_main.c").read_text()
platform = (project / "components/platform_client/src/platform_client.c").read_text()
media = (project / "components/starter_media/src/starter_media.c").read_text()
pcm_writer = media[media.index("esp_err_t starter_media_play_pcm8k_at_epoch("):
                   media.index("esp_err_t starter_media_start(")]
prompt = app[app.index("static esp_err_t play_verification_prompt("):
             app.index("static esp_err_t provision_and_save(")]
event = platform[platform.index("static void provision_mqtt_event("):
                 platform.index("static esp_err_t wait_for_auth_grant(")]
test = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
typedef int esp_event_base_t;
typedef struct { int msg_id, data_len, current_data_offset, total_data_len; char *data; } mqtt_event;
typedef mqtt_event *esp_mqtt_event_handle_t;
typedef struct {
    void *mqtt;
    unsigned *events;
    char temp_client_id[65], message[256];
    int subscribe_message_id, ack_message_id, message_id;
    size_t message_size;
    void (*prompt_cancel_callback)(void *);
    void *prompt_user_data;
} provision_mqtt_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_ERR_INVALID_ARG -3
#define ESP_ERR_TIMEOUT -4
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define PROVISION_READY_BIT 4
#define PROVISION_DONE_BIT 1
#define PROVISION_ERROR_BIT 2
#define MQTT_EVENT_CONNECTED 10
#define MQTT_EVENT_SUBSCRIBED 11
#define MQTT_EVENT_DATA 12
#define MQTT_EVENT_PUBLISHED 13
#define MQTT_EVENT_ERROR 14
#define VERIFICATION_PROMPT_REPEAT_COUNT 3U
#define VERIFICATION_PROMPT_GAP_MS 350U
#define pdMS_TO_TICKS(ms) (ms)
static unsigned clock_ms, trigger_ms, media_fail, enable_calls;
static atomic_uint s_pcm8k_cancel_sequence;
static atomic_bool s_ready, s_active, s_speaker_muted;
static void *s_speaker_dev = (void *)1, *s_audio_output_mutex = (void *)2;
static int held;
#define AUDIO_RX_BYTES 1500U
#define AUDIO_PLAYBACK_I2S_VALUES_PER_INPUT 4U
#define ESP_CODEC_DEV_OK 0
#define pdTRUE 1
static int16_t s_play_stereo[AUDIO_RX_BYTES * AUDIO_PLAYBACK_I2S_VALUES_PER_INPUT];
static atomic_bool cancelled;
static provision_mqtt_t ctx;
static void provision_mqtt_event(void *, esp_event_base_t, int32_t, void *);
static void advance(unsigned ms) {
    clock_ms += ms;
    if (trigger_ms && clock_ms >= trigger_ms) {
        trigger_ms = 0;
        mqtt_event event = {.msg_id = ctx.ack_message_id};
        provision_mqtt_event(&ctx, 0, MQTT_EVENT_PUBLISHED, &event);
    }
}
static void vTaskDelay(unsigned ms) { advance(ms); }
static void starter_media_cancel_pcm8k_playback(void) { ++s_pcm8k_cancel_sequence; }
static uint32_t starter_media_playback_epoch(void) { return s_pcm8k_cancel_sequence; }
static int xSemaphoreTake(void *mutex, unsigned ticks) {
    assert(mutex == s_audio_output_mutex && ticks == 500 && !held); held = 1; return 1;
}
static void xSemaphoreGive(void *mutex) { assert(mutex == s_audio_output_mutex && held); held = 0; }
#define OUTPUT_OWNER_PROMPT 5
static bool take_audio_output_lock(int owner, unsigned ticks) {
    assert(owner == OUTPUT_OWNER_PROMPT);
    return xSemaphoreTake(s_audio_output_mutex, ticks) == pdTRUE;
}
static void give_audio_output_lock(void) { xSemaphoreGive(s_audio_output_mutex); }
static int audio_output_set_locked(bool enabled) { assert(held); enable_calls += enabled; return ESP_OK; }
static int esp_codec_dev_write(void *dev, void *data, size_t bytes) {
    assert(dev == s_speaker_dev && data == s_play_stereo && bytes <= sizeof(s_play_stereo));
    if (media_fail) return ESP_FAIL;
    advance((unsigned)((bytes * 1000 + 63999) / 64000));
    return ESP_CODEC_DEV_OK;
}
static int esp_mqtt_client_subscribe(void *mqtt, const char *topic, int qos) {
    (void)mqtt; (void)topic; assert(qos == 1); return 7;
}
static void xEventGroupSetBits(unsigned *events, unsigned bits) { *events |= bits; }
static void provision_finish_with_error(provision_mqtt_t *context) { *context->events |= 2; }
static void provision_handle_message(provision_mqtt_t *context, const char *json, size_t size) {
    (void)context; (void)json; (void)size;
}
''' + pcm_writer + prompt + event + r'''
static void reset(unsigned trigger) {
    static unsigned events;
    events = clock_ms = enable_calls = media_fail = 0;
    s_pcm8k_cancel_sequence = 0;
    s_ready = true;
    assert(!held);
    trigger_ms = trigger;
    atomic_store(&cancelled, false);
    ctx = (provision_mqtt_t){.events=&events, .ack_message_id=23,
        .prompt_cancel_callback=cancel_verification_prompt, .prompt_user_data=&cancelled};
}
int main(void) {
    int16_t pcm[8000] = {0};
    reset(1);
    assert(play_verification_prompt(pcm, 8000, &cancelled) == ESP_OK);
    assert(clock_ms == 188 && enable_calls == 1 && *ctx.events == PROVISION_DONE_BIT);
    reset(1045); /* During the gap: no second repeat. */
    assert(play_verification_prompt(pcm, 8000, &cancelled) == ESP_OK);
    assert(clock_ms == 1068 && enable_calls == 1);
    reset(0);
    mqtt_event unrelated = {.msg_id=22};
    provision_mqtt_event(&ctx, 0, MQTT_EVENT_PUBLISHED, &unrelated);
    assert(!atomic_load(&cancelled) && *ctx.events == 0);
    assert(play_verification_prompt(pcm, 8000, &cancelled) == ESP_OK);
    assert(enable_calls == 3 && clock_ms == 3829);
    reset(0); cancel_verification_prompt(&cancelled);
    assert(play_verification_prompt(pcm, 8000, &cancelled) == ESP_OK && enable_calls == 0);
    assert(*ctx.events == 0); /* Stop audio never fabricates binding success. */
    reset(0); media_fail = 1;
    assert(play_verification_prompt(pcm, 8000, &cancelled) == ESP_FAIL);
    assert(*ctx.events == 0);
    puts("PASS: binding PUBACK interrupts playback/gap; unrelated ACK ignored; errors preserved");
}
'''
with tempfile.TemporaryDirectory(prefix="xiaotai-binding-") as folder:
    path = Path(folder)
    (path / "test.c").write_text(test)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)

assert ".prompt_cancel_callback = cancel_verification_prompt" in app
assert platform.index("esp_mqtt_client_stop(context.mqtt)", platform.index("static esp_err_t wait_for_auth_grant")) < platform.index('snprintf(result->device_id', platform.index("static esp_err_t wait_for_auth_grant"))
assert "NVS" not in prompt[prompt.index("static void cancel_verification_prompt"):]
