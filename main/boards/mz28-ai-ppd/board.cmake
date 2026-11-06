if(NOT IDF_TARGET STREQUAL "esp32c3")
    message(FATAL_ERROR "MZ28 requires ESP32-C3")
endif()
