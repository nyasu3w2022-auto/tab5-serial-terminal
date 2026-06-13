idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_timer hal soc m5_tab5_component i2c_bus m5_tab5_keyboard_component nvs_flash)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-field-initializers)
