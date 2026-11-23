// SPDX-License-Identifier: MIT
#include "recognition_decision.h"
#include "recognition_playback.h"

#include <string.h>

#define RC(name) XIAOTAI_RECOGNITION_##name
#define RJ(name) XIAOTAI_RECOGNITION_JOURNAL_##name

const rc_item_t *rc_find_item(const rc_task_t *task, const char *id)
{
    if (!task) return NULL;
    for (size_t i = 0; i < task->info.items; ++i) {
        char candidate[XIAOTAI_RECOGNITION_ID_MAX];
        if (rc_task_string(task, task->items[i].id, candidate, sizeof(candidate), NULL) == XIAOTAI_OK &&
            !strcmp(id, candidate)) return &task->items[i];
    }
    return NULL;
}

static xiaotai_err_t check_dispositions(const rc_task_t *task, const xiaotai_recognition_journal_mirror_t *mirror,
                                         bool archive)
{
    if (!task) return XIAOTAI_ERR_INVALID_STATE;
    if (!task->info.items) return XIAOTAI_ERR_NOT_FOUND;
    for (size_t i = 0; i < task->info.items; ++i) {
        const rc_item_t *item = &task->items[i];
        char id[XIAOTAI_RECOGNITION_ID_MAX];
        if (rc_task_string(task, item->id, id, sizeof(id), NULL) != XIAOTAI_OK) return RC_INVALID_RESPONSE;
        const xiaotai_recognition_journal_item_state_t state = rc_item_consumption(mirror, id);
        if (state == RJ(PLAY_STARTED) || (archive && state == RJ(UNTOUCHED) && !rc_speech_skippable(item)))
            return XIAOTAI_ERR_INVALID_STATE;
    }
    return XIAOTAI_OK;
}

xiaotai_err_t rc_allow_resume(const rc_task_t *task, const xiaotai_recognition_journal_mirror_t *mirror)
{
    return check_dispositions(task, mirror, false);
}

xiaotai_err_t rc_allow_archive(const rc_task_t *task, const xiaotai_recognition_journal_mirror_t *mirror,
                               uint64_t revision, bool rejected)
{
    if (!task) return rejected && !revision && mirror->version == 4U ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
    if (task->info.revision != revision || task->info.state == RC(TASK_QUEUED) ||
        task->info.state == RC(TASK_PROCESSING) || task->info.state == RC(TASK_UNKNOWN)) return XIAOTAI_ERR_INVALID_STATE;
    return check_dispositions(task, mirror, true);
}
