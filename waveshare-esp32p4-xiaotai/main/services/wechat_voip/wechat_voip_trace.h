#pragma once

#include "esp_log.h"
#include "wechat_voip_config.h"

#if WECHAT_VOIP_DEBUG_LOG
#define WX_VOIP_TRACEI(tag, fmt, ...) ESP_LOGI(tag, "[debug] " fmt, ##__VA_ARGS__)
#define WX_VOIP_TRACEW(tag, fmt, ...) ESP_LOGW(tag, "[debug] " fmt, ##__VA_ARGS__)
#else
#define WX_VOIP_TRACEI(tag, fmt, ...)             \
    do                                            \
    {                                             \
        if (0)                                    \
        {                                         \
            ESP_LOGI(tag, "[debug] " fmt, ##__VA_ARGS__); \
        }                                         \
    } while (0)
#define WX_VOIP_TRACEW(tag, fmt, ...)             \
    do                                            \
    {                                             \
        if (0)                                    \
        {                                         \
            ESP_LOGW(tag, "[debug] " fmt, ##__VA_ARGS__); \
        }                                         \
    } while (0)
#endif
