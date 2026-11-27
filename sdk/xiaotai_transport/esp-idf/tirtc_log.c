// SPDX-License-Identifier: MIT
#include <stdatomic.h>
#include <string.h>
#include "tirtc_internal.h"
#include "xiaotai/net.h"
#include "xiaotai/port/log.h"

static _Atomic uint32_t s_bytes, s_facts, s_messages, s_samples, s_skipped;
static atomic_int s_service;
/* The threshold in force, and whether the console asked to see the lines. */
static atomic_uint s_level;
static atomic_bool s_print;
#if XT_TIRTC_TGTRP
static atomic_bool s_collect;
#endif

static void add(_Atomic uint32_t *counter, uint32_t amount)
{
    uint32_t seen = atomic_load_explicit(counter, memory_order_relaxed);
    uint32_t next;
    do {
        next = amount > UINT32_MAX - seen ? UINT32_MAX : seen + amount;
    } while (!atomic_compare_exchange_weak_explicit(counter, &seen, next,
                memory_order_relaxed, memory_order_relaxed));
}

void xt_tirtc_log_message(void) { add(&s_messages, 1); }

static bool has(const char *text, size_t size, const char *marker)
{
    size_t length = strlen(marker);
    if (length > size) return false;
    for (size_t i = 0; i <= size - length; ++i)
        if (!memcmp(text + i, marker, length)) return true;
    return false;
}

static int service_failure(const char *text, size_t size)
{
    static const char path[] = "/v1/start";
    const size_t path_length = sizeof(path) - 1U;
    if (size < path_length + 3U) return 0;
    for (size_t i = 0; i <= size - path_length; ++i) {
        if (memcmp(text + i, path, path_length)) continue;
        size_t at = i + path_length;
        if (size - at >= 2U && !memcmp(text + at, ": ", 2)) at += 2U;
        else if (size - at >= 9U && !memcmp(text + at, ", status ", 9)) at += 9U;
        else continue;
        int code = 0;
        size_t digits = 0;
        while (at < size && text[at] >= '0' && text[at] <= '9') {
            if (++digits > 7U) return 0;
            code = code * 10 + (text[at++] - '0');
        }
        if (!digits || !code) return 0;
        if (at < size && text[at] && !strchr("(,: \r\n", text[at])) return 0;
        return code;
    }
    return 0;
}

static void sdk_log(const char *text, uint32_t size)
{
    if (!text || !size) return;
    add(&s_bytes, size);
    /* SDK strings can contain credentials: they are rendered only after the
     * console asked for them (xiaotai_rtc_set_log_level), never copied. */
    if (atomic_load_explicit(&s_print, memory_order_relaxed)) {
        uint32_t shown = size;
        while (shown && (text[shown - 1] == '\n' || text[shown - 1] == '\r' || !text[shown - 1])) --shown;
        if (shown) XIAOTAI_LOGI("tirtc", "%.*s", (int)(shown > 480U ? 480U : shown), text);
    }
    if (size > 2048U) return;
    const int code = service_failure(text, size);
    if (code) {
        atomic_store_explicit(&s_service, code, memory_order_relaxed);
        add(&s_facts, 1);
    } else if (has(text, size, "peer_connection_tgtrp_init ok ice=") ||
               has(text, size, "peer_connection_kcp_init ikcp_nodelay(")) {
        add(&s_facts, 1);
    }
}

