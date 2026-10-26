idf_component_register(
    SRCS "cal_sched_parse.c" "cal_sched_store.cc" "cal_sched_http.cc"
    INCLUDE_DIRS "include"
    REQUIRES cal_net
    PRIV_REQUIRES esp_http_client json mbedtls nvs_flash esp_timer
)
