idf_component_register(SRC_DIRS          "."
                       INCLUDE_DIRS      ".")

target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_SOURCE_DIR}/magic_enum/include/magic_enum")

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
