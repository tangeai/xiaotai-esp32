// SPDX-License-Identifier: MIT
/* A C product can describe itself with the public API, and the error codes keep
 * the ESP-IDF values. (That every header compiles alone is the all-headers
 * check in tests/sdk.) */
#include "xiaotai/xiaotai.h"
#include "xiaotai/framework.h"
#include "xiaotai/view.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/err.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port/sys.h"

_Static_assert(XIAOTAI_OK == 0 && XIAOTAI_ERR_NO_MEM == 0x101, "error codes match ESP-IDF");
int main(void)
{
    const xiaotai_config_t config = {.home = "companion"};
    const xiaotai_view_t view = {.kind = XIAOTAI_VIEW_HOME, .title = config.home};
    return view.kind == XIAOTAI_VIEW_HOME ? 0 : 1;
}
