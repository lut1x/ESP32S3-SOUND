idf_component_register(SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi esp_netif nvs_flash esp_http_server json
                  fatfs sdmmc esp_driver_sdmmc esp_driver_i2s esp_driver_gpio
    EMBED_TXTFILES "index.html")
