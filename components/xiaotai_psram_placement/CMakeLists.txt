# Linker placement rules that apply to every product: anything a library keeps in
# internal RAM but can live in PSRAM goes there when the board has PSRAM. Each rule
# is conditional on ESP_ALLOW_BSS_SEG_EXTERNAL_MEMORY and harmless without the library.
idf_component_register(LDFRAGMENTS "esp_sr_nsnet3_psram.lf")
