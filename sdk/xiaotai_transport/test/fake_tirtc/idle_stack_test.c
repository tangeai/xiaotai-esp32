// SPDX-License-Identifier: MIT
#include <stdlib.h>
#include <stdio.h>

#include "freertos/task.h"
#include "tirtc_internal.h"
#include "xiaotai/port/mem.h"

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "line %d: %s\n", __LINE__, #test); abort(); } } while (0)

unsigned kernel_calls;
uint32_t applied;
int next_rc = 1;
TaskFunction_t task_code;
const char *task_name;
void *task_arg;
UBaseType_t task_prio;
BaseType_t task_core;
TaskHandle_t *task_created;

static bool external;
static uintptr_t current;
static uint32_t expected_listener, expected_queue;
static unsigned start_calls;
static const TIRTCCALLBACKS callbacks = {0};
static const char device[] = "fixture";

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)current; }
bool xiaotai_mem_has_external(void) { return external; }
static void body(void *arg) { (void)arg; }
static unsigned context;

/* Exhibit a real wrapped reference with a normal return inside the linker
 * bounds, separate from emulated app/FreeRTOS call sites. */
__attribute__((section("sdk_idle_caller"), noinline, noclone))
static BaseType_t sdk_create(const char *name, uint32_t bytes, TaskHandle_t *created)
{
    volatile BaseType_t rc = xTaskCreatePinnedToCore(body, name, bytes, &context, 17, created, -1);
    return rc;
}

static void run_kernel(const char *name, uint32_t bytes, uint32_t wanted, bool in_sdk, int result)
{
    TaskHandle_t handle = (void *)(uintptr_t)0x86;
    unsigned before = kernel_calls;
    next_rc = result;
    BaseType_t rc = in_sdk ? sdk_create(name, bytes, &handle) :
        xTaskCreatePinnedToCore(body, name, bytes, &context, 17, &handle, 0);
    CHECK(kernel_calls == before + 1 && rc == result && applied == wanted);
    CHECK(task_code == body && task_name == name && task_arg == &context &&
          task_prio == 17 && task_core == (in_sdk ? -1 : 0) && task_created == &handle);
    CHECK((uintptr_t)handle == (result == 1 ? 0x42U : 0x86U));
}

int TiRtcStart(const char *id, const TIRTCCALLBACKS *cb)
{
    CHECK(id == device && cb == &callbacks);
    ++start_calls;
    uintptr_t saved = current;
    current = 2;
    run_kernel("rtc_thread", 8192, 8192, true, 1);
    current = saved;
    run_kernel("rtc_thread", 8192, 8192, false, 1);
    run_kernel(NULL, 8192, 8192, true, 1);
    run_kernel("other", 8192, 8192, true, 1);
    run_kernel("rtc_thread", 6144, 6144, true, 1);
    run_kernel("rtc_thread", 8192, expected_listener, true, -7);
    run_kernel("rtc_thread", 8192, expected_listener, true, 1);
    run_kernel("rtc_thread", 8192, 8192, true, 1);
    run_kernel("tg_taskq", 8192, expected_queue, true, 1);
    run_kernel("tg_taskq", 8192, 8192, true, 1);
    return start_calls & 1U ? -12 : 0;
}

static void trial(uint32_t policy, uint32_t wanted, int result)
{
    expected_listener = expected_queue = wanted;
#if defined(CONFIG_XIAOTAI_TIRTC_IDLE_STACK_COMPACT) && CONFIG_XIAOTAI_TIRTC_IDLE_STACK_COMPACT
    if (wanted == 4096) {
        expected_listener = 8192;
        expected_queue = 2048;
    }
#endif
    unsigned before = start_calls;
    CHECK(xt_tirtc_start_with_idle_stack(device, &callbacks, policy) == result);
    CHECK(start_calls == before + 1);
    run_kernel("rtc_thread", 8192, 8192, true, 1);
    run_kernel("tg_taskq", 8192, 8192, true, 1);
    CHECK(sdk_create("rtc_thread", 8192, NULL) == 1 && applied == 8192 && !task_created);
}

int main(void)
{
    current = 1;
    trial(4096, 4096, -12);
    trial(4096, 4096, 0);
    trial(6144, 6144, -12);
    trial(0, 8192, 0);
    trial(8192, 8192, -12);
    trial(4100, 8192, 0);
    external = true;
    trial(4096, 8192, -12);
    external = false;
    current = 0;
    trial(4096, 8192, 0);
    current = 3;
    trial(4096, 4096, -12);
    trial(2560, 8192, 0);
    trial(2048, 8192, -12);
    return 0;
}
