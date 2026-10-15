idf_component_register(SRCS "voip.c" "voip_state.c" "voip_protocol.c" "sample_audio.c" "opus_packet.c"
                       INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "."
                       REQUIRES demo_platform rtc_adapter product_audio json esp_timer memory_probe)
