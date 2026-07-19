idf_component_register(
    SRCS "eye_demo_main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_gpio esp_driver_spi
)