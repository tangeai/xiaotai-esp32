// SPDX-License-Identifier: MIT
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include "audio_test_stubs.h"

static jmp_buf worker_done;
static unsigned critical_depth, writes, reads, opens, closes, transmitted;
static bool codec_open, fail_open, fail_request_send;
static bool stop_after_capture;
static int64_t clock_us;
static void (*critical_exit_hook)(void);
static void (*downlink_send_hook)(void);
static void (*audio_worker)(void *);
static unsigned stop_after_frames;
static bool feed_downlink, change_live_controls, track_playback;
static bool require_reset_owner_lock;
static bool reject_unscoped_stop;
static bool fail_task_create;
static unsigned stack_samples, stack_bytes;
static int audio_task_handle;
static unsigned volume_calls, gain_calls, encoded_tones, played_count;
static unsigned echo_off_after;
static int encoded_sample;
static int applied_volume, applied_gain;
static uint8_t played_values[50];
const uint8_t _binary_guidance_alaw_start[585120] = {0};

#include "../components/xmini_audio/xmini_audio.c"

const char *esp_err_to_name(esp_err_t error) { return error ? "failure" : "OK"; }
void test_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
void test_enter_critical(portMUX_TYPE *lock) { (void)lock; ++critical_depth; }
void test_exit_critical(portMUX_TYPE *lock)
{
    (void)lock;
    assert(critical_depth);
    if (--critical_depth == 0 && critical_exit_hook) {
        void (*hook)(void) = critical_exit_hook;
        critical_exit_hook = NULL;
        hook();
    }
}

