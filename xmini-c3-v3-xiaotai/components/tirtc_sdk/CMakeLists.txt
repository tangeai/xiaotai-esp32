idf_component_register(SRCS "sdk_contract.c" INCLUDE_DIRS "include"
                       REQUIRES esp_timer lwip mbedtls)
set(TIRTC_C3_LIBRARY "${CMAKE_CURRENT_LIST_DIR}/lib/libTiRTC.a")
if(NOT EXISTS "${TIRTC_C3_LIBRARY}")
    message(FATAL_ERROR "TiRTC archive not found: ${TIRTC_C3_LIBRARY}")
endif()
message(STATUS "C3 TiRTC archive: ${TIRTC_C3_LIBRARY}")
# httpclt.o embeds mbedTLS objects by value with the archive's own layout
# (session 892 B: conf 140 B, entropy@140). See sdk_contract.c.
if(CONFIG_MBEDTLS_DHM_C OR CONFIG_MBEDTLS_THREADING_C)
    message(FATAL_ERROR "C3 TiRTC archive requires MBEDTLS_DHM_C and MBEDTLS_THREADING_C disabled (mbedTLS object ABI)")
endif()
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/release-lock.json")
    file(READ "${CMAKE_CURRENT_LIST_DIR}/release-lock.json" release_lock)
    string(JSON expected_sdk GET "${release_lock}" sdk_sha256)
    file(SHA256 "${TIRTC_C3_LIBRARY}" actual_sdk)
    if(NOT actual_sdk STREQUAL expected_sdk)
        message(FATAL_ERROR "Release SDK hash mismatch; do not mix SDK variants")
    endif()
    foreach(header tiRTC basedef)
        if(header STREQUAL "tiRTC")
            set(hash_key header_sha256)
        else()
            set(hash_key basedef_sha256)
        endif()
        string(JSON expected_header GET "${release_lock}" ${hash_key})
        file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/include/${header}.h" actual_header)
        if(NOT actual_header STREQUAL expected_header)
            message(FATAL_ERROR "C3 SDK header/archive input mismatch: ${header}")
        endif()
    endforeach()
    if(CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE OR CONFIG_MBEDTLS_SSL_PROTO_TLS1_3
       OR NOT CONFIG_MBEDTLS_DYNAMIC_BUFFER OR CONFIG_DEMO_MEMORY_DIAGNOSTICS)
        message(FATAL_ERROR "C3 TiRTC requires dynamic TLS buffers, TLS1.3/peer retention/trace disabled")
    endif()
endif()
add_prebuilt_library(tirtc_c3 "${TIRTC_C3_LIBRARY}"
                     REQUIRES lwip mbedtls esp_timer)
# Keep the open/archive/close flags indivisible: CMake must not propagate an
# unmatched whole-archive across ESP-IDF dependencies while reordering libraries.
target_link_libraries(${COMPONENT_LIB} INTERFACE
                      "-Wl,--whole-archive,$<TARGET_FILE:tirtc_c3>,--no-whole-archive"
                      "$<TARGET_FILE:idf::lwip>")
