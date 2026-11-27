// SPDX-License-Identifier: MIT
#pragma once

#include "recognition_journal.h"
#include "recognition_protocol.h"

/* Adopt takes the task on success. The owner may borrow it until replacing
 * or clearing the view; each reader pins its own immutable snapshot. */
xiaotai_err_t rc_result_adopt(rc_task_t *task, uint32_t generation,
                             const xiaotai_recognition_journal_record_t *record,
                             const xiaotai_recognition_journal_mirror_t *mirror);
void rc_result_update(const xiaotai_recognition_journal_mirror_t *mirror);
void rc_result_flush(void);
void rc_result_clear(void);
