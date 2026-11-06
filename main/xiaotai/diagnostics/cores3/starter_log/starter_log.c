#include "starter_log.h"

#include <stdbool.h>
#include <stdatomic.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

#define STARTER_LOG_INTERVAL_MS 5000U

typedef struct {
    atomic_uint sequence, count, missed, at_ms, values[STARTER_LOG_VALUES];
} log_mailbox_t;
typedef struct {
    uint32_t sequence, missed, at_ms, count, values[STARTER_LOG_VALUES];
} log_snapshot_t;
typedef struct { uint32_t sequence, missed, last_ms; } log_reader_t;

static EXT_RAM_BSS_ATTR log_mailbox_t s_pending[STARTER_LOG_TOPIC_COUNT];
static EXT_RAM_BSS_ATTR log_reader_t s_readers[STARTER_LOG_TOPIC_COUNT];
_Static_assert(sizeof(s_pending) + sizeof(s_readers) == 384U,
               "deferred audio log PSRAM budget changed");

void starter_log_record(starter_log_topic_t topic,
                        const uint32_t values[STARTER_LOG_VALUES])
{
    if ((unsigned)topic >= STARTER_LOG_TOPIC_COUNT || !values) return;
    log_mailbox_t *box = &s_pending[topic];
    unsigned sequence = atomic_load(&box->sequence);
    /* One attempt only, even if old/new SDK callback threads overlap. */
    if ((sequence & 1U) ||
        !atomic_compare_exchange_strong(&box->sequence, &sequence, sequence + 1U)) {
        atomic_fetch_add(&box->missed, 1U);
        return;
    }
    atomic_fetch_add(&box->count, 1U);
    atomic_store(&box->at_ms, (uint32_t)(esp_timer_get_time() / 1000));
    for (unsigned i = 0; i < STARTER_LOG_VALUES; ++i)
        atomic_store(&box->values[i], values[i]);
    atomic_store(&box->sequence, sequence + 2U);
}

static bool read_pending(const log_mailbox_t *box, log_snapshot_t *out)
{
    unsigned sequence = atomic_load(&box->sequence);
    if (sequence & 1U) return false;
    out->sequence = sequence;
    out->missed = atomic_load(&box->missed);
    out->count = atomic_load(&box->count);
    out->at_ms = atomic_load(&box->at_ms);
    for (unsigned i = 0; i < STARTER_LOG_VALUES; ++i)
        out->values[i] = atomic_load(&box->values[i]);
    return sequence == atomic_load(&box->sequence);
}

void starter_log_flush_pending(void)
{
    for (unsigned topic = 0; topic < STARTER_LOG_TOPIC_COUNT; ++topic) {
        log_reader_t *reader = &s_readers[topic];
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if ((reader->sequence || reader->missed) &&
            now - reader->last_ms < STARTER_LOG_INTERVAL_MS) continue;
        log_snapshot_t report;
        if (!read_pending(&s_pending[topic], &report) ||
            (report.sequence == reader->sequence && report.missed == reader->missed)) continue;
        uint32_t age_ms = (uint32_t)(esp_timer_get_time() / 1000) - report.at_ms;
#define V(n) ((unsigned long)report.values[n])
#define META (unsigned long)report.count, (unsigned long)report.missed, (unsigned long)age_ms
        switch (topic) {
        case STARTER_LOG_AFE_FULL:
            ESP_LOGE("starter_aec", "AFE in-ovf=%lu deferred=%lu missed=%lu age_ms=%lu", V(0), META);
            break;
        case STARTER_LOG_RX_ACCEPT:
            ESP_LOGI("starter_tirtc", "downlink accepted mode=%lu gen=%lu stream=%lu media=%lu flags=%lu len=%lu n=%lu deferred=%lu missed=%lu age_ms=%lu",
                     V(0), V(1), V(2), V(3), V(4), V(5), V(6), META);
            break;
        case STARTER_LOG_RX_REJECT:
            ESP_LOGW("starter_tirtc", "downlink rejected mode=%lu gen=%lu stream=%lu media=%lu flags=%lu len=%lu n=%lu deferred=%lu missed=%lu age_ms=%lu",
                     V(0), V(1), V(2), V(3), V(4), V(5), V(6), META);
            break;
        case STARTER_LOG_PLAYOUT:
            ESP_LOGI("starter_media", "JB target=%lu q=%lu jitter=%lu gap=%lu late=%lu ovf=%lu lock=%lu/%lu owner=%lu wr=%lu/%lu pcm=%lu/%lu/%lu+%lu gen=%lu epoch=%lu deferred=%lu missed=%lu age_ms=%lu",
                     V(0), V(1), V(2), V(3), V(4), V(5), V(6), V(7), V(8), V(9), V(10),
                     V(11), V(12), V(13), V(14), V(15), V(16), META);
            break;
        default:
            break;
        }
#undef V
#undef META
        /* A producer may publish during slow output. Acknowledge only this
         * snapshot; never clear a newer report or its contention count. */
        reader->sequence = report.sequence;
        reader->missed = report.missed;
        reader->last_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }
}
