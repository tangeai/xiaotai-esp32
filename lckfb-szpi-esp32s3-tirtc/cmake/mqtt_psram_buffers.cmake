# IDF 5.5.4 exposes MQTT buffer sizes, but no RX/TX allocator selection.
# Adapt only task-context payload allocations in build-local source copies.
# Do not change the SDK installation, task lifecycle, locks or global malloc.
function(starter_mqtt_psram_source source output expected_hash before after)
    file(READ "${source}" original)
    string(REPLACE "\r\n" "\n" original "${original}")
    string(SHA256 source_hash "${original}")
    if(NOT source_hash STREQUAL expected_hash)
        message(FATAL_ERROR "MQTT source changed: ${source}; review the PSRAM buffer adapter")
    endif()
    string(FIND "${original}" "${before}" allocation_pos)
    if(allocation_pos EQUAL -1)
        message(FATAL_ERROR "MQTT allocation anchor missing: ${source}")
    endif()
    string(REPLACE "${before}" "${after}" patched "${original}")
    # mqtt_msg.c does not otherwise include the capability allocator header.
    set(patched "#include \"esp_heap_caps.h\"\n${patched}")
    get_filename_component(output_dir "${output}" DIRECTORY)
    file(MAKE_DIRECTORY "${output_dir}")
    file(WRITE "${output}.tmp" "${patched}")
    configure_file("${output}.tmp" "${output}" COPYONLY)
    file(REMOVE "${output}.tmp")
endfunction()

function(starter_enable_mqtt_psram_buffers)
    if(NOT CONFIG_SPIRAM OR NOT CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY)
        message(FATAL_ERROR "S3 MQTT payload placement requires PSRAM and external BSS")
    endif()
    if(CONFIG_MQTT_CUSTOM_OUTBOX)
        message(FATAL_ERROR "Custom MQTT outbox needs its own reviewed PSRAM allocation policy")
    endif()
    idf_component_get_property(mqtt_dir mqtt COMPONENT_DIR)
    idf_component_get_property(mqtt_lib mqtt COMPONENT_LIB)
    # IDF's mqtt component is a wrapper; its registered sources belong to the
    # esp-mqtt submodule. Resolve that layout before matching target sources.
    set(mqtt_dir "${mqtt_dir}/esp-mqtt")
    get_target_property(sources ${mqtt_lib} SOURCES)

    set(files mqtt_client.c lib/mqtt_msg.c lib/mqtt_outbox.c)
    set(hashes
        3e558fa88e7bd98d4342bd19fe6d99c56031d04ff7a26405ab1bcd09408cf6f3
        f536a71cb75bcf054f997bda520c3a2f804daa29831ed18277fb3d8f9fe00087
        e7c4a58f7d36ada36d51b7a045a541a19a765e242b1be4007f397a90033fdb8a)
    set(old_allocations
        "(uint8_t *)malloc(buffer_size)"
        "(uint8_t *)calloc(buffer_size, sizeof(uint8_t))"
        "heap_caps_malloc(message->len + message->remaining_len, MQTT_OUTBOX_MEMORY)")
    set(new_allocations
        "(uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)"
        "(uint8_t *)heap_caps_calloc(buffer_size, sizeof(uint8_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)"
        "heap_caps_malloc(message->len + message->remaining_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)")

    foreach(index RANGE 0 2)
        list(GET files ${index} relative)
        list(GET hashes ${index} expected_hash)
        list(GET old_allocations ${index} before)
        list(GET new_allocations ${index} after)
        set(original "${mqtt_dir}/${relative}")
        set(generated "${CMAKE_BINARY_DIR}/starter_mqtt/${relative}")
        list(FIND sources "${original}" source_index)
        if(source_index EQUAL -1)
            message(FATAL_ERROR "MQTT source registration changed: ${relative}")
        endif()
        starter_mqtt_psram_source("${original}" "${generated}"
            "${expected_hash}" "${before}" "${after}")
        list(REMOVE_ITEM sources "${original}")
        list(APPEND sources "${generated}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
    endforeach()
    # Keep the upstream target's compile flags, include paths and dependencies.
    # NULL checks and free() remain upstream-owned, with no internal fallback.
    set_property(TARGET ${mqtt_lib} PROPERTY SOURCES "${sources}")
endfunction()
