# JD9165 driver snapshot

`esp_lcd_jd9165.c`, `include/esp_lcd_jd9165.h`, `LICENSE`, and
`UPSTREAM.md` are exact copies of the WT99 driver snapshot previously kept
in `main/boards/wt99p461-s1/drivers/esp_lcd_jd9165/`. The source note records
the supplier and the original integration. This component builds the
snapshot alongside `panel_io.c`; board timing and buffer count come from
the panel configuration, not a new supplier initialization table.
