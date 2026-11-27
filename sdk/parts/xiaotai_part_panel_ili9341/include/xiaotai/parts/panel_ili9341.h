// SPDX-License-Identifier: MIT
#pragma once
/* Parts library: the ILI9341 family of SPI panel controllers (ILI9341,
 * ILI9342C) as a controller object for xiaotai/parts/panel_spi.h. Variants of
 * the family differ only in the initialization sequence, which the board gives
 * in xiaotai_panel_spi_config_t.init. */
#include "xiaotai/parts/panel_spi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* [any] Static object. */
const xiaotai_panel_spi_controller_t *xiaotai_panel_ili9341(void);

#ifdef __cplusplus
}
#endif
