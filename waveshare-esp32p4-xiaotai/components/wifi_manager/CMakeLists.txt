idf_component_register(
 SRCS "${CMAKE_CURRENT_LIST_DIR}/src/wifi_manager.c" "${CMAKE_CURRENT_LIST_DIR}/src/captive_dns.c"
      "${CMAKE_CURRENT_LIST_DIR}/src/wifi_history.c"
 INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/include"
 PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/src"
 REQUIRES esp_wifi esp_netif esp_event nvs_flash nvs_store
 PRIV_REQUIRES esp_http_server json esp_system esp_timer lwip
 EMBED_TXTFILES "web/setup.html"
)
