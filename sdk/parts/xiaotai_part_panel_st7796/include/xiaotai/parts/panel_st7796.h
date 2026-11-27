// SPDX-License-Identifier: MIT
#pragma once
/* Parts library: the ST7796 SPI panel controller as a controller object for
 * xiaotai/parts/panel_spi.h. */
#include "xiaotai/parts/panel_spi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* [any] Static object. */
const xiaotai_panel_spi_controller_t *xiaotai_panel_st7796(void);

#ifdef __cplusplus
}
#endif
