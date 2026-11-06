/* Temporary bounded LAN probe for AI ingress loss. No audio or identifiers. */
#include "wifi_manager.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static atomic_bool s_probe_active;

static void udp_probe_task(void *argument)
{
    (void)argument;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    esp_netif_ip_info_t ip = {0};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (fd < 0 || netif == NULL || esp_netif_get_ip_info(netif, &ip) != ESP_OK) goto done;
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(49161),
                                .sin_addr.s_addr = ip.ip.addr};
    struct timeval timeout = {.tv_usec = 200000};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) goto done;
    uint8_t seen[512] = {0};
    uint8_t packet[256];
    uint32_t unique = 0, duplicate = 0, invalid = 0, max_seq = 0;
    uint32_t phases[5] = {0};
    int64_t start = esp_timer_get_time();
    puts("+NETRX:READY,port=49161,limit_ms=12000");
    fflush(stdout);
    while (esp_timer_get_time() - start < 12000000) {
        int n = recv(fd, packet, sizeof(packet), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            ESP_LOGE("wifi_probe", "recv failed errno=%d", errno);
            break;
        }
        uint32_t seq = 0;
        if (n >= 8) memcpy(&seq, packet + 4, sizeof(seq));
        seq = ntohl(seq);
        if (n < 8 || memcmp(packet, "XTP1", 4) != 0 || seq >= 500) {
            invalid++;
            continue;
        }
        if (seen[seq / 8] & (1U << (seq % 8))) duplicate++;
        else {
            seen[seq / 8] |= 1U << (seq % 8);
            unique++;
            phases[seq / 100]++;
        }
        if (seq > max_seq) max_seq = seq;
    }
    printf("+NETRX:DONE,unique=%lu,duplicate=%lu,invalid=%lu,max_seq=%lu,phases=%lu/%lu/%lu/%lu/%lu\r\n",
           (unsigned long)unique, (unsigned long)duplicate, (unsigned long)invalid,
           (unsigned long)max_seq, (unsigned long)phases[0], (unsigned long)phases[1],
           (unsigned long)phases[2], (unsigned long)phases[3], (unsigned long)phases[4]);
    fflush(stdout);
done:
    if (fd >= 0) close(fd);
    atomic_store(&s_probe_active, false);
    vTaskDelete(NULL);
}

esp_err_t wifi_manager_start_udp_probe(void)
{
    if (!wifi_manager_connected()) return ESP_ERR_INVALID_STATE;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_probe_active, &expected, true)) return ESP_ERR_INVALID_STATE;
    if (xTaskCreate(udp_probe_task, "wifi_probe", 4096, NULL, 8, NULL) != pdPASS) {
        atomic_store(&s_probe_active, false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
