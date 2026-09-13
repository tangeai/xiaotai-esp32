idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_http_client esp_netif esp_timer hal json lwip mbedtls soc
)

set(TIRTC_LIB "${CMAKE_CURRENT_LIST_DIR}/lib/esp32p4/libTiRTC.a")

if(NOT EXISTS "${TIRTC_LIB}")
    message(FATAL_ERROR "TiRTC static library not found: ${TIRTC_LIB}")
endif()

# This archive is a patched P4 build, not interchangeable with the S3 SDK.
if(NOT IDF_TARGET STREQUAL "esp32p4")
    message(FATAL_ERROR "XiaoTai P4 requires the ESP32-P4 target")
endif()
if(NOT IDF_VERSION_MAJOR EQUAL 5 OR NOT IDF_VERSION_MINOR EQUAL 5 OR
   NOT IDF_VERSION_PATCH EQUAL 4)
    message(FATAL_ERROR "The pinned TiRTC P4 baseline requires ESP-IDF 5.5.4")
endif()
if(NOT CONFIG_FREERTOS_HZ EQUAL 1000 OR
   CONFIG_FREERTOS_USE_TRACE_FACILITY OR CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS OR
   CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS)
    message(FATAL_ERROR "TiRTC P4 requires 1000 Hz ticks and disabled trace/runtime stats")
endif()
file(SHA256 "${TIRTC_LIB}" TIRTC_LIB_SHA256)
if(NOT TIRTC_LIB_SHA256 STREQUAL
   "a7a01ffd496a55364c7e4d665ff3884d078147bba96752a965d97befca12e451")
    message(FATAL_ERROR "TiRTC archive differs from the pinned P4 baseline; review its ABI before updating the contract")
endif()

add_library(tirtc_single_prebuilt STATIC IMPORTED)
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/manifest/build-contract.env"
    TIRTC_SEMAPHORE_CONTRACT REGEX "^STATIC_SEMAPHORE_T_SIZE=[0-9]+$")
if(NOT TIRTC_SEMAPHORE_CONTRACT STREQUAL "STATIC_SEMAPHORE_T_SIZE=84")
    message(FATAL_ERROR "Pinned P4 archive requires an 84-byte StaticSemaphore_t (see VERSION.md)")
endif()
target_compile_definitions(${COMPONENT_LIB} INTERFACE TIRTC_SDK_STATIC_SEMAPHORE_SIZE=84)
set_target_properties(tirtc_single_prebuilt PROPERTIES IMPORTED_LOCATION "${TIRTC_LIB}")

# libwebrtc_nosctp.a is bundled into libTiRTC.a; the app links this SDK archive only.
add_library(tirtc_link_bundle INTERFACE)
target_link_libraries(tirtc_link_bundle INTERFACE
    "-Wl,--start-group"
    tirtc_single_prebuilt
    $<TARGET_FILE:__idf_lwip>
    $<TARGET_FILE:__idf_mbedtls>
    $<TARGET_FILE:mbedcrypto>
    $<TARGET_FILE:mbedtls>
    $<TARGET_FILE:mbedx509>
    $<TARGET_FILE:everest>
    $<TARGET_FILE:p256m>
    $<TARGET_FILE:__idf_hal>
    $<TARGET_FILE:__idf_soc>
    "-Wl,--end-group")

target_link_libraries(${COMPONENT_LIB} INTERFACE tirtc_link_bundle)
