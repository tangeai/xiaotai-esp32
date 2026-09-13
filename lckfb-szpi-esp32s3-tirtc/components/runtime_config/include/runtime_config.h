#ifndef RUNTIME_CONFIG_H
#define RUNTIME_CONFIG_H

/**
 * @file runtime_config.h
 * @brief 已绑定设备凭证的 NVS 持久化接口。
 *
 * 本模块只负责校验和持久化，不负责绑定或登录。模板使用普通 NVS；量产固件
 * 应在组合根启用 NVS 加密或改用安全芯片，并继续保持这个窄接口。
 */

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char device_id[65];      /**< 服务端下发的设备 ID。 */
    char device_secret[257]; /**< 敏感设备密钥，禁止写日志。 */
    char client_id[65];      /**< 可选客户端 ID，空值由组合根生成。 */
} runtime_tirtc_config_t;

/** 从 tirtc_cfg 命名空间加载凭证；失败时清空输出结构。 */
esp_err_t runtime_config_load_tirtc(runtime_tirtc_config_t *config);

/** 校验并原子提交一组凭证到 NVS。 */
esp_err_t runtime_config_save_tirtc(const runtime_tirtc_config_t *config);

/** 清空整个 TiRTC 凭证命名空间。 */
esp_err_t runtime_config_clear_tirtc(void);

/** 只校验字段长度；error 可为 NULL。 */
bool runtime_config_tirtc_valid(const runtime_tirtc_config_t *config,
                                char *error,
                                size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
