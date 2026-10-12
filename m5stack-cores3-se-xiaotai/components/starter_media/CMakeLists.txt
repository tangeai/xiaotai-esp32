idf_component_register(
    SRCS
        "src/starter_aec.c"
        "src/starter_agc.c"
        "src/starter_audio_resampler.c"
        "src/starter_preroll.c"
        "src/starter_playout.c"
        "src/starter_camera_unavailable.c"
        "src/starter_media.c"
    INCLUDE_DIRS "include"
    LDFRAGMENTS "esp_sr_hotpath.lf"
    REQUIRES starter_tirtc starter_voice starter_board esp_driver_i2c
    PRIV_REQUIRES
        starter_log
        esp_driver_gpio
        esp_driver_i2s
        esp_timer
        esp_system
        freertos
        log
        esp_codec_dev
        esp_audio_codec
        esp-sr
        perfmon
)

if(CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE)
    option(XIAOTAI_AFE_KERNEL_PROBE "Enable intrusive ESP-SR inner-kernel timing" OFF)
    option(XIAOTAI_BSS_WEIGHT_INTERNAL "Experiment: place one BSS weight block in internal RAM" OFF)
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=aec_linear_process"
        "-Wl,--wrap=aec_nlp_process"
        "-Wl,--wrap=esp_bss_hps16fft_process"
        "-Wl,--wrap=esp_bss_728_process"
        "-Wl,--wrap=esp_bss_hps16fft_init"
        "-Wl,--wrap=dl_lib_calloc"
        "-Wl,--wrap=dl_lib_calloc_psram"
    )
    if(XIAOTAI_BSS_WEIGHT_INTERNAL)
        file(SHA256 "${PROJECT_DIR}/managed_components/espressif__esp-sr/lib/esp32s3/libesp_audio_processor.a" bss_hash)
        file(SHA256 "${PROJECT_DIR}/managed_components/espressif__esp-sr/lib/esp32s3/libdl_lib.a" allocator_hash)
        if(NOT bss_hash STREQUAL "241c2450b46bcf15d7f221f8afc398dea365bcdd170cdd9efc195318a6af31ca" OR
           NOT allocator_hash STREQUAL "fa3c9c299a566f2648b64e8136def1673238f8ea24a1aabeeddae5b4f9fcc2b3")
            message(FATAL_ERROR "BSS internal-weight experiment requires the verified ESP-SR 2.5.0 allocation/free ABI")
        endif()
    endif()
    if(XIAOTAI_BSS_WEIGHT_INTERNAL)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE XIAOTAI_BSS_WEIGHT_INTERNAL=1)
        target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=dl_lib_free")
    endif()
    if(XIAOTAI_AFE_KERNEL_PROBE)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE XIAOTAI_AFE_KERNEL_PROBE=1)
        target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=test_real_fft_hp"
        "-Wl,--wrap=test_real_ifft_hp"
        "-Wl,--wrap=bss_update_variance_728"
        "-Wl,--wrap=bss_update_covariance_728"
        "-Wl,--wrap=bss_compute_middleW_728"
        "-Wl,--wrap=bss_compute_normalizing_factor_728"
        "-Wl,--wrap=div_rvf_rvf_rvf_728"
        "-Wl,--wrap=sqrtinv_rvf_rvf_728"
        "-Wl,--wrap=mul_cvf_ccvf_cvf_728"
        "-Wl,--wrap=power_cvf_rvf_728"
        "-Wl,--wrap=memcpy_int16_728"
        "-Wl,--wrap=memzero_int16_728"
        "-Wl,--wrap=short2float_728"
        "-Wl,--wrap=float2short_728"
        "-Wl,--wrap=dl_audio_float2short_728"
        "-Wl,--wrap=mul_cvf_rvf_cvf_728"
        )
    endif()
endif()

# These sample-by-sample loops share the 32 ms dual-AEC deadline. Keep debug
# symbols and explicitly optimize these loops without fast-math.
set_source_files_properties(
    "src/starter_aec.c"
    "src/starter_audio_resampler.c"
    PROPERTIES COMPILE_OPTIONS "-O2"
)
