idf_component_register(SRCS "xmini_memtrace.c" "xmini_heap_failure.c" INCLUDE_DIRS "include"
                       REQUIRES freertos heap esp_timer spi_flash)
option(XMINI_MEMTRACE "Instrument only SDK Mx allocation ownership" OFF)
if(XMINI_MEMTRACE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE XMINI_MEMTRACE_ENABLED=1)
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=Mx_malloc,--wrap=Mx_calloc,--wrap=Mx_realloc,--wrap=Mx_free")
endif()
