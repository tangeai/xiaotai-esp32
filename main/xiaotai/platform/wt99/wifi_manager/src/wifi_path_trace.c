#include "wifi_path_trace.h"
#include "sdkconfig.h"

#ifdef CONFIG_APP_PACKET_PATH_TRACE
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lwip/sockets.h"

#define PATH_TRACE_SECONDS 1800U
typedef struct {
    uint32_t net_wan, net_lan, net_wan_bytes, socket_wan, socket_lan, socket_wan_bytes;
    uint32_t audio, video, tp_audio, tp_video;
    uint32_t net_gap_ms, socket_gap_ms, audio_gap_ms, video_gap_ms;
    uint32_t rtt_ms, nack, retx, send_loss, receive_loss;
    uint32_t probe, probe_gaps, probe_duplicate, probe_last;
} path_bin_t;

EXT_RAM_BSS_ATTR static path_bin_t s_bins[PATH_TRACE_SECONDS];
static portMUX_TYPE s_path_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool s_armed;
static uint32_t s_start_ms, s_used, s_last[4];
static bool s_have_last[4], s_have_probe;
static uint32_t s_probe_last;

static uint16_t be16(const uint8_t *p) { return (uint16_t)p[0] * 256U + p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)be16(p) * 65536U + be16(p + 2); }
static bool private_v4(const uint8_t *p)
{
    return p[0] == 10U || p[0] == 127U ||
        (p[0] == 192U && p[1] == 168U) ||
        (p[0] == 172U && p[1] >= 16U && p[1] <= 31U) ||
        (p[0] == 169U && p[1] == 254U);
}

/* Lock only the small counter update, never a packet, socket or UART call. */
static path_bin_t *bin_locked(uint32_t now)
{
    uint32_t index = (now - s_start_ms) / 1000U;
    if (!atomic_load_explicit(&s_armed, memory_order_relaxed)) return NULL;
    if (index >= PATH_TRACE_SECONDS) {
        atomic_store_explicit(&s_armed, false, memory_order_release);
        return NULL;
    }
    if (s_used <= index) s_used = index + 1U;
    return &s_bins[index];
}

static void gap_locked(unsigned kind, uint32_t now, uint32_t *maximum)
{
    uint32_t gap = now - s_last[kind];
    if (s_have_last[kind] && gap > *maximum) *maximum = gap;
    s_have_last[kind] = true;
    s_last[kind] = now;
}

bool wifi_path_trace_active(void) { return atomic_load_explicit(&s_armed, memory_order_acquire); }
bool wifi_path_trace_transport_active(void)
{
#ifdef CONFIG_APP_PACKET_PATH_TRANSPORT_STATS
    return wifi_path_trace_active();
#else
    return false;
#endif
}

esp_err_t wifi_path_trace_start(void)
{
    if (wifi_path_trace_active()) return ESP_ERR_INVALID_STATE;
    taskENTER_CRITICAL(&s_path_lock);
    atomic_store_explicit(&s_armed, false, memory_order_release);
    taskEXIT_CRITICAL(&s_path_lock);
    memset(s_bins, 0, sizeof(s_bins));
    taskENTER_CRITICAL(&s_path_lock);
    s_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_used = 0;
    memset(s_have_last, 0, sizeof(s_have_last));
    s_have_probe = false;
    atomic_store_explicit(&s_armed, true, memory_order_release);
    taskEXIT_CRITICAL(&s_path_lock);
    return ESP_OK;
}

void wifi_path_trace_stop(void)
{
    taskENTER_CRITICAL(&s_path_lock);
    atomic_store_explicit(&s_armed, false, memory_order_release);
    taskEXIT_CRITICAL(&s_path_lock);
}

