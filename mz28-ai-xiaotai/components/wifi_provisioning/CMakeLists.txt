idf_component_register(
    SRCS "wifi_provisioning.c" "captive_dns.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi esp_netif esp_event esp_http_server esp_timer json lwip
    EMBED_TXTFILES "web/setup.html")
