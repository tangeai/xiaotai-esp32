# Check effective Kconfig: shared module wires do not make SPI/SDIO equivalent.
if(CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN OR
   CONFIG_ESP_HOSTED_HOST_TRANSPORT_RESTART_ON_FAILURE)
    message(FATAL_ERROR "WT99P461-S1 bootstrap owns Hosted startup and explicit user retry")
endif()
if(NOT CONFIG_SLAVE_IDF_TARGET_ESP32C61 OR
   NOT CONFIG_ESP_HOSTED_CP_TARGET_ESP32C61 OR
   NOT CONFIG_ESP_HOSTED_HOST_TRANSPORT_BUS_SPI OR
   CONFIG_ESP_HOSTED_HOST_TRANSPORT_BUS_SDIO OR
   NOT CONFIG_ESP_HOSTED_HOST_RESET_ACTIVE_LOW)
    message(FATAL_ERROR "WT99P461-S1 requires ESP32-C61 SPI full-duplex with active-low EN")
endif()
foreach(pair IN ITEMS "MOSI:14" "MISO:15" "CLK:18" "CS:19")
    string(REPLACE ":" ";" fields "${pair}")
    list(GET fields 0 signal)
    list(GET fields 1 pin)
    if(NOT CONFIG_ESP_HOSTED_HOST_SPI_${signal}_GPIO EQUAL pin)
        message(FATAL_ERROR "WT99P461-S1 SPI ${signal} must use GPIO${pin}")
    endif()
endforeach()
if(NOT CONFIG_ESP_HOSTED_HOST_HANDSHAKE_GPIO EQUAL 16 OR
   NOT CONFIG_ESP_HOSTED_HOST_DATA_READY_GPIO EQUAL 17 OR
   NOT CONFIG_ESP_HOSTED_HOST_RESET_GPIO EQUAL 13 OR
   NOT CONFIG_ESP_HOSTED_HOST_SPI_MODE EQUAL 3 OR
   NOT CONFIG_ESP_HOSTED_HOST_SPI_CLK_MHZ EQUAL 10)
    message(FATAL_ERROR "WT99P461-S1 requires HS16/DR17/RST13, SPI mode3 at 10MHz")
endif()
if(NOT CONFIG_CAMERA_SC2336 OR CONFIG_CAMERA_OV5647 OR
   NOT CONFIG_CAMERA_SC2336_MIPI_DEFAULT_FMT_RAW8_1024X600_30FPS OR
   CONFIG_LV_COLOR_16_SWAP)
    message(FATAL_ERROR "WT99P461-S1 requires SC2336 RAW8 1024x600 and native RGB565 byte order")
endif()
if(NOT CONFIG_CAMERA_SC2336_CUSTOMIZED_IPA_JSON_CONFIGURATION_FILE OR
   NOT CONFIG_CAMERA_SC2336_CUSTOMIZED_IPA_JSON_CONFIGURATION_FILE_PATH STREQUAL "main/boards/wt99p461-s1/drivers/wt99p461_s1/sc2336_isp.json")
    message(FATAL_ERROR "WT99P461-S1 requires its hardware-valid SC2336 gamma grid")
endif()
execute_process(COMMAND "${PYTHON}" "${PROJECT_SOURCE_DIR}/scripts/generate_sc2336_isp.py" --check
    RESULT_VARIABLE _tuning_result OUTPUT_VARIABLE _tuning_output ERROR_VARIABLE _tuning_error)
if(NOT _tuning_result EQUAL 0)
    message(FATAL_ERROR "SC2336 tuning check failed: ${_tuning_output}${_tuning_error}")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/cmake/hosted_spi_diagnostics.cmake")
xiaotai_enable_hosted_spi_diagnostics()
