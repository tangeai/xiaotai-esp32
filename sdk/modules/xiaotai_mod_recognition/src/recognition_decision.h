// SPDX-License-Identifier: MIT
#pragma once

#include "recognition_journal.h"
#include "recognition_protocol.h"

const rc_item_t *rc_find_item(const rc_task_t *task, const char *id);
xiaotai_err_t rc_allow_resume(const rc_task_t *task, const xiaotai_recognition_journal_mirror_t *mirror);
xiaotai_err_t rc_allow_archive(const rc_task_t *task, const xiaotai_recognition_journal_mirror_t *mirror,
                               uint64_t revision, bool rejected);
