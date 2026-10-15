#include "sample_audio.h"

#ifdef ESP_PLATFORM

#include "esp_log.h"
#include "product_audio.h"

void sample_audio_begin(tirtc_conn_t connection)
{
    product_audio_session_begin(connection);
}

void sample_audio_end(void)
{
    product_audio_session_end();
}

void sample_audio_subscribed(uint8_t stream, bool enabled)
{
    product_audio_set_subscribed(stream, enabled);
}

void sample_audio_tick(void)
{
    product_audio_tick();
}

void sample_audio_receive(tirtc_conn_t connection, const TIRTCFRAMEINFO *frame,
                          const void *data)
{
    product_audio_receive(connection, frame, data);
}

void sample_audio_status(void)
{
    product_audio_log_status();
}

void sample_audio_dump(void)
{
    ESP_LOGI("audio", "RX dump is disabled on the live MZ28 audio path");
}

void sample_audio_record(void)
{
    ESP_LOGI("audio", "RX recording is disabled on the live MZ28 audio path");
}

#else

#include "rtc_adapter.h"
#include "opus_packet.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
extern const uint8_t audio_start[] asm("_binary_audio_opuspkt_start");
extern const uint8_t audio_end[] asm("_binary_audio_opuspkt_end");
#define RECORD_BYTES 8000
#define RECORD_SAMPLES_48K 48000
#define REPORT_MS 2000
#define TX_BACKLOG_BYTES 300 /* 100 ms at the recording's 24 kbit/s bitrate. */
#define TAG "audio"
typedef struct {
    uint32_t packets, bytes, bad, samples_48k, max_gap_ms, first_samples;
    int64_t first_ms, last_ms;
    TIRTCFRAMEINFO first, invalid;
    uint8_t preview[16];
    size_t preview_size;
} rx_stats_t;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static tirtc_conn_t s_connection;
static bool s_subscribed;
static uint8_t s_stream;
static size_t s_offset;
static int64_t s_next_ms, s_report_ms, s_begin_ms;
static uint32_t s_tx, s_tx_bytes, s_tx_failed, s_tx_drop;
static uint32_t s_report_tx, s_report_rx, s_report_bytes, s_report_bad;
static rx_stats_t s_rx;
static bool s_first_reported, s_record_reported;
/* CPU-only length-prefixed packets; no DMA, decoding or allocation in callbacks. */
static RTC_DATA_ATTR uint8_t s_record[RECORD_BYTES];
static size_t s_recorded;
static uint32_t s_record_samples;
static unsigned s_record_done; /* 1 = duration reached, 2 = storage full. */

