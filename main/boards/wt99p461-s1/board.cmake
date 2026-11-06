if(NOT IDF_TARGET STREQUAL "esp32p4")
    message(FATAL_ERROR "WT99P461-S1 requires ESP32-P4")
endif()
