# esp-tflite-micro 1.3.5 passes a legacy zero filter-channel count to ESP-NN.
# ESP-NN 1.3.2 dispatches that to ANSI rather than S3 SIMD. Preserve the model
# and kernels; supply the real channel dimension at the adapter boundary.
function(starter_patch_tflm_conv source output)
    file(READ "${source}" original)
    string(REPLACE "\r\n" "\n" original "${original}")
    string(SHA256 source_hash "${original}")
    if(NOT source_hash STREQUAL "d5cf0ecaee5b6d8b7f141be364c85beab42a0f72bec8e230e2e69d8f605ea42d")
        message(FATAL_ERROR "TFLM conv adapter changed; review/remove the channel compatibility fix before building")
    endif()

    set(legacy ".channels = 0, .extra = 0")
    string(FIND "${original}" "${legacy}" prepare_pos)
    string(SUBSTRING "${original}" 0 ${prepare_pos} prefix)
    string(LENGTH "${legacy}" legacy_len)
    math(EXPR after_prepare "${prepare_pos} + ${legacy_len}")
    string(SUBSTRING "${original}" ${after_prepare} -1 suffix)
    string(REPLACE "${legacy}" ".channels = filter_shape.Dims(3), .extra = 0" suffix "${suffix}")
    set(patched "${prefix}.channels = filter_input_channels, .extra = 0${suffix}")
    # COPYONLY avoids touching the generated source on every configure.
    file(WRITE "${output}.tmp" "${patched}")
    configure_file("${output}.tmp" "${output}" COPYONLY)
    file(REMOVE "${output}.tmp")
endfunction()

function(starter_enable_tflm_conv_channels)
    idf_component_get_property(tflm_dir espressif__esp-tflite-micro COMPONENT_DIR)
    idf_component_get_property(tflm_lib espressif__esp-tflite-micro COMPONENT_LIB)
    set(original "${tflm_dir}/tensorflow/lite/micro/kernels/esp_nn/conv.cc")
    set(generated "${CMAKE_BINARY_DIR}/starter_tflm/conv.cc")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/starter_tflm")
    starter_patch_tflm_conv("${original}" "${generated}")

    get_target_property(sources ${tflm_lib} SOURCES)
    list(FIND sources "${original}" source_index)
    if(source_index EQUAL -1)
        message(FATAL_ERROR "TFLM conv source registration changed; refusing an unverified override")
    endif()
    list(REMOVE_ITEM sources "${original}")
    list(APPEND sources "${generated}")
    set_property(TARGET ${tflm_lib} PROPERTY SOURCES "${sources}")
    # The replacement remains in the upstream target, retaining its flags,
    # include paths and dependencies. managed_components is never edited.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
endfunction()
