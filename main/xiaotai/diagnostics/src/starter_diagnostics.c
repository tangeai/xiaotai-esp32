#include "starter_diagnostics.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include "xiaotai_ui_diagnostics.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "platform_client.h"
#include "starter_media.h"
#include "starter_runtime.h"
#include "starter_tirtc.h"
#include "starter_voice.h"
#include "wifi_manager.h"

esp_err_t starter_diagnostics_ui(void)
{
    xiaotai_ui_snapshot_t ui;
    esp_err_t err = xiaotai_ui_inspect(&ui);
    if (err != ESP_OK) return err;
    printf("+UI: ready=%d awake=%d width=%u height=%u page=%u objects=%u ticks=%lu capture-stack-min=%lu\n",
           ui.ready, ui.awake, ui.width, ui.height, ui.page, ui.objects, (unsigned long)ui.ticks,
           (unsigned long)ui.capture_stack_min_free);
    return ESP_OK;
}

esp_err_t starter_diagnostics_screen(void)
{
    xiaotai_ui_frame_t frame = {0};
    esp_err_t err = xiaotai_ui_capture(&frame);
    if (err != ESP_OK) return err;
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < frame.size; ++i) hash = (hash ^ frame.pixels[i]) * 16777619U;
    printf("+SCREEN: format=RGB565LE width=%u height=%u bytes=%u fnv=%08lx\n",
           frame.width, frame.height, (unsigned)frame.size, (unsigned long)hash);
    const char digits[] = "0123456789abcdef";
    for (size_t offset = 0, seq = 0; offset < frame.size; offset += 128, ++seq) {
        char hex[257];
        size_t count = frame.size - offset < 128 ? frame.size - offset : 128;
        for (size_t i = 0; i < count; ++i) {
            uint8_t pixel = frame.pixels[offset + i];
            hex[2 * i] = digits[pixel >> 4];
            hex[2 * i + 1] = digits[pixel & 15];
        }
        hex[2 * count] = '\0';
        printf("+SCREEN-DATA: seq=%u hex=%s\n", (unsigned)seq, hex);
    }
    printf("+SCREEN-END: fnv=%08lx\n", (unsigned long)hash);
    free(frame.pixels);
    return ESP_OK;
}

void starter_diagnostics_identity(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char elf_sha256[65];
    (void)esp_app_get_elf_sha256(elf_sha256, sizeof(elf_sha256));
    printf("Firmware: version=%s built=%s %s elf-sha256=%s\n",
           app->version,
           app->date,
           app->time,
           elf_sha256);
}

void starter_diagnostics_wake(void)
{
    starter_media_audio_diagnostics_t audio;
    starter_media_audio_diagnostics(&audio);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    starter_voice_status_t v = starter_voice_status();
    printf("Voicute: ready=%d init-error=%d listening=%d threshold=%u/1000 frontend=%s "
           "fed=%lu dropped=%lu resets=%lu inference=%lu hits=%lu probability=%lu/1000 "
           "avg/max-us=%lu/%lu over-hop=%lu reject-epoch/age/error=%lu/%lu/%lu\n",
           v.ready, v.init_error, v.listening, v.threshold_milli,
#if CONFIG_XIAOTAI_WAKE_CAPTURE_AGC
           "aec-clean-agc",
#else
           "aec-clean",
#endif
           (unsigned long)v.fed,
           (unsigned long)v.dropped, (unsigned long)v.resets, (unsigned long)v.model_frames,
           (unsigned long)v.detections, (unsigned long)v.last_raw_probability_milli,
           (unsigned long)v.detect_avg_us, (unsigned long)v.max_detect_us,
           (unsigned long)v.detect_over_budget, (unsigned long)v.reject_epoch,
           (unsigned long)v.reject_age, (unsigned long)v.reject_id);
    printf("[DEBUG-wake49] level mic/ref/clean rms-avg=%lu/%lu/%lu rms-max=%lu/%lu/%lu "
           "peak=%lu/%lu/%lu dc=%ld/%ld/%ld\n",
           (unsigned long)audio.rms_avg[0], (unsigned long)audio.rms_avg[1], (unsigned long)audio.rms_avg[2],
           (unsigned long)audio.rms_max[0], (unsigned long)audio.rms_max[1], (unsigned long)audio.rms_max[2],
           (unsigned long)audio.peak[0], (unsigned long)audio.peak[1], (unsigned long)audio.peak[2],
           (long)audio.dc_avg[0], (long)audio.dc_avg[1], (long)audio.dc_avg[2]);
    printf("[DEBUG-wake49] window=%lu frames=%lu at-ms=%lu age-ms=%lu "
           "aec-us-avg=%lu measure-us-avg/max=%lu/%lu\n",
           (unsigned long)audio.window, (unsigned long)audio.frames,
           (unsigned long)audio.at_ms, (unsigned long)(now - audio.at_ms),
           (unsigned long)audio.aec_avg_us, (unsigned long)audio.measurement_avg_us,
           (unsigned long)audio.measurement_max_us);
}

