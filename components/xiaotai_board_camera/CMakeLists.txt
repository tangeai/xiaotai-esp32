idf_component_register(SRCS "esp32_camera_capture.c" INCLUDE_DIRS "include"
    REQUIRES esp_common PRIV_REQUIRES esp32-camera log)
