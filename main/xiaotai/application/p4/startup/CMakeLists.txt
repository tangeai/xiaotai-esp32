set(_c6_app "${CMAKE_CURRENT_LIST_DIR}/assets/c6_app.bin")
if(NOT EXISTS "${_c6_app}")
    message(FATAL_ERROR
        "Missing bundled C6 APP image: main/assets/c6_app.bin. "
        "Provide the approved release attachment with the documented SHA-256 before building P4.")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_c6_app}")
file(SIZE "${_c6_app}" _c6_app_size)
file(SHA256 "${_c6_app}" _c6_app_sha256)
if(NOT _c6_app_size EQUAL 1111904 OR
   NOT _c6_app_sha256 STREQUAL "8700a7215c77ffc9810ab27dbf7687d6222c620c13d7814dc502ace6a784b4f5")
    message(FATAL_ERROR "C6 APP attachment size or SHA-256 does not match the approved image")
endif()

idf_component_register(SRCS "xiaotai_main.c" "c6_updater.c" INCLUDE_DIRS "."
 PRIV_REQUIRES esp_app_format esp_system esp_wifi nvs_flash nvs_store platform_client runtime_config
 starter_at starter_button starter_console starter_media starter_product starter_runtime starter_tirtc
 starter_voice wifi_manager p4_hardware bootloader_support mbedtls espressif__esp_hosted
 EMBED_FILES "assets/c6_app.bin")
