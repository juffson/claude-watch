idf_component_register(
    SRCS "claude_watch_service.c" "cw_status.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif esp_http_server nvs_flash lwip esp_timer esp_driver_usb_serial_jtag vfs
)
