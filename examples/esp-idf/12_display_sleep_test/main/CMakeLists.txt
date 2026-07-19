idf_component_register(
    SRCS main.cpp
    INCLUDE_DIRS .
    REQUIRES XPowersLib waveshare__esp32_s3_touch_amoled_2_06
    PRIV_REQUIRES nvs_flash lvgl__lvgl espressif__esp_lvgl_port)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=cpp)