void starter_diagnostics_status(void)
{
    starter_runtime_status_t runtime = starter_runtime_status();
    starter_media_status_t media = starter_media_status();
    starter_diagnostics_identity();
    printf("Wi-Fi: %s", wifi_manager_connected() ? "connected" : "disconnected");
    if (wifi_manager_provisioning()) {
        printf(" (SoftAP %s)", wifi_manager_provisioning_ssid());
    }
    printf("\nPlatform: api=%s mqtt=%s\n",
           platform_client_ready() ? "ready" : "offline",
           platform_client_mqtt_connected() ? "connected" : "disconnected");
    printf("Local voice: wake=voicute-tflite phrase=你好小钛 ready=%d listening=%d\n",
           starter_voice_ready(), starter_voice_status().listening);
    printf("TiRTC: started=%s connection=%s mode=%d send-buffer=%u\n",
           starter_tirtc_started() ? "yes" : "no",
           starter_tirtc_connected() ? "active" : "none",
           (int)starter_tirtc_mode(),
           (unsigned)starter_tirtc_send_buffer_used());
    printf("Session: %s generation=%lu connection=%lu last-error=%d stack-min-free=%lu\n",
           starter_runtime_state_name(runtime.state),
           (unsigned long)runtime.session_generation,
           (unsigned long)runtime.connection_generation,
           runtime.last_error,
           (unsigned long)runtime.stack_high_water_bytes);
    printf("Media: active=%s audio-tx=%lu audio-rx=%lu dropped=%lu\n",
           media.active ? "yes" : "no",
           (unsigned long)media.audio_sent,
           (unsigned long)media.audio_received,
           (unsigned long)media.audio_dropped);
    printf("Downlink: decoded=%lu played=%lu decode-fail=%lu gate=%lu i2s-fail=%lu\n",
           (unsigned long)media.audio_decoded,
           (unsigned long)media.audio_played,
           (unsigned long)media.audio_decode_failed,
           (unsigned long)media.audio_playback_blocked,
           (unsigned long)media.audio_write_failed);
#if CONFIG_IDF_TARGET_ESP32S3
    starter_media_playback_status_t playback = starter_media_playback_status();
    printf("JB: gen=%lu target=%lu q/peak=%lu/%lu ms buffer=%d jitter/gap/hold=%lu/%lu/%lu us starts=%lu late=%lu ts-est/break=%lu/%lu\n",
           (unsigned long)playback.generation, (unsigned long)playback.target_ms,
           (unsigned long)playback.queued_ms, (unsigned long)playback.queued_peak_ms,
           playback.buffering, (unsigned long)playback.jitter_us,
           (unsigned long)playback.gap_max_us, (unsigned long)playback.residence_max_us,
           (unsigned long)playback.starts, (unsigned long)playback.late_bursts,
           (unsigned long)playback.untimed_pairs, (unsigned long)playback.timestamp_breaks);
    printf("RX: invalid/full/stale/slot=%lu/%lu/%lu/%lu; OUT: lock-timeout=%lu wait-max=%lu us owner=%lu write-max=%lu us slow=%lu\n",
           (unsigned long)playback.rx_invalid, (unsigned long)playback.rx_overflow,
           (unsigned long)playback.rx_stale, (unsigned long)playback.slot_errors,
           (unsigned long)playback.lock_timeouts, (unsigned long)playback.lock_max_us,
           (unsigned long)playback.last_lock_owner, (unsigned long)playback.write_max_us,
           (unsigned long)playback.slow_writes);
    printf("PCM source: epoch=%lu in/consumed/drop/pending=%lu/%lu/%lu/%lu samples chunks/tails=%lu/%lu hash=%08lx/%08lx\n",
           (unsigned long)playback.pcm_epoch,
           (unsigned long)playback.decoded_samples, (unsigned long)playback.written_samples,
           (unsigned long)playback.discarded_samples, (unsigned long)playback.pending_samples,
           (unsigned long)playback.chunks, (unsigned long)playback.tails,
           (unsigned long)playback.pcm_in_hash, (unsigned long)playback.pcm_out_hash);
    printf("PCM timing: ingress/decode/service/write-gap-max=%lu/%lu/%lu/%lu us\n",
           (unsigned long)playback.ingress_max_us, (unsigned long)playback.decode_max_us,
           (unsigned long)playback.service_gap_max_us, (unsigned long)playback.write_gap_max_us);
    printf("RATE: mode=%d slow/fast=%lu/%lu render=%lu samples fade=%lu DMA-est=%lu us\n",
           playback.rate_mode, (unsigned long)playback.slow_chunks, (unsigned long)playback.fast_chunks,
           (unsigned long)playback.output_samples, (unsigned long)playback.fade_starts,
           (unsigned long)playback.dma_estimate_us);
    starter_media_pipeline_status_t pipeline = starter_media_pipeline_status();
    printf("IN: frames/err=%lu/%lu read/gap-max=%lu/%lu us AFE-age-max=%lu ms\n",
           (unsigned long)pipeline.read_frames, (unsigned long)pipeline.read_errors,
           (unsigned long)pipeline.read_max_us, (unsigned long)pipeline.read_gap_max_us,
           (unsigned long)pipeline.afe_age_max_ms);
    printf("DSP: fetch-wait/AGC-max=%lu/%lu us post16k-peak/rms=%lu/%lu\n",
           (unsigned long)pipeline.fetch_wait_max_us, (unsigned long)pipeline.agc_max_us,
           (unsigned long)pipeline.post_agc_peak,
           (unsigned long)pipeline.post_agc_rms);
    printf("TX: q-peak=%lu age/gain/encode/send-max=%lu/%lu/%lu/%lu us peak/rms=%lu/%lu clip=%lu frames=%lu measure-max=%lu us\n",
           (unsigned long)pipeline.tx_queue_peak, (unsigned long)pipeline.tx_age_max_us,
           (unsigned long)pipeline.tx_gain_max_us, (unsigned long)pipeline.encode_max_us,
           (unsigned long)pipeline.send_max_us, (unsigned long)pipeline.tx_peak,
           (unsigned long)pipeline.tx_rms, (unsigned long)pipeline.tx_clipped,
           (unsigned long)pipeline.tx_frames, (unsigned long)pipeline.tx_measure_max_us);
#endif
    printf("AEC: processed=%lu errors=%lu clipped-mic/ref=%lu/%lu\n",
           (unsigned long)media.aec_processed,
           (unsigned long)media.aec_errors,
           (unsigned long)media.aec_mic_clipped,
           (unsigned long)media.aec_reference_clipped);
#if CONFIG_IDF_TARGET_ESP32S3
    printf("Media timing: max-feed-normalized=%lu us feed-budget-miss=%lu\n",
#else
    printf("Media timing: max-aec=%lu us deadline-miss=%lu\n",
#endif
           (unsigned long)media.aec_max_process_us,
           (unsigned long)media.aec_deadline_misses);
#if CONFIG_IDF_TARGET_ESP32P4
    printf("Capture: i2s-queue-overflow=%lu\n", (unsigned long)media.capture_queue_overflows);
#endif
    printf("Heap: internal-free=%u internal-largest=%u psram-free=%u psram-largest=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    starter_diagnostics_wake();
}
