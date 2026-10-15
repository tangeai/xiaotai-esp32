#pragma once
#include <stdbool.h>
#include "tiRTC.h"
void sample_audio_begin(tirtc_conn_t connection);
void sample_audio_end(void);
void sample_audio_subscribed(uint8_t stream, bool enabled);
void sample_audio_tick(void);
void sample_audio_receive(tirtc_conn_t connection, const TIRTCFRAMEINFO *frame, const void *data);
void sample_audio_status(void);
void sample_audio_dump(void);
void sample_audio_record(void);