void sample_audio_begin(tirtc_conn_t connection)
{
    portENTER_CRITICAL(&s_lock);
    s_connection = connection;
    memset(&s_rx, 0, sizeof(s_rx));
    s_recorded = s_record_samples = s_record_done = 0;
    portEXIT_CRITICAL(&s_lock);
    s_offset = 0;
    s_tx = s_tx_bytes = s_tx_failed = s_tx_drop = 0;
    s_subscribed = true;
    s_stream = 10;
    s_next_ms = esp_timer_get_time() / 1000;
    s_report_ms = s_begin_ms = s_next_ms;
    s_report_tx = s_report_rx = s_report_bytes = s_report_bad = 0;
    s_first_reported = s_record_reported = false;
    ESP_LOGI(TAG, "START | TX=C3->WeChat RX=WeChat->C3 codec=Opus rate=16000Hz channels=1");
    ESP_LOGI(TAG, "TX source=flash recording stream=10 packet=20ms bitrate=24000 file=%uB; "
                 "RX=encoded capture, no speaker/decoder", (unsigned)(audio_end - audio_start));
}
void sample_audio_end(void)
{
    portENTER_CRITICAL(&s_lock);
    bool was_active = s_connection != NULL;
    s_connection = NULL;
    portEXIT_CRITICAL(&s_lock);
    s_subscribed = false;
    if (was_active) {
        ESP_LOGI(TAG, "STOP | active_ms=%lld",
                 (long long)(esp_timer_get_time() / 1000 - s_begin_ms));
        sample_audio_status();
    }
}
void sample_audio_subscribed(uint8_t stream, bool enabled)
{
    bool changed = s_subscribed != enabled || (enabled && s_stream != stream);
    if (enabled)
        s_stream = stream;
    s_subscribed = enabled;
    if (changed)
        ESP_LOGI(TAG, "TX subscription | stream=%u enabled=%d", stream, enabled);
}
void sample_audio_record(void)
{
    portENTER_CRITICAL(&s_lock);
    s_recorded = s_record_samples = s_record_done = 0;
    portEXIT_CRITICAL(&s_lock);
    s_record_reported = false;
    ESP_LOGI(TAG, "RX RECORD | target=1000ms capacity=8000B; whole Opus packets only");
}
void sample_audio_status(void)
{
    portENTER_CRITICAL(&s_lock);
    rx_stats_t rx = s_rx;
    size_t record = s_recorded;
    uint32_t samples = s_record_samples;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG,
             "TOTAL | TX packets=%lu bytes=%lu failed=%lu backlog_drop=%lu | "
             "RX packets=%lu bytes=%lu bad_format=%lu recorded=%uB/%lums | "
             "tx_stream=%u subscribed=%d codec=Opus",
             (unsigned long)s_tx, (unsigned long)s_tx_bytes, (unsigned long)s_tx_failed,
             (unsigned long)s_tx_drop, (unsigned long)rx.packets, (unsigned long)rx.bytes,
             (unsigned long)rx.bad, (unsigned)record, (unsigned long)(samples / 48),
             s_stream, s_subscribed);
}
static void report_receive(int64_t now)
{
    bool periodic = now - s_report_ms >= REPORT_MS;
    portENTER_CRITICAL(&s_lock);
    rx_stats_t rx = s_rx;
    size_t recorded = s_recorded;
    uint32_t samples = s_record_samples;
    unsigned done = s_record_done;
    if (periodic)
        s_rx.max_gap_ms = 0;
    portEXIT_CRITICAL(&s_lock);
    /* Only the control task prints; compressed bytes do not imply PCM amplitude. */
    if (rx.packets && !s_first_reported) {
        char hex[33];
        static const char digits[] = "0123456789abcdef";
        for (size_t i = 0; i < rx.preview_size; ++i) {
            hex[2 * i] = digits[rx.preview[i] >> 4];
            hex[2 * i + 1] = digits[rx.preview[i] & 15];
        }
        hex[2 * rx.preview_size] = 0;
        ESP_LOGI(TAG,
                 "RX FIRST | WeChat->C3 wait=%lldms stream=%u media=%u(Opus) flags=%u "
                 "packet=%luB duration_us=%lu rate=16000Hz channels=1 ts=%lu",
                 (long long)(rx.first_ms - s_begin_ms), rx.first.stream_id, rx.first.media,
                 rx.first.flags, (unsigned long)rx.first.length,
                 (unsigned long)rx.first_samples * 1000 / 48, (unsigned long)rx.first.ts);
        ESP_LOGI(TAG, "RX PREVIEW | opus_hex=%s; PCM level requires offline decoding", hex);
        s_first_reported = true;
    }
    if (done && !s_record_reported) {
        ESP_LOGI(TAG, "RX RECORD READY | bytes=%u duration_ms=%lu stop=%s; hangup then rx-dump",
                 (unsigned)recorded, (unsigned long)(samples / 48),
                 done == 1 ? "duration" : "capacity");
        s_record_reported = true;
    }
    if (!periodic)
        return;
    uint32_t packets = rx.packets - s_report_rx;
    ESP_LOGI(TAG, "TX | window=%lldms packets=%lu total=%lu failed=%lu backlog_drop=%lu stream=%u",
             (long long)(now - s_report_ms), (unsigned long)(s_tx - s_report_tx),
             (unsigned long)s_tx, (unsigned long)s_tx_failed, (unsigned long)s_tx_drop, s_stream);
    ESP_LOGI(TAG,
             "RX | packets=%lu bytes=%lu total=%lu max_gap=%lums last_age=%lldms "
             "status=%s codec=Opus",
             (unsigned long)packets, (unsigned long)(rx.bytes - s_report_bytes),
             (unsigned long)rx.packets, (unsigned long)rx.max_gap_ms,
             (long long)(rx.packets ? now - rx.last_ms : -1),
             packets ? "encoded_packets_received" : "no_packet");
    if (rx.bad != s_report_bad)
        ESP_LOGW(TAG, "RX FORMAT REJECTED | count=%lu media=%u flags=%u bytes=%lu; "
                     "expected mono Opus/16kHz, valid framing, length=1..1275",
                 (unsigned long)(rx.bad - s_report_bad), rx.invalid.media, rx.invalid.flags,
                 (unsigned long)rx.invalid.length);
    s_report_tx = s_tx;
    s_report_rx = rx.packets;
    s_report_bytes = rx.bytes;
    s_report_bad = rx.bad;
    s_report_ms = now;
}
void sample_audio_tick(void)
{
    int64_t now = esp_timer_get_time() / 1000;
    tirtc_conn_t h = s_connection;
    if (!h)
        return;
    report_receive(now);
    if (!s_subscribed || now < s_next_ms)
        return;
    if (now - s_next_ms >= 40)
        s_next_ms = now; /* Do not burst old audio after a control task stall. */
    s_next_ms += 20;
    size_t size = (size_t)(audio_end - audio_start);
    if (s_offset == size)
        s_offset = 0;
    size_t length = 0;
    if (s_offset + 2 <= size)
        length = audio_start[s_offset] | ((size_t)audio_start[s_offset + 1] << 8);
    const uint8_t *packet = audio_start + s_offset + (s_offset + 2 <= size ? 2 : 0);
    if (!length || length > OPUS_PACKET_MAX_BYTES || length > size - s_offset - 2 ||
        opus_packet_samples(packet, length) != 960 || (packet[0] & 4)) {
        ESP_LOGE(TAG, "TX ASSET INVALID | offset=%u; expected whole 20ms mono Opus packet",
                 (unsigned)s_offset);
        ++s_tx_failed;
        s_subscribed = false;
        return;
    }
    if (rtc_send_buffer_used(h) > TX_BACKLOG_BYTES)
        ++s_tx_drop;
    else {
        TIRTCFRAMEINFO frame = {.stream_id = s_stream,
                                .media = TIRTC_AUDIO_OPUS,
                                .flags = TIRTC_AUDIOSAMPLE_16K16B1C,
                                .ts = (uint32_t)now,
                                .length = length};
        int ret = rtc_send_audio(h, &frame, packet);
        if (ret > 0) {
            if (!s_tx)
                ESP_LOGI(TAG, "TX FIRST | C3->WeChat codec=Opus stream=%u payload=%uB ts=%lu sdk_ret=%d",
                         s_stream, (unsigned)length, (unsigned long)frame.ts, ret);
            ++s_tx;
            s_tx_bytes += length;
        } else {
            if (!s_tx_failed)
                ESP_LOGE(TAG, "TX FAILED | sdk_ret=%d", ret);
            ++s_tx_failed;
        }
    }
    s_offset += 2 + length;
}
void sample_audio_receive(tirtc_conn_t h, const TIRTCFRAMEINFO *frame, const void *data)
{
    if (!frame || !data)
        return;
    const uint8_t *packet = data;
    unsigned samples = frame->media == TIRTC_AUDIO_OPUS &&
                               frame->flags == TIRTC_AUDIOSAMPLE_16K16B1C
                           ? opus_packet_samples(packet, frame->length) : 0;
    if (!samples || (packet[0] & 4)) {
        portENTER_CRITICAL(&s_lock);
        if (h && h == s_connection) {
            ++s_rx.bad;
            s_rx.invalid = *frame;
        }
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    int64_t now = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_lock);
    if (h && h == s_connection) {
        if (!s_rx.packets) {
            s_rx.first_ms = now;
            s_rx.first = *frame;
            s_rx.first_samples = samples;
            s_rx.preview_size = frame->length < sizeof(s_rx.preview)
                                    ? frame->length : sizeof(s_rx.preview);
            memcpy(s_rx.preview, packet, s_rx.preview_size);
        } else if (now - s_rx.last_ms > s_rx.max_gap_ms)
            s_rx.max_gap_ms = now - s_rx.last_ms;
        s_rx.last_ms = now;
        ++s_rx.packets;
        s_rx.bytes += frame->length;
        s_rx.samples_48k += samples;
        if (!s_record_done) {
            size_t length = frame->length;
            if (length + 2 > RECORD_BYTES - s_recorded)
                s_record_done = 2;
            else {
                s_record[s_recorded++] = length & 255;
                s_record[s_recorded++] = length >> 8;
                memcpy(s_record + s_recorded, packet, length);
                s_recorded += length;
                s_record_samples += samples;
                if (s_record_samples >= RECORD_SAMPLES_48K)
                    s_record_done = 1;
            }
        }
    }
    portEXIT_CRITICAL(&s_lock);
}
void sample_audio_dump(void)
{
    if (s_connection) {
        ESP_LOGW(TAG, "hang up before rx-dump");
        return;
    }
    printf("RX_OPUS_BEGIN %u rate=16000 channels=1\n", (unsigned)s_recorded);
    for (size_t i = 0; i < s_recorded; i += 32) {
        static const char hex[] = "0123456789abcdef";
        char line[65];
        size_t count = s_recorded - i;
        if (count > 32)
            count = 32;
        for (size_t j = 0; j < count; ++j) {
            line[j * 2] = hex[s_record[i + j] >> 4];
            line[j * 2 + 1] = hex[s_record[i + j] & 15];
        }
        line[count * 2] = 0;
        printf("RX %s\n", line);
    }
    printf("RX_OPUS_END\n");
}

#endif
