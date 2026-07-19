idf_component_register(
    SRCS
        "board_audio.cpp"
        "repeater_main.cpp"
        "wifi_manager.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        esp_codec_dev
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_event
        esp_http_client
        esp_http_server
        esp_netif
        esp_wifi
        nvs_flash
)
