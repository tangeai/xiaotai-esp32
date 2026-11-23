# Shared LVGL layer of the colour-screen products: draw hooks and the 18 px Chinese font.
idf_component_register(SRCS xiaotai_lvgl.c fonts/ui_font_cn_18.c fonts/ui_font_cn_18_supplement.c
    INCLUDE_DIRS include REQUIRES lvgl esp_common)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(NOT CONFIG_LV_USE_DRAW_SW)
        message(FATAL_ERROR "XiaoTai image hooks require the LVGL software renderer")
    endif()
endif()