QueueHandle_t xQueueCreateStatic(unsigned count, unsigned item_size, uint8_t *buffer, StaticQueue_t *queue)
{
    *queue = (StaticQueue_t){.capacity = count, .item_size = item_size, .buffer = buffer};
    return queue;
}
QueueHandle_t xQueueCreate(unsigned count, unsigned item_size)
{
    QueueHandle_t queue = malloc(sizeof(*queue));
    assert(queue);
    xQueueCreateStatic(count, item_size, malloc(count * item_size), queue);
    assert(queue->buffer);
    queue->dynamic = true;
    return queue;
}
int xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait)
{
    assert(queue && wait == 0);
    if (queue == s_downlink && downlink_send_hook) {
        void (*hook)(void) = downlink_send_hook;
        downlink_send_hook = NULL;
        hook();
    }
    if (queue == s_requests && fail_request_send) { fail_request_send = false; return 0; }
    if (queue->count == queue->capacity) return 0;
    unsigned tail = (queue->head + queue->count) % queue->capacity;
    memcpy(queue->buffer + tail * queue->item_size, item, queue->item_size);
    ++queue->count;
    return pdTRUE;
}
int xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait)
{
    assert(queue);
    if (!queue->count) {
        if (queue == s_requests && wait == portMAX_DELAY) longjmp(worker_done, 1);
        return 0;
    }
    memcpy(item, queue->buffer + queue->head * queue->item_size, queue->item_size);
    queue->head = (queue->head + 1) % queue->capacity;
    --queue->count;
    return pdTRUE;
}
void xQueueReset(QueueHandle_t queue)
{
    assert(queue);
    if (require_reset_owner_lock) assert(critical_depth);
    queue->count = queue->head = 0;
}
void vQueueDelete(QueueHandle_t queue) { assert(queue && queue->dynamic); free(queue->buffer); free(queue); }
int xTaskCreate(void (*function)(void *), const char *name, unsigned stack, void *argument, unsigned priority, TaskHandle_t *handle)
{
    assert(!strcmp(name, "xmini_audio") && stack == 6144 && priority == 5 && !argument && handle);
    if (fail_task_create) return 0;
    *handle = &audio_task_handle;
    audio_worker = function;
    return pdPASS;
}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    assert(task == &audio_task_handle && !critical_depth);
    ++stack_samples;
    return stack_bytes;
}
int64_t esp_timer_get_time(void) { return clock_us; }
void *xmini_board_i2c(void) { return (void *)1; }
int gpio_set_level(int pin, int value) { (void)pin; (void)value; return ESP_OK; }
int i2s_new_channel(const i2s_chan_config_t *config, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx)
{ (void)config; *tx = (void *)1; *rx = (void *)2; return ESP_OK; }
int i2s_channel_init_std_mode(i2s_chan_handle_t channel, const i2s_std_config_t *config)
{ (void)channel; (void)config; return ESP_OK; }
int i2s_channel_enable(i2s_chan_handle_t channel) { (void)channel; return ESP_OK; }
int i2s_channel_disable(i2s_chan_handle_t channel) { (void)channel; return ESP_OK; }
int i2s_del_channel(i2s_chan_handle_t channel) { (void)channel; return ESP_OK; }
const audio_codec_data_if_t *audio_codec_new_i2s_data(const audio_codec_i2s_cfg_t *config)
{ static audio_codec_data_if_t object; (void)config; return &object; }
const audio_codec_ctrl_if_t *audio_codec_new_i2c_ctrl(const audio_codec_i2c_cfg_t *config)
{ static audio_codec_ctrl_if_t object; (void)config; return &object; }
const audio_codec_gpio_if_t *audio_codec_new_gpio(void) { static audio_codec_gpio_if_t object; return &object; }
const audio_codec_if_t *es8311_codec_new(const es8311_codec_cfg_t *config)
{ static audio_codec_if_t object; (void)config; return &object; }
esp_codec_dev_handle_t esp_codec_dev_new(const esp_codec_dev_cfg_t *config) { (void)config; return (void *)1; }
void esp_codec_dev_delete(esp_codec_dev_handle_t device) { (void)device; }
void audio_codec_delete_codec_if(const audio_codec_if_t *object) { (void)object; }
void audio_codec_delete_ctrl_if(const audio_codec_ctrl_if_t *object) { (void)object; }
void audio_codec_delete_gpio_if(const audio_codec_gpio_if_t *object) { (void)object; }
void audio_codec_delete_data_if(const audio_codec_data_if_t *object) { (void)object; }
int esp_codec_dev_open(esp_codec_dev_handle_t device, const esp_codec_dev_sample_info_t *format)
{
    (void)device;
    assert(format->sample_rate == 8000 && format->bits_per_sample == 16 && format->channel == 1);
    ++opens;
    if (fail_open) return ESP_FAIL;
    assert(!codec_open);
    codec_open = true;
    return ESP_OK;
}
int esp_codec_dev_close(esp_codec_dev_handle_t device) { (void)device; codec_open = false; ++closes; return ESP_OK; }
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t device, int volume) { (void)device; ++volume_calls; applied_volume = volume; return ESP_OK; }
int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t device, int gain) { (void)device; ++gain_calls; applied_gain = gain; return ESP_OK; }
int esp_codec_dev_read(esp_codec_dev_handle_t device, void *buffer, unsigned length)
{
    (void)device; assert(codec_open && length == 320); ++reads; clock_us += 20000;
    int16_t *pcm = buffer;
    for (unsigned i = 0; i < 160; ++i) pcm[i] = 123;
    if (stop_after_capture) {
        stop_after_capture = false;
        assert(xmini_audio_stream_stop(atomic_load(&s_stream_generation)) == ESP_OK);
    }
    return ESP_OK;
}
int esp_codec_dev_write(esp_codec_dev_handle_t device, void *buffer, unsigned length)
{ (void)device; (void)buffer; assert(codec_open && length == 320); ++writes; return ESP_OK; }
int esp_g711a_enc_open(const esp_g711_enc_config_t *config, unsigned size, void **encoder)
{ (void)config; (void)size; *encoder = (void *)1; return ESP_OK; }
int esp_g711_enc_process(void *encoder, esp_audio_enc_in_frame_t *input, esp_audio_enc_out_frame_t *output)
{
    (void)encoder; assert(input->len == 320 && output->len == 160);
    const int16_t *pcm = (const int16_t *)input->buffer;
    if (pcm[0] == 0 && pcm[1] == 927 && pcm[5] == 3000 && pcm[15] == -3000) ++encoded_tones;
    else encoded_sample = pcm[0];
    memset(output->buffer, 0xd5, 160); output->encoded_bytes = 160; return ESP_OK;
}
void esp_g711_enc_close(void *encoder) { (void)encoder; }
int esp_g711_dec_open(void *config, unsigned size, void **decoder)
{ (void)config; (void)size; *decoder = (void *)1; return ESP_OK; }
int esp_g711a_dec_decode(void *decoder, esp_audio_dec_in_raw_t *input, esp_audio_dec_out_frame_t *output, esp_audio_dec_info_t *info)
{
    (void)decoder; (void)info; assert(input->len == 160 && output->len == 320);
    if (track_playback) { assert(played_count < 50); played_values[played_count++] = input->buffer[0]; }
    memset(output->buffer, 0, 320); input->consumed = 160; output->decoded_size = 320; return ESP_OK;
}
void esp_g711_dec_close(void *decoder) { (void)decoder; }

