idf_component_register(SRCS "app_main.c" "xmini_command.c" "xmini_product.c" "xmini_runtime.c" INCLUDE_DIRS "."
                       REQUIRES xmini_board xmini_ui xmini_audio esp_timer heap esp_app_format
                                esp_hw_support esp_driver_usb_serial_jtag
                                demo_platform wifi_provisioning nvs_flash esp_wifi esp_netif xmini_business xmini_rtc xmini_settings xmini_memtrace)
