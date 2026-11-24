# SPDX-License-Identifier: MIT
# XiaoTai UI system: keeps the views applications show (xiaotai/view.h), picks
# the theme that matches each display part, turns pointer and navigation input
# into the intents a view names, and owns the screen's sleep timeout. The core
# (src/) is portable and holds the whole-frame backend; the LVGL backend has a
# portable part (src/lvgl/) and a part per port (esp-idf/, host/).
#
# Implemented: the view slots, the engine lifecycle and configuration
# (src/ui_core.c), the bounded view copies and their comparison
# (src/ui_store.c), the theme, template and image registries
# (src/ui_registry.c), and the scene and input (src/ui_scene.c, src/ui_input.c).
# src/ui_internal.h is what the later parts build on.
# UI task scheduling, power and whole-frame output are shared by both
# execution forms (src/ui_task.c, src/ui_power.c, src/ui_frame.c).
# Diagnostics and commands, and the LVGL backend join the same list package
# by package (esp-idf/ and host/ with the LVGL requirement in the two
# branches). On the host LVGL comes from the directory tests/sdk/run.py passes
# (prepared by tests/host_components.py): the default entry fails when it is
# missing, and only an explicit --allow-missing skips the LVGL tests, listing
# them as not run.
#
# Optional part, compiled when the product selects the manifest name (build
# property XIAOTAI_SELECTED; the host build takes every one that exists):
#   src/with_video.c   "video" (adds xiaotai_video to REQUIRES): the built-in
#                      video source behind xiaotai_ui_video_show()
#   src/video_none.c   without "video": xiaotai_ui_video_show() answers
#                      XIAOTAI_ERR_NOT_SUPPORTED
# The public headers include no optional component.
set(sources src/ui_core.c src/ui_store.c src/ui_registry.c src/ui_scene.c src/ui_input.c
            src/ui_task.c src/ui_power.c src/ui_frame.c src/ui_diag.c src/ui_inject.c src/ui_commands.c
            src/ui_video.c)
set(requires xiaotai_port xiaotai_framework xiaotai_hal xiaotai_eventlog xiaotai_at)
if(ESP_PLATFORM)
    idf_build_get_property(selected XIAOTAI_SELECTED)
elseif(XIAOTAI_HOST_PRODUCT_SELECTION)
    set(selected ${XIAOTAI_SELECTED})
else()
    set(selected video)
endif()
if("video" IN_LIST selected)
    list(APPEND requires xiaotai_video)
    list(APPEND sources src/with_video.c)
else()
    list(APPEND sources src/video_none.c)
endif()
if(ESP_PLATFORM)
    if(CONFIG_XIAOTAI_UI_LVGL)
        list(APPEND sources src/lvgl/ui_lvgl.c src/lvgl/ui_lvgl_draw.c
                            esp-idf/ui_lvgl_idf.c esp-idf/ui_lvgl_mem.c)
    else()
        list(APPEND sources src/ui_no_lvgl.c)
    endif()
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src"
                           REQUIRES ${requires} PRIV_REQUIRES esp_lcd heap)
    idf_component_optional_requires(PRIVATE lvgl lvgl__lvgl)
    if(CONFIG_XIAOTAI_UI_LVGL)
        # LVGL calls our allocator after the linker has scanned this archive.
        target_link_options(${COMPONENT_LIB} INTERFACE "LINKER:-u,lv_mem_init")
    endif()
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
if(NOT XIAOTAI_HOST_PRODUCT_SELECTION OR CONFIG_XIAOTAI_UI_LVGL)
    xiaotai_host_role_path(lvgl_root lvgl ui_lvgl_painter ui_lvgl_video ui_lvgl_pointer ui_lvgl_even_areas
                          ui_lvgl_idf_port ui_lvgl_idf_504 ui_lvgl_idf_555 ui_lvgl_idf_602
                          ui_lvgl_idf_no_dsi ui_lvgl_idf_memory)
endif()
if(lvgl_root)
    if(NOT TARGET lvgl)
        set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
        set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
        set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/host/lv_conf.h"
            CACHE FILEPATH "Configuration for the pinned SDK host LVGL" FORCE)
        add_subdirectory("${lvgl_root}" "${CMAKE_CURRENT_BINARY_DIR}/lvgl" EXCLUDE_FROM_ALL)
        target_compile_options(lvgl PRIVATE -Wno-error)
    endif()
    list(APPEND sources src/lvgl/ui_lvgl.c src/lvgl/ui_lvgl_draw.c host/ui_lvgl_host.c)
else()
    list(APPEND sources src/ui_no_lvgl.c)
endif()
xiaotai_host_require(${requires})
add_library(xiaotai_ui STATIC ${sources})
target_include_directories(xiaotai_ui PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_ui PUBLIC ${requires})
if(lvgl_root)
    target_compile_definitions(xiaotai_ui PUBLIC CONFIG_XIAOTAI_UI_LVGL=1)
    target_include_directories(xiaotai_ui PUBLIC host/include)
    target_link_libraries(xiaotai_ui PUBLIC lvgl::lvgl m)
endif()
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
