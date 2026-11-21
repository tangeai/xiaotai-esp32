# SPDX-License-Identifier: MIT
# XiaoTai theme for colour displays of 480x320 and larger
# (xiaotai/theme_lcd480.h), drawn with LVGL. It scales the page layouts of the
# colour theme kit (xiaotai_theme_lcd_kit, xiaotai/theme_lcd.h) and adds the
# background, the clock on HOME and the video region. theme.json depends on
# theme_lcd_kit as this file requires it (an undeclared requirement fails at
# configure time, when only selected components are on the search path), and
# not on theme_lcd320: a product with a large display builds no 320x240 theme.
# The LVGL sdkconfig values come with the kit; this component carries none.
set(sources src/theme_lcd480.c src/lcd480_background.c)
set(requires xiaotai_port xiaotai_framework xiaotai_hal xiaotai_ui xiaotai_theme_lcd_kit)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" REQUIRES ${requires} PRIV_REQUIRES lvgl)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
if(TARGET lvgl::lvgl)
    add_library(xiaotai_theme_lcd480 STATIC ${sources})
    target_include_directories(xiaotai_theme_lcd480 PUBLIC include)
    target_link_libraries(xiaotai_theme_lcd480 PUBLIC ${requires} PRIVATE lvgl::lvgl)
else()
    add_library(xiaotai_theme_lcd480 INTERFACE)
    target_include_directories(xiaotai_theme_lcd480 INTERFACE include)
    target_link_libraries(xiaotai_theme_lcd480 INTERFACE ${requires})
endif()
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
