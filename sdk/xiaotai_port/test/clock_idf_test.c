// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_clock_fallback.py
#include "clock_idf.h"
#include "xiaotai/port/clock.h"
#include <assert.h>
#include <stddef.h>

#define BOUNDARY (XIAOTAI_CLOCK_VALID_AFTER_S * 1000LL)
#define EPOCH 1760000000000LL

static struct { clock_idf_work_t run; void *arg; } queued, timer;
static bool on_tcpip, external_running = true, settling;
static int sync_mode = SNTP_SYNC_MODE_IMMED;
static int64_t wall_ms = EPOCH;
static u8_t reach[SNTP_MAX_SERVERS];
static unsigned external_notifications, changes_to_owner;
static struct timeval last_notification;

static void external_answer(struct timeval *tv)
{
    assert(on_tcpip);
    ++external_notifications;
    last_notification = *tv;
}

static clock_idf_answer_t callback = external_answer;

int __wrap_gettimeofday(struct timeval *tv, void *zone)
{
    assert(!zone);
    *tv = (struct timeval){.tv_sec = wall_ms / 1000, .tv_usec = (wall_ms % 1000) * 1000};
    return 0;
}

int __wrap_settimeofday(const struct timeval *tv, const struct timezone *zone)
{
    (void)tv;
    (void)zone;
    assert(false); /* The observer never sets or adjusts an owner's clock. */
    return -1;
}

int __wrap_adjtime(const struct timeval *delta, struct timeval *remaining)
{
    assert(on_tcpip && !delta && remaining);
    *remaining = (struct timeval){.tv_usec = settling ? 5000 : 0};
    return 0;
}

bool sys_thread_tcpip(int type) { assert(type == LWIP_CORE_IS_TCPIP_INITIALIZED); return true; }

err_t tcpip_try_callback(clock_idf_work_t work, void *arg)
{
    assert(!queued.run && !on_tcpip);
    queued.run = work;
    queued.arg = arg;
    return ERR_OK;
}

void sys_timeout(uint32_t ms, clock_idf_work_t work, void *arg)
{
    assert(on_tcpip && !timer.run && (ms == 20 || ms == 250));
    timer.run = work;
    timer.arg = arg;
}

static void step_tcpip(bool timeout)
{
    clock_idf_work_t work = timeout ? timer.run : queued.run;
    void *arg = timeout ? timer.arg : queued.arg;
    assert(work && !on_tcpip);
    if (timeout) timer.run = NULL;
    else queued.run = NULL;
    on_tcpip = true;
    work(arg);
    on_tcpip = false;
}

u8_t sntp_enabled(void) { assert(on_tcpip); return external_running; }
u8_t sntp_getreachability(u8_t idx)
{ assert(on_tcpip && idx < SNTP_MAX_SERVERS); return reach[idx]; }
int esp_sntp_get_sync_mode(void) { assert(on_tcpip); return sync_mode; }

void esp_sntp_set_time_sync_notification_cb(clock_idf_answer_t fn)
{ ++changes_to_owner; callback = fn; }
void esp_sntp_set_sync_mode(int mode) { ++changes_to_owner; sync_mode = mode; }
void sntp_setoperatingmode(u8_t mode) { (void)mode; ++changes_to_owner; }
void sntp_setserver(u8_t i, const ip_addr_t *a) { (void)i; (void)a; ++changes_to_owner; }
void sntp_setservername(u8_t i, const char *n) { (void)i; (void)n; ++changes_to_owner; }
void sntp_init(void) { ++changes_to_owner; external_running = true; }
void sntp_stop(void) { ++changes_to_owner; external_running = false; }

uint32_t esp_random(void) { assert(false); return 0; }
err_t dns_gethostbyname_addrtype(const char *n, ip_addr_t *a, clock_idf_dns_t fn, void *c, u8_t ty)
{ (void)n; (void)a; (void)fn; (void)c; (void)ty; assert(false); return ERR_INPROGRESS; }

static void external_update(int64_t epoch_ms)
{
    /* Match lwIP's order: shift on send, apply reply and notify owner, set low bit. */
    assert(external_running && callback == external_answer);
    reach[0] <<= 1;
    if (!settling) wall_ms = epoch_ms;
    struct timeval tv = {.tv_sec = epoch_ms / 1000, .tv_usec = (epoch_ms % 1000) * 1000};
    on_tcpip = true;
    callback(&tv);
    on_tcpip = false;
    reach[0] |= 1U;
}

static xiaotai_clock_stats_t stats(void)
{
    xiaotai_clock_stats_t snapshot;
    xiaotai_clock_stats(&snapshot);
    return snapshot;
}

int main(void)
{
    const char *servers[] = {"a.invalid", "b.invalid"};
    assert(xiaotai_clock_init(&(xiaotai_clock_config_t){.servers = servers, .server_count = 2}) == XIAOTAI_OK);
    assert(xiaotai_clock_sync(0) == XIAOTAI_ERR_TIMEOUT && queued.run);
    step_tcpip(false);
    assert(!xiaotai_clock_synced() && stats().running && stats().starts == 1);

    external_update(BOUNDARY);
    step_tcpip(true);
    assert(reach[0] == 1 && !xiaotai_clock_synced() && stats().invalid == 1);
    step_tcpip(true);
    assert(stats().invalid == 1); /* A repeated sample does not count as another invalid reply. */

    external_update(EPOCH);
    assert(reach[0] == 3); /* Neither poll observes the intervening shift to 2. */
    step_tcpip(true);
    assert(xiaotai_clock_sync(0) == XIAOTAI_OK && stats().syncs == 1 && stats().invalid == 1);
    assert(stats().running && !changes_to_owner && external_notifications == 2);

    sync_mode = SNTP_SYNC_MODE_SMOOTH;
    settling = true;
    external_update(EPOCH + 1000);
    step_tcpip(true);
    assert(stats().syncs == 1);
    settling = false;
    wall_ms = EPOCH + 1000;
    step_tcpip(true);
    assert(stats().syncs == 2 && xiaotai_clock_unix_ms() == wall_ms);

    for (unsigned i = 0; i < 5; ++i) {
        external_update(wall_ms + 1000);
        step_tcpip(true);
    }
    assert(reach[0] == UINT8_MAX && stats().syncs == 7);
    external_update(EPOCH + 8000);
    step_tcpip(true);
    step_tcpip(true);
    assert(stats().syncs == 7 && external_notifications == 9);
    assert(last_notification.tv_sec == EPOCH / 1000 + 8 && xiaotai_clock_unix_ms() == EPOCH + 8000);

    external_running = false;
    step_tcpip(true);
    assert(!stats().running && !timer.run && !queued.run && !changes_to_owner && callback == external_answer);
    return 0;
}
