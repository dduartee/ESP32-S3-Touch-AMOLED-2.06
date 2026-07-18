idf_component_register(
    SRCS main.cpp pcf85063.c
    INCLUDE_DIRS .)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=cpp)
