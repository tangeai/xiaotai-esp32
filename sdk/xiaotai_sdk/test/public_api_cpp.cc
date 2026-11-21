// SPDX-License-Identifier: MIT
// The public API is usable from C++ products, and its version is pinned.
#include "xiaotai/xiaotai.h"
#include "xiaotai/framework.h"
#include "xiaotai/view.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port/sys.h"

static_assert(XIAOTAI_SDK_API_VERSION == 1, "API version");
