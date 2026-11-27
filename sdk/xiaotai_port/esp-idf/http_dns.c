// SPDX-License-Identifier: MIT
#include "http_dns.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port_config.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include "arch/sys_arch.h"
#include <stdatomic.h>
#include <string.h>

enum { FREE, WAITING, READY, ABANDONED, DNS_JOBS_MAX = 4 };

typedef struct {
    atomic_uint state;
    char name[HTTP_HOST_MAX + 1];
    ip_addr_t address;
    bool valid;
} dns_job_t;

#if XIAOTAI_PORT_HTTP_DNS_ON_DEMAND
static atomic_uint jobs_used;

static dns_job_t *job_acquire(void)
{
    unsigned used = atomic_load_explicit(&jobs_used, memory_order_relaxed);
    do {
        if (used >= DNS_JOBS_MAX) return NULL;
    } while (!atomic_compare_exchange_weak_explicit(&jobs_used, &used, used + 1U,
                                                     memory_order_relaxed, memory_order_relaxed));
    dns_job_t *job = xiaotai_mem_alloc(XIAOTAI_MEM_INTERNAL, sizeof(*job));
    if (!job) atomic_fetch_sub_explicit(&jobs_used, 1U, memory_order_relaxed);
    else atomic_init(&job->state, WAITING);
    return job;
}

static void job_release(dns_job_t *job)
{
    xiaotai_mem_free(job);
    atomic_fetch_sub_explicit(&jobs_used, 1U, memory_order_relaxed);
}
#else
static dns_job_t jobs[DNS_JOBS_MAX];

static dns_job_t *job_acquire(void)
{
    for (size_t i = 0; i < DNS_JOBS_MAX; ++i) {
        unsigned expect = FREE;
        if (atomic_compare_exchange_strong_explicit(&jobs[i].state, &expect, WAITING,
                                                     memory_order_acquire, memory_order_relaxed))
            return &jobs[i];
    }
    return NULL;
}

static void job_release(dns_job_t *job)
{
    atomic_store_explicit(&job->state, FREE, memory_order_release);
}
#endif

/* Timed-out DNS jobs stay owned by lwIP until its callback retires them. */
static void complete(const char *name, const ip_addr_t *address, void *ctx)
{
    (void)name;
    dns_job_t *job = ctx;
    if (address) job->address = *address;
    job->valid = address != NULL;
    unsigned expect = WAITING;
    if (!atomic_compare_exchange_strong_explicit(&job->state, &expect, READY,
                                                  memory_order_release, memory_order_relaxed))
        job_release(job);
}

static void resolve(void *ctx)
{
    dns_job_t *job = ctx;
    if (atomic_load_explicit(&job->state, memory_order_acquire) == ABANDONED) {
        job_release(job);
        return;
    }
    ip_addr_t address;
    err_t error = dns_gethostbyname(job->name, &address, complete, job);
    if (error != ERR_INPROGRESS) complete(job->name, error == ERR_OK ? &address : NULL, job);
}

xiaotai_err_t http_idf_resolve(const char *host, ip_addr_t *address, http_dns_check_t check, void *ctx)
{
    if (!sys_thread_tcpip(LWIP_CORE_IS_TCPIP_INITIALIZED)) return XIAOTAI_ERR_INVALID_STATE;
    if (ipaddr_aton(host, address)) return XIAOTAI_OK;
    dns_job_t *job = job_acquire();
    if (!job) return XIAOTAI_ERR_NO_MEM;
    memcpy(job->name, host, strlen(host) + 1);
    if (tcpip_try_callback(resolve, job) != ERR_OK) {
        job_release(job);
        return XIAOTAI_ERR_NO_MEM;
    }
    while (atomic_load_explicit(&job->state, memory_order_acquire) != READY) {
        xiaotai_err_t result = check(ctx);
        if (result != XIAOTAI_OK) {
            unsigned expect = WAITING;
            if (!atomic_compare_exchange_strong_explicit(&job->state, &expect, ABANDONED,
                                                         memory_order_release, memory_order_relaxed))
                job_release(job);
            return result;
        }
        xiaotai_delay_ms(1);
    }
    bool valid = job->valid;
    if (valid) *address = job->address;
    job_release(job);
    return valid ? XIAOTAI_OK : XIAOTAI_FAIL;
}