static bool transmit(uint32_t generation, const uint8_t *packet, size_t length, uint64_t pts, void *context)
{
    assert(generation == (uint32_t)(uintptr_t)context && packet && length == 160 && pts > 0);
    assert(xmini_audio_status().device_open);
    ++transmitted;
    if (reject_unscoped_stop && transmitted == 1) {
        assert(xmini_audio_request((xmini_audio_request_t){.kind = XMINI_AUDIO_STOP}) == ESP_ERR_INVALID_STATE);
        assert(!atomic_load(&s_stop) && xmini_audio_stream_busy(generation));
    }
    if (change_live_controls && transmitted == 1) {
        unsigned previous_volume = volume_calls, previous_gain = gain_calls;
        assert(xmini_audio_request((xmini_audio_request_t){.kind = XMINI_AUDIO_VOLUME, .value = 82}) == ESP_OK);
        assert(xmini_audio_request((xmini_audio_request_t){.kind = XMINI_AUDIO_GAIN, .value = 30}) == ESP_OK);
        assert(volume_calls == previous_volume && gain_calls == previous_gain);
    }
    if (echo_off_after && transmitted == echo_off_after) {
        assert(encoded_sample == 0 && xmini_audio_echo_guard_set(false) == ESP_OK);
        assert(xmini_audio_status().echo_closed_frames == echo_off_after);
    }
    if (feed_downlink) {
        uint8_t received[160];
        memset(received, transmitted, sizeof(received));
        assert(xmini_audio_receive(generation, received, sizeof(received)));
    }
    if (transmitted == stop_after_frames) assert(xmini_audio_stream_stop(generation) == ESP_OK);
    return true;
}
static void run_pending(void)
{
    assert(audio_worker);
    if (!setjmp(worker_done)) audio_worker(NULL);
    assert(!critical_depth && !xmini_audio_status().busy && !xmini_audio_status().device_open && !codec_open);
}
static void invalidate_ready(void) { s_status.ready = false; }
static void invalidate_recording(void) { s_status.recording_ready = false; }
static void cancel_at_publish(void)
{
    uint32_t generation = atomic_load(&s_stream_generation);
    if (!generation) { critical_exit_hook = cancel_at_publish; return; }
    assert(xmini_audio_stream_stop(generation) == ESP_OK);
}
static void replace_before_old_frame_enqueue(void)
{
    assert(xmini_audio_stream_stop(6) == ESP_OK);
    run_pending();
    assert(xmini_audio_stream_start(7, transmit, (void *)7) == ESP_OK);
}

