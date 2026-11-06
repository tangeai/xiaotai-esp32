if(NOT DEFINED CROSS_NM OR NOT EXISTS "${CROSS_NM}")
    message(FATAL_ERROR "Firmware check requires CROSS_NM")
endif()
if(NOT DEFINED ELF OR NOT EXISTS "${ELF}")
    message(FATAL_ERROR "Firmware ELF is missing: ${ELF}")
endif()

execute_process(
    COMMAND "${CROSS_NM}" -C --defined-only "${ELF}"
    RESULT_VARIABLE nm_result
    OUTPUT_VARIABLE symbols
    ERROR_VARIABLE nm_error)
if(NOT nm_result EQUAL 0)
    message(FATAL_ERROR "Unable to inspect firmware ELF: ${nm_error}")
endif()

set(required_symbols
    app_main
    starter_runtime_start
    starter_product_start
    starter_media_init
    starter_voice_start
    starter_voice_feed_pcm16k
    wifi_manager_start
    platform_client_start
    p4_video_submit
    camera_pipeline_set_rtc_video_enabled
    call_video_renderer_submit_h264
    call_video_renderer_submit_mjpeg
    starter_tirtc_send_h264
    starter_runtime_call_contact_video
    starter_runtime_contacts_refresh
    starter_runtime_call_contact
    starter_runtime_ai_start
    starter_runtime_ai_stop
    starter_tirtc_is_ai_command
    wifi_manager_disconnect)
foreach(symbol IN LISTS required_symbols)
    string(REGEX MATCH "(^|\n)[^\n]*[ \t]${symbol}(\r?\n|$)" found "${symbols}")
    if(NOT found)
        message(FATAL_ERROR "Firmware is missing product implementation: ${symbol}")
    endif()
endforeach()

string(REGEX MATCHALL "(^|\n)[^\n]*[ \t]app_main(\r?\n|$)" app_entries "${symbols}")
list(LENGTH app_entries app_count)
if(NOT app_count EQUAL 1)
    message(FATAL_ERROR "Firmware entry point is not unique")
endif()

foreach(symbol IN ITEMS app_init app_run device_call_init rtc_transport_init
                        wechat_voip_service_init esp_camera_init)
    string(REGEX MATCH "(^|\n)[^\n]*[ \t]${symbol}(\r?\n|$)" found "${symbols}")
    if(found)
        message(FATAL_ERROR "Firmware contains foreign application owner: ${symbol}")
    endif()
endforeach()

if(DEFINED CONFIG AND EXISTS "${CONFIG}")
    file(READ "${CONFIG}" config_json)
    string(REGEX MATCH "\"XIAOTAI_DEVELOPMENT_CONSOLE\"[ \t]*:[ \t]*false" console_disabled
           "${config_json}")
    if(console_disabled)
        string(REGEX MATCH "(^|\n)[^\n]*[ \t]starter_console_start(\r?\n|$)" console_linked
               "${symbols}")
        if(console_linked)
            message(FATAL_ERROR "Disabled development console remains linked")
        endif()
    endif()
endif()

string(REGEX MATCH "(^|\n)[^\n]*[ \t]i2c_driver_install(\r?\n|$)" legacy_i2c "${symbols}")
string(REGEX MATCH "(^|\n)[^\n]*[ \t]i2c_new_master_bus(\r?\n|$)" modern_i2c "${symbols}")
if(legacy_i2c AND modern_i2c)
    message(FATAL_ERROR "Firmware links both legacy I2C and driver_ng")
endif()

if(DEFINED MAP AND EXISTS "${MAP}")
    file(READ "${MAP}" link_map)
    string(REPLACE "\\" "/" link_map "${link_map}")
    if(link_map MATCHES "driver/libdriver\\.a\\(i2c\\.c\\.obj\\)" OR
       link_map MATCHES "esp_lcd_panel_io_i2c_v1\\.c\\.obj")
        message(FATAL_ERROR "Firmware link map contains a legacy I2C object")
    endif()
endif()

message(STATUS "Firmware product and I2C ownership checks passed")
