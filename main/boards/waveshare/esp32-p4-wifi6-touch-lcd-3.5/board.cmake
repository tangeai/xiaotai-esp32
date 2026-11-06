if(NOT IDF_TARGET STREQUAL "esp32p4")
    message(FATAL_ERROR "Waveshare P4 requires ESP32-P4")
endif()
