# SPDX-License-Identifier: MIT
# XiaoTai theme for 320x240 colour displays (xiaotai/theme_lcd320.h): the
# colour theme kit (xiaotai_theme_lcd_kit, xiaotai/theme_lcd.h) without
# additions. This component holds only the registration (name, match,
# variant); the drawing code and the LVGL configuration are the kit's, so
# theme.json depends on theme_lcd_kit as this file requires it (an undeclared
# requirement fails at configure time, when only selected components are on
# the search path).
set(sources src/theme_lcd320.c)
set(requires xiaotai_port xiaotai_framework xiaotai_hal xiaotai_ui xiaotai_theme_lcd_kit)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" REQUIRES ${requires})
    return()
endif()
# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
if(TARGET lvgl::lvgl)
    add_library(xiaotai_theme_lcd320 STATIC ${sources})
    target_include_directories(xiaotai_theme_lcd320 PUBLIC include)
    target_link_libraries(xiaotai_theme_lcd320 PUBLIC ${requires})
else()
    add_library(xiaotai_theme_lcd320 INTERFACE)
    target_include_directories(xiaotai_theme_lcd320 INTERFACE include)
    target_link_libraries(xiaotai_theme_lcd320 INTERFACE ${requires})
endif()
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
