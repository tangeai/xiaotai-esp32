#ifndef STARTER_CONSOLE_H
#define STARTER_CONSOLE_H

/**
 * @file starter_console.h
 * @brief 开发期串口命令入口。
 *
 * 控制台只负责把人工操作转换成各业务模块的公开调用，不保存会话状态，
 * 也不直接调用 TiRTC SDK。量产产品可以删除这个模块，改由按键或 UI 调用
 * starter_runtime.h 中的接口。
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 注册命令并启动 ESP-IDF REPL。重复调用安全。 */
esp_err_t starter_console_start(void);

#ifdef __cplusplus
}
#endif

#endif
