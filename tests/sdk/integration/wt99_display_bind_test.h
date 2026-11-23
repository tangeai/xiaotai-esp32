// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { CREATE_FAIL, POINTER_FAIL, SCANOUT_TIMEOUT, DETACH_FAIL, HEALTHY } wt99_bind_mode_t;
void wt99_ui_bind_probe(wt99_bind_mode_t mode, bool already_open);
#ifdef __cplusplus
}
#endif
