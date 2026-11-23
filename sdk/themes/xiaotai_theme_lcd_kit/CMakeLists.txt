# SPDX-License-Identifier: MIT
# XiaoTai colour theme kit (xiaotai/theme_lcd.h): the one LVGL implementation
# of the generic view kinds for RGB565 displays, which every colour theme
# (xiaotai_theme_lcd320, xiaotai_theme_lcd480, any later size) depends on and
# forwards to. theme.json has no display rule: the kit registers no theme and
# takes no part in the build-time display check, so a product selecting only
# a large-display theme no longer brings in the 320x240 one. UI_LVGL in
# theme.json switches the UI system's LVGL backend on for every product that
# selects a colour theme; the LVGL configuration the colour products share
# goes into this component's sdkconfig.defaults, and only here.
#
# The generated glyphs are a common asset, not a dependency on the old UI
# component. Keep exactly one source and leave the old build inputs intact.
set(sources src/lcd_kit_unit.c)
set(views src/lcd_kit.c src/lcd_layout.c src/lcd_widgets.c src/lcd_home.c
    src/lcd_face.c src/lcd_face_data.c src/lcd_face_live.c src/lcd_pages.c src/lcd_form.c)
set(assets "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/xiaotai_lvgl/fonts")
list(APPEND views "${assets}/ui_font_cn_18.c" "${assets}/ui_font_cn_18_supplement.c")
set(phone "${CMAKE_CURRENT_SOURCE_DIR}/assets/phone_image.inc")
set(requires xiaotai_port xiaotai_framework xiaotai_hal xiaotai_ui)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} ${views} INCLUDE_DIRS "include"
                           REQUIRES ${requires} PRIV_REQUIRES lvgl)
    # The drawing code (face rasterizer, page layout) has always been built at
    # -O2 on the product that renders the face directly into the frame; the
    # kit applies it on every target.
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE XIAOTAI_LCD_PHONE_IMAGE="${phone}")
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
add_library(xiaotai_theme_lcd_kit STATIC ${sources})
target_include_directories(xiaotai_theme_lcd_kit PUBLIC include)
target_link_libraries(xiaotai_theme_lcd_kit PUBLIC ${requires})
if(TARGET lvgl::lvgl)
    target_sources(xiaotai_theme_lcd_kit PRIVATE ${views})
    target_link_libraries(xiaotai_theme_lcd_kit PRIVATE lvgl::lvgl)
    target_compile_definitions(xiaotai_theme_lcd_kit PRIVATE XIAOTAI_LCD_PHONE_IMAGE="${phone}")
endif()
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
