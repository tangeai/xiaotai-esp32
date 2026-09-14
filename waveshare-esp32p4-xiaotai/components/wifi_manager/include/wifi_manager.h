#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

/**
 * @file wifi_manager.h
 * @brief Wi-Fi STA 连接和首次 SoftAP 配网。
 *
 * 有 NVS 配置时持续重连 STA；配置缺失、主动断开或多次失败时开启 SoftAP 配网页。
 * 模块关闭 Wi-Fi 省电以降低实时音视频抖动。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_MANAGER_SSID_MAX 32
#define WIFI_MANAGER_PASSWORD_MAX 64

typedef struct {
    char ssid[WIFI_MANAGER_SSID_MAX + 1];         /**< UTF-8 SSID。 */
    char password[WIFI_MANAGER_PASSWORD_MAX + 1]; /**< 空串表示开放网络。 */
} wifi_manager_credentials_t;

/** 初始化网络栈并启动 STA/SoftAP 流程；重复调用安全。 */
esp_err_t wifi_manager_start(void);

/** 以下查询函数返回当前瞬时状态。 */
bool wifi_manager_connected(void);
bool wifi_manager_provisioning(void);
/** 已保存网络连续连接失败时返回 true，但仍会重试；从未配置不算失败。 */
bool wifi_manager_connection_failed(void);

/** Nonblocking intent handled by the Wi-Fi event owner: disconnect and open
 * provisioning. Retains credentials; pauses retries until the next boot. */
esp_err_t wifi_manager_disconnect(void);
bool wifi_manager_manually_disconnected(void);
bool wifi_manager_control_pending(void);
esp_err_t wifi_manager_control_error(void);

/** 信号样本或连接状态变化时递增；只读内存，不执行硬件查询。 */
uint32_t wifi_manager_signal_revision(void);
/** 读取后台最近采样的 RSSI（dBm）；无有效样本时返回 false。 */
bool wifi_manager_signal_dbm(int8_t *rssi);

/** 配网未启用时返回空字符串；返回值由模块持有。 */
const char *wifi_manager_provisioning_ssid(void);
/** 配网热点始终开放，因此始终返回空字符串。 */
const char *wifi_manager_provisioning_password(void);

/** 固定本地配网入口；账号密码只提交到设备自身，不上传第三方。 */
const char *wifi_manager_provisioning_url(void);
/** 面向本地配网页的可理解状态，不包含 Wi-Fi 密码。 */
const char *wifi_manager_provisioning_status(void);

/** 从 wifi_cfg 加载完整记录；仅在无新记录时读取旧 SSID/password；失败清空输出。 */
esp_err_t wifi_manager_load_credentials(wifi_manager_credentials_t *credentials);

/** 用单条版本化记录更新整组配置；调用后不会自动重启。
 *  返回错误不保证 NVS 回滚，但不会分两次写入 SSID/password。
 *  写入由公共 NVS 任务执行，调用方可使用 PSRAM 栈，最多等待 5 秒。
 *  超时不撤销已接受的写入；不得把返回成功理解为仅入队。 */
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password);

/** 由公共 NVS 任务删除配置，最多等待 5 秒；调用后不会自动重启。 */
esp_err_t wifi_manager_forget_credentials(void);

/** 按 ESP32 字节长度规则校验 SSID 和密码；error 可为 NULL。 */
bool wifi_manager_credentials_valid(const char *ssid,
                                    const char *password,
                                    char *error,
                                    size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
