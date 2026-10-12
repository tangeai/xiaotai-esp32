# Keep LVGL's bounded TLSF pool and allocator semantics. Only its backing BSS
# moves; esp_lvgl_port's ISR controls and SPI DMA transfer buffer are untouched.
function(starter_enable_lvgl_psram_pool)
    if(NOT CONFIG_IDF_TARGET_ESP32S3 OR NOT CONFIG_SPIRAM OR
       NOT CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY)
        message(FATAL_ERROR "LVGL pool placement requires S3 PSRAM and external BSS")
    endif()
    if(CONFIG_LV_MEM_CUSTOM OR NOT CONFIG_LV_MEM_ADDR MATCHES "^(0[xX]0+|0+)$")
        message(FATAL_ERROR "LVGL allocator configuration changed; review PSRAM pool placement")
    endif()
    idf_component_get_property(lvgl_dir lvgl__lvgl COMPONENT_DIR)
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
    set(source "${lvgl_dir}/src/misc/lv_mem.c")
    get_target_property(sources ${lvgl_lib} SOURCES)
    list(FIND sources "${source}" source_index)
    if(source_index EQUAL -1)
        message(FATAL_ERROR "LVGL memory source registration changed")
    endif()
    file(READ "${source}" original)
    foreach(anchor IN ITEMS
            "#include LV_MEM_POOL_INCLUDE"
            "static LV_ATTRIBUTE_LARGE_RAM_ARRAY MEM_UNIT work_mem_int[LV_MEM_SIZE / sizeof(MEM_UNIT)];")
        string(FIND "${original}" "${anchor}" anchor_pos)
        if(anchor_pos EQUAL -1)
            message(FATAL_ERROR "LVGL pool hooks changed; review ${source}")
        endif()
    endforeach()
    # Scope these supported LVGL hooks to lv_mem.c in its owning target's
    # directory. No managed source edits, custom allocator or global remapping.
    set_property(SOURCE "${source}" TARGET_DIRECTORY ${lvgl_lib} APPEND
        PROPERTY COMPILE_DEFINITIONS
        "LV_MEM_POOL_INCLUDE=\"esp_attr.h\""
        "LV_ATTRIBUTE_LARGE_RAM_ARRAY=EXT_RAM_BSS_ATTR")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
endfunction()
