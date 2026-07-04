idf_component_register(
    SRCS "ssd1680.c" "ssd1680_guard.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_spi esp_driver_gpio
)
