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
                            "fonts/lv_font_cjk_16.c"
                            "fonts/lv_font_cjk_28.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_timer hal soc esp_driver_uart m5_tab5_component i2c_bus m5_tab5_keyboard_component nvs_flash spiffs)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-field-initializers)

# Bundle the UTF-8/LF SKK-JISYO.S dictionary into the existing `storage`
# SPIFFS partition.  It becomes /skk/SKK-JISYO.S.txt at runtime.
spiffs_create_partition_image(storage "${CMAKE_CURRENT_LIST_DIR}/../assets" FLASH_IN_PROJECT)
