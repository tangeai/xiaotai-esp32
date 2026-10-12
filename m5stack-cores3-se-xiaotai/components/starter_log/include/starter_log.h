#ifndef STARTER_LOG_H
#define STARTER_LOG_H

#include <stdint.h>

typedef enum {
    STARTER_LOG_AFE_FULL,
    STARTER_LOG_RX_ACCEPT,
    STARTER_LOG_RX_REJECT,
    STARTER_LOG_PLAYOUT,
    STARTER_LOG_TOPIC_COUNT,
} starter_log_topic_t;

enum { STARTER_LOG_VALUES = 17 };

/* Task context only. Fixed PSRAM mailboxes; no formatting, allocation, output
 * or waiting in the producer. Replaced details and contention are counted. */
void starter_log_record(starter_log_topic_t topic,
                        const uint32_t values[STARTER_LOG_VALUES]);

/* Single reader: the existing idle ringtone worker, outside all media/UI locks. */
void starter_log_flush_pending(void);

#endif
