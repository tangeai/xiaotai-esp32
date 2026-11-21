// SPDX-License-Identifier: MIT
#include "preroll.h"

#include <limits.h>
#include <stdatomic.h>

#include "xiaotai/framework.h"
#include "xiaotai/port/os.h"

static atomic_uint s_epoch = 1;
static atomic_uint s_posted;
static atomic_uint s_readers;

static void deliver_loss(void *arg)
{
    unsigned epoch = (unsigned)(uintptr_t)arg;
    atomic_fetch_add(&s_readers, 1);
    if (epoch != atomic_load(&s_epoch)) {
        atomic_fetch_sub(&s_readers, 1);
        return;
    }
    media_preroll_loss_t event = {0};
    const bool lost = media_preroll_take_loss(media_preroll_current(), xiaotai_time_us(), &event);
    atomic_compare_exchange_strong(&s_posted, &epoch, 0);
    atomic_fetch_sub(&s_readers, 1);
    if (lost && event.generation) media_session_preroll_lost(event.generation);
    else if (lost && event.lost) event.lost(event.token, event.ctx);
}

bool media_preroll_service(void)
{
    media_preroll_t *history = media_preroll_current();
    bool pending;
    if (media_preroll_poll_loss(history, xiaotai_time_us(), &pending) != XIAOTAI_OK) return true;
    if (!pending) return false;
    unsigned empty = 0;
    unsigned epoch = atomic_load(&s_epoch);
    if (!atomic_compare_exchange_strong(&s_posted, &empty, epoch)) return true;

    xiaotai_err_t err = xiaotai_run(deliver_loss, (void *)(uintptr_t)epoch);
    if (err == XIAOTAI_OK) return false;
    atomic_compare_exchange_strong(&s_posted, &epoch, 0);
    if (err == XIAOTAI_ERR_NO_MEM) media_preroll_post_retry(history);
    return true;
}

void media_preroll_dispatch_reset(void)
{
    if (atomic_fetch_add(&s_epoch, 1) == UINT_MAX) atomic_store(&s_epoch, 1);
    atomic_store(&s_posted, 0);
    /* The host tears down the ring after workers stop; a queued owner job may
       still be claiming its event. Its application callback does not hold a reference. */
    while (atomic_load(&s_readers)) xiaotai_delay_ms(1);
}

xiaotai_err_t xiaotai_media_preroll_hold(const xiaotai_media_preroll_t *request, uint32_t *token)
{
    if (token) *token = 0;
    media_preroll_t *history = media_preroll_current();
    if (!history) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_media_status_t status;
    xiaotai_media_status(&status);
    if (!status.ready || status.mic_muted || status.active || status.recording) return XIAOTAI_ERR_INVALID_STATE;
    return media_preroll_hold(history, request, xiaotai_time_us(), token);
}

void xiaotai_media_preroll_release(uint32_t token)
{
    if (xiaotai_on_owner()) media_preroll_release(media_preroll_current(), token);
}
