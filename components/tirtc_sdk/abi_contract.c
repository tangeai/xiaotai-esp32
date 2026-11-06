#include <stdalign.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

_Static_assert(sizeof(StaticSemaphore_t) == TIRTC_SDK_STATIC_SEMAPHORE_SIZE, "TiRTC semaphore ABI");
_Static_assert(sizeof(StaticTask_t) == TIRTC_SDK_STATIC_TASK_SIZE, "TiRTC task ABI");
_Static_assert(sizeof(TickType_t) == TIRTC_SDK_TICK_SIZE, "TiRTC tick ABI");
_Static_assert(sizeof(time_t) == TIRTC_SDK_TIME_SIZE, "TiRTC time ABI");
_Static_assert(alignof(StaticSemaphore_t) == TIRTC_SDK_SEMAPHORE_ALIGNMENT_SIZE, "TiRTC alignment ABI");
