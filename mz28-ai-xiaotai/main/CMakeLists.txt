idf_component_register(SRCS "app_main.c" "boot_button.c" "boot_button_logic.c"
                       INCLUDE_DIRS "."
                       REQUIRES demo_platform wechat_voip product_audio nvs_flash
                                memory_probe status_led wifi_provisioning
                                esp_driver_gpio)