#if XT_TIRTC_TGTRP
static void transport_log(const char *format, va_list input)
{
    if (!format) return;
    static const char stat[] = "(%s:%d) tgtrp stat ondata-ex conn=%p ch=%p dc=%p dci=%u tr=%u media=%u "
        "stream=%u ft=%d size=%d payload=%u off=%u peer=%u dbg=%u diff=%d rtt=%u one=%u sndbuf=%u "
        "fid=%u send_span=%u recv_span=%u deliv=%u deliv_rtt=%u lrtt=%u prtt=%u nack=%u retx=%u "
        "pend_wait=%u pend_q=%u pacer_q_max=%u pacer_wait_max=%u send_samples=%u send_loss=%u recv_samples=%u recv_loss=%u";
    static const char stat_prefix[] = "(%s:%d) tgtrp stat ";
    if (!strncmp(format, stat_prefix, sizeof(stat_prefix) - 1U)) {
        if (!atomic_load_explicit(&s_collect, memory_order_relaxed)) return;
        /* The producer's exact format is the varargs ABI. Unknown revisions
         * are counted without touching their argument list. */
        if (strcmp(format, stat)) {
            add(&s_skipped, 1);
            return;
        }
        va_list args;
        va_copy(args, input);
        (void)va_arg(args, const char *);
        (void)va_arg(args, int);
        for (unsigned i = 0; i < 3; ++i) (void)va_arg(args, void *);
        xiaotai_net_trace_transport_t sample = {0};
        for (unsigned i = 0; i < 31; ++i) {
            const uint32_t value = i == 4 || i == 5 || i == 10 ?
                (uint32_t)va_arg(args, int) : va_arg(args, unsigned);
            switch (i) {
            case 2: sample.media = value; break;
            case 20: sample.rtt_ms = value; break;
            case 21: sample.nack = value; break;
            case 22: sample.retransmitted = value; break;
            case 28: sample.send_loss = value; break;
            case 30: sample.receive_loss = value; break;
            default: break;
            }
        }
        va_end(args);
        if (sample.media != 1U && sample.media != 2U) {
            add(&s_skipped, 1);
            return;
        }
        xiaotai_net_trace_transport(&sample);
        add(&s_samples, 1);
    } else if (!strcmp(format, "(%s:%d) peer_connection_tgtrp_init ok ice=%p mtu=%u "
                       "poll_segments=%u pacing=%u send_buffer_max=%zu") ||
               !strcmp(format, "(%s:%d) peer_connection_kcp_init ikcp_nodelay(%d %d %d %d) sndwnd=%d mtu=%d") ||
               !strcmp(format, "(%s:%d) media transport selected without tgtrp_enable_media_transport")) {
        add(&s_facts, 1);
    }
}
#endif

xiaotai_err_t xiaotai_rtc_set_log_level(uint8_t level)
{
    if (level > XIAOTAI_RTC_LOG_LEVEL_MAX) return XIAOTAI_ERR_INVALID_ARG;
    TiRtcLogSetLevel(level);
    atomic_store(&s_level, level);
    atomic_store(&s_print, level > 0);
    return XIAOTAI_OK;
}

uint8_t xiaotai_rtc_log_level(bool *printed)
{
    if (printed) *printed = atomic_load(&s_print);
    return (uint8_t)atomic_load(&s_level);
}

void xt_tirtc_log_configure(uint8_t level)
{
    atomic_store_explicit(&s_service, 0, memory_order_relaxed);
    atomic_store(&s_level, level);
    atomic_store(&s_print, false);
    TiRtcLogSetCallback(sdk_log);
    TiRtcLogSetLevel(level);
#if XT_TIRTC_TGTRP
    atomic_store_explicit(&s_collect, false, memory_order_relaxed);
    tgtrp_set_log_callback(transport_log);
    tgtrp_set_log_level(level & TGTRP_LOG_LEVEL_MASK);
#endif
}

void xiaotai_rtc_set_statistics(bool enabled)
{
#if XT_TIRTC_TGTRP
    atomic_store_explicit(&s_collect, enabled, memory_order_relaxed);
    /* Capture only statistics; unsolicited lower-layer messages can perturb
     * the weak-network timing. TiRTC's normal log level is independent. */
    tgtrp_set_log_level(enabled ? TGTRP_LOG_FLAG_STAT : 0);
#else
    (void)enabled;
#endif
}

void xiaotai_rtc_diag(xiaotai_rtc_diag_t *diag)
{
    if (!diag) return;
    *diag = (xiaotai_rtc_diag_t){
        .log_bytes = atomic_load_explicit(&s_bytes, memory_order_relaxed),
        .log_facts = atomic_load_explicit(&s_facts, memory_order_relaxed),
        .messages = atomic_load_explicit(&s_messages, memory_order_relaxed),
        .statistics = atomic_load_explicit(&s_samples, memory_order_relaxed),
        .statistics_skipped = atomic_load_explicit(&s_skipped, memory_order_relaxed),
        .service_status = atomic_load_explicit(&s_service, memory_order_relaxed),
    };
}
