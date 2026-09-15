idf_component_register(SRCS "main.cpp"
                            "terminal.cpp"
                            "display.cpp"
                            "usb_serial.cpp"
                            "porta_uart.cpp"
                            "mbus_uart.cpp"
                            "serial_transport.cpp"
                            "settings.cpp"
                            "settings_ui.cpp"
                            "ime_skk.cpp"
                            "ime_ui.cpp"
                            "dictionary_ui.cpp"
                            "fonts/lv_font_cjk_16.c"
                            "fonts/lv_font_cjk_28.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_timer hal soc esp_driver_uart m5_tab5_component i2c_bus m5_tab5_keyboard_component nvs_flash spiffs)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-field-initializers)

# Bundle the read-only UTF-8/LF system and TAB5 supplementary SKK dictionaries
# into the `skk` SPIFFS partition. The separate `userdict` partition is writable at runtime
# and therefore remains intact when the bundled dictionary image is reflashed.
spiffs_create_partition_image(skk "${CMAKE_CURRENT_LIST_DIR}/../assets" FLASH_IN_PROJECT)
