idf_component_register(SRCS main.cpp INCLUDE_DIRS "."
    REQUIRES bt_nus bt nvs_flash esp_driver_gpio)
