#include "sample_audio.h"
#include "rtc_adapter.h"
#include "opus_packet.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

unsigned test_critical_depth;
static int64_t s_time;
static char s_log[32768];
static size_t s_used, s_buffered;
static unsigned s_sent;
static int s_send_result = 3;
static uint8_t s_stream;
static bool s_in_receive;
int64_t esp_timer_get_time(void) { return s_time * 1000; }
void test_log(const char *level, const char *tag, const char *format, ...)
{
    (void)level;
    (void)tag;
    assert(!test_critical_depth && !s_in_receive);
    for (const char *p = format; (p = strchr(p, '%')) != NULL; ++p) {
        ++p;
        p += strspn(p, "-+ #0.123456789*hljztL");
        assert(*p && !strchr("aAeEfFgG", *p));
    }
    va_list args;
    va_start(args, format);
    int n = vsnprintf(s_log + s_used, sizeof(s_log) - s_used, format, args);
    va_end(args);
    assert(n >= 0 && (size_t)n + 2 < sizeof(s_log) - s_used);
    s_used += n;
    s_log[s_used++] = '\n';
    s_log[s_used] = 0;
}
size_t rtc_send_buffer_used(tirtc_conn_t h) { assert(h); return s_buffered; }
int rtc_send_audio(tirtc_conn_t h, const TIRTCFRAMEINFO *frame, const void *data)
{
    assert(h && data && frame->length == 3 && frame->media == TIRTC_AUDIO_OPUS);
    assert(frame->flags == TIRTC_AUDIOSAMPLE_16K16B1C);
    assert(memcmp(data, "\xf8\xff\xfe", 3) == 0); /* No storage prefix on the wire. */
    ++s_sent;
    s_stream = frame->stream_id;
    return s_send_result;
}
static void clear_log(void) { s_used = 0; s_log[0] = 0; }
static void expect(const char *text) { assert(strstr(s_log, text)); }
static void receive(tirtc_conn_t h, TIRTCFRAMEINFO *frame, uint8_t *data)
{
    size_t before = s_used;
    s_in_receive = true;
    sample_audio_receive(h, frame, data);
    s_in_receive = false;
    assert(s_used == before);
}
static void framing(void)
{
    uint8_t packet[1276] = {0xf8, 0xff, 0xfe};
    assert(opus_packet_samples(packet, 3) == 960);
    assert(!opus_packet_samples(NULL, 1));
    assert(!opus_packet_samples(packet, 0));
    assert(!opus_packet_samples(packet, sizeof(packet)));
    packet[0] = 0xf9; /* CBR two-frame packet. */
    assert(opus_packet_samples(packet, 3) == 1920);
    assert(!opus_packet_samples(packet, 2));
    packet[0] = 0xfa;
    packet[1] = 255; /* Truncated VBR size. */
    assert(!opus_packet_samples(packet, 2));
    packet[0] = 0xfb;
    packet[1] = 0;
    assert(!opus_packet_samples(packet, 2));
    packet[1] = 7; /* 140 ms is invalid. */
    assert(!opus_packet_samples(packet, 2));
    packet[1] = 0x41; /* Padded one-frame packet, missing padding length. */
    assert(!opus_packet_samples(packet, 2));
    packet[2] = 5;
    assert(!opus_packet_samples(packet, 4));
}
int main(void)
{
    framing();
    tirtc_conn_t h = (tirtc_conn_t)(uintptr_t)1;
    uint8_t data[1276] = {0xf8, 0xff, 0xfe};
    TIRTCFRAMEINFO frame = {.media = TIRTC_AUDIO_OPUS,
                            .flags = TIRTC_AUDIOSAMPLE_16K16B1C,
                            .stream_id = 0, .ts = 123, .length = 3};
    sample_audio_begin(h);
    sample_audio_tick();
    assert(s_sent == 1 && s_stream == 10);
    expect("TX FIRST");
    clear_log();
    s_time = 40;
    receive(NULL, &frame, data);
    receive((tirtc_conn_t)(uintptr_t)2, &frame, data);
    receive(h, &frame, data);
    sample_audio_tick();
    expect("RX FIRST");
    expect("wait=40ms");
    expect("packet=3B duration_us=20000");
    expect("opus_hex=f8fffe");
    clear_log();
    s_time = 2000;
    sample_audio_tick();
    expect("RX | packets=1 bytes=3 total=1");
    expect("encoded_packets_received");
    assert(!strstr(s_log, "peak_pcm"));
    frame.media = TIRTC_AUDIO_ALAW;
    receive(h, &frame, data);
    frame.media = TIRTC_AUDIO_OPUS;
    frame.flags = TIRTC_AUDIOSAMPLE_8K16B1C;
    receive(h, &frame, data);
    frame.flags = TIRTC_AUDIOSAMPLE_16K16B1C;
    frame.length = 1276;
    receive(h, &frame, data);
    frame.length = 0;
    receive(h, &frame, data);
    frame.length = 3;
    data[0] = 0xfc; /* Stereo bit contradicts the negotiated mono format. */
    receive(h, &frame, data);
    data[0] = 0xf8;
    s_time = 4000;
    sample_audio_tick();
    expect("RX FORMAT REJECTED | count=5");
    clear_log();
    s_time = 6000;
    sample_audio_tick();
    expect("status=no_packet");
    assert(!strstr(s_log, "FORMAT REJECTED"));
    sample_audio_subscribed(0, true);
    s_time += 20;
    sample_audio_tick();
    assert(s_stream == 0);
    unsigned sent = s_sent;
    sample_audio_subscribed(0, false);
    s_time += 100;
    sample_audio_tick();
    assert(s_sent == sent);
    sample_audio_subscribed(0, true);
    s_buffered = 301;
    sample_audio_tick();
    assert(s_sent == sent);
    s_buffered = 0;
    s_send_result = -1;
    s_time += 20;
    sample_audio_tick();
    expect("TX FAILED");
    sample_audio_record();
    for (int i = 0; i < 50; ++i) {
        s_time += 20;
        receive(h, &frame, data);
    }
    clear_log();
    sample_audio_tick();
    expect("RX RECORD READY | bytes=250 duration_ms=1000 stop=duration");
    sample_audio_record();
    frame.length = 1275;
    for (int i = 0; i < 7; ++i)
        receive(h, &frame, data);
    sample_audio_tick();
    expect("bytes=7662 duration_ms=120 stop=capacity");
    clear_log();
    sample_audio_end();
    expect("bad_format=5 recorded=7662B/120ms");
    expect("failed=2 backlog_drop=1");
    clear_log();
    sample_audio_end();
    receive(h, &frame, data);
    sample_audio_tick();
    assert(s_used == 0);
    s_send_result = 3;
    sample_audio_begin(h);
    sample_audio_status();
    expect("RX packets=0 bytes=0 bad_format=0 recorded=0B/0ms");
    frame.length = 3;
    receive(h, &frame, data);
    sample_audio_tick();
    expect("RX FIRST");
    sample_audio_end();
    sample_audio_dump();
    puts("Opus framing, diagnostics and media scheduling tests passed");
}
