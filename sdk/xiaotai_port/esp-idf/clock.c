// SPDX-License-Identifier: MIT
#include "clock_internal.h"
#include "xiaotai/port/os.h"
#include "esp_random.h"
/* The callbacks below are already on the TCP/IP task. Suppress IDF's legacy
 * inline names, which conflict with lwIP and requeue operations to that task. */
#define ESP_LWIP_COMPONENT_BUILD
#include "esp_sntp.h"
#undef ESP_LWIP_COMPONENT_BUILD
/* esp_sntp.h also declares a compatibility spelling for this lwIP macro. */
#undef SNTP_OPMODE_POLL
#include "lwip/apps/sntp.h"
#include "lwip/apps/sntp_opts.h"
#include "lwip/dns.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "lwip/timeouts.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>

static portMUX_TYPE s_ref_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_reference_ms, s_reference_us;
static bool s_release_after_start;

static int64_t millis(const struct timeval *tv) { return (int64_t)tv->tv_sec * 1000 + tv->tv_usec / 1000; }

int64_t clock_wall_ms(void)
{
    struct timeval tv;
    return gettimeofday(&tv, NULL) == 0 ? millis(&tv) : 0;
}

static int64_t record_reference(int64_t wall_ms)
{
    const int64_t now = xiaotai_time_us();
    portENTER_CRITICAL(&s_ref_lock);
    const int64_t before = s_reference_ms + (now - s_reference_us) / 1000;
    s_reference_ms = wall_ms;
    s_reference_us = now;
    portEXIT_CRITICAL(&s_ref_lock);
    return before;
}

bool clock_wall_set(int64_t unix_ms)
{
    const time_t sec = (time_t)(unix_ms / 1000);
    if ((int64_t)sec != unix_ms / 1000) return false;
    const struct timeval tv = {.tv_sec = sec, .tv_usec = (suseconds_t)((unix_ms % 1000) * 1000)};
    if (settimeofday(&tv, NULL) != 0) return false;
    (void)record_reference(unix_ms);
    return true;
}

uint32_t clock_service_budget(size_t servers)
{
    uint32_t budget = ((uint32_t)servers + 1U) * SNTP_RECV_TIMEOUT;
#if SNTP_STARTUP_DELAY
    budget += CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY;
#endif
    return budget;
}

static void stop_ours(void *unused)
{
    (void)unused;
    sntp_stop();
    esp_sntp_set_time_sync_notification_cb(NULL);
    clock_service_stopped();
}

static void own_answer(struct timeval *tv)
{
    const int64_t actual = tv ? millis(tv) : 0;
    if (clock_service_answer(record_reference(actual), actual)) {
        /* Stop after the SNTP receive handler has returned and armed its timers. */
        sys_timeout(0, stop_ours, NULL);
    }
}

#if SNTP_MONITOR_SERVER_REACHABILITY
static uint8_t s_reach[SNTP_MAX_SERVERS];
static bool s_external_observed, s_external_pending;

static void poll_existing(void *unused)
{
    (void)unused;
    if (!sntp_enabled()) {
        clock_service_stopped();
        return;
    }
    bool replied = false, fresh = false;
    for (size_t i = 0; i < SNTP_MAX_SERVERS; ++i) {
        const uint8_t reach = sntp_getreachability((u8_t)i);
        replied |= reach != 0;
        /* A send and its reply can both land between polls (e.g. 1 -> 3).
         * An unchanged register, especially saturated 0xff, cannot prove a new reply. */
        fresh |= (reach & 1U) && reach != s_reach[i];
        s_reach[i] = reach;
    }
    /* Reachability records accepted replies in this boot. Observe it without
     * consuming the owner's completion semaphore or replacing its callback. */
    s_external_pending |= fresh || (replied && !s_external_observed);
    struct timeval remaining = {0};
    const bool settling = esp_sntp_get_sync_mode() == SNTP_SYNC_MODE_SMOOTH &&
        (adjtime(NULL, &remaining) != 0 || remaining.tv_sec || remaining.tv_usec);
    if (s_external_pending && !settling) {
        const int64_t actual = clock_wall_ms();
        (void)clock_service_answer(record_reference(actual), actual);
        s_external_observed = true;
        s_external_pending = false;
    }
    sys_timeout(s_external_observed ? 250 : 20, poll_existing, NULL);
}
#endif

