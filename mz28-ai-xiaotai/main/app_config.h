#pragma once
/* Application-level choices only. Do not put SDK ABI or stack sizes here. */
#define WECHAT_VERSION_RELEASE 0
#define WECHAT_VERSION_DEVELOPMENT 1
#define WECHAT_VERSION_TRIAL 2
#if __has_include("app_config.local.h")
#include "app_config.local.h"
#endif
#ifndef APP_WIFI_SSID
#define APP_WIFI_SSID ""
#endif
#ifndef APP_WIFI_PASSWORD
#define APP_WIFI_PASSWORD ""
#endif
#ifndef APP_DISCOVERY_URL
#define APP_DISCOVERY_URL "https://ep-open.tangeopen.com/services"
#endif
#ifndef APP_WECHAT_VERSION
#define APP_WECHAT_VERSION WECHAT_VERSION_RELEASE
#endif
#ifndef APP_BOOT_BUTTON_GPIO
#define APP_BOOT_BUTTON_GPIO 9
#endif
#ifndef APP_BOOT_LONG_PRESS_MS
#define APP_BOOT_LONG_PRESS_MS 7000U
#endif
#ifndef APP_BOOT_DOUBLE_CLICK_MS
#define APP_BOOT_DOUBLE_CLICK_MS 350U
#endif
#ifndef APP_STATUS_LED_GPIO
/* MZ28 schematic: RGB_DI is ESP32-C3 GPIO8. */
#define APP_STATUS_LED_GPIO 8
#endif
#ifndef APP_STATUS_LED_COUNT
#define APP_STATUS_LED_COUNT 7U
#endif
#ifndef APP_STATUS_LED_BRIGHTNESS
/* Keep seven WS2812B LEDs expressive without a full-white current spike. */
#define APP_STATUS_LED_BRIGHTNESS 72U
#endif
#ifndef APP_VB6824_UART_NUM
#define APP_VB6824_UART_NUM 1
#endif
#ifndef APP_VB6824_TX_GPIO
/* MZ28 net RX is VB6824 PB10/RX, so ESP32-C3 drives it from GPIO20. */
#define APP_VB6824_TX_GPIO 20
#endif
#ifndef APP_VB6824_RX_GPIO
/* MZ28 net TX is VB6824 PB9/TX, so ESP32-C3 receives it on GPIO10. */
#define APP_VB6824_RX_GPIO 10
#endif
#ifndef APP_VB6824_RESET_GPIO
/* The MZ28 schematic has no ESP32-controlled VB6824 reset line. */
#define APP_VB6824_RESET_GPIO (-1)
#endif
#ifndef APP_VB6824_BAUD_RATE
#define APP_VB6824_BAUD_RATE 2000000
#endif
#ifndef APP_SPEAKER_VOLUME
#define APP_SPEAKER_VOLUME 100
#endif
#if APP_WECHAT_VERSION < 0 || APP_WECHAT_VERSION > 2
#error "Invalid WeChat mini-program version"
#endif
