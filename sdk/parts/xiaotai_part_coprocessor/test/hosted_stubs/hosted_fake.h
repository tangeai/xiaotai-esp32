// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
enum { ESP_OK = 0, ESP_ERR_INVALID_RESPONSE = 0x108, ESP_ERR_WIFI_NOT_INIT = 0x3001, WIFI_IF_STA = 0,
       RPC_ID__Req_GetMACAddress = 257, EH_HOST_EVENT_PEER_DATA_RX = 6 };
typedef const char *esp_event_base_t;
extern esp_event_base_t EH_HOST_EVENT;
typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);
typedef struct { uint32_t msg_id; const uint8_t *data; size_t len; } eh_host_event_peer_data_t;
typedef struct {
    uint32_t major1, minor1, patch1;
    int32_t revision, prerelease, build;
} eh_host_coprocessor_fwver_t;
typedef struct {
    uint32_t magic_word;
    char project_name[32], version[32], idf_ver[32];
    uint8_t app_elf_sha256[32];
} esp_hosted_app_desc_t;
typedef struct { int32_t resp_event_status; union { struct { int32_t mode; } wifi_mac; } u; } eh_rpc_ctrl_cmd_t;
typedef struct eh_host_init_cfg eh_host_init_cfg_t;
struct eh_priv_sdio_buf_config;

int eh_host_init(const eh_host_init_cfg_t *cfg);
int eh_host_deinit(void);
int eh_host_connect_to_slave(void);
int eh_host_wait_auto_init_ready(uint32_t timeout_ms);
uint8_t eh_host_mcu_transport_get_chip_id(void);
uint32_t eh_host_mcu_transport_get_fw_version(void);
uint8_t eh_host_mcu_transport_get_capabilities(void);
uint32_t eh_host_mcu_transport_get_ext_capabilities(void);
bool eh_host_mcu_transport_peer_advertised_rpc_version(void);
const struct eh_priv_sdio_buf_config *eh_host_mcu_transport_sdio_buf_config(void);
esp_err_t eh_host_sys_get_cp_fw_version(eh_host_coprocessor_fwver_t *info);
esp_err_t eh_host_sys_get_cp_app_desc(esp_hosted_app_desc_t *info);
eh_rpc_ctrl_cmd_t *eh_rpc_ctrl_cmd_alloc(void);
void eh_rpc_ctrl_cmd_free(eh_rpc_ctrl_cmd_t *cmd);
int eh_host_feat_rpc_request_sync(int32_t id, void *req, void **out);
esp_err_t eh_host_cp_ota_begin(void);
esp_err_t eh_host_cp_ota_write(const uint8_t *data, uint32_t size);
esp_err_t eh_host_cp_ota_end(void);
esp_err_t eh_host_cp_ota_activate(void);
esp_err_t eh_host_peer_data_send(uint32_t channel, const uint8_t *data, size_t size);
esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id, esp_event_handler_t handler, void *ctx);