static void start_tcpip(void *ctx)
{
    const clock_servers_t *servers = ctx;
    (void)record_reference(clock_wall_ms());
    if (sntp_enabled()) {
#if SNTP_MONITOR_SERVER_REACHABILITY
        for (size_t i = 0; i < SNTP_MAX_SERVERS; ++i) s_reach[i] = 0;
        s_external_observed = s_external_pending = false;
        clock_service_started(true);
        poll_existing(NULL);
#else
        clock_service_started(false);
#endif
        return;
    }
    if (servers->count > SNTP_MAX_SERVERS) {
        clock_service_started(false);
        return;
    }
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_set_time_sync_notification_cb(own_answer);
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    for (size_t i = 0; i < SNTP_MAX_SERVERS; ++i) {
        sntp_setserver((u8_t)i, NULL);
        if (i < servers->count) sntp_setservername((u8_t)i, servers->servers[i]);
    }
    sntp_init();
    const bool running = sntp_enabled();
    clock_service_started(running);
    if (running && s_release_after_start) stop_ours(NULL);
}

bool clock_service_start(const clock_servers_t *servers, bool release_after_start)
{
    s_release_after_start = release_after_start;
    return sys_thread_tcpip(LWIP_CORE_IS_TCPIP_INITIALIZED) &&
           tcpip_try_callback(start_tcpip, (void *)servers) == ERR_OK;
}

void clock_service_poll(void) {}
void clock_service_wait_timed_out(void) {}

typedef enum { DNS_IDLE, DNS_PENDING, DNS_READY, DNS_FAILED } dns_state_t;
typedef struct { dns_state_t state; uint32_t ipv4; } dns_query_t;
static portMUX_TYPE s_dns_lock = portMUX_INITIALIZER_UNLOCKED;
static dns_query_t s_dns[XIAOTAI_CLOCK_SERVERS_MAX];

static void resolved(const char *name, const ip_addr_t *addr, void *ctx)
{
    (void)name;
    dns_query_t *query = ctx;
    portENTER_CRITICAL(&s_dns_lock);
    if (addr && IP_IS_V4(addr)) {
        query->ipv4 = ip4_addr_get_u32(ip_2_ip4(addr));
        query->state = DNS_READY;
    } else query->state = DNS_FAILED;
    portEXIT_CRITICAL(&s_dns_lock);
}

static void resolve_tcpip(void *ctx)
{
    const clock_servers_t *servers = ctx;
    for (size_t i = 0; i < servers->count; ++i) {
        dns_query_t *query = &s_dns[i];
        portENTER_CRITICAL(&s_dns_lock);
        const bool submit = query->state != DNS_PENDING;
        if (submit) query->state = DNS_PENDING;
        portEXIT_CRITICAL(&s_dns_lock);
        if (!submit) continue;
        ip_addr_t addr;
        const err_t err = dns_gethostbyname_addrtype(servers->servers[i], &addr, resolved, query, LWIP_DNS_ADDRTYPE_IPV4);
        if (err != ERR_INPROGRESS) resolved(servers->servers[i], err == ERR_OK ? &addr : NULL, query);
    }
}

typedef struct { int fd; bool opened; uint64_t cookie; int64_t sent_us; } peer_t;

