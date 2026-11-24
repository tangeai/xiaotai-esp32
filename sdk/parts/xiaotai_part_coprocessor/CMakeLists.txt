# SPDX-License-Identifier: MIT
# XiaoTai parts library: the network part of a Wi-Fi coprocessor
# (xiaotai/parts/coprocessor.h) - link bring-up, firmware check and update,
# diagnostics.
#
# Portable (src/): creation, prepare() with the decision rules, the attempt
# marker (kv port), the transfer and coprocessor_info() (coprocessor.c), and
# the reader of the application image format shared with the manual update
# (coprocessor_internal.h). The platform does the smallest link actions
# behind src/coprocessor_link.h:
#   esp-idf/coprocessor_link.c   the host-link component (work package A15)
#   host/coprocessor_link.c      a scripted fake coprocessor that tests of this
#                                part and of the boards built on it program
#                                through host/include/xiaotai_coprocessor_host.h
set(sources src/coprocessor.c src/coprocessor_update.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} esp-idf/coprocessor_link.c
                           INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src"
                           REQUIRES xiaotai_port xiaotai_hal PRIV_REQUIRES espressif__esp_hosted esp_event esp_wifi)
    include(esp-idf/hosted_overlay.cmake)
    # Target properties must be edited after all Hosted targets exist.
    if(CONFIG_ESP_HOSTED_HOST_TRANSPORT_BUS_SDIO)
        cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL xiaotai_coprocessor_hosted_overlay TRUE)
    else()
        cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL xiaotai_coprocessor_hosted_overlay FALSE)
    endif()
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port xiaotai_hal)
add_library(xiaotai_part_coprocessor STATIC ${sources} host/coprocessor_link.c)
target_include_directories(xiaotai_part_coprocessor PUBLIC include host/include PRIVATE src)
target_link_libraries(xiaotai_part_coprocessor PUBLIC xiaotai_port xiaotai_hal)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
