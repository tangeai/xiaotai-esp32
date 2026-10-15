#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tiRTC.h"
#if !CONFIG_IDF_TARGET_ESP32C3 || CONFIG_FREERTOS_HZ != 1000
#error "TiRTC C3 archive requires ESP32-C3 and 1000 Hz ticks"
#endif
#ifdef CONFIG_FREERTOS_USE_TRACE_FACILITY
#error "TiRTC C3 archive requires trace disabled"
#endif
_Static_assert(sizeof(StaticSemaphore_t) == 84, "TiRTC FreeRTOS ABI mismatch");
_Static_assert(TIRTC_VERSION_MAJOR == 2 && TIRTC_VERSION_MINOR == 5 &&
               TIRTC_VERSION_PATCH == 0, "Unexpected TiRTC headers");
