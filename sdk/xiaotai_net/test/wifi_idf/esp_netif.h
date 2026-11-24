// SPDX-License-Identifier: MIT
#pragma once
#include "esp_event.h"
#include <stdbool.h>

#define CONFIG_LWIP_IPV6 1
#define CONFIG_LWIP_IPV6_NUM_ADDRESSES 3
#define ESP_IPADDR_TYPE_V4 0
#define ESP_IP_IS_ANY(ip_) (!(ip_).u_addr.ip4.addr)
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED 0x5004
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED 0x5005
#define ESP_IP6_ADDR_IS_GLOBAL 1
#define ESP_NETIF_OP_SET 1
#define ESP_NETIF_DOMAIN_NAME_SERVER 6
enum {IP_EVENT_STA_GOT_IP, IP_EVENT_STA_LOST_IP, IP_EVENT_GOT_IP6};
typedef struct esp_netif esp_netif_t;
typedef struct {bool ap;} esp_netif_config_t;
#define ESP_NETIF_DEFAULT_WIFI_STA() ((esp_netif_config_t){false})
#define ESP_NETIF_DEFAULT_WIFI_AP() ((esp_netif_config_t){true})
typedef enum {ESP_NETIF_DNS_MAIN, ESP_NETIF_DNS_BACKUP, ESP_NETIF_DNS_FALLBACK} esp_netif_dns_type_t;
typedef struct {uint32_t addr;} esp_ip4_addr_t;
typedef struct {uint32_t addr[4]; uint8_t zone;} esp_ip6_addr_t;
typedef struct {esp_ip4_addr_t ip, netmask, gw;} esp_netif_ip_info_t;
typedef struct {struct {uint8_t type; union {esp_ip4_addr_t ip4;} u_addr;} ip;} esp_netif_dns_info_t;
typedef struct {esp_netif_t *esp_netif; esp_netif_ip_info_t ip_info;} ip_event_got_ip_t;
typedef struct {esp_netif_t *esp_netif; struct {esp_ip6_addr_t ip;} ip6_info;} ip_event_got_ip6_t;

esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_new(const esp_netif_config_t *);
esp_err_t esp_netif_attach_wifi_station(esp_netif_t *);
esp_err_t esp_netif_attach_wifi_ap(esp_netif_t *);
esp_err_t esp_wifi_set_default_wifi_sta_handlers(void);
esp_err_t esp_wifi_set_default_wifi_ap_handlers(void);
void esp_netif_destroy_default_wifi(void *);
esp_err_t esp_netif_get_dns_info(esp_netif_t *, esp_netif_dns_type_t, esp_netif_dns_info_t *);
esp_err_t esp_netif_set_dns_info(esp_netif_t *, esp_netif_dns_type_t, esp_netif_dns_info_t *);
esp_err_t esp_netif_str_to_ip4(const char *, esp_ip4_addr_t *);
void esp_netif_set_ip4_addr(esp_ip4_addr_t *, uint8_t, uint8_t, uint8_t, uint8_t);
esp_err_t esp_netif_set_ip_info(esp_netif_t *, const esp_netif_ip_info_t *);
bool esp_netif_is_netif_up(esp_netif_t *);
esp_err_t esp_netif_get_ip_info(esp_netif_t *, esp_netif_ip_info_t *);
esp_err_t esp_netif_dhcps_stop(esp_netif_t *);
esp_err_t esp_netif_dhcps_start(esp_netif_t *);
esp_err_t esp_netif_dhcps_option(esp_netif_t *, int, int, void *, uint32_t);
esp_err_t esp_netif_create_ip6_linklocal(esp_netif_t *);
int esp_netif_get_all_ip6(esp_netif_t *, esp_ip6_addr_t []);
int esp_netif_ip6_get_addr_type(esp_ip6_addr_t *);
