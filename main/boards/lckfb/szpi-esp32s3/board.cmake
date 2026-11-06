if(NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR "SZPI V1.0.1 requires ESP32-S3")
endif()
set(XIAOTAI_BOARD_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/board.cc"
    "${CMAKE_CURRENT_LIST_DIR}/xiaotai_hardware.c")
set(XIAOTAI_MEDIA_MODULE xiaotai/media/s3)
set(XIAOTAI_UI_MODULE xiaotai/ui/lcd320)
