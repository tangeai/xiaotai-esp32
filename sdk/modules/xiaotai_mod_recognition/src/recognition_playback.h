// SPDX-License-Identifier: MIT
#pragma once

#include "recognition_journal.h"
#include "recognition_protocol.h"
#include "xiaotai/sounds.h"

typedef enum { RC_ORDER_NONE, RC_ORDER_WAIT, RC_ORDER_READY, RC_ORDER_BLOCKED } rc_order_t;

bool rc_speech_skippable(const rc_item_t *item);
bool rc_speech_ready(const rc_item_t *item);
rc_order_t rc_choose_speech(const rc_task_t *task, const xiaotai_recognition_journal_mirror_t *mirror,
                            char id[XIAOTAI_RECOGNITION_ID_MAX]);
xiaotai_err_t rc_playback_wav(const uint8_t *data, size_t size, xiaotai_sound_t *sound, uint32_t *duration_ms);
