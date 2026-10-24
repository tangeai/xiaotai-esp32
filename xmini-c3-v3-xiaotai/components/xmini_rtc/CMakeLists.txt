idf_component_register(SRCS "xmini_rtc.c" INCLUDE_DIRS "include"
                       REQUIRES tirtc_sdk freertos esp_timer)
file(READ "${CMAKE_CURRENT_LIST_DIR}/../tirtc_sdk/release-lock.json" sdk_lock)
string(JSON expected_diagnostic_header GET "${sdk_lock}" diagnostic_header_sha256)
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/include/tgtrp_internal_diag.h" actual_diagnostic_header)
if(NOT actual_diagnostic_header STREQUAL expected_diagnostic_header)
    message(FATAL_ERROR "C3 transport diagnostic header/archive mismatch")
endif()
