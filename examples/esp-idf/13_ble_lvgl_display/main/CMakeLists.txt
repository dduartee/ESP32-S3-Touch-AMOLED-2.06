idf_component_register(
    SRCS main.c
    INCLUDE_DIRS .
    REQUIRES bt_nus nvs_flash)

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
if(TARGET ${LVGL_LIB})
    target_compile_options(${LVGL_LIB} PRIVATE -Wno-error=attributes -Wno-error=cpp)
endif()