static int open_peer(peer_t *peer, uint32_t address)
{
    struct sockaddr_in remote = {.sin_family = AF_INET, .sin_port = htons(123)};
    remote.sin_addr.s_addr = address;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;
    int flags = fcntl(fd, F_GETFL, 0);
    if (fd >= FD_SETSIZE || flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0 ||
        connect(fd, (const struct sockaddr *)&remote, sizeof(remote)) != 0) {
        close(fd);
        return -1;
    }
    const int64_t wall = clock_wall_ms();
    const uint32_t seconds = (uint32_t)((wall > 0 ? (uint64_t)wall / 1000U : 0) + 2208988800ULL);
    const uint32_t fraction = esp_random();
    peer->cookie = ((uint64_t)seconds << 32) | (fraction ? fraction : 1U);
    peer->sent_us = xiaotai_time_us();
    uint8_t packet[CLOCK_NTP_BYTES];
    clock_ntp_request(packet, peer->cookie);
    if (send(fd, packet, sizeof(packet), 0) == (ssize_t)sizeof(packet)) return fd;
    close(fd);
    return -1;
}

bool clock_parallel_probe(const clock_servers_t *servers, int64_t *unix_ms, uint32_t *invalid)
{
    if (!sys_thread_tcpip(LWIP_CORE_IS_TCPIP_INITIALIZED)) return false;
    peer_t peers[XIAOTAI_CLOCK_SERVERS_MAX] = {0};
    const int64_t started = xiaotai_time_us();
    int64_t deadline = started + 3000000;
    for (size_t i = 0; i < servers->count; ++i) {
        peers[i].fd = -1;
        portENTER_CRITICAL(&s_dns_lock);
        if (s_dns[i].state != DNS_PENDING) s_dns[i].state = DNS_IDLE;
        portEXIT_CRITICAL(&s_dns_lock);
    }
    if (tcpip_try_callback(resolve_tcpip, (void *)servers) != ERR_OK) return false;
    uint64_t samples[XIAOTAI_CLOCK_SERVERS_MAX];
    size_t n = 0;
    while (xiaotai_time_us() < deadline) {
        fd_set read_set;
        FD_ZERO(&read_set);
        bool resolving = false;
        int max_fd = -1;
        for (size_t i = 0; i < servers->count; ++i) {
            peer_t *peer = &peers[i];
            if (!peer->opened) {
                portENTER_CRITICAL(&s_dns_lock);
                const dns_query_t query = s_dns[i];
                portEXIT_CRITICAL(&s_dns_lock);
                if (query.state == DNS_READY) {
                    peer->opened = true;
                    peer->fd = open_peer(peer, query.ipv4);
                } else if (query.state == DNS_FAILED) peer->opened = true;
                else resolving = true;
            }
            if (peer->fd < 0) continue;
            FD_SET(peer->fd, &read_set);
            if (peer->fd > max_fd) max_fd = peer->fd;
        }
        if (max_fd < 0) {
            if (!resolving) break;
            xiaotai_delay_ms(10);
            continue;
        }
        int64_t left = deadline - xiaotai_time_us();
        if (left <= 0) break;
        if (resolving && left > 10000) left = 10000;
        struct timeval wait = {.tv_sec = left / 1000000, .tv_usec = left % 1000000};
        const int ready = select(max_fd + 1, &read_set, NULL, NULL, &wait);
        if (ready < 0 && errno != EINTR) break;
        if (ready <= 0) continue;
        for (size_t i = 0; i < servers->count; ++i) {
            peer_t *peer = &peers[i];
            if (peer->fd < 0 || !FD_ISSET(peer->fd, &read_set)) continue;
            uint8_t packet[CLOCK_NTP_BYTES];
            const ssize_t got = recv(peer->fd, packet, sizeof(packet), 0);
            const int64_t received = xiaotai_time_us();
            if (got < 0 || received > deadline) continue;
            if (clock_ntp_answer(packet, (size_t)got, peer->cookie, (uint64_t)(received - peer->sent_us), &samples[n])) {
                close(peer->fd);
                peer->fd = -1;
                if (++n == 1 && received + 500000 < deadline) deadline = received + 500000;
            } else ++*invalid;
        }
    }
    for (size_t i = 0; i < servers->count; ++i) if (peers[i].fd >= 0) close(peers[i].fd);
    uint64_t result;
    if (!n) return false;
    if (!clock_ntp_mean(samples, n, &result)) {
        ++*invalid;
        return false;
    }
    *unix_ms = (int64_t)(result / 1000U);
    return true;
}
