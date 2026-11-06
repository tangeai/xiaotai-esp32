idf_component_register(
 SRCS "${CMAKE_CURRENT_LIST_DIR}/src/wifi_manager.c" "${CMAKE_CURRENT_LIST_DIR}/src/captive_dns.c"
      "${CMAKE_CURRENT_LIST_DIR}/src/wifi_history.c"
      "${CMAKE_CURRENT_LIST_DIR}/src/wifi_udp_probe.c"
      "${CMAKE_CURRENT_LIST_DIR}/src/wifi_path_trace.c"
 INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/include"
 PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/src"
 REQUIRES esp_wifi esp_netif esp_event nvs_flash nvs_store
 PRIV_REQUIRES esp_http_server cjson esp_system esp_timer lwip
 EMBED_TXTFILES "web/setup.html"
)
if(CONFIG_APP_PACKET_PATH_TRACE)
 target_link_libraries(${COMPONENT_LIB} INTERFACE
     "-Wl,--wrap=esp_netif_receive" "-Wl,--wrap=lwip_recvfrom")
endif()
