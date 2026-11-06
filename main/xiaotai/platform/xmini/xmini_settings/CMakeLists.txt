idf_component_register(SRCS "xmini_settings.c" "xmini_settings_state.c"
                       INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_timer)
