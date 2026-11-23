// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/ui_theme.h"

#if XIAOTAI_UI_OWNER_FRAME_WORKSPACE
#ifdef __cplusplus
extern "C" {
#endif

/* The provider owns typed storage and retains it until the display stops.
 * This does not lend the data or buffers passed to xiaotai_view_show(). */
typedef uint8_t *(*xiaotai_ui_owner_frame_fn)(size_t bytes);
xiaotai_err_t xiaotai_ui_owner_frame_offer(xiaotai_ui_owner_frame_fn provide);
void xiaotai_ui_owner_frame_forget(xiaotai_ui_owner_frame_fn provide);

/* start(): opt into the offer only on a driven owner-timer FRAME display.
 * render(): borrow bytes until return. Calls outside render return NULL. */
bool xiaotai_ui_host_prepare_owner_frame(const xiaotai_ui_host_t *host, size_t bytes);
uint8_t *xiaotai_ui_host_owner_frame(const xiaotai_ui_host_t *host);

#ifdef __cplusplus
}
#endif

#endif
