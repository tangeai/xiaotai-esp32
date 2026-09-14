idf_component_register(
    SRCS "src/platform_client.c" "src/platform_http_trace.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp_netif mqtt mbedtls json
    PRIV_REQUIRES esp-tls esp_timer tcp_transport lwip
)

# Observe IDF calls only inside a scoped platform HTTP request. SDK/IDF sources
# and archives stay untouched; MQTT and TiRTC calls otherwise pass straight through.
if(CMAKE_BUILD_EARLY_EXPANSION)
    return()
endif()
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=lwip_getaddrinfo"
    "-Wl,--wrap=esp_transport_connect"
    "-Wl,--wrap=esp_transport_write"
    "-Wl,--wrap=esp_transport_read"
)
