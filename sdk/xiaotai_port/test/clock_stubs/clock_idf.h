// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))

typedef uint8_t u8_t;
typedef int err_t;
typedef struct { uint32_t addr; } ip_addr_t;
typedef void (*clock_idf_work_t)(void *);
typedef void (*clock_idf_answer_t)(struct timeval *);
typedef void (*clock_idf_dns_t)(const char *, const ip_addr_t *, void *);

enum {
    ERR_OK = 0, ERR_INPROGRESS = -5,
    LWIP_CORE_IS_TCPIP_INITIALIZED = 1, LWIP_DNS_ADDRTYPE_IPV4 = 0,
    SNTP_SYNC_MODE_IMMED = 0, SNTP_SYNC_MODE_SMOOTH = 1,
};

#define IP_IS_V4(ip) ((ip) != NULL)
#define ip_2_ip4(ip) (ip)
#define ip4_addr_get_u32(ip) ((ip)->addr)
#define SNTP_RECV_TIMEOUT 15000
#define SNTP_STARTUP_DELAY 0
#define SNTP_MONITOR_SERVER_REACHABILITY 1
#define SNTP_MAX_SERVERS 2

uint32_t esp_random(void);
bool sys_thread_tcpip(int type);
err_t tcpip_try_callback(clock_idf_work_t work, void *arg);
void sys_timeout(uint32_t ms, clock_idf_work_t work, void *arg);
err_t dns_gethostbyname_addrtype(const char *name, ip_addr_t *addr,
                               clock_idf_dns_t callback, void *arg, u8_t type);

u8_t sntp_enabled(void);
u8_t sntp_getreachability(u8_t idx);
void sntp_setoperatingmode(u8_t mode);
void sntp_setserver(u8_t idx, const ip_addr_t *addr);
void sntp_setservername(u8_t idx, const char *name);
void sntp_init(void);
void sntp_stop(void);
void esp_sntp_set_sync_mode(int mode);
int esp_sntp_get_sync_mode(void);
void esp_sntp_set_time_sync_notification_cb(clock_idf_answer_t fn);
