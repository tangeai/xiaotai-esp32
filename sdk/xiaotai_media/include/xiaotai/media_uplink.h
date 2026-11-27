// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/media_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Engine-to-engine adapter, not an application sink replacement. The
 * descriptor and its sink live for the program. Only that sink is admitted
 * by a media pipeline which starts with the attached executor selected. */
typedef struct {
    const xiaotai_media_sink_t *sink;
    void (*wake)(void);         /* nonblocking; called only after admission */
    bool (*is_current)(void);   /* only this task may drain */
} xiaotai_media_uplink_executor_t;

typedef enum {
    XIAOTAI_MEDIA_UPLINK_IDLE,
    XIAOTAI_MEDIA_UPLINK_PENDING,
    XIAOTAI_MEDIA_UPLINK_CONTENDED,
} xiaotai_media_uplink_work_t;

/* [init] Attach before media starts. A disabled or ineligible pipeline uses
 * its own task. [reset] Detach before tearing down the wake target. */
xiaotai_err_t xiaotai_media_uplink_attach(const xiaotai_media_uplink_executor_t *executor);
void xiaotai_media_uplink_detach(const xiaotai_media_uplink_executor_t *executor);

/* [executor] Examine/consume at most one frame. No waiting for the media
 * lock or queue. The admitted sink call itself may block in the backend. */
xiaotai_media_uplink_work_t xiaotai_media_uplink_step(const xiaotai_media_uplink_executor_t *executor);

#ifdef __cplusplus
}
#endif
