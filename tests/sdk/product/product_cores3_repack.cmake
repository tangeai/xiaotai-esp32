# SPDX-License-Identifier: MIT
# Add the CoreS3 MR probe to a real product build without changing its source graph.
function(attach_cores3_repack)
    if(NOT XIAOTAI_PRODUCT_NAME STREQUAL "xiaotai-cores3-lite" OR
       NOT XIAOTAI_HOST_PRODUCT_CASE STREQUAL "default" OR NOT CONFIG_XIAOTAI_MEDIA_PROCESSOR_AEC_SINGLE_FRONTEND)
        message(FATAL_ERROR "The MR oracle requires the default CoreS3 frontend")
    endif()
    target_sources(product_host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/product_cores3_repack.c")
    target_include_directories(product_host PRIVATE "${XIAOTAI_ROOT}/sdk/xiaotai_media/src")
    target_link_options(product_host PRIVATE "LINKER:--wrap=xiaotai_start,--wrap=xiaotai_queue_init,--wrap=xiaotai_queue_send")
    target_link_libraries(product_host PRIVATE m)
    product_test(cores3_mr_legacy start)
    set_property(TEST "product_${product_id}_cores3_mr_legacy"
                 APPEND PROPERTY ENVIRONMENT "XIAOTAI_HOST_MR_LEGACY=1")
endfunction()
cmake_language(DEFER CALL attach_cores3_repack)
