# ESP-IDF 5.5 SPI LCD PSRAM DMA backport

This component keeps the project on ESP-IDF 5.5.4 while backporting the
opt-in SPI LCD PSRAM DMA path from Espressif's `release/v5.5` branch.

Upstream changes:

- `4cb1dc08c248`: add PSRAM support in the SPI LCD panel IO
- `a4f8a4146795`: gate direct PSRAM DMA behind `psram_dma_direct`

Only the SPI panel IO source and its public configuration header are patched.
All other sources are compiled from the active IDF's `esp_lcd` component. This
local override can be removed after the selected ESP-IDF release contains both
upstream changes.