static void note_network(const void *data, size_t length)
{
    if (!wifi_path_trace_active() || !data || length < 42U) return;
    const uint8_t *p = data;
    if (be16(p + 12) != 0x0800U || (p[14] >> 4) != 4U || p[23] != 17U) return;
    size_t ihl = (p[14] & 15U) * 4U;
    uint16_t total = be16(p + 16);
    if (ihl < 20U || total < ihl + 8U || length < 14U + total) return;
    bool lan = private_v4(p + 26);
    bool probe = false;
    uint32_t sequence = 0;
    if (!(be16(p + 20) & 0x3fffU) && total >= ihl + 16U) {
        const uint8_t *udp = p + 14U + ihl;
        if (be16(udp + 2) == 49162U && be16(udp + 4) >= 16U && !memcmp(udp + 8, "XTLN", 4)) {
            probe = true;
            sequence = be32(udp + 12);
        }
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    taskENTER_CRITICAL(&s_path_lock);
    path_bin_t *bin = bin_locked(now);
    if (bin) {
        if (lan) ++bin->net_lan;
        else {
            ++bin->net_wan;
            bin->net_wan_bytes += total;
            gap_locked(0, now, &bin->net_gap_ms);
        }
        if (probe) {
            ++bin->probe;
            if (s_have_probe) {
                uint32_t delta = sequence - s_probe_last;
                if (!delta || delta >= 0x80000000U) ++bin->probe_duplicate;
                else {
                    if (delta > 1U) bin->probe_gaps += delta - 1U;
                    s_probe_last = sequence;
                }
            } else {
                s_probe_last = sequence;
            }
            s_have_probe = true;
            bin->probe_last = sequence;
        }
    }
    taskEXIT_CRITICAL(&s_path_lock);
}

esp_err_t __real_esp_netif_receive(esp_netif_t *netif, void *buffer, size_t length, void *owner);
esp_err_t __wrap_esp_netif_receive(esp_netif_t *netif, void *buffer, size_t length, void *owner)
{
    note_network(buffer, length);
    return __real_esp_netif_receive(netif, buffer, length, owner);
}

ssize_t __real_lwip_recvfrom(int socket, void *buffer, size_t length, int flags,
                           struct sockaddr *from, socklen_t *from_length);
ssize_t __wrap_lwip_recvfrom(int socket, void *buffer, size_t length, int flags,
                           struct sockaddr *from, socklen_t *from_length)
{
    ssize_t result = __real_lwip_recvfrom(socket, buffer, length, flags, from, from_length);
    int saved_errno = errno;
    if (result > 0 && wifi_path_trace_active() && from && from_length &&
        *from_length >= sizeof(struct sockaddr_in) && from->sa_family == AF_INET) {
        const struct sockaddr_in *peer = (const struct sockaddr_in *)from;
        bool lan = private_v4((const uint8_t *)&peer->sin_addr.s_addr);
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        taskENTER_CRITICAL(&s_path_lock);
        path_bin_t *bin = bin_locked(now);
        if (bin) {
            if (lan) ++bin->socket_lan;
            else {
                ++bin->socket_wan;
                bin->socket_wan_bytes += (uint32_t)result;
                gap_locked(1, now, &bin->socket_gap_ms);
            }
        }
        taskEXIT_CRITICAL(&s_path_lock);
    }
    errno = saved_errno;
    return result;
}

void wifi_path_trace_media(bool video)
{
    if (!wifi_path_trace_active()) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    taskENTER_CRITICAL(&s_path_lock);
    path_bin_t *bin = bin_locked(now);
    if (bin) {
        if (video) { ++bin->video; gap_locked(3, now, &bin->video_gap_ms); }
        else { ++bin->audio; gap_locked(2, now, &bin->audio_gap_ms); }
    }
    taskEXIT_CRITICAL(&s_path_lock);
}

void wifi_path_trace_transport(uint32_t media, uint32_t rtt, uint32_t nack,
                               uint32_t retx, uint32_t send_loss, uint32_t receive_loss)
{
    if (!wifi_path_trace_active()) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    taskENTER_CRITICAL(&s_path_lock);
    path_bin_t *bin = bin_locked(now);
    if (bin) {
        if (media == 1U) ++bin->tp_audio;
        if (media == 2U) ++bin->tp_video;
        if (rtt > bin->rtt_ms) bin->rtt_ms = rtt;
        if (nack > bin->nack) bin->nack = nack;
        if (retx > bin->retx) bin->retx = retx;
        if (send_loss > bin->send_loss) bin->send_loss = send_loss;
        if (receive_loss > bin->receive_loss) bin->receive_loss = receive_loss;
    }
    taskEXIT_CRITICAL(&s_path_lock);
}

esp_err_t wifi_path_trace_dump(void)
{
    if (wifi_path_trace_active()) return ESP_ERR_INVALID_STATE;
    printf("+PTRACE:BEGIN,start_ms=%lu,bins=%lu,bytes=%u,transport=%u\r\n", (unsigned long)s_start_ms,
           (unsigned long)s_used, (unsigned)sizeof(s_bins),
#ifdef CONFIG_APP_PACKET_PATH_TRANSPORT_STATS
           1U);
#else
           0U);
#endif
    puts("+PTRACE:columns=second,net_wan,net_lan,net_wan_bytes,socket_wan,socket_lan,socket_wan_bytes,audio,video,tp_audio,tp_video,net_gap_ms,socket_gap_ms,audio_gap_ms,video_gap_ms,rtt_ms,nack,retx,send_loss,receive_loss,probe,probe_gaps,probe_duplicate,probe_last");
    for (uint32_t i = 0; i < s_used; ++i) {
        const path_bin_t *b = &s_bins[i];
        printf("+PTRACE:%lu", (unsigned long)i);
#define FIELD(name) printf(",%lu", (unsigned long)b->name)
        FIELD(net_wan); FIELD(net_lan); FIELD(net_wan_bytes); FIELD(socket_wan);
        FIELD(socket_lan); FIELD(socket_wan_bytes); FIELD(audio); FIELD(video);
        FIELD(tp_audio); FIELD(tp_video); FIELD(net_gap_ms); FIELD(socket_gap_ms);
        FIELD(audio_gap_ms); FIELD(video_gap_ms); FIELD(rtt_ms); FIELD(nack);
        FIELD(retx); FIELD(send_loss); FIELD(receive_loss); FIELD(probe);
        FIELD(probe_gaps); FIELD(probe_duplicate); FIELD(probe_last);
#undef FIELD
        puts("");
    }
    puts("+PTRACE:END");
    return ESP_OK;
}
#else
esp_err_t wifi_path_trace_start(void) { return ESP_ERR_NOT_SUPPORTED; }
void wifi_path_trace_stop(void) { }
bool wifi_path_trace_active(void) { return false; }
bool wifi_path_trace_transport_active(void) { return false; }
esp_err_t wifi_path_trace_dump(void) { return ESP_ERR_NOT_SUPPORTED; }
void wifi_path_trace_media(bool video) { (void)video; }
void wifi_path_trace_transport(uint32_t media, uint32_t rtt, uint32_t nack,
                               uint32_t retx, uint32_t send_loss, uint32_t receive_loss)
{
    (void)media; (void)rtt; (void)nack; (void)retx; (void)send_loss; (void)receive_loss;
}
#endif
