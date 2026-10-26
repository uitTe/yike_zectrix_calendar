idf_component_register(
    SRCS "cal_wifi.cc" "cal_provision.cc" "cal_cred.cc"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_timer
    PRIV_REQUIRES esp_http_server lwip json esp_driver_usb_serial_jtag
)
