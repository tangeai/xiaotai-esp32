if(NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR "CoreS3-Lite requires ESP32-S3")
endif()
