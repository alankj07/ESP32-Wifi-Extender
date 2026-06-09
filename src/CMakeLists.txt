idf_component_register(
    SRCS "main.c" "wifi_manager.c" "napt_router.c" "status_monitor.c" "web_portal.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "index_html_gz.txt"
    REQUIRES cjson esp_http_server
)