int main(void)
{
    unsigned watermark = 999;
    assert(!xmini_audio_stack_watermark(&watermark) && watermark == 0 && !stack_samples);
    assert(!xmini_audio_stack_watermark(NULL) && !stack_samples);
    assert(xmini_audio_initial_settings(-1, 24) == ESP_ERR_INVALID_ARG);
    assert(xmini_audio_initial_settings(53, 18) == ESP_OK);
    fail_task_create = true;
    assert(xmini_audio_init() == ESP_ERR_NO_MEM);
    assert(!xmini_audio_stack_watermark(&watermark) && watermark == 0 && !stack_samples);
    fail_task_create = false;
    assert(xmini_audio_init() == ESP_OK && xmini_audio_status().ready);
    stack_bytes = 1234;
    assert(xmini_audio_stack_watermark(&watermark) && watermark == 1234 && stack_samples == 1);
    stack_bytes = 0;
    assert(xmini_audio_stack_watermark(&watermark) && watermark == 0 && stack_samples == 2);
    assert(applied_volume == 53 && applied_gain == 18);
    assert(xmini_audio_initial_settings(40, 24) == ESP_ERR_INVALID_STATE);
    assert(xmini_audio_echo_guard_enabled() && xmini_audio_status().echo_enabled);
    uint8_t packet[160] = {0};
    assert(xmini_audio_stream_start(0, transmit, NULL) == ESP_ERR_INVALID_ARG);
    assert(xmini_audio_stream_start(1, NULL, NULL) == ESP_ERR_INVALID_ARG);
    assert(!xmini_audio_receive(1, packet, sizeof(packet)));

    // An accepted stop after publication must survive the rest of start().
    critical_exit_hook = cancel_at_publish;
    assert(xmini_audio_stream_start(1, transmit, (void *)1) == ESP_OK);
    assert(atomic_load(&s_stop));
    unsigned before_opens = opens;
    run_pending();
    assert(opens == before_opens && atomic_load(&s_stream_generation) == 0);
    assert(xmini_audio_status().stopped && !xmini_audio_receive(1, packet, sizeof(packet)));

    // Stale stop/downlink cannot affect a newer owner. Queue-full is bounded.
    assert(xmini_audio_stream_start(2, transmit, (void *)2) == ESP_OK);
    assert(xmini_audio_status().busy && !xmini_audio_status().device_open);
    // A caller may have observed a guidance owner before this RTC reservation.
    // Its delayed unscoped STOP must not cancel the newly published stream.
    assert(xmini_audio_request((xmini_audio_request_t){.kind = XMINI_AUDIO_STOP}) == ESP_ERR_INVALID_STATE);
    assert(!atomic_load(&s_stop) && xmini_audio_stream_busy(2));
    assert(xmini_audio_stream_stop(1) == ESP_ERR_INVALID_STATE && !atomic_load(&s_stop));
    assert(xmini_audio_prompt(XMINI_PROMPT_STARTUP, NULL) == ESP_ERR_INVALID_STATE);
    assert(!xmini_audio_receive(1, packet, sizeof(packet)));
    assert(!xmini_audio_receive(2, packet, 159));
    for (unsigned i = 0; i < 8; ++i) assert(xmini_audio_receive(2, packet, sizeof(packet)));
    assert(!xmini_audio_receive(2, packet, sizeof(packet)));
    transmitted = 0; stop_after_frames = 3; reject_unscoped_stop = true;
    run_pending();
    reject_unscoped_stop = false;
    xmini_audio_status_t status = xmini_audio_status();
    assert(status.owner == XMINI_AUDIO_RTC && status.generation == 2 && status.stopped && !status.error);
    assert(status.frames == 3 && status.tx_frames == 3 && status.rx_frames == 3 && reads == 3);
    assert(atomic_load(&s_stream_generation) == 0 && !xmini_audio_receive(2, packet, sizeof(packet)));

    // Request-queue failure retires the generation, without opening hardware.
    fail_request_send = true;
    assert(xmini_audio_stream_start(3, transmit, (void *)3) == ESP_ERR_NO_MEM);
    assert(!atomic_load(&s_pending) && atomic_load(&s_stream_generation) == 0);

    // A failed open is not reported as successful playback or left active.
    fail_open = true;
    assert(xmini_audio_stream_start(4, transmit, (void *)4) == ESP_OK);
    run_pending();
    assert(xmini_audio_status().error && atomic_load(&s_stream_generation) == 0);
    fail_open = false;

    // State may change after the optimistic status snapshot, before reservation.
    critical_exit_hook = invalidate_ready;
    assert(xmini_audio_stream_start(5, transmit, (void *)5) == ESP_ERR_INVALID_STATE);
    s_status.ready = true;
    s_status.recording_ready = true;
    critical_exit_hook = invalidate_recording;
    assert(xmini_audio_request((xmini_audio_request_t){.kind = XMINI_AUDIO_PLAY}) == ESP_ERR_INVALID_STATE);
    assert(!atomic_load(&s_pending));

    // Callback already passed the old-generation check, then the owner changes
    // before queue insertion. Its carried generation still prevents rendering.
    assert(xmini_audio_stream_start(6, transmit, (void *)6) == ESP_OK);
    downlink_send_hook = replace_before_old_frame_enqueue;
    assert(xmini_audio_receive(6, packet, sizeof(packet)));
    assert(s_downlink->count == 1 && atomic_load(&s_stream_generation) == 7);
    transmitted = 0; stop_after_frames = 2;
    run_pending();
    status = xmini_audio_status();
    assert(status.generation == 7 && status.rx_frames == 0 && status.underruns == 2);

    // A stop delivered while the codec read was blocked must gate the callback.
    assert(xmini_audio_stream_start(8, transmit, (void *)8) == ESP_OK);
    transmitted = 0; stop_after_frames = 1; stop_after_capture = true;
    run_pending();
    assert(transmitted == 0 && xmini_audio_status().stopped);

    // Injection is exactly 25 frames (500 ms); received snapshots preserve the
    // newest 50 frames in chronological order after wrapping the recording ring.
    assert(xmini_audio_stream_start(9, transmit, (void *)9) == ESP_OK);
    assert(xmini_audio_stream_busy(9) && !xmini_audio_stream_busy(8));
    assert(xmini_audio_receive(9, packet, sizeof(packet)) && s_downlink->count == 1);
    require_reset_owner_lock = true;
    xmini_audio_stream_flush(8);
    assert(s_downlink->count == 1);
    xmini_audio_stream_flush(9);
    assert(s_downlink->count == 0);
    require_reset_owner_lock = false;
    assert(xmini_audio_stream_tone(8) == ESP_ERR_INVALID_STATE);
    assert(xmini_audio_stream_tone(9) == ESP_OK);
    uint8_t recording[8000];
    assert(xmini_audio_recording_copy(recording, sizeof(recording)) == ESP_ERR_INVALID_STATE);
    transmitted = 0; stop_after_frames = 70;
    feed_downlink = change_live_controls = true;
    run_pending();
    feed_downlink = change_live_controls = false;
    status = xmini_audio_status();
    // The echo guard runs before injection, so tone frames stay unattenuated.
    assert(status.injected_frames == 25 && encoded_tones == 25);
    assert(status.echo_closed_frames == 70 && !status.echo_dt_frames && !status.echo_open_frames);
    assert(s_echo.erl_q8 == 511000 && s_echo.closed_q15 == 1 && status.echo_gain_q15 == 1 && encoded_sample == 0);
    assert(status.recording_ready && status.recording_source == 1 && status.rx_frames == 70);
    assert(applied_volume == 82 && applied_gain == 30 && !xmini_audio_stream_busy(9));
    assert(xmini_audio_recording_copy(NULL, sizeof(recording)) == ESP_ERR_INVALID_ARG);
    assert(xmini_audio_recording_copy(recording, 7999) == ESP_ERR_INVALID_ARG);
    assert(xmini_audio_recording_copy(recording, sizeof(recording)) == ESP_OK);
    for (unsigned frame = 0; frame < 50; ++frame)
        for (unsigned sample = 0; sample < 160; ++sample) assert(recording[frame * 160 + sample] == frame + 21);
    track_playback = true;
    assert(xmini_audio_request((xmini_audio_request_t){.kind = XMINI_AUDIO_PLAY}) == ESP_OK);
    run_pending();
    track_playback = false;
    assert(played_count == 50);
    for (unsigned i = 0; i < 50; ++i) assert(played_values[i] == i + 21);
    // Diagnostics and guidance keep their existing explicit cancellation path.
    for (unsigned owner = XMINI_AUDIO_DIAGNOSTIC; owner <= XMINI_AUDIO_GUIDANCE; ++owner) {
        assert(xmini_audio_request((xmini_audio_request_t){.kind = XMINI_AUDIO_TONE, .owner = owner}) == ESP_OK);
        assert(xmini_audio_request((xmini_audio_request_t){.kind = XMINI_AUDIO_STOP}) == ESP_OK);
        before_opens = opens;
        run_pending();
        assert(opens == before_opens && xmini_audio_status().stopped);
    }
    // Diagnostics stay raw; disabling the guard takes effect from the next stream frame.
    assert(xmini_audio_request((xmini_audio_request_t){.kind = XMINI_AUDIO_RECORD}) == ESP_OK);
    run_pending();
    status = xmini_audio_status();
    assert(encoded_sample == 123 && status.recording_ready && !status.echo_closed_frames && !status.echo_gain_q15);
    assert(xmini_audio_stream_start(10, transmit, (void *)10) == ESP_OK);
    transmitted = 0; stop_after_frames = 4; echo_off_after = 2;
    run_pending();
    echo_off_after = 0;
    status = xmini_audio_status();
    assert(!status.echo_enabled && !xmini_audio_echo_guard_enabled() && status.tx_frames == 4);
    assert(status.echo_closed_frames == 2 && !status.echo_dt_frames && !status.echo_open_frames);
    assert(encoded_sample == 123 && s_echo.closed_frames == 2);
    assert(xmini_audio_echo_guard_set(true) == ESP_OK && xmini_audio_echo_guard_enabled());
    assert(closes >= 3 && writes >= 4);
    release_resources();
    return 0;
}
