# ESP-Hosted 3.0.7 selects the SDIO interrupt bits of a C61 coprocessor with
# EH_HOST_CP_TARGET_ESP32C61 (port/os/include/eh_host_port_sdio_reg.h) but
# its configuration header never defines that name, so a C61 on SDIO does not
# compile. The vendor tree stays untouched: the two targets that compile the
# SDIO transport get the definition here, from CONFIG_ESP_HOSTED_CP_TARGET_ESP32C61.
function(xiaotai_hosted_sdio_c61)
    if(NOT CONFIG_ESP_HOSTED_CP_TARGET_ESP32C61 OR NOT CONFIG_ESP_HOSTED_HOST_TRANSPORT_BUS_SDIO)
        return()
    endif()
    foreach(target eh_host_port_idf eh_host_mcu_transport)
        if(NOT TARGET ${target})
            message(FATAL_ERROR "Hosted SDIO target ${target} is missing; review the C61 adapter")
        endif()
        target_compile_definitions(${target} PRIVATE EH_HOST_CP_TARGET_ESP32C61=1)
    endforeach()
endfunction()
