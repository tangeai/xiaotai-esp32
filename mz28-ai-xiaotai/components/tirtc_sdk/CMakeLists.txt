idf_component_register(SRCS "sdk_contract.c" INCLUDE_DIRS "include"
                       REQUIRES esp_timer lwip mbedtls)
set(TIRTC_C3_LIBRARY "${CMAKE_CURRENT_LIST_DIR}/lib/libTiRTC.a"
    CACHE FILEPATH "ESP32-C3 TiRTC archive (override only for controlled SDK regression)")
if(NOT EXISTS "${TIRTC_C3_LIBRARY}")
    message(FATAL_ERROR "TiRTC archive not found: ${TIRTC_C3_LIBRARY}")
endif()
message(STATUS "C3 TiRTC archive: ${TIRTC_C3_LIBRARY}")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/release-lock.json")
    file(READ "${CMAKE_CURRENT_LIST_DIR}/release-lock.json" release_lock)
    string(JSON expected_sdk GET "${release_lock}" sdk_sha256)
    file(SHA256 "${TIRTC_C3_LIBRARY}" actual_sdk)
    if(NOT actual_sdk STREQUAL expected_sdk)
        message(FATAL_ERROR "Release SDK hash mismatch; do not mix SDK variants")
    endif()
    if(CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE OR CONFIG_MBEDTLS_SSL_PROTO_TLS1_3
       OR NOT CONFIG_MBEDTLS_DYNAMIC_BUFFER OR CONFIG_DEMO_MEMORY_DIAGNOSTICS)
        message(FATAL_ERROR "Use the supplied release sdkconfig with the matching C3 SDK")
    endif()
endif()
add_prebuilt_library(tirtc_c3 "${TIRTC_C3_LIBRARY}"
                     REQUIRES lwip mbedtls esp_timer)
target_link_libraries(${COMPONENT_LIB} INTERFACE
                      "-Wl,--whole-archive" tirtc_c3 "-Wl,--no-whole-archive"
                      "$<TARGET_FILE:idf::lwip>")
