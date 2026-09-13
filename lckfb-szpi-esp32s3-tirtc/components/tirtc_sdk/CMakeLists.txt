# 这个 CMake 模块只负责校验和链接预编译 TiRTC SDK，不承载业务代码。
if(NOT DEFINED TIRTC_SDK_DIR OR TIRTC_SDK_DIR STREQUAL "")
    if(DEFINED ENV{TIRTC_SDK_DIR} AND NOT "$ENV{TIRTC_SDK_DIR}" STREQUAL "")
        set(TIRTC_SDK_DIR "$ENV{TIRTC_SDK_DIR}")
    else()
        get_filename_component(TIRTC_SDK_DIR
            "${CMAKE_CURRENT_LIST_DIR}/../../third_party/tirtc" ABSOLUTE)
    endif()
endif()

set(TIRTC_HEADER "${TIRTC_SDK_DIR}/include/tirtc/tiRTC.h")
set(TIRTC_LIBRARY "${TIRTC_SDK_DIR}/lib/libTiRTC.a")
set(TIRTC_CONTRACT "${TIRTC_SDK_DIR}/manifest/build-contract.env")

foreach(required_file IN ITEMS "${TIRTC_HEADER}" "${TIRTC_LIBRARY}" "${TIRTC_CONTRACT}")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Incomplete TiRTC ESP32 SDK, missing: ${required_file}")
    endif()
endforeach()

idf_component_register(
    SRCS "src/tirtc_task_stack_policy.c"
    INCLUDE_DIRS "${TIRTC_SDK_DIR}/include"
    REQUIRES freertos esp_http_client esp_netif esp_timer hal json lwip mbedtls soc
    # Keep the linker wrapper even though no application object references it
    # directly; TiRTC's prebuilt archive is the sole caller after --wrap.
    WHOLE_ARCHIVE
)

if(CMAKE_BUILD_EARLY_EXPANSION)
    return()
endif()

function(tirtc_read_contract contract_key output_variable)
    file(STRINGS "${TIRTC_CONTRACT}" contract_line REGEX "^${contract_key}=")
    if(contract_line STREQUAL "")
        message(FATAL_ERROR "TiRTC build contract is missing ${contract_key}")
    endif()
    string(REGEX REPLACE "^[^=]+=" "" contract_value "${contract_line}")
    set(${output_variable} "${contract_value}" PARENT_SCOPE)
endfunction()

tirtc_read_contract("CONFIG_FREERTOS_HZ" TIRTC_CONTRACT_FREERTOS_HZ)
tirtc_read_contract("CONFIG_FREERTOS_USE_TRACE_FACILITY" TIRTC_CONTRACT_TRACE)
tirtc_read_contract("CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS" TIRTC_CONTRACT_STATS_FORMAT)
tirtc_read_contract("CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS" TIRTC_CONTRACT_RUNTIME_STATS)
tirtc_read_contract("STATIC_SEMAPHORE_T_SIZE" TIRTC_CONTRACT_STATIC_SEMAPHORE_SIZE)

# 预编译库与编译它时的 FreeRTOS ABI 必须一致；不一致时在链接前明确失败。
if(NOT "${CONFIG_FREERTOS_HZ}" STREQUAL "${TIRTC_CONTRACT_FREERTOS_HZ}")
    message(FATAL_ERROR
        "TiRTC requires CONFIG_FREERTOS_HZ=${TIRTC_CONTRACT_FREERTOS_HZ}, "
        "project has ${CONFIG_FREERTOS_HZ}")
endif()
if(TIRTC_CONTRACT_TRACE STREQUAL "off" AND CONFIG_FREERTOS_USE_TRACE_FACILITY)
    message(FATAL_ERROR "TiRTC SDK requires CONFIG_FREERTOS_USE_TRACE_FACILITY=n")
endif()
if(TIRTC_CONTRACT_STATS_FORMAT STREQUAL "off" AND CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS)
    message(FATAL_ERROR "TiRTC SDK requires CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS=n")
endif()
if(TIRTC_CONTRACT_RUNTIME_STATS STREQUAL "off" AND CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)
    message(FATAL_ERROR "TiRTC SDK requires CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=n")
endif()

target_compile_definitions(${COMPONENT_LIB} INTERFACE
    TIRTC_SDK_STATIC_SEMAPHORE_SIZE=${TIRTC_CONTRACT_STATIC_SEMAPHORE_SIZE}
)

add_library(tirtc_esp32s3_prebuilt STATIC IMPORTED GLOBAL)
set_target_properties(tirtc_esp32s3_prebuilt PROPERTIES
    IMPORTED_LOCATION "${TIRTC_LIBRARY}"
)

# start-group/end-group 解决预编译 SDK 与 ESP-IDF 静态库之间的循环符号依赖。
add_library(tirtc_esp32s3_link_bundle INTERFACE)
target_link_libraries(tirtc_esp32s3_link_bundle INTERFACE
    "-Wl,--start-group"
    tirtc_esp32s3_prebuilt
    $<TARGET_FILE:__idf_lwip>
    $<TARGET_FILE:__idf_mbedtls>
    $<TARGET_FILE:mbedcrypto>
    $<TARGET_FILE:mbedtls>
    $<TARGET_FILE:mbedx509>
    $<TARGET_FILE:everest>
    $<TARGET_FILE:p256m>
    $<TARGET_FILE:__idf_hal>
    $<TARGET_FILE:__idf_soc>
    "-Wl,--end-group"
)
target_link_libraries(${COMPONENT_LIB} INTERFACE tirtc_esp32s3_link_bundle)

# Keep the SDK task-stack compatibility boundary local to this component.
# This is deliberately a linker wrapper rather than a binary archive patch:
# it survives SDK library layout changes and covers every TiRTC call site.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=freertos_ThreadCreateWithStackSize"
)
