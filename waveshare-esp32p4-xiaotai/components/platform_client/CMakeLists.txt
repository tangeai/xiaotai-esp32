idf_component_register(
 SRCS "${CMAKE_CURRENT_LIST_DIR}/src/platform_client.c"
      "${CMAKE_CURRENT_LIST_DIR}/src/platform_http_trace.c"
 INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/include"
 REQUIRES esp_http_client esp_netif mqtt mbedtls json
 PRIV_REQUIRES esp-tls esp_timer tcp_transport lwip
)

if(CMAKE_BUILD_EARLY_EXPANSION)
    return()
endif()
# Observe scoped platform HTTP only. IDF/SDK archives are not patched, and
# calls outside that task/scope retain their original arguments and results.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=lwip_getaddrinfo"
    "-Wl,--wrap=esp_transport_connect"
    "-Wl,--wrap=esp_transport_write"
    "-Wl,--wrap=esp_transport_read"
)
