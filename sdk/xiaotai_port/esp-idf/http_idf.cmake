# SPDX-License-Identifier: MIT
function(xiaotai_port_configure_http)
    idf_component_get_property(http_dir esp_http_client COMPONENT_DIR)
    idf_component_get_property(http_lib esp_http_client COMPONENT_LIB)
    set(source "${http_dir}/esp_http_client.c")
    get_target_property(sources ${http_lib} SOURCES)
    if(NOT source IN_LIST sources)
        message(FATAL_ERROR "ESP HTTP source registration changed; review payload logging")
    endif()
    # The stock DEBUG messages include full request and response headers.
    # Keep INFO/WARN/ERROR and all port timing/error data; no global log change.
    set_property(SOURCE "${source}" TARGET_DIRECTORY ${http_lib} APPEND
                 PROPERTY COMPILE_DEFINITIONS "LOG_LOCAL_LEVEL=ESP_LOG_INFO")
endfunction()
